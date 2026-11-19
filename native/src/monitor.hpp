#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <thread>

#include "browsers.hpp"
#include "resolver.hpp"

namespace browsermem {

enum class EventKind : int32_t {
  Snapshot = 0,
  TabChanged = 1,
  WindowActivated = 2,
  UrlChanged = 3,
  WindowClosed = 4,
};

struct MonitorOptions {
  uint32_t browserMask = kAllBrowsers;
  uint32_t debounceMs = 600;
  uint32_t rescanMs = 0;
  uint32_t cacheTtlMs = 30000;
  bool allWindows = false;
  bool emitUnresolved = true;
};

struct TabEvent {
  EventKind kind;
  BrowserWindow window;
  Resolution resolution;
  uint64_t timestampMs;
};

// Event-driven tab watcher.
//
//   hook thread   : SetWinEventHook (foreground / caption change / destroy)
//                   -> lightweight signals into a queue
//   worker thread : debounces signals, snapshots memory, resolves the URL and
//                   invokes the callback
class TabMonitor {
 public:
  using Callback = std::function<void(const TabEvent&)>;

  TabMonitor(MonitorOptions options, Callback callback);
  ~TabMonitor();
  TabMonitor(const TabMonitor&) = delete;
  TabMonitor& operator=(const TabMonitor&) = delete;

  void Start();  // throws std::runtime_error
  void Stop();   // idempotent; throws std::logic_error from the callback thread
  bool running() const { return running_; }
  bool IsCallbackThread() const { return std::this_thread::get_id() == workerId_; }

 private:
  using Clock = std::chrono::steady_clock;

  enum class SignalType { Seed, Foreground, NameChange, Destroy };
  struct Signal {
    SignalType type;
    HWND hwnd;
  };
  struct Tracked {
    BrowserWindow window;
    Clock::time_point changedAt{};
    Clock::time_point lastScan{};
    bool pending = false;
    bool activation = false;
    bool initial = false;
    std::wstring reportedCaption;
    std::wstring reportedTitle;
    std::string reportedUrl;
  };
  struct CacheEntry {
    Resolution resolution;
    Clock::time_point at;
  };

  static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject,
                                    LONG idChild, DWORD, DWORD);
  void HookLoop(std::promise<std::string>& ready);
  void WorkerLoop();
  void Post(Signal signal);

  void HandleForeground(HWND hwnd);
  void HandleNameChange(HWND hwnd);
  void HandleDestroy(HWND hwnd);
  void Seed();
  void Observe(const BrowserWindow& window, bool activation, bool initial = false);
  void FlushPending(Clock::time_point now);
  void MaybeRescan(Clock::time_point now);
  Resolution Resolve(const BrowserWindow& window, bool allowCache);
  void Emit(EventKind kind, const BrowserWindow& window, const Resolution& resolution);

  const MonitorOptions options_;
  const Callback callback_;

  std::thread hookThread_;
  std::thread worker_;
  std::thread::id workerId_;
  std::atomic<DWORD> hookThreadId_{0};
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Signal> queue_;

  // Worker-thread state.
  std::map<HWND, Tracked> tracked_;
  std::map<std::pair<HWND, std::wstring>, CacheEntry> cache_;
  HWND activeHwnd_ = nullptr;
  DWORD activePid_ = 0;
  bool leftBrowser_ = true;
};

}  // namespace browsermem
