# Activity tracking

`TabHistory` turns live events into a timeline of visits: which URL was active, when, and
for how long.

```python
import time
from browsermem import TabMonitor, TabHistory

monitor = TabMonitor()
history = TabHistory(monitor)      # attaches itself as a handler
monitor.start()

time.sleep(600)                    # browse for a while

for domain, spent in history.time_by_domain().items():
    print(f"{domain:30} {spent}")

for visit in history.visits:
    print(visit.started, visit.duration, visit.url)

monitor.stop()
```

## Rules

- A new **visit** starts when the active URL changes (a tab switch, a navigation, or a
  switch to another browser window) and ends when the next one starts.
- Returning to the same page in the same window continues the current visit.
- Events whose URL couldn't be resolved are skipped.
- `WINDOW_CLOSED` ends the visit of that window.

!!! note
    Windows only reports events *in* the browser. Time spent in another application is
    counted towards the last browser visit until the browser sends a new event, so
    durations are an upper bound. If you need exact focus time, combine this with your
    own foreground-window tracking.

## Export

```python
import json
json.dump(history.to_list(), open("activity.json", "w"), indent=2)
```

Each entry contains `url`, `title`, `domain`, `browser`, `started`, `ended` and `seconds`.

## Without a monitor

`TabHistory.record(tab)` accepts any `Tab`, so you can feed it events from your own
pipeline or replay a saved log.
