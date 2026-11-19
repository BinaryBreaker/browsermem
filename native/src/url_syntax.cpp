#include "url_syntax.hpp"

#include <algorithm>
#include <iterator>

namespace browsermem {

bool IsInterestingScheme(std::string_view s) {
  static constexpr std::string_view kSchemes[] = {
      "http",    "https",         "file",          "ftp",     "chrome", "chrome-extension",
      "chrome-untrusted", "chrome-search", "devtools", "edge", "brave", "opera",
      "vivaldi", "about",         "moz-extension", "view-source"};
  return std::find(std::begin(kSchemes), std::end(kSchemes), s) != std::end(kSchemes);
}

bool LooksLikeSubresource(std::string_view url) {
  // Path only: drop the query and fragment.
  size_t end = url.find_first_of("?#");
  std::string_view path = url.substr(0, end);
  size_t scheme = path.find("://");
  if (scheme != std::string_view::npos) {
    size_t slash = path.find('/', scheme + 3);
    path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash);
  }
  if (path.empty()) return false;

  std::string lower(path);
  for (char& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
  if (lower.find("/favicon") != std::string::npos) return true;

  static constexpr std::string_view kExtensions[] = {
      ".ico", ".png", ".svg", ".jpg", ".jpeg", ".gif", ".webp", ".avif", ".bmp",
      ".css", ".js",  ".mjs", ".woff", ".woff2", ".ttf", ".otf", ".map", ".json"};
  for (std::string_view ext : kExtensions)
    if (lower.size() >= ext.size() && lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0)
      return true;
  return false;
}

bool ParseUrlAt(const uint8_t* d, size_t n, std::string& out) {
  // UTF-8
  size_t i = 0;
  while (i < n && i < 20 && IsSchemeChar(d[i])) ++i;
  if (i > 0 && i + 2 < n && d[i] == ':' && d[i + 1] == '/' && d[i + 2] == '/') {
    if (!IsInterestingScheme({reinterpret_cast<const char*>(d), i})) return false;
    size_t e = i + 3;
    while (e < n && e < kMaxUrlLength && IsUrlChar(d[e])) ++e;
    if (e - (i + 3) < 2) return false;
    out.assign(reinterpret_cast<const char*>(d), e);
    return true;
  }

  // UTF-16LE
  std::string s;
  size_t k = 0;
  while (k + 1 < n && s.size() < 20 && d[k + 1] == 0 && IsSchemeChar(d[k])) {
    s.push_back(static_cast<char>(d[k]));
    k += 2;
  }
  if (s.empty() || k + 5 >= n || d[k] != ':' || d[k + 1] != 0 || d[k + 2] != '/' ||
      d[k + 3] != 0 || d[k + 4] != '/' || d[k + 5] != 0 || !IsInterestingScheme(s))
    return false;
  const size_t schemeLen = s.size();
  s += "://";
  for (size_t e = k + 6; e + 1 < n && s.size() < kMaxUrlLength && d[e + 1] == 0 && IsUrlChar(d[e]);
       e += 2)
    s.push_back(static_cast<char>(d[e]));
  if (s.size() - schemeLen - 3 < 2) return false;
  out = std::move(s);
  return true;
}

}  // namespace browsermem
