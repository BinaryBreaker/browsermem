<div align="center">

# browsermem

**Get the real-time URL of the active browser tab from Python. No extension, no WebDriver, no setup.**

[![PyPI](https://img.shields.io/pypi/v/browsermem.svg)](https://pypi.org/project/browsermem/)
[![Python](https://img.shields.io/pypi/pyversions/browsermem.svg)](https://pypi.org/project/browsermem/)
[![CI](https://github.com/BinaryBreaker/browsermem/actions/workflows/ci.yml/badge.svg)](https://github.com/BinaryBreaker/browsermem/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Docs](https://img.shields.io/badge/docs-binarybreaker.github.io-informational)](https://binarybreaker.github.io/browsermem/)

</div>

```python
import browsermem

browsermem.current_url()
# 'https://github.com/BinaryBreaker/browsermem'
```

`browsermem` reads the active tab's URL and title straight from the browser's memory, and
sends an event every time you switch tabs, open a tab or navigate. It works with the
browser you already use: the user doesn't need to install anything or restart the browser
with special flags.

## Features

- **Active tab:** `current_url()`, `get_active_tab()`, or the active tab of every open window.
- **Live events:** tab switched, new tab, navigation, browser window focused or closed.
- **Several ways to handle events:** callbacks, decorators, iterators or `async for`.
- **Filters:** `@monitor.on_domain("youtube.com")`, `@monitor.on_url(r"/checkout")`, and
  `monitor.wait_for(...)` to block until a matching page opens.
- **Activity tracking:** `TabHistory` records visits and time spent per domain.
- **Browsers:** Chrome, Edge, Brave, Opera and Vivaldi (any Chromium-based browser).
  Firefox is experimental.
- **Fast:** event-driven (no polling loop in Python); about 0.1–0.5 s per lookup, with caching.
- **Command line:** `browsermem active`, `browsermem watch --json` and more.
- **Typed and dependency-free:** one wheel covers Python 3.9–3.14. Nothing to compile.
- **Bonus: memory forensics.** Analyse minidumps of a browser offline.

## Install

```bash
pip install browsermem
```

Requires Windows 10/11 (x64) and Python 3.9+.

## Quick start

### The active tab

```python
import browsermem

tab = browsermem.get_active_tab()
print(tab.browser_name)   # Google Chrome
print(tab.title)          # BinaryBreaker/browsermem
print(tab.url)            # https://github.com/BinaryBreaker/browsermem
print(tab.domain)         # github.com
```

### React to tab changes

```python
from browsermem import TabMonitor, EventKind

monitor = TabMonitor()

@monitor.on(EventKind.TAB_CHANGED, EventKind.WINDOW_ACTIVATED)
def changed(tab):
    print(f"{tab.browser_name}: {tab.title} -> {tab.url}")

@monitor.on_domain("youtube.com")
def youtube(tab):
    print("Watching:", tab.title)

monitor.run_forever()   # Ctrl+C to stop
```

### Iterate, or use asyncio

```python
with TabMonitor() as monitor:
    for tab in monitor.events():
        print(tab.url)

async with TabMonitor() as monitor:
    async for tab in monitor.aevents():
        print(tab.url)
```

### Wait for a page

```python
with TabMonitor() as monitor:
    tab = monitor.wait_for(r"/payment/success", timeout=300)
    print("Done!" if tab else "Timed out")
```

### Track browsing time

```python
from browsermem import TabMonitor, TabHistory

monitor = TabMonitor()
history = TabHistory(monitor)
monitor.start()
# ... later
for domain, spent in history.time_by_domain().items():
    print(f"{domain:30} {spent}")
```

### Command line

```bash
browsermem active                # URL of the active tab
browsermem windows               # all browser windows
browsermem watch                 # live tab events
browsermem watch --json          # JSON lines, ready to pipe
```

## Events

| `EventKind` | When |
|---|---|
| `SNAPSHOT` | Current state when the monitor starts (also returned by one-shot queries) |
| `TAB_CHANGED` | Tab switch, new tab, or a navigation that changed the page title |
| `WINDOW_ACTIVATED` | You switched to a browser window from another app or window |
| `URL_CHANGED` | Same title, different URL (single-page apps). Needs `rescan_ms` |
| `WINDOW_CLOSED` | A browser window closed; the event holds its last tab |

## How it works

1. Windows notifies browsermem when a browser window gains focus or its title changes,
   which happens on every tab switch and navigation.
2. browsermem takes a snapshot of the browser process's own memory (read-only, the same
   access Task Manager uses).
3. It finds the tab's title in that memory, follows the pointers that refer to it, and
   picks the URL stored inside the same browser objects.

See [How it works](https://binarybreaker.github.io/browsermem/how-it-works/) for details.

## Supported browsers

| Browser | Status |
|---|---|
| Google Chrome | ✅ tested |
| Microsoft Edge | ✅ tested |
| Brave, Opera, Vivaldi, Yandex | ✅ supported (same Chromium engine) |
| Firefox | 🧪 experimental |

## Limitations

- Windows x64 only for now.
- A navigation that keeps the same page title is only detected with `TabMonitor(rescan_ms=...)`.
- Two tabs with exactly the same title can be confused.
- browsermem only reads processes your user account can open (no elevation needed for
  your own browser).

## Responsible use

browsermem reads memory of processes that belong to **your** user account. Use it for your
own productivity tools, automation, research and authorised investigations. Monitoring
other people's browsing without their knowledge and consent may be illegal where you live.

## Contributing

Bug reports and pull requests are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE) © BinaryBreaker
