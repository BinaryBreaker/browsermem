# How it works

Browsers don't publish the active tab's URL to other programs. They do keep it in their
own memory, next to the tab's title. browsermem finds it there.

## 1. Notice a change

A thread installs `SetWinEventHook` listeners for three Windows events:

| Event | Meaning |
|---|---|
| `EVENT_SYSTEM_FOREGROUND` | a window gained focus |
| `EVENT_OBJECT_NAMECHANGE` | a window's caption changed (every tab switch or titled navigation) |
| `EVENT_OBJECT_DESTROY` | a window closed |

A browser window is identified by its class (`Chrome_WidgetWin_1`, `MozillaWindowClass`)
and its executable (`chrome.exe`, `msedge.exe`, …). The caption is
`<tab title> - Google Chrome`. While a page loads it changes several times, so a worker
thread waits until it has stayed the same for `debounce_ms`.

## 2. Read the browser's memory

The browser's main process is opened with `PROCESS_VM_READ`. Only committed, private,
writable regions are read (`VirtualQueryEx` + `ReadProcessMemory`): that is the heap where
the browser keeps its C++ objects. Code, mapped files and other processes are never
touched, and nothing is ever written.

Memory is streamed in 2 MiB chunks through reusable per-thread buffers, on up to 8 threads.
Nothing is copied wholesale, so a lookup doesn't allocate hundreds of megabytes.

## 3. Follow the title to the URL

The browser process holds thousands of URLs: history, other tabs, images, scripts. To find
the active tab's URL:

1. **Find the title.** Search memory for the tab title taken from the window caption, in
   UTF-16 (`std::u16string`) and UTF-8. The search is SIMD-accelerated (SSE2).
2. **Find pointers to it.** Every 8-byte value that points to one of those title copies is
   a field inside an object that describes the tab, such as `TabRendererData` or
   `NavigationEntryImpl`.
3. **Look next door.** The same objects hold the page's `GURL`. In the ±512 bytes around
   each pointer, browsermem reads the strings that neighbouring pointers point to, and
   keeps those that are URLs. It also picks up short URLs stored inside the object itself
   (the small-string optimisation).
4. **Score.** Each URL is scored by how many objects reference it and how close the
   reference is. URLs that are clearly page resources, such as favicons, images, scripts
   and styles, are down-weighted, because browsers store the favicon URL right next to the
   page URL. The highest score wins, and `confidence` is `best / (best + runner_up)`.

```
TabRendererData (somewhere on the heap)
┌──────────────────────────┐
│ ...                      │
│ title_ ──────────────────┼──► u"BinaryBreaker/browsermem"
│ ...                      │
│ visible_url_.spec_ ──────┼──► "https://github.com/BinaryBreaker/browsermem"
│ ...                      │
└──────────────────────────┘
```

This doesn't depend on exact struct offsets, so it keeps working when browsers update.

## 4. Remember where tabs live

The browser keeps its tab objects in the same few memory regions for as long as it runs.
After each successful lookup, browsermem remembers the regions that held the winning
evidence: the tab objects, the title copies they point to, and the URL strings. The next
lookup searches only those *hot regions*, usually a few MB. If that doesn't produce a
confident answer, it falls back to a full scan, so the cache can make a lookup faster but
never less accurate.

## Performance

Measured on a Chrome profile with about 280 MB of heap:

| Lookup | Time |
|---|---|
| First lookup / new tab (full parallel scan) | ~45 ms |
| Tab seen before (hot regions) | **1–4 ms** |
| v0.1.0, for comparison (copied all memory) | ~350 ms |

On top of that, `TabMonitor` caches results per window and caption (`cache_ttl_ms`).

To profile on your own machine, build the developer benchmark:

```bash
cmake -B build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DBROWSERMEM_BUILD_TOOLS=ON
cmake --build build/bench
build\bench\browsermem_bench.exe
```

It prints a per-step breakdown (enumerate, title search, pointer search, scoring) for a
full scan and for the fast path.

## Limits

- **Same title, different URL** (single-page apps): no caption change means no event. Use
  `rescan_ms`.
- **Duplicate titles:** two tabs with the same title share an anchor, so the result may be
  the other tab's URL.
- **Firefox:** stores strings and objects differently. Support is best-effort.
