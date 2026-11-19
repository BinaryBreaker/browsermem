# Memory forensics

The engine behind the real-time API also works offline, on a memory image of a browser:
for example a minidump collected during an investigation. `MemoryImage` holds a set of
`(address, bytes)` regions and answers the same questions without a running browser.

## Acquire

```python
from browsermem.forensics import write_minidump

write_minidump(pid, "chrome.dmp")    # full-memory dump, like Task Manager's "Create dump file"
```

Dumps from Task Manager, Sysinternals ProcDump (`procdump -ma`) or WinDbg also work, as
long as they are **full-memory** dumps. Dump the browser's main process: the one that
owns the windows, whose `pid` is shown by `browsermem windows`.

## Analyse

```python
from browsermem import MemoryImage

with MemoryImage.from_minidump("chrome.dmp") as image:
    print(image)                                   # <MemoryImage 4077 regions, 275.0 MiB>

    for url, count in image.urls()[:20]:           # every URL, most frequent first
        print(count, url)

    tab = image.resolve("Inbox (3) - Gmail")       # the URL belonging to a tab title
    print(tab.url, tab.confidence)

    # a full window caption works too, if you say which browser it came from
    image.resolve("Inbox (3) - Gmail - Google Chrome", browser="chrome")
```

By default only private, writable memory is loaded, which is where browsers keep their
objects. Pass `private_only=False` to load every region in the dump.

## Other sources

`MemoryImage.from_regions()` accepts any iterable of `(base_address, bytes)` pairs. Use it
to feed regions from Volatility, a raw acquisition, or your own tooling:

```python
image = MemoryImage.from_regions(my_regions)
```

`MemoryImage.capture(pid)` takes a snapshot of a live process instead, so you can analyse
the same state several times.

## Command line

```bash
browsermem minidump 48836 chrome.dmp
browsermem analyze chrome.dmp -n 50                      # top URLs
browsermem analyze chrome.dmp --title "Inbox - Gmail"    # resolve a tab
browsermem analyze chrome.dmp --json > urls.json
```
