"""Live tab monitor. Switch tabs, open tabs, navigate; Ctrl+C to quit."""

import browsermem
from browsermem import EventKind

monitor = browsermem.TabMonitor(all_windows=True)


@monitor.on(EventKind.SNAPSHOT, EventKind.TAB_CHANGED, EventKind.WINDOW_ACTIVATED,
            EventKind.URL_CHANGED)
def on_tab(tab: browsermem.Tab) -> None:
    print(f"[{tab.timestamp:%H:%M:%S}] {tab.event.name:<16} {tab.browser_name}")
    print(f"    {tab.title}")
    print(f"    {tab.url or '<unresolved>'}  (confidence {tab.confidence:.2f}, {tab.scan_ms} ms)")


@monitor.on(EventKind.WINDOW_CLOSED)
def on_closed(tab: browsermem.Tab) -> None:
    print(f"[{tab.timestamp:%H:%M:%S}] window closed, last tab: {tab.url}")


print("Watching browser tabs... (Ctrl+C to stop)")
monitor.run_forever()
