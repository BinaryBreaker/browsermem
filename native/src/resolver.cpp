#include "resolver.hpp"

#include <emmintrin.h>
#include <intrin.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "url_syntax.hpp"

namespace browsermem {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int64_t kNeighbourhood = 512;   // bytes either side of an anchor
constexpr size_t kMaxTitleHits = 4000;
constexpr size_t kMaxPointerRefs = 8000;
constexpr size_t kMaxInlineUrlBytes = 22;  // libc++ std::string SSO capacity
constexpr size_t kMaxCandidates = 8;
constexpr double kSubresourcePenalty = 0.25;  // favicons, images, scripts, ...
constexpr size_t kPage = 4096;
constexpr uint64_t kMinUserAddress = 0x10000;
constexpr uint64_t kMaxUserAddress = 0x7FFFFFFFFFFFull;

// Hot-region cache limits.
constexpr size_t kMaxHotRegions = 96;
constexpr size_t kMaxHotRegionSize = size_t(16) << 20;
constexpr size_t kMaxHotBytes = size_t(48) << 20;  // keep the fast path a small scan
constexpr double kFastPathMinConfidence = 0.6;
constexpr double kFastPathMinScore = 0.5;

double MsSince(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

struct Needle {
  std::vector<uint8_t> bytes;
  uint64_t alignment;
  size_t probe;  // index of the last non-zero byte: a better SIMD filter than a trailing 0
};

Needle MakeNeedle(std::vector<uint8_t> bytes, uint64_t alignment) {
  size_t probe = bytes.size() - 1;
  while (probe > 0 && bytes[probe] == 0) --probe;
  return {std::move(bytes), alignment, probe};
}

// All occurrences of `n` starting before `owned`. SSE2 filters candidate
// positions on the first byte and the probe byte, then memcmp confirms.
template <class F>
void FindNeedle(const uint8_t* d, size_t size, size_t owned, const Needle& n, F&& onHit) {
  const size_t len = n.bytes.size();
  if (size < len) return;
  const uint8_t* nd = n.bytes.data();
  const size_t limit = std::min(owned, size - len + 1);  // valid start positions
  const __m128i first = _mm_set1_epi8(static_cast<char>(nd[0]));
  const __m128i probe = _mm_set1_epi8(static_cast<char>(nd[n.probe]));
  size_t i = 0;
  for (; i + 16 <= limit && i + n.probe + 16 <= size; i += 16) {
    __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(d + i));
    __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(d + i + n.probe));
    unsigned mask = static_cast<unsigned>(
        _mm_movemask_epi8(_mm_and_si128(_mm_cmpeq_epi8(a, first), _mm_cmpeq_epi8(b, probe))));
    while (mask) {
      unsigned long bit;
      _BitScanForward(&bit, mask);
      if (std::memcmp(d + i + bit, nd, len) == 0) onHit(i + bit);
      mask &= mask - 1;
    }
  }
  for (; i < limit; ++i)
    if (d[i] == nd[0] && std::memcmp(d + i, nd, len) == 0) onHit(i);
}

std::vector<uint8_t> Utf16Bytes(const std::wstring& s) {
  const auto* p = reinterpret_cast<const uint8_t*>(s.data());
  return {p, p + s.size() * sizeof(wchar_t)};
}

std::vector<uint8_t> Utf8Bytes(const std::wstring& s) {
  std::string u = ToUtf8(s);
  return {u.begin(), u.end()};
}

bool InKnownRegion(const std::vector<RegionInfo>& regions, uint64_t address) {
  auto it = std::upper_bound(regions.begin(), regions.end(), address,
                             [](uint64_t a, const RegionInfo& r) { return a < r.base; });
  if (it == regions.begin()) return false;
  --it;
  return address < it->base + it->size;
}

// Reads URL strings at pointer targets, with a per-resolution cache.
class UrlReader {
 public:
  UrlReader(const MemorySource& src, const std::vector<RegionInfo>* known)
      : src_(src), known_(known) {}

  // Id of the URL starting exactly at `address`, or -1.
  int32_t At(uint64_t address) {
    if (address < kMinUserAddress || address > kMaxUserAddress) return -1;
    auto [it, inserted] = cache_.emplace(address, -1);
    if (!inserted) return it->second;
    if (known_ && !InKnownRegion(*known_, address)) return -1;

    uint8_t probe[48];
    size_t n = src_.Read(address, probe, sizeof(probe));
    std::string url;
    if (n < 8 || !LooksLikeUrlStart(probe, n)) return -1;
    std::vector<uint8_t> buf(kMaxUrlLength * 2 + 8);
    n = src_.Read(address, buf.data(), buf.size());
    if (!ParseUrlAt(buf.data(), n, url)) return -1;
    return it->second = Intern(std::move(url));
  }

  int32_t Intern(std::string url) {
    auto [it, inserted] = ids_.emplace(std::move(url), static_cast<int32_t>(urls_.size()));
    if (inserted) urls_.push_back(it->first);
    return it->second;
  }

  const std::string& url(int32_t id) const { return urls_[id]; }

 private:
  static bool LooksLikeUrlStart(const uint8_t* p, size_t n) {
    // Cheap pre-check before the full parse: [a-z-]+ then ':' (UTF-8 or UTF-16).
    size_t i = 0;
    while (i < n && IsSchemeChar(p[i])) ++i;
    if (i > 0 && i < n && p[i] == ':') return true;
    i = 0;
    while (i + 1 < n && p[i + 1] == 0 && IsSchemeChar(p[i])) i += 2;
    return i > 0 && i < n && p[i] == ':';
  }

  const MemorySource& src_;
  const std::vector<RegionInfo>* known_;
  std::unordered_map<uint64_t, int32_t> cache_;
  std::unordered_map<std::string, int32_t> ids_;
  std::vector<std::string> urls_;
};

// Scores URLs referenced from (or stored inline in) the bytes around `anchor`.
void VoteAround(const MemorySource& src, UrlReader& reader, uint64_t anchor,
                std::unordered_map<int32_t, double>& scores,
                std::unordered_map<int32_t, std::vector<uint64_t>>& where) {
  const uint64_t lo = anchor > uint64_t(kNeighbourhood) ? (anchor - kNeighbourhood) & ~7ull : 0;
  const uint64_t hi = anchor + kNeighbourhood + 8;
  uint8_t buf[2 * kNeighbourhood + 32];

  // Read page by page so an unreadable neighbour page doesn't hide the rest.
  std::unordered_map<int32_t, double> local;  // best contribution per URL for this anchor
  auto bump = [&](int32_t id, uint64_t at, uint64_t evidenceAddress) {
    double dist = at > anchor ? double(at - anchor) : double(anchor - at);
    double s = 1.0 / (1.0 + dist / 64.0);
    double& cur = local[id];
    if (s > cur) cur = s;
    auto& w = where[id];
    w.push_back(evidenceAddress);  // the URL string
    w.push_back(anchor);           // the object (or inline title) that referenced it
  };

  for (uint64_t seg = lo; seg < hi;) {
    uint64_t segEnd = std::min<uint64_t>(hi, (seg / kPage + 1) * kPage);
    size_t segLen = static_cast<size_t>(segEnd - seg);
    size_t got = src.Read(seg, buf + (seg - lo), segLen);
    if (got >= 8) {
      const uint8_t* data = buf + (seg - lo);
      // (a) pointers to heap-allocated URL strings (e.g. GURL::spec_)
      for (size_t off = 0; off + 8 <= got; off += 8) {
        uint64_t v;
        std::memcpy(&v, data + off, 8);
        int32_t id = reader.At(v);
        if (id >= 0) bump(id, seg + off, v);
      }
      // (b) short URLs stored inline in the object (small-string optimisation)
      CarveUrls(data, got, seg, got, [&](std::string url, uint64_t at) {
        if (url.size() <= kMaxInlineUrlBytes) bump(reader.Intern(std::move(url)), at, at);
      });
    }
    seg = segEnd;
  }
  for (const auto& [id, s] : local) scores[id] += s;
}

}  // namespace

// ---- CorrelateTitle ---------------------------------------------------------------

Resolution CorrelateTitle(const MemorySource& src, const std::vector<RegionInfo>& regions,
                          const std::vector<std::wstring>& titleCandidates,
                          size_t inlineTitleBytes, Evidence* evidence,
                          const std::vector<RegionInfo>* knownRegions) {
  Resolution res;
  if (!titleCandidates.empty()) res.title = titleCandidates.front();
  for (const RegionInfo& r : regions) res.stats.bytesScanned += r.size;

  // UTF-16 first (Chromium std::u16string, Gecko nsString), then UTF-8 /
  // Latin-1 (SpiderMonkey strings, nsCString).
  struct Candidate {
    size_t title;
    Needle needle;
  };
  std::vector<Candidate> candidates;
  size_t longest = 0;
  for (size_t t = 0; t < titleCandidates.size(); ++t) {
    for (int enc = 0; enc < 2; ++enc) {
      auto bytes = enc == 0 ? Utf16Bytes(titleCandidates[t]) : Utf8Bytes(titleCandidates[t]);
      if (bytes.size() < 4) continue;
      longest = std::max(longest, bytes.size());
      candidates.push_back({t, MakeNeedle(std::move(bytes), enc == 0 ? 2 : 1)});
    }
  }
  if (candidates.empty()) return res;

  // Pass 1: every needle in one streaming pass over memory.
  auto t0 = Clock::now();
  const unsigned workers = src.workers();
  std::vector<std::vector<std::vector<uint64_t>>> hitsByWorker(
      workers, std::vector<std::vector<uint64_t>>(candidates.size()));
  src.Scan(regions, longest - 1,
           [&](unsigned w, uint64_t base, const uint8_t* data, size_t size, size_t owned) {
             for (size_t c = 0; c < candidates.size(); ++c) {
               auto& out = hitsByWorker[w][c];
               if (out.size() >= kMaxTitleHits) continue;
               const Needle& n = candidates[c].needle;
               FindNeedle(data, size, owned, n, [&](size_t off) {
                 uint64_t a = base + off;
                 if (a % n.alignment == 0 && out.size() < kMaxTitleHits) out.push_back(a);
               });
             }
           });
  res.stats.titleMs += MsSince(t0);

  for (size_t c = 0; c < candidates.size(); ++c) {
    std::vector<uint64_t> hits;
    for (auto& perWorker : hitsByWorker) hits.insert(hits.end(), perWorker[c].begin(), perWorker[c].end());
    if (hits.empty()) continue;
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());

    // Pass 2: pointers to any of the title copies.
    t0 = Clock::now();
    const uint64_t lo = hits.front(), hi = hits.back();
    std::vector<std::vector<uint64_t>> refsByWorker(workers);
    src.Scan(regions, 0, [&](unsigned w, uint64_t base, const uint8_t* data, size_t, size_t owned) {
      auto& out = refsByWorker[w];
      const auto* q = reinterpret_cast<const uint64_t*>(data);
      const size_t count = owned / 8;
      for (size_t i = 0; i < count; ++i) {
        const uint64_t v = q[i];
        if (v < lo || v > hi) continue;
        if (out.size() < kMaxPointerRefs && std::binary_search(hits.begin(), hits.end(), v))
          out.push_back(base + i * 8);
      }
    });
    std::vector<uint64_t> refs;
    for (auto& r : refsByWorker) refs.insert(refs.end(), r.begin(), r.end());
    if (refs.size() > kMaxPointerRefs) refs.resize(kMaxPointerRefs);
    res.stats.pointerMs += MsSince(t0);

    // Pass 3: look around each anchor.
    t0 = Clock::now();
    UrlReader reader(src, knownRegions);
    std::unordered_map<int32_t, double> scores;
    std::unordered_map<int32_t, std::vector<uint64_t>> where;
    for (uint64_t ref : refs) VoteAround(src, reader, ref, scores, where);
    // A short title is stored inside its owning object, so the title bytes
    // themselves are an anchor.
    if (candidates[c].needle.bytes.size() <= inlineTitleBytes)
      for (uint64_t h : hits) VoteAround(src, reader, h, scores, where);
    res.stats.voteMs += MsSince(t0);
    if (scores.empty()) continue;

    for (const auto& [id, s] : scores) {
      const std::string& url = reader.url(id);
      res.candidates.push_back({url, LooksLikeSubresource(url) ? s * kSubresourcePenalty : s});
    }
    std::sort(res.candidates.begin(), res.candidates.end(),
              [](const UrlCandidate& a, const UrlCandidate& b) { return a.score > b.score; });
    if (res.candidates.size() > kMaxCandidates) res.candidates.resize(kMaxCandidates);

    const double best = res.candidates[0].score;
    const double second = res.candidates.size() > 1 ? res.candidates[1].score : 0.0;
    res.url = res.candidates[0].url;
    res.title = titleCandidates[candidates[c].title];
    res.confidence = second > 0 ? best / (best + second) : std::min(1.0, 0.5 + best / 2);

    if (evidence) {
      // Only what supported the winner: its URL strings, the objects that
      // referenced them, and the title copies those objects point to.
      int32_t winner = -1;
      for (const auto& [id, s] : scores)
        if (reader.url(id) == res.url) winner = id;
      if (winner >= 0) {
        evidence->addresses = where[winner];
        std::unordered_set<uint64_t> refSet(refs.begin(), refs.end());
        for (uint64_t a : where[winner]) {
          uint64_t target;
          if (refSet.count(a) && src.Read(a, &target, sizeof(target)) == sizeof(target))
            evidence->addresses.push_back(target);
        }
      }
    }
    return res;
  }
  return res;
}

// ---- LiveResolver -------------------------------------------------------------------

namespace {

uint64_t ProcessCreationTime(DWORD pid) {
  ScopedHandle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!h || !GetProcessTimes(h.get(), &created, &exited, &kernel, &user)) return 0;
  return (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

}  // namespace

void LiveResolver::ForgetHotRegions() {
  std::lock_guard<std::mutex> lock(mu_);
  hot_.clear();
}

void LiveResolver::Remember(const Evidence& evidence) {
  // Most recently useful first; older entries stay while they fit the budget.
  std::vector<RegionInfo> merged;
  size_t bytes = 0;
  auto add = [&](const std::optional<RegionInfo>& r) {
    if (!r || r->size > kMaxHotRegionSize || merged.size() >= kMaxHotRegions ||
        bytes + r->size > kMaxHotBytes)
      return;
    if (std::any_of(merged.begin(), merged.end(),
                    [&](const RegionInfo& x) { return x.base == r->base; }))
      return;
    merged.push_back(*r);
    bytes += r->size;
  };
  for (uint64_t a : evidence.addresses) add(source_.QueryRegion(a));
  for (uint64_t b : hot_) add(source_.QueryRegion(b));
  hot_.clear();
  for (const RegionInfo& r : merged) hot_.push_back(r.base);
}

bool LiveResolver::Resolve(const std::vector<std::wstring>& titles, size_t inlineTitleBytes,
                           Resolution& out, std::string& error, bool allowFastPath) {
  std::lock_guard<std::mutex> lock(mu_);
  const auto start = Clock::now();
  if (!source_.IsOpen()) {
    if (!source_.Open(pid_, error)) return false;
    creationTime_ = ProcessCreationTime(pid_);
  }

  Evidence evidence;
  std::string fastMiss;
  if (allowFastPath && !hot_.empty()) {
    auto t0 = Clock::now();
    std::vector<RegionInfo> regions;
    for (uint64_t base : hot_) {
      auto r = source_.QueryRegion(base);
      if (r && std::none_of(regions.begin(), regions.end(),
                            [&](const RegionInfo& x) { return x.base == r->base; }))
        regions.push_back(*r);
    }
    std::sort(regions.begin(), regions.end(),
              [](const RegionInfo& a, const RegionInfo& b) { return a.base < b.base; });
    double enumerateMs = MsSince(t0);

    Resolution r = CorrelateTitle(source_, regions, titles, inlineTitleBytes, &evidence);
    r.stats.enumerateMs = enumerateMs;
    if (r.resolved() && r.confidence >= kFastPathMinConfidence &&
        r.candidates[0].score >= kFastPathMinScore) {
      r.stats.fastPath = true;
      Remember(evidence);
      r.elapsedMs = static_cast<uint32_t>(MsSince(start));
      out = std::move(r);
      return true;
    }
    fastMiss = !r.resolved() ? "unresolved in " + std::to_string(regions.size()) + " hot regions"
                             : "weak: score " + std::to_string(r.candidates[0].score) + " conf " +
                                   std::to_string(r.confidence) + " " + r.url;
  }

  // Full scan.
  auto t0 = Clock::now();
  std::vector<RegionInfo> regions = source_.EnumerateRegions();
  double enumerateMs = MsSince(t0);
  if (regions.empty()) {
    error = "no readable memory in process " + std::to_string(pid_);
    return false;
  }
  evidence = {};
  Resolution r = CorrelateTitle(source_, regions, titles, inlineTitleBytes, &evidence, &regions);
  r.stats.enumerateMs = enumerateMs;
  r.stats.fastPathMiss = std::move(fastMiss);
  if (r.resolved()) Remember(evidence);
  r.elapsedMs = static_cast<uint32_t>(MsSince(start));
  out = std::move(r);
  return true;
}

std::shared_ptr<LiveResolver> ResolverFor(DWORD pid) {
  static std::mutex mu;
  static std::map<DWORD, std::shared_ptr<LiveResolver>> resolvers;

  const uint64_t created = ProcessCreationTime(pid);
  std::lock_guard<std::mutex> lock(mu);
  auto it = resolvers.find(pid);
  if (it != resolvers.end() && (it->second->creationTime() == 0 || it->second->creationTime() == created))
    return it->second;
  if (resolvers.size() >= 16) resolvers.clear();  // processes come and go; keep it bounded
  auto resolver = std::make_shared<LiveResolver>(pid);
  resolvers[pid] = resolver;
  return resolver;
}

bool ResolveActiveTab(const BrowserWindow& window, Resolution& out, std::string& error) {
  size_t inlineBytes = window.profile ? window.profile->inlineTitleBytes : 22;
  return ResolverFor(window.pid)->Resolve(TitleCandidates(window), inlineBytes, out, error);
}

}  // namespace browsermem
