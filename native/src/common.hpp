#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace browsermem {

std::string ToUtf8(std::wstring_view text);
std::wstring ToWide(std::string_view text);

// Milliseconds since the Unix epoch.
uint64_t UnixMillis();

bool EndsWith(std::wstring_view text, std::wstring_view suffix);
bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b);

// Closes a Win32 HANDLE on scope exit.
class ScopedHandle {
 public:
  explicit ScopedHandle(HANDLE h = nullptr) : h_(h) {}
  ~ScopedHandle() {
    if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
  }
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  HANDLE get() const { return h_; }
  explicit operator bool() const { return h_ && h_ != INVALID_HANDLE_VALUE; }

 private:
  HANDLE h_;
};

}  // namespace browsermem
