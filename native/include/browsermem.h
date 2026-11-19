/*
 * browsermem -- recover browser tab URLs and titles from browser process
 * memory (Windows x64): live monitoring and volatile-memory forensics.
 *
 * Plain C ABI so the DLL can be consumed from Python (ctypes), C#, Rust, ...
 *
 * Memory ownership
 * ----------------
 *  - Objects returned through an out-parameter are owned by the caller and
 *    must be released with the matching bm_*_free function.
 *  - The bm_tab passed to a bm_tab_callback is only valid for the duration
 *    of the callback; copy what you need.
 *  - All strings are UTF-8 and NUL-terminated.
 *
 * Threading
 * ---------
 *  - All functions are thread-safe. A bm_image may be shared between threads.
 *  - Monitor callbacks run on a library-owned worker thread. Do not call
 *    bm_monitor_stop() from inside a callback (it returns BM_ERR_STATE).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BROWSERMEM_H_
#define BROWSERMEM_H_

#include <stdint.h>

#if defined(BM_BUILD_DLL)
#  define BM_API __declspec(dllexport)
#elif defined(BM_STATIC)
#  define BM_API
#else
#  define BM_API __declspec(dllimport)
#endif
#define BM_CALL __cdecl

#ifdef __cplusplus
extern "C" {
#endif

/* ---- status codes ------------------------------------------------------ */
#define BM_OK                 0
#define BM_ERR_INVALID_ARG   -1
#define BM_ERR_NOT_FOUND     -2  /* no matching browser window / process      */
#define BM_ERR_ACCESS        -3  /* could not open / read the process          */
#define BM_ERR_STATE         -4  /* call not allowed in the current state      */
#define BM_ERR_INTERNAL      -5

/* ---- browsers (use BM_BROWSER_MASK(x) to build a filter mask) ----------- */
#define BM_BROWSER_UNKNOWN  0
#define BM_BROWSER_CHROME   1
#define BM_BROWSER_EDGE     2
#define BM_BROWSER_BRAVE    3
#define BM_BROWSER_OPERA    4
#define BM_BROWSER_VIVALDI  5
#define BM_BROWSER_YANDEX   6
#define BM_BROWSER_FIREFOX  7  /* experimental: different object model      */
#define BM_BROWSER_MASK(b)  (1u << (b))
#define BM_BROWSER_ALL      0xFFFFFFFFu

/* ---- event kinds -------------------------------------------------------- */
#define BM_EVENT_SNAPSHOT          0  /* one-shot query, or initial state at start  */
#define BM_EVENT_TAB_CHANGED       1  /* tab switch, new tab, or titled navigation  */
#define BM_EVENT_WINDOW_ACTIVATED  2  /* user switched to a browser window          */
#define BM_EVENT_URL_CHANGED       3  /* periodic rescan found a new URL, same title*/
#define BM_EVENT_WINDOW_CLOSED     4  /* browser window destroyed (last known tab)  */

typedef struct bm_candidate {
  const char* url;
  double score;
} bm_candidate;

typedef struct bm_tab {
  int32_t event;                    /* BM_EVENT_*                              */
  int32_t browser;                  /* BM_BROWSER_*                            */
  const char* browser_name;         /* "Google Chrome", "Microsoft Edge", ...  */
  uint32_t pid;                     /* browser (UI) process id; 0 offline      */
  uint32_t candidate_count;
  uint64_t hwnd;                    /* top-level window handle; 0 offline      */
  const char* title;                /* active tab title                        */
  const char* caption;              /* raw window caption                      */
  const char* url;                  /* "" if it could not be resolved          */
  double confidence;                /* 0..1                                    */
  uint64_t timestamp_ms;            /* Unix epoch, milliseconds                */
  uint32_t scan_ms;                 /* time spent reading + analysing memory   */
  uint32_t reserved;
  const bm_candidate* candidates;   /* ranked, best first                      */
} bm_tab;

typedef struct bm_window {
  uint64_t hwnd;
  uint32_t pid;
  int32_t browser;
  const char* browser_name;
  const char* caption;
  const char* title;                /* caption without the browser suffix      */
  int32_t is_foreground;
  int32_t reserved;
} bm_window;

typedef struct bm_url_entry {
  const char* url;
  uint32_t count;                   /* occurrences in memory                   */
  uint32_t reserved;
} bm_url_entry;

typedef struct bm_monitor_options {
  uint32_t struct_size;     /* sizeof(bm_monitor_options)                         */
  uint32_t browser_mask;    /* default BM_BROWSER_ALL                             */
  uint32_t debounce_ms;     /* caption must be stable this long; default 600      */
  uint32_t rescan_ms;       /* periodic rescan of the focused tab; 0 = off        */
  uint32_t cache_ttl_ms;    /* reuse results for an unchanged caption; 30000      */
  int32_t all_windows;      /* 1 = also track background browser windows          */
  int32_t emit_unresolved;  /* 1 = deliver events even when the URL is unknown    */
  int32_t reserved;
} bm_monitor_options;

typedef struct bm_monitor bm_monitor;
typedef struct bm_image bm_image;
typedef void(BM_CALL* bm_tab_callback)(const bm_tab* tab, void* user_data);

/* ---- general ------------------------------------------------------------ */
BM_API const char* BM_CALL bm_version(void);
/* Message for the last failing call on this thread (never NULL). */
BM_API const char* BM_CALL bm_last_error(void);

/* ---- discovery ---------------------------------------------------------- */
BM_API int BM_CALL bm_list_windows(uint32_t browser_mask, bm_window** out, uint32_t* count);
BM_API void BM_CALL bm_windows_free(bm_window* windows);

/* ---- one-shot live queries ---------------------------------------------- */
/* Foreground browser window if there is one, otherwise the most recently
 * active browser window (top of the z-order). */
BM_API int BM_CALL bm_get_active_tab(uint32_t browser_mask, bm_tab** out);
BM_API int BM_CALL bm_get_tab(uint64_t hwnd, bm_tab** out);
BM_API void BM_CALL bm_tab_free(bm_tab* tab);

/* Every URL string in a process's memory. pid == 0 selects the most
 * recently active browser. */
BM_API int BM_CALL bm_dump_urls(uint32_t pid, bm_url_entry** out, uint32_t* count);
BM_API void BM_CALL bm_urls_free(bm_url_entry* urls);

/* ---- memory images (forensics) ------------------------------------------ */
/* A bm_image is a set of (base address, bytes) regions: either captured
 * from a live process or assembled by the caller (e.g. from a minidump or a
 * raw acquisition). Analysis needs no running browser. */
BM_API int BM_CALL bm_image_create(bm_image** out);
/* Copy the private read/write memory of `pid` (0 = active browser). */
BM_API int BM_CALL bm_image_capture(uint32_t pid, bm_image** out);
/* Add a region; `data` is copied. Regions must not overlap. */
BM_API int BM_CALL bm_image_add_region(bm_image* image, uint64_t base, const void* data,
                                       uint64_t size);
BM_API int BM_CALL bm_image_info(bm_image* image, uint64_t* total_bytes, uint32_t* region_count);
BM_API int BM_CALL bm_image_urls(bm_image* image, bm_url_entry** out, uint32_t* count);
/* Resolve the URL belonging to a tab title. `browser` (BM_BROWSER_*, may be
 * 0) lets `title` also be a full window caption such as
 * "Inbox - Google Chrome". */
BM_API int BM_CALL bm_image_resolve(bm_image* image, const char* title, int32_t browser,
                                    bm_tab** out);
BM_API void BM_CALL bm_image_free(bm_image* image);

/* ---- live monitoring ---------------------------------------------------- */
BM_API void BM_CALL bm_monitor_options_init(bm_monitor_options* options);
/* options may be NULL for defaults. */
BM_API int BM_CALL bm_monitor_start(const bm_monitor_options* options, bm_tab_callback callback,
                                    void* user_data, bm_monitor** out);
/* Stops the monitor, waits for its threads, and frees it. */
BM_API int BM_CALL bm_monitor_stop(bm_monitor* monitor);

#ifdef __cplusplus
}
#endif
#endif /* BROWSERMEM_H_ */
