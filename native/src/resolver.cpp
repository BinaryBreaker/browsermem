#include "resolver.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace browsermem {

namespace {

constexpr int64_t kNeighbourhood = 512;   // bytes either side of an anchor
constexpr size_t kMaxTitleHits = 4000;
constexpr size_t kMaxPointerRefs = 8000;
constexpr size_t kMaxInlineUrlBytes = 22;  // libc++ std::string SSO capacity
constexpr size_t kMaxCandidates = 8;

std::vector<uint8_t> Utf16Bytes(const std::wstring& s) {
  const auto* p = reinterpret_cast<const uint8_t*>(s.data());
  return {p, p + s.size() * sizeof(wchar_t)};
}

std::vector<uint8_t> Utf8Bytes(const std::wstring& s) {
  std::string u = ToUtf8(s);
  return {u.begin(), u.end()};
}

std::vector<uint64_t> FindAll(const ProcessSnapshot& snap, const std::vector<uint8_t>& needle,
                              uint64_t alignment) {
  std::vector<uint64_t> hits;
  std::boyer_moore_horspool_searcher searcher(needle.begin(), needle.end());
  for (const MemoryRegion& r : snap.regions()) {
    auto first = r.bytes.begin();
    while (hits.size() < kMaxTitleHits) {
      auto it = std::search(first, r.bytes.end(), searcher);
      if (it == r.bytes.end()) break;
      uint64_t address = r.base + (it - r.bytes.begin());
      if (address % alignment == 0) hits.push_back(address);
      first = it + 1;
    }
  }
  return hits;
}

// Addresses of aligned qwords whose value is one of `targets`.
std::vector<uint64_t> FindPointersTo(const ProcessSnapshot& snap,
                                     const std::vector<uint64_t>& targets) {
  std::vector<uint64_t> refs;
  if (targets.empty()) return refs;
  std::unordered_set<uint64_t> set(targets.begin(), targets.end());
  auto [lo, hi] = std::minmax_element(targets.begin(), targets.end());
  for (const MemoryRegion& r : snap.regions()) {
    const auto* q = reinterpret_cast<const uint64_t*>(r.bytes.data());
    const size_t count = r.bytes.size() / 8;
    for (size_t i = 0; i < count; ++i) {
      const uint64_t v = q[i];
      if (v < *lo || v > *hi || !set.count(v)) continue;
      refs.push_back(r.base + i * 8);
      if (refs.size() >= kMaxPointerRefs) return refs;
    }
  }
  return refs;
}

void VoteAround(const ProcessSnapshot& snap, const UrlIndex& urls, uint64_t anchor,
                std::unordered_map<uint32_t, double>& scores) {
  std::unordered_map<uint32_t, double> local;  // best contribution per URL for this anchor
  auto bump = [&](uint32_t id, uint64_t at) {
    double dist = at > anchor ? double(at - anchor) : double(anchor - at);
    double s = 1.0 / (1.0 + dist / 64.0);
    double& cur = local[id];
    cur = std::max(cur, s);
  };

  const uint64_t lo = anchor > uint64_t(kNeighbourhood) ? (anchor - kNeighbourhood) & ~7ull : 0;
  const uint64_t hi = anchor + kNeighbourhood;

  // (a) pointers to heap-allocated URL strings (e.g. GURL::spec_)
  for (uint64_t a = lo; a <= hi; a += 8) {
    const uint8_t* p = snap.At(a, 8);
    if (!p) continue;
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    if (auto id = urls.AtAddress(v)) bump(*id, a);
  }
  // (b) short URLs stored inline in the object (small-string optimisation)
  urls.ForEachInRange(lo, hi, [&](uint64_t at, uint32_t id) {
    if (urls.url(id).size() <= kMaxInlineUrlBytes) bump(id, at);
  });

  for (const auto& [id, s] : local) scores[id] += s;
}

}  // namespace

Resolution CorrelateTitle(const ProcessSnapshot& snapshot, const UrlIndex& urls,
                          const std::vector<std::wstring>& titleCandidates,
                          size_t inlineTitleBytes) {
  Resolution res;
  if (!titleCandidates.empty()) res.title = titleCandidates.front();

  for (const std::wstring& title : titleCandidates) {
    // UTF-16 first (Chromium std::u16string, Gecko nsString), then UTF-8 /
    // Latin-1 (SpiderMonkey strings, nsCString).
    for (int encoding = 0; encoding < 2; ++encoding) {
      std::vector<uint8_t> needle = encoding == 0 ? Utf16Bytes(title) : Utf8Bytes(title);
      if (needle.size() < 4) continue;

      std::vector<uint64_t> hits = FindAll(snapshot, needle, encoding == 0 ? 2 : 1);
      if (hits.empty()) continue;

      std::unordered_map<uint32_t, double> scores;
      for (uint64_t ref : FindPointersTo(snapshot, hits)) VoteAround(snapshot, urls, ref, scores);
      // A short title is stored inside its owning object, so the title bytes
      // themselves are an anchor.
      if (needle.size() <= inlineTitleBytes)
        for (uint64_t h : hits) VoteAround(snapshot, urls, h, scores);
      if (scores.empty()) continue;

      for (const auto& [id, s] : scores) res.candidates.push_back({urls.url(id), s});
      std::sort(res.candidates.begin(), res.candidates.end(),
                [](const UrlCandidate& a, const UrlCandidate& b) { return a.score > b.score; });
      if (res.candidates.size() > kMaxCandidates) res.candidates.resize(kMaxCandidates);

      const double best = res.candidates[0].score;
      const double second = res.candidates.size() > 1 ? res.candidates[1].score : 0.0;
      res.url = res.candidates[0].url;
      res.title = title;
      res.confidence = second > 0 ? best / (best + second) : std::min(1.0, 0.5 + best / 2);
      return res;
    }
  }
  return res;
}

bool ResolveActiveTab(const BrowserWindow& window, Resolution& out, std::string& error) {
  const auto start = std::chrono::steady_clock::now();

  ProcessSnapshot snapshot;
  if (!snapshot.Capture(window.pid, error)) return false;
  UrlIndex urls;
  urls.Build(snapshot);

  size_t inlineBytes = window.profile ? window.profile->inlineTitleBytes : 22;
  out = CorrelateTitle(snapshot, urls, TitleCandidates(window), inlineBytes);
  out.bytesScanned = snapshot.totalBytes();
  out.uniqueUrls = urls.size();
  out.elapsedMs = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
  return true;
}

}  // namespace browsermem
