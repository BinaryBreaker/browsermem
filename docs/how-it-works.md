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

## 2. Snapshot the browser's memory

The worker opens the browser's main process with `PROCESS_VM_READ` and copies every
committed, private, writable region (`VirtualQueryEx` + `ReadProcessMemory`). That is the
heap where the browser keeps its C++ objects. Code, mapped files and other processes are
never touched, and nothing is ever written.

## 3. Collect URL strings

Every `scheme://` string is collected, in both encodings Chromium uses:

- **UTF-8** `std::string`, e.g. `GURL::spec_`
- **UTF-16** `std::u16string`, e.g. text shown in the address bar

The exact address where each one starts is recorded.

## 4. Follow the title

The browser process holds thousands of URLs: history, other tabs, images, scripts. To find
the active tab's URL:

1. Search memory for the tab title (UTF-16) taken from the window caption.
2. Find every 8-byte value that **points to** one of those title copies. Each pointer is a
   field inside an object that describes the tab, such as `TabRendererData` or
   `NavigationEntryImpl`.
3. Those same objects also hold a `GURL`. Look at the ±512 bytes around each pointer for
   pointers to URL strings, and for short URLs stored inside the object itself.
4. Score each URL by how many objects reference it and how close the reference is. The
   highest score wins. `confidence` is `best / (best + runner_up)`.

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

## Performance

A lookup reads 100–600 MB and takes about 0.1–0.5 s, mostly copying memory. Results are
cached per window and caption (`cache_ttl_ms`), so switching back to a tab you've already
seen is instant.

## Limits

- **Same title, different URL** (single-page apps): no caption change means no event. Use
  `rescan_ms`.
- **Duplicate titles:** two tabs with the same title share an anchor, so the result may be
  the other tab's URL.
- **Firefox:** stores strings and objects differently. Support is best-effort.
