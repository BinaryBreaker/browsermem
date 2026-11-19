# Getting started

## Install

```bash
pip install browsermem
```

| Requirement | |
|---|---|
| OS | Windows 10 or 11, 64-bit |
| Python | 3.9 – 3.14 (64-bit) |
| Browsers | Chrome, Edge, Brave, Opera, Vivaldi, Yandex; Firefox experimental |

The wheel contains a small native DLL with its runtime linked in, so there's nothing else
to install or compile.

## Your first URL

```python
import browsermem

print(browsermem.current_url())
```

`current_url()` returns the URL of the active tab in the focused browser window. If no
browser is focused, it uses the most recently used browser window. It returns `None` when
no browser is open.

## The full tab

```python
tab = browsermem.get_active_tab()
if tab:
    print(tab.browser_name, tab.title, tab.url)
    print(tab.domain, tab.is_web, f"{tab.confidence:.0%}")
```

A [`Tab`](api.md#browsermem.Tab) carries:

| Field | Example |
|---|---|
| `url` | `https://github.com/BinaryBreaker/browsermem` (`None` if it couldn't be resolved) |
| `title` | `BinaryBreaker/browsermem` |
| `browser`, `browser_name` | `Browser.CHROME`, `"Google Chrome"` |
| `domain`, `host`, `scheme` | `github.com`, `github.com`, `https` |
| `confidence` | `0.0` – `1.0`: how clearly the URL stood out from other candidates |
| `pid`, `hwnd` | browser process id and window handle |
| `timestamp`, `scan_ms` | when it was read, and how long the lookup took |

## Every window

```python
for window in browsermem.list_windows():      # instant: doesn't read memory
    print(window.browser_name, window.title)

for tab in browsermem.get_all_tabs():         # the active tab of every window
    print(tab.title, tab.url)
```

## Only some browsers

Every function and `TabMonitor` accepts `browsers=`:

```python
from browsermem import Browser

browsermem.current_url(browsers=Browser.EDGE)
browsermem.get_active_tab(browsers=["chrome", "brave"])
```

## Errors

| Exception | Raised when |
|---|---|
| `NotFoundError` | the window or process doesn't exist or isn't a supported browser |
| `AccessDeniedError` | the browser runs elevated or under another user |
| `BrowserMemError` | base class of all of the above |

Next: [react to tab changes](events.md).
