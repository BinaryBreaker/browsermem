# Command line

Installing the package adds a `browsermem` command (also available as
`python -m browsermem`).

| Command | What it does |
|---|---|
| `browsermem active` | URL and title of the active tab |
| `browsermem windows` | All browser windows (`*` marks the focused one) |
| `browsermem watch` | Live tab events until Ctrl+C |
| `browsermem urls` | Every URL found in a browser's memory |
| `browsermem minidump PID FILE` | Save a full-memory dump of a process |
| `browsermem analyze FILE` | Analyse a dump offline (see [forensics](forensics.md)) |

## Common options

| Option | |
|---|---|
| `--json` | Machine-readable output. `watch` prints one JSON object per line |
| `-b, --browser NAME` | Limit to `chrome`, `edge`, `brave`, `opera`, `vivaldi`, `yandex`, `firefox` (repeatable) |
| `-v, --verbose` | Also show the runner-up URL candidates and their scores |

`watch` also accepts `--all` (include background windows), `--rescan MS` and
`--debounce MS`.

## Examples

```bash
browsermem active
browsermem watch --browser chrome --browser edge
browsermem watch --json > activity.jsonl
browsermem urls -n 20
```

Example JSON event:

```json
{"domain": "github.com", "event": "tab_changed", "browser": "chrome",
 "browser_name": "Google Chrome", "pid": 48836, "hwnd": 4395312,
 "title": "BinaryBreaker/browsermem", "caption": "BinaryBreaker/browsermem - Google Chrome",
 "url": "https://github.com/BinaryBreaker/browsermem", "confidence": 0.81,
 "timestamp": "2026-11-19T14:02:11.204", "scan_ms": 132, "candidates": [...]}
```

Exit codes: `0` success, `1` error (message on stderr), `130` interrupted.
