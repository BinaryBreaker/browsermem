#include "monitor.hpp"

#include <stdexcept>

namespace browsermem {

namespace {
// Out-of-context WinEvent callbacks run on the thread that installed the hook.
thread_local TabMonitor* t_hookOwner = nullptr;
constexpr auto kPollInterval = std::chrono::milliseconds(500);
constexpr auto kTick = std::chrono::milliseconds(100);
}  // namespace

TabMonitor::TabMonitor(MonitorOptions options, Callback callback)
    : options_(options), callback_(std::move(callback)) {}

TabMonitor::~TabMonitor() {
  if (running_ && !IsCallbackThread()) Stop();
}

void TabMonitor::Start() {
  if (running_) return;
  stop_ = false;

  std::promise<std::string> ready;
  auto result = ready.get_future();
  hookThread_ = std::thread([this, &ready] { HookLoop(ready); });
  std::string error = result.get();
  if (!error.empty()) {
    hookThread_.join();
    throw std::runtime_error(error);
  }

  worker_ = std::thread([this] { WorkerLoop(); });
  workerId_ = worker_.get_id();
  running_ = true;
  Post({SignalType::Seed, nullptr});  // report the current state
}

void TabMonitor::Stop() {
  if (IsCallbackThread())
    throw std::logic_error("TabMonitor::Stop() cannot be called from the callback thread");
  if (!running_.exchange(false)) return;

  stop_ = true;
  if (DWORD tid = hookThreadId_.exchange(0)) PostThreadMessageW(tid, WM_QUIT, 0, 0);
  cv_.notify_all();
  if (hookThread_.joinable()) hookThread_.join();
  if (worker_.joinable()) worker_.join();

  workerId_ = {};
  queue_.clear();
  tracked_.clear();
  cache_.clear();
  activeHwnd_ = nullptr;
  activePid_ = 0;
  leftBrowser_ = true;
}

// ---------------------------------------------------------------------------
// Hook thread
// ---------------------------------------------------------------------------

void CALLBACK TabMonitor::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject,
                                       LONG idChild, DWORD, DWORD) {
  TabMonitor* self = t_hookOwner;
  if (!self || !hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
  switch (event) {
    case EVENT_SYSTEM_FOREGROUND:
      self->Post({SignalType::Foreground, hwnd});
      break;
    case EVENT_OBJECT_NAMECHANGE:
      if (GetAncestor(hwnd, GA_ROOT) == hwnd) self->Post({SignalType::NameChange, hwnd});
      break;
    case EVENT_OBJECT_DESTROY:
      if (GetAncestor(hwnd, GA_ROOT) == hwnd) self->Post({SignalType::Destroy, hwnd});
      break;
  }
}

void TabMonitor::HookLoop(std::promise<std::string>& ready) {
  t_hookOwner = this;
  MSG msg;
  PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // create the message queue

  const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
  HWINEVENTHOOK hooks[] = {
      SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0,
                      0, flags),
      SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, nullptr, WinEventProc, 0,
                      0, flags),
      SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, nullptr, WinEventProc, 0, 0,
                      flags),
  };
  for (HWINEVENTHOOK h : hooks) {
    if (!h) {
      for (HWINEVENTHOOK x : hooks)
        if (x) UnhookWinEvent(x);
      t_hookOwner = nullptr;
      ready.set_value("SetWinEventHook failed, Win32 error " + std::to_string(GetLastError()));
      return;
    }
  }

  hookThreadId_ = GetCurrentThreadId();
  ready.set_value({});

  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  for (HWINEVENTHOOK h : hooks) UnhookWinEvent(h);
  t_hookOwner = nullptr;
}

void TabMonitor::Post(Signal signal) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_.size() > 4096) queue_.pop_front();
    queue_.push_back(signal);
  }
  cv_.notify_one();
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

void TabMonitor::WorkerLoop() {
  auto nextPoll = Clock::now();
  while (!stop_) {
    std::deque<Signal> batch;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait_for(lock, kTick, [this] { return stop_ || !queue_.empty(); });
      batch.swap(queue_);
    }
    if (stop_) break;

    for (const Signal& s : batch) {
      switch (s.type) {
        case SignalType::Seed: Seed(); break;
        case SignalType::Foreground: HandleForeground(s.hwnd); break;
        case SignalType::NameChange: HandleNameChange(s.hwnd); break;
        case SignalType::Destroy: HandleDestroy(s.hwnd); break;
      }
    }

    auto now = Clock::now();
    if (now >= nextPoll) {  // safety net in case a WinEvent was missed
      HandleForeground(GetForegroundWindow());
      nextPoll = now + kPollInterval;
    }
    FlushPending(now);
    MaybeRescan(now);
  }
}

void TabMonitor::HandleForeground(HWND hwnd) {
  if (!hwnd) return;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  auto window = IdentifyWindow(hwnd, options_.browserMask);
  if (!window) {
    // Menus and popups of the same browser don't count as leaving it.
    if (pid != activePid_) {
      leftBrowser_ = true;
      activeHwnd_ = nullptr;
    }
    return;
  }
  bool activation = hwnd != activeHwnd_ || leftBrowser_;
  activeHwnd_ = hwnd;
  activePid_ = pid;
  leftBrowser_ = false;
  Observe(*window, activation);
}

void TabMonitor::HandleNameChange(HWND hwnd) {
  if (!options_.allWindows && hwnd != GetForegroundWindow()) return;
  if (auto window = IdentifyWindow(hwnd, options_.browserMask)) Observe(*window, false);
}

void TabMonitor::HandleDestroy(HWND hwnd) {
  auto it = tracked_.find(hwnd);
  if (it == tracked_.end()) return;
  Resolution last;
  last.url = it->second.reportedUrl;
  last.title = it->second.reportedTitle;
  BrowserWindow window = it->second.window;
  tracked_.erase(it);
  for (auto c = cache_.begin(); c != cache_.end();)
    c = c->first.first == hwnd ? cache_.erase(c) : std::next(c);
  if (activeHwnd_ == hwnd) activeHwnd_ = nullptr;
  Emit(EventKind::WindowClosed, window, last);
}

// Initial state, reported once as Snapshot events and tracked from then on:
// every browser window with allWindows; otherwise the foreground browser
// window, or -- when e.g. a terminal has focus -- the most recently used one.
void TabMonitor::Seed() {
  HWND foreground = GetForegroundWindow();
  auto windows = EnumerateBrowserWindows(options_.browserMask);  // z-order
  HWND primary = nullptr;
  for (const BrowserWindow& w : windows)
    if (w.hwnd == foreground) primary = w.hwnd;
  if (!primary && !windows.empty()) primary = windows.front().hwnd;

  for (const BrowserWindow& w : windows) {
    if (!options_.allWindows && w.hwnd != primary) continue;
    if (w.hwnd == foreground) {
      activeHwnd_ = w.hwnd;
      activePid_ = w.pid;
      leftBrowser_ = false;
    }
    Observe(w, false, true);
  }
}

void TabMonitor::Observe(const BrowserWindow& window, bool activation, bool initial) {
  Tracked& t = tracked_[window.hwnd];
  bool captionChanged = !t.window.hwnd || t.window.caption != window.caption;
  t.window = window;
  if (activation) t.activation = true;
  if (initial) t.initial = true;
  if (activation || captionChanged) {
    t.pending = true;
    t.changedAt = Clock::now();
  }
}

void TabMonitor::FlushPending(Clock::time_point now) {
  const auto debounce = std::chrono::milliseconds(options_.debounceMs);
  for (auto it = tracked_.begin(); it != tracked_.end() && !stop_;) {
    Tracked& t = it->second;
    if (!t.pending || now - t.changedAt < debounce) {
      ++it;
      continue;
    }
    if (!IsWindow(t.window.hwnd)) {
      it = tracked_.erase(it);
      continue;
    }
    // The caption flickers while a page loads; wait until it is stable.
    std::wstring caption = ReadCaption(t.window.hwnd);
    if (caption.empty() || caption != t.window.caption) {
      if (!caption.empty()) t.window.caption = caption;
      t.changedAt = now;
      ++it;
      continue;
    }

    t.pending = false;
    const bool activation = std::exchange(t.activation, false);
    const bool initial = std::exchange(t.initial, false);
    if (!activation && !initial && caption == t.reportedCaption) {  // flickered back
      ++it;
      continue;
    }

    Resolution r = Resolve(t.window, true);
    t.lastScan = Clock::now();
    t.reportedCaption = caption;
    t.reportedTitle = r.title;
    t.reportedUrl = r.url;
    const EventKind kind = activation ? EventKind::WindowActivated
                           : initial  ? EventKind::Snapshot
                                      : EventKind::TabChanged;
    if (r.resolved() || options_.emitUnresolved) Emit(kind, t.window, r);
    ++it;
  }
}

void TabMonitor::MaybeRescan(Clock::time_point now) {
  if (!options_.rescanMs || !activeHwnd_ || leftBrowser_) return;
  auto it = tracked_.find(activeHwnd_);
  if (it == tracked_.end()) return;
  Tracked& t = it->second;
  if (t.pending || now - t.lastScan < std::chrono::milliseconds(options_.rescanMs)) return;

  Resolution r = Resolve(t.window, false);
  t.lastScan = Clock::now();
  if (r.resolved() && r.url != t.reportedUrl) {
    t.reportedUrl = r.url;
    t.reportedTitle = r.title;
    Emit(EventKind::UrlChanged, t.window, r);
  }
}

Resolution TabMonitor::Resolve(const BrowserWindow& window, bool allowCache) {
  const auto now = Clock::now();
  const auto ttl = std::chrono::milliseconds(options_.cacheTtlMs);
  auto key = std::make_pair(window.hwnd, window.caption);

  if (allowCache && options_.cacheTtlMs) {
    auto it = cache_.find(key);
    if (it != cache_.end() && now - it->second.at < ttl && it->second.resolution.resolved()) {
      Resolution r = it->second.resolution;
      r.elapsedMs = 0;
      return r;
    }
  }

  Resolution r;
  std::string error;
  if (!ResolveActiveTab(window, r, error)) {
    r = {};
    auto titles = TitleCandidates(window);
    if (!titles.empty()) r.title = titles.front();
  }

  if (options_.cacheTtlMs) {
    if (cache_.size() > 256) {
      for (auto it = cache_.begin(); it != cache_.end();)
        it = now - it->second.at >= ttl ? cache_.erase(it) : std::next(it);
    }
    cache_[key] = {r, Clock::now()};
  }
  return r;
}

void TabMonitor::Emit(EventKind kind, const BrowserWindow& window, const Resolution& resolution) {
  if (!callback_) return;
  TabEvent event{kind, window, resolution, UnixMillis()};
  try {
    callback_(event);
  } catch (...) {
    // Never let a client exception kill the worker thread.
  }
}

}  // namespace browsermem
