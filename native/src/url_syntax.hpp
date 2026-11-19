#pragma once

// What counts as a URL string in memory. Shared by the full URL index
// (forensics) and the targeted lookup used for live resolution.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace browsermem {

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

bool IsInterestingScheme(std::string_view scheme);

// Calls onUrl(std::string url, uint64_t address) for every URL (UTF-8 or
// UTF-16LE) that starts at an offset < `owned` inside [d, d + n). `base` is
// the address of d[0].
template <class F>
void CarveUrls(const uint8_t* d, size_t n, uint64_t base, size_t owned, F&& onUrl);

// If a URL string (UTF-8 or UTF-16LE) starts exactly at d[0], stores it in
// `out` and returns true.
bool ParseUrlAt(const uint8_t* d, size_t n, std::string& out);

// True for URLs that are almost certainly page *resources* (favicons, images,
// scripts, styles, fonts) rather than pages. Browsers keep e.g. the favicon
// URL in the same objects as the page URL, right next to the tab title.
bool LooksLikeSubresource(std::string_view url);

// ---- implementation ---------------------------------------------------------

template <class F>
void CarveUrls(const uint8_t* d, size_t n, uint64_t base, size_t owned, F&& onUrl) {
  const uint8_t* cursor = d;
  const uint8_t* end = d + n;
  // Every URL contains "://"; memchr for ':' is SIMD-fast, then look around.
  while (cursor < end && (cursor = static_cast<const uint8_t*>(memchr(cursor, ':', end - cursor)))) {
    const size_t p = cursor - d;
    ++cursor;

    // UTF-8:  scheme "://" rest
    if (p + 2 < n && d[p + 1] == '/' && d[p + 2] == '/') {
      size_t q = p;
      while (q > 0 && p - q < 20 && IsSchemeChar(d[q - 1])) --q;
      if (q < p && q < owned && (q == 0 || !IsAlnum(d[q - 1])) &&
          IsInterestingScheme({reinterpret_cast<const char*>(d + q), p - q})) {
        size_t e = p + 3;
        while (e < n && e - q < kMaxUrlLength && IsUrlChar(d[e])) ++e;
        if (e - (p + 3) >= 2) onUrl(std::string(reinterpret_cast<const char*>(d + q), e - q), base + q);
      }
    }

    // UTF-16LE:  s\0c\0h\0 :\0/\0/\0 rest
    if (((base + p) & 1) == 0 && p + 5 < n && d[p + 1] == 0 && d[p + 2] == '/' &&
        d[p + 3] == 0 && d[p + 4] == '/' && d[p + 5] == 0) {
      size_t q = p;
      while (q >= 2 && p - q < 40 && d[q - 1] == 0 && IsSchemeChar(d[q - 2])) q -= 2;
      bool boundary = q < 2 || !(d[q - 1] == 0 && IsAlnum(d[q - 2]));
      if (q < p && q < owned && boundary) {
        std::string url;
        for (size_t i = q; i < p; i += 2) url.push_back(static_cast<char>(d[i]));
        if (IsInterestingScheme(url)) {
          const size_t schemeLen = url.size();
          url += "://";
          for (size_t e = p + 6;
               e + 1 < n && url.size() < kMaxUrlLength && d[e + 1] == 0 && IsUrlChar(d[e]); e += 2)
            url.push_back(static_cast<char>(d[e]));
          if (url.size() - schemeLen - 3 >= 2) onUrl(std::move(url), base + q);
        }
      }
    }
  }
}

}  // namespace browsermem
