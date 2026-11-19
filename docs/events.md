# Live events

`TabMonitor` sends an event whenever the active tab changes. It is event-driven: it uses
Windows notifications for focus and title changes, debounces them, and only then reads
browser memory.

## Event kinds

| `EventKind` | When |
|---|---|
| `SNAPSHOT` | Once per window when the monitor starts: the current state |
| `TAB_CHANGED` | Tab switch, new tab, or a navigation that changed the title |
| `WINDOW_ACTIVATED` | The user switched to a browser window from another app or window |
| `URL_CHANGED` | Same title, different URL. Needs `rescan_ms` (see below) |
| `WINDOW_CLOSED` | A browser window closed; `url` and `title` hold its last tab |

## Four ways to handle events

=== "Decorators"

    ```python
    from browsermem import TabMonitor, EventKind

    monitor = TabMonitor()

    @monitor.on(EventKind.TAB_CHANGED, EventKind.WINDOW_ACTIVATED)
    def changed(tab):
        print(tab.title, tab.url)

    @monitor.on()               # no arguments: every event
    def log_all(tab):
        ...

    monitor.run_forever()
    ```

=== "Callback"

    ```python
    TabMonitor(lambda tab: print(tab.url)).run_forever()
    ```

=== "Iterator"

    ```python
    with TabMonitor() as monitor:
        for tab in monitor.events():
            print(tab.url)
    ```

=== "asyncio"

    ```python
    async with TabMonitor() as monitor:
        async for tab in monitor.aevents():
            print(tab.url)
    ```

Callbacks and decorated handlers run on a background thread owned by browsermem. Keep them
short, or hand the work to your own queue, thread or event loop. `events()` and `aevents()`
deliver on *your* thread or event loop instead.

## Filters

```python
@monitor.on_domain("youtube.com", "twitch.tv")      # subdomains included
def video(tab):
    ...

@monitor.on_url(r"github\.com/.+/pull/\d+")         # regex, re.search
def pull_request(tab):
    ...
```

## Waiting for a page

`wait_for` blocks until an event matches, then returns that tab. On timeout it returns
`None`.

```python
with TabMonitor() as monitor:
    tab = monitor.wait_for(r"/oauth/callback\?code=", timeout=120)

    # or with any predicate
    tab = monitor.wait_for(lambda t: t.domain == "mail.google.com")
```

## Options

```python
TabMonitor(
    callback=None,          # called for every event
    browsers=None,          # e.g. Browser.CHROME or ["chrome", "edge"]
    all_windows=False,      # also report changes in background windows
    debounce_ms=600,        # wait for the title to settle while a page loads
    rescan_ms=0,            # >0: rescan the focused tab periodically (URL_CHANGED)
    cache_ttl_ms=30_000,    # reuse results while a window's title is unchanged
    emit_unresolved=True,   # deliver events even if the URL couldn't be found
)
```

!!! tip "Single-page apps"
    Some sites change the URL without changing the page title. Set `rescan_ms=2000` to
    catch those as `URL_CHANGED` events. Each rescan costs roughly 0.1–0.5 s of CPU, and
    only the focused tab is rescanned.

## Lifecycle

- `start()` and `stop()` are idempotent. A monitor can be started again after stopping.
- `stop()` is safe to call from inside a handler.
- `with TabMonitor() as m:` / `async with TabMonitor() as m:` stop automatically.
- `run_forever()` starts the monitor, dispatches events until Ctrl+C, then stops.
