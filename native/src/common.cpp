#include "common.hpp"

namespace browsermem {

std::string ToUtf8(std::wstring_view text) {
  if (text.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr,
                              nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring ToWide(std::string_view text) {
  if (text.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), n);
  return out;
}

uint64_t UnixMillis() {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
  return (t - 116444736000000000ull) / 10000;  // 100ns since 1601 -> ms since 1970
}

bool EndsWith(std::wstring_view text, std::wstring_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) {
  return CompareStringOrdinal(a.data(), (int)a.size(), b.data(), (int)b.size(), TRUE) ==
         CSTR_EQUAL;
}

}  // namespace browsermem
