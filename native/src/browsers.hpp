#pragma once

#include <optional>
#include <vector>

#include "common.hpp"

namespace browsermem {

enum class Browser : int32_t {
  Unknown = 0,
  Chrome = 1,
  Edge = 2,
  Brave = 3,
  Opera = 4,
  Vivaldi = 5,
  Yandex = 6,
  Firefox = 7,
};

constexpr uint32_t kAllBrowsers = 0xFFFFFFFFu;
constexpr uint32_t MaskOf(Browser b) { return 1u << static_cast<int>(b); }

// Static description of how a browser looks from the outside.
struct BrowserProfile {
  Browser id;
  const char* name;
  std::vector<std::wstring> executables;     // image names, case-insensitive
  std::vector<std::wstring> windowClasses;   // top-level window class
  std::vector<std::wstring> captionSuffixes; // longest first
  size_t inlineTitleBytes;  // titles up to this size may be stored inside the owning object
};

const std::vector<BrowserProfile>& AllProfiles();
const char* BrowserName(Browser b);
const BrowserProfile* FindProfile(Browser b);  // null for Unknown

struct BrowserWindow {
  HWND hwnd = nullptr;
  DWORD pid = 0;
  const BrowserProfile* profile = nullptr;
  std::wstring caption;

  Browser browser() const { return profile ? profile->id : Browser::Unknown; }
};

// Image name ("chrome.exe") of a process; cached briefly.
std::wstring ExecutableName(DWORD pid);

// Window caption without sending messages to the (possibly busy) window.
std::wstring ReadCaption(HWND hwnd);

// Returns the window if it is a visible, titled, top-level browser window.
std::optional<BrowserWindow> IdentifyWindow(HWND hwnd, uint32_t mask = kAllBrowsers);

// All browser windows, in z-order (most recently active first).
std::vector<BrowserWindow> EnumerateBrowserWindows(uint32_t mask = kAllBrowsers);

// Likely tab titles derived from the caption, most specific first:
//   "Inbox - Gmail - Google Chrome"  ->  ["Inbox - Gmail", "Inbox"]
std::vector<std::wstring> TitleCandidates(const BrowserWindow& window);

}  // namespace browsermem
