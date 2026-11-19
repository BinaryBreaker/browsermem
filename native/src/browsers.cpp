#include "browsers.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace browsermem {

namespace {

const std::wstring kChromiumClass = L"Chrome_WidgetWin_1";

// Edge collapses grouped tabs into "Title and 3 more pages".
std::wstring StripMorePages(std::wstring t) {
  size_t pos = t.rfind(L" and ");
  if (pos == std::wstring::npos) return t;
  size_t i = pos + 5, digits = 0;
  while (i < t.size() && iswdigit(t[i])) ++i, ++digits;
  if (digits && t.compare(i, 10, L" more page") == 0) t.resize(pos);
  return t;
}

}  // namespace

const std::vector<BrowserProfile>& AllProfiles() {
  // Chromium titles live in std::u16string (11-char SSO = 22 bytes); Firefox
  // uses nsAutoString with larger inline buffers.
  static const std::vector<BrowserProfile> profiles = {
      {Browser::Chrome, "Google Chrome", {L"chrome.exe"}, {kChromiumClass},
       {L" - Google Chrome", L" - Chromium"}, 22},
      {Browser::Edge, "Microsoft Edge", {L"msedge.exe"}, {kChromiumClass},
       {L" - Microsoft​ Edge", L" - Microsoft Edge"}, 22},
      {Browser::Brave, "Brave", {L"brave.exe"}, {kChromiumClass}, {L" - Brave"}, 22},
      {Browser::Opera, "Opera", {L"opera.exe"}, {kChromiumClass}, {L" - Opera"}, 22},
      {Browser::Vivaldi, "Vivaldi", {L"vivaldi.exe"}, {kChromiumClass}, {L" - Vivaldi"}, 22},
      {Browser::Yandex, "Yandex Browser", {L"browser.exe"}, {kChromiumClass},
       {L" - Yandex Browser", L" - Yandex"}, 22},
      {Browser::Firefox, "Mozilla Firefox", {L"firefox.exe"}, {L"MozillaWindowClass"},
       {L" — Mozilla Firefox Private Browsing", L" — Mozilla Firefox",
        L" - Mozilla Firefox"},
       128},
  };
  return profiles;
}

const BrowserProfile* FindProfile(Browser b) {
  for (const auto& p : AllProfiles())
    if (p.id == b) return &p;
  return nullptr;
}

const char* BrowserName(Browser b) {
  const BrowserProfile* p = FindProfile(b);
  return p ? p->name : "Unknown";
}

std::wstring ExecutableName(DWORD pid) {
  using Clock = std::chrono::steady_clock;
  struct Entry {
    std::wstring name;
    Clock::time_point at;
  };
  static std::mutex mu;
  static std::unordered_map<DWORD, Entry> cache;

  auto now = Clock::now();
  {
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(pid);
    if (it != cache.end() && now - it->second.at < std::chrono::seconds(10)) return it->second.name;
  }

  std::wstring name;
  ScopedHandle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (h) {
    wchar_t buf[MAX_PATH];
    DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameW(h.get(), 0, buf, &len)) {
      name.assign(buf, len);
      size_t slash = name.find_last_of(L"\\/");
      if (slash != std::wstring::npos) name.erase(0, slash + 1);
    }
  }

  std::lock_guard<std::mutex> lock(mu);
  if (cache.size() > 512) cache.clear();
  cache[pid] = {name, now};
  return name;
}

std::wstring ReadCaption(HWND hwnd) {
  wchar_t buf[2048];
  int n = InternalGetWindowText(hwnd, buf, (int)std::size(buf));
  return std::wstring(buf, n > 0 ? n : 0);
}

std::optional<BrowserWindow> IdentifyWindow(HWND hwnd, uint32_t mask) {
  if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return std::nullopt;
  if (GetAncestor(hwnd, GA_ROOT) != hwnd) return std::nullopt;

  wchar_t cls[128] = {};
  GetClassNameW(hwnd, cls, (int)std::size(cls));
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);

  const BrowserProfile* match = nullptr;
  for (const auto& p : AllProfiles()) {
    if (!(mask & MaskOf(p.id))) continue;
    bool classOk = std::any_of(p.windowClasses.begin(), p.windowClasses.end(),
                               [&](const std::wstring& c) { return c == cls; });
    if (classOk) {
      match = &p;
      break;
    }
  }
  if (!match) return std::nullopt;

  // Several Chromium browsers share a window class: decide by executable.
  std::wstring exe = ExecutableName(pid);
  match = nullptr;
  for (const auto& p : AllProfiles()) {
    if (!(mask & MaskOf(p.id))) continue;
    for (const auto& e : p.executables)
      if (EqualsIgnoreCase(exe, e)) match = &p;
    if (match) break;
  }
  if (!match) return std::nullopt;
  if (std::none_of(match->windowClasses.begin(), match->windowClasses.end(),
                   [&](const std::wstring& c) { return c == cls; }))
    return std::nullopt;

  BrowserWindow w;
  w.hwnd = hwnd;
  w.pid = pid;
  w.profile = match;
  w.caption = ReadCaption(hwnd);
  if (w.caption.empty()) return std::nullopt;  // menus, bubbles, hidden helpers
  return w;
}

std::vector<BrowserWindow> EnumerateBrowserWindows(uint32_t mask) {
  struct Ctx {
    uint32_t mask;
    std::vector<BrowserWindow> out;
  } ctx{mask, {}};
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (auto w = IdentifyWindow(hwnd, c->mask)) c->out.push_back(std::move(*w));
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  return std::move(ctx.out);
}

std::vector<std::wstring> TitleCandidates(const BrowserWindow& window) {
  std::wstring t = window.caption;
  if (window.profile) {
    for (const auto& s : window.profile->captionSuffixes) {
      if (EndsWith(t, s)) {
        t.resize(t.size() - s.size());
        break;
      }
    }
  }

  // Captions may carry extra trailing segments (" - Audio playing",
  // " - Profile 1"), so peel " - " segments off one at a time.
  std::vector<std::wstring> out;
  auto push = [&](std::wstring s) {
    s = StripMorePages(std::move(s));
    if (!s.empty() && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
  };
  for (int i = 0; i < 3 && !t.empty(); ++i) {
    push(t);
    size_t pos = t.rfind(L" - ");
    if (pos == std::wstring::npos || pos == 0) break;
    t.resize(pos);
  }
  return out;
}

}  // namespace browsermem
