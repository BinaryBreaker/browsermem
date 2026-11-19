#include "url_index.hpp"

#include <algorithm>
#include <cstring>

namespace browsermem {

namespace {

constexpr size_t kMaxUrlLength = 4096;

inline bool IsUrlChar(uint32_t c) {
  if (c < 0x21 || c > 0x7E) return false;
  switch (c) {
    case '"': case '<': case '>': case '\\': case '^':
    case '`': case '{': case '}': case '|':
      return false;
    default:
      return true;
  }
}
inline bool IsSchemeChar(uint32_t c) { return (c >= 'a' && c <= 'z') || c == '-'; }
inline bool IsAlnum(uint32_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool IsInterestingScheme(std::string_view s) {
  static constexpr std::string_view kSchemes[] = {
      "http",   "https",         "file",          "ftp",   "chrome", "chrome-extension",
      "chrome-untrusted", "chrome-search", "devtools", "edge", "brave", "opera",
      "vivaldi", "about",        "moz-extension", "view-source"};
  return std::find(std::begin(kSchemes), std::end(kSchemes), s) != std::end(kSchemes);
}

}  // namespace

void UrlIndex::Add(std::string url, uint64_t address) {
  auto [it, inserted] = byUrl_.emplace(std::move(url), (uint32_t)urls_.size());
  if (inserted) {
    urls_.push_back(it->first);
    counts_.push_back(0);
  }
  counts_[it->second]++;
  byAddress_[address] = it->second;
  sorted_.emplace_back(address, it->second);
}

void UrlIndex::Build(const ProcessSnapshot& snapshot) {
  for (const MemoryRegion& region : snapshot.regions()) {
    const uint8_t* d = region.bytes.data();
    const size_t n = region.bytes.size();
    const uint8_t* cursor = d;

    // Every URL contains "://"; memchr for ':' is SIMD-fast, then look around.
    while ((cursor = static_cast<const uint8_t*>(memchr(cursor, ':', n - (cursor - d))))) {
      const size_t p = cursor - d;
      ++cursor;

      // UTF-8:  scheme "://" rest
      if (p + 2 < n && d[p + 1] == '/' && d[p + 2] == '/') {
        size_t q = p;
        while (q > 0 && p - q < 20 && IsSchemeChar(d[q - 1])) --q;
        if (q < p && (q == 0 || !IsAlnum(d[q - 1])) &&
            IsInterestingScheme({reinterpret_cast<const char*>(d + q), p - q})) {
          size_t e = p + 3;
          while (e < n && e - q < kMaxUrlLength && IsUrlChar(d[e])) ++e;
          if (e - (p + 3) >= 2)
            Add(std::string(reinterpret_cast<const char*>(d + q), e - q), region.base + q);
        }
      }

      // UTF-16LE:  s\0c\0h\0 :\0/\0/\0 rest
      if (((region.base + p) & 1) == 0 && p + 5 < n && d[p + 1] == 0 && d[p + 2] == '/' &&
          d[p + 3] == 0 && d[p + 4] == '/' && d[p + 5] == 0) {
        size_t q = p;
        while (q >= 2 && p - q < 40 && d[q - 1] == 0 && IsSchemeChar(d[q - 2])) q -= 2;
        bool boundary = q < 2 || !(d[q - 1] == 0 && IsAlnum(d[q - 2]));
        if (q < p && boundary) {
          std::string url;
          for (size_t i = q; i < p; i += 2) url.push_back((char)d[i]);
          if (IsInterestingScheme(url)) {
            size_t schemeLen = url.size();
            url += "://";
            for (size_t e = p + 6; e + 1 < n && url.size() < kMaxUrlLength && d[e + 1] == 0 &&
                                   IsUrlChar(d[e]);
                 e += 2)
              url.push_back((char)d[e]);
            if (url.size() - schemeLen - 3 >= 2) Add(std::move(url), region.base + q);
          }
        }
      }
    }
  }
  std::sort(sorted_.begin(), sorted_.end());
}

}  // namespace browsermem
