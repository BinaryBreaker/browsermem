// C ABI over the C++ core. Everything that crosses the boundary is a POD
// struct whose strings are owned by a hidden "holder" object.
//
// SPDX-License-Identifier: MIT

#include "browsermem.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>

#include "browsers.hpp"
#include "memory.hpp"
#include "monitor.hpp"
#include "resolver.hpp"
#include "url_index.hpp"

#ifndef BM_VERSION
#define BM_VERSION "0.0.0-dev"
#endif

using namespace browsermem;

namespace {

thread_local std::string t_lastError;

int Fail(int code, std::string message) {
  t_lastError = std::move(message);
  return code;
}
int Ok() {
  t_lastError.clear();
  return BM_OK;
}

// ---- holders ---------------------------------------------------------------

struct TabHolder {
  bm_tab tab{};
  std::string title, caption, url;
  std::deque<std::string> candidateUrls;  // deque: stable addresses
  std::vector<bm_candidate> candidates;

  TabHolder(EventKind kind, const BrowserWindow& w, const Resolution& r, uint64_t timestampMs)
      : title(ToUtf8(r.title)), caption(ToUtf8(w.caption)), url(r.url) {
    for (const auto& c : r.candidates) {
      candidateUrls.push_back(c.url);
      candidates.push_back({candidateUrls.back().c_str(), c.score});
    }
    tab.event = static_cast<int32_t>(kind);
    tab.browser = static_cast<int32_t>(w.browser());
    tab.browser_name = BrowserName(w.browser());
    tab.pid = w.pid;
    tab.hwnd = reinterpret_cast<uint64_t>(w.hwnd);
    tab.title = title.c_str();
    tab.caption = caption.c_str();
    tab.url = url.c_str();
    tab.confidence = r.confidence;
    tab.timestamp_ms = timestampMs;
    tab.scan_ms = r.elapsedMs;
    tab.candidate_count = static_cast<uint32_t>(candidates.size());
    tab.candidates = candidates.empty() ? nullptr : candidates.data();
  }
  TabHolder(const TabHolder&) = delete;
  TabHolder& operator=(const TabHolder&) = delete;
};

struct WindowListHolder {
  std::vector<bm_window> items;
  std::deque<std::string> strings;
};

struct UrlListHolder {
  std::vector<bm_url_entry> items;
  std::deque<std::string> strings;
};

// Maps the pointer handed to the caller to the object that owns it.
class LiveObjects {
 public:
  template <class T>
  void Keep(const void* key, std::shared_ptr<T> owner) {
    std::lock_guard<std::mutex> lock(mu_);
    objects_[key] = std::move(owner);
  }
  void Release(const void* key) {
    std::shared_ptr<void> doomed;  // destroyed outside the lock
    std::lock_guard<std::mutex> lock(mu_);
    auto it = objects_.find(key);
    if (it != objects_.end()) {
      doomed = std::move(it->second);
      objects_.erase(it);
    }
  }

 private:
  std::mutex mu_;
  std::unordered_map<const void*, std::shared_ptr<void>> objects_;
};

LiveObjects& Live() {
  static LiveObjects live;
  return live;
}

int ReturnTab(EventKind kind, const BrowserWindow& window, const Resolution& r, bm_tab** out) {
  auto holder = std::make_shared<TabHolder>(kind, window, r, UnixMillis());
  *out = &holder->tab;
  Live().Keep(*out, holder);
  return Ok();
}

int ReturnUrls(const UrlIndex& urls, bm_url_entry** out, uint32_t* count) {
  std::vector<uint32_t> ids(urls.size());
  for (uint32_t i = 0; i < ids.size(); ++i) ids[i] = i;
  std::sort(ids.begin(), ids.end(),
            [&](uint32_t a, uint32_t b) { return urls.count(a) > urls.count(b); });

  auto holder = std::make_shared<UrlListHolder>();
  holder->items.reserve(ids.size());
  for (uint32_t id : ids) {
    const auto& s = holder->strings.emplace_back(urls.url(id));
    holder->items.push_back({s.c_str(), urls.count(id), 0});
  }
  *count = static_cast<uint32_t>(holder->items.size());
  if (holder->items.empty()) return Ok();
  *out = holder->items.data();
  Live().Keep(*out, holder);
  return Ok();
}

std::optional<BrowserWindow> ActiveBrowserWindow(uint32_t mask) {
  if (auto w = IdentifyWindow(GetForegroundWindow(), mask)) return w;
  auto all = EnumerateBrowserWindows(mask);
  if (all.empty()) return std::nullopt;
  return all.front();
}

#define BM_GUARD_BEGIN try {
#define BM_GUARD_END                                 \
  }                                                  \
  catch (const std::bad_alloc&) {                    \
    return Fail(BM_ERR_INTERNAL, "out of memory");   \
  }                                                  \
  catch (const std::exception& e) {                  \
    return Fail(BM_ERR_INTERNAL, e.what());          \
  }

}  // namespace

// ---- opaque handles ----------------------------------------------------------

struct bm_monitor {
  std::unique_ptr<TabMonitor> impl;
  bm_tab_callback callback = nullptr;
  void* userData = nullptr;
};

struct bm_image {
  std::mutex mu;
  ProcessSnapshot snapshot;
  std::unique_ptr<UrlIndex> index;  // built lazily, reset when regions change

  const UrlIndex& Index() {
    if (!index) {
      snapshot.Finalize();
      index = std::make_unique<UrlIndex>();
      index->Build(snapshot);
    }
    return *index;
  }
};

extern "C" {

const char* BM_CALL bm_version(void) { return BM_VERSION; }

const char* BM_CALL bm_last_error(void) { return t_lastError.c_str(); }

// ---- discovery -----------------------------------------------------------------

int BM_CALL bm_list_windows(uint32_t browser_mask, bm_window** out, uint32_t* count) {
  if (!out || !count) return Fail(BM_ERR_INVALID_ARG, "out and count must not be NULL");
  *out = nullptr;
  *count = 0;
  BM_GUARD_BEGIN
  auto holder = std::make_shared<WindowListHolder>();
  HWND foreground = GetForegroundWindow();
  for (const BrowserWindow& w : EnumerateBrowserWindows(browser_mask)) {
    auto titles = TitleCandidates(w);
    const auto& caption = holder->strings.emplace_back(ToUtf8(w.caption));
    const auto& title = holder->strings.emplace_back(titles.empty() ? "" : ToUtf8(titles[0]));
    bm_window item{};
    item.hwnd = reinterpret_cast<uint64_t>(w.hwnd);
    item.pid = w.pid;
    item.browser = static_cast<int32_t>(w.browser());
    item.browser_name = BrowserName(w.browser());
    item.caption = caption.c_str();
    item.title = title.c_str();
    item.is_foreground = w.hwnd == foreground;
    holder->items.push_back(item);
  }
  *count = static_cast<uint32_t>(holder->items.size());
  if (holder->items.empty()) return Ok();
  *out = holder->items.data();
  Live().Keep(*out, holder);
  return Ok();
  BM_GUARD_END
}

void BM_CALL bm_windows_free(bm_window* windows) {
  if (windows) Live().Release(windows);
}

// ---- one-shot live queries ----------------------------------------------------

int BM_CALL bm_get_active_tab(uint32_t browser_mask, bm_tab** out) {
  if (!out) return Fail(BM_ERR_INVALID_ARG, "out must not be NULL");
  *out = nullptr;
  BM_GUARD_BEGIN
  auto window = ActiveBrowserWindow(browser_mask);
  if (!window) return Fail(BM_ERR_NOT_FOUND, "no browser window found");
  Resolution r;
  std::string error;
  if (!ResolveActiveTab(*window, r, error)) return Fail(BM_ERR_ACCESS, error);
  return ReturnTab(EventKind::Snapshot, *window, r, out);
  BM_GUARD_END
}

int BM_CALL bm_get_tab(uint64_t hwnd, bm_tab** out) {
  if (!out) return Fail(BM_ERR_INVALID_ARG, "out must not be NULL");
  *out = nullptr;
  BM_GUARD_BEGIN
  auto window = IdentifyWindow(reinterpret_cast<HWND>(hwnd));
  if (!window) return Fail(BM_ERR_NOT_FOUND, "not a supported browser window");
  Resolution r;
  std::string error;
  if (!ResolveActiveTab(*window, r, error)) return Fail(BM_ERR_ACCESS, error);
  return ReturnTab(EventKind::Snapshot, *window, r, out);
  BM_GUARD_END
}

void BM_CALL bm_tab_free(bm_tab* tab) {
  if (tab) Live().Release(tab);
}

int BM_CALL bm_dump_urls(uint32_t pid, bm_url_entry** out, uint32_t* count) {
  if (!out || !count) return Fail(BM_ERR_INVALID_ARG, "out and count must not be NULL");
  *out = nullptr;
  *count = 0;
  BM_GUARD_BEGIN
  if (!pid) {
    auto window = ActiveBrowserWindow(kAllBrowsers);
    if (!window) return Fail(BM_ERR_NOT_FOUND, "no browser window found");
    pid = window->pid;
  }
  ProcessSnapshot snapshot;
  std::string error;
  if (!snapshot.Capture(pid, error)) return Fail(BM_ERR_ACCESS, error);
  UrlIndex urls;
  urls.Build(snapshot);
  return ReturnUrls(urls, out, count);
  BM_GUARD_END
}

void BM_CALL bm_urls_free(bm_url_entry* urls) {
  if (urls) Live().Release(urls);
}

// ---- memory images ------------------------------------------------------------

int BM_CALL bm_image_create(bm_image** out) {
  if (!out) return Fail(BM_ERR_INVALID_ARG, "out must not be NULL");
  BM_GUARD_BEGIN
  *out = new bm_image();
  return Ok();
  BM_GUARD_END
}

int BM_CALL bm_image_capture(uint32_t pid, bm_image** out) {
  if (!out) return Fail(BM_ERR_INVALID_ARG, "out must not be NULL");
  *out = nullptr;
  BM_GUARD_BEGIN
  if (!pid) {
    auto window = ActiveBrowserWindow(kAllBrowsers);
    if (!window) return Fail(BM_ERR_NOT_FOUND, "no browser window found");
    pid = window->pid;
  }
  auto image = std::make_unique<bm_image>();
  std::string error;
  if (!image->snapshot.Capture(pid, error)) return Fail(BM_ERR_ACCESS, error);
  *out = image.release();
  return Ok();
  BM_GUARD_END
}

int BM_CALL bm_image_add_region(bm_image* image, uint64_t base, const void* data, uint64_t size) {
  if (!image || (!data && size)) return Fail(BM_ERR_INVALID_ARG, "image and data are required");
  BM_GUARD_BEGIN
  std::lock_guard<std::mutex> lock(image->mu);
  image->snapshot.AddRegion(base, static_cast<const uint8_t*>(data), static_cast<size_t>(size));
  image->index.reset();
  return Ok();
  BM_GUARD_END
}

int BM_CALL bm_image_info(bm_image* image, uint64_t* total_bytes, uint32_t* region_count) {
  if (!image) return Fail(BM_ERR_INVALID_ARG, "image must not be NULL");
  std::lock_guard<std::mutex> lock(image->mu);
  if (total_bytes) *total_bytes = image->snapshot.totalBytes();
  if (region_count) *region_count = static_cast<uint32_t>(image->snapshot.regions().size());
  return Ok();
}

int BM_CALL bm_image_urls(bm_image* image, bm_url_entry** out, uint32_t* count) {
  if (!image || !out || !count) return Fail(BM_ERR_INVALID_ARG, "arguments must not be NULL");
  *out = nullptr;
  *count = 0;
  BM_GUARD_BEGIN
  std::lock_guard<std::mutex> lock(image->mu);
  return ReturnUrls(image->Index(), out, count);
  BM_GUARD_END
}

int BM_CALL bm_image_resolve(bm_image* image, const char* title, int32_t browser, bm_tab** out) {
  if (!image || !title || !out) return Fail(BM_ERR_INVALID_ARG, "arguments must not be NULL");
  *out = nullptr;
  BM_GUARD_BEGIN
  const auto start = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(image->mu);

  BrowserWindow window;  // pseudo window: no hwnd/pid offline
  window.profile = FindProfile(static_cast<Browser>(browser));
  window.caption = ToWide(title);
  const size_t inlineBytes = window.profile ? window.profile->inlineTitleBytes : 22;

  const UrlIndex& urls = image->Index();
  Resolution r = CorrelateTitle(image->snapshot, urls, TitleCandidates(window), inlineBytes);
  r.bytesScanned = image->snapshot.totalBytes();
  r.uniqueUrls = urls.size();
  r.elapsedMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count());
  return ReturnTab(EventKind::Snapshot, window, r, out);
  BM_GUARD_END
}

void BM_CALL bm_image_free(bm_image* image) { delete image; }

// ---- live monitoring -----------------------------------------------------------

void BM_CALL bm_monitor_options_init(bm_monitor_options* o) {
  if (!o) return;
  MonitorOptions d;
  *o = {};
  o->struct_size = sizeof(*o);
  o->browser_mask = d.browserMask;
  o->debounce_ms = d.debounceMs;
  o->rescan_ms = d.rescanMs;
  o->cache_ttl_ms = d.cacheTtlMs;
  o->all_windows = d.allWindows;
  o->emit_unresolved = d.emitUnresolved;
}

int BM_CALL bm_monitor_start(const bm_monitor_options* options, bm_tab_callback callback,
                             void* user_data, bm_monitor** out) {
  if (!callback || !out) return Fail(BM_ERR_INVALID_ARG, "callback and out must not be NULL");
  *out = nullptr;

  bm_monitor_options o;
  bm_monitor_options_init(&o);
  if (options) {
    if (options->struct_size != sizeof(bm_monitor_options))
      return Fail(BM_ERR_INVALID_ARG, "options->struct_size mismatch");
    o = *options;
  }
  MonitorOptions mo;
  mo.browserMask = o.browser_mask;
  mo.debounceMs = o.debounce_ms;
  mo.rescanMs = o.rescan_ms;
  mo.cacheTtlMs = o.cache_ttl_ms;
  mo.allWindows = o.all_windows != 0;
  mo.emitUnresolved = o.emit_unresolved != 0;

  BM_GUARD_BEGIN
  auto monitor = std::make_unique<bm_monitor>();
  monitor->callback = callback;
  monitor->userData = user_data;
  bm_monitor* raw = monitor.get();
  monitor->impl = std::make_unique<TabMonitor>(mo, [raw](const TabEvent& e) {
    TabHolder holder(e.kind, e.window, e.resolution, e.timestampMs);
    raw->callback(&holder.tab, raw->userData);
  });
  monitor->impl->Start();
  *out = monitor.release();
  return Ok();
  BM_GUARD_END
}

int BM_CALL bm_monitor_stop(bm_monitor* monitor) {
  if (!monitor) return Fail(BM_ERR_INVALID_ARG, "monitor must not be NULL");
  if (monitor->impl->IsCallbackThread())
    return Fail(BM_ERR_STATE, "bm_monitor_stop() cannot be called from a monitor callback");
  BM_GUARD_BEGIN
  monitor->impl->Stop();
  delete monitor;
  return Ok();
  BM_GUARD_END
}

}  // extern "C"
