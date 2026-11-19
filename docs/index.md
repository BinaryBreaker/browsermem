# browsermem

**Get the real-time URL of the active browser tab from Python. No extension, no WebDriver, no setup.**

```python
import browsermem

browsermem.current_url()
# 'https://github.com/BinaryBreaker/browsermem'
```

browsermem reads the active tab's URL and title straight from the browser's memory, and
sends an event every time the user switches tabs, opens a tab or navigates. It works with
the browser that's already running: the browser doesn't need an extension, a remote
debugging port or a restart.

## What you can build

- Productivity and time-tracking apps that know which site is in focus.
- Automations that react when a particular page opens (`monitor.wait_for(...)`).
- Context-aware assistants, note takers and clipboard tools.
- Parental-control or kiosk tools on machines you administer.
- Research tools and authorised investigations, including offline
  [memory forensics](forensics.md).

## Where to go next

| | |
|---|---|
| [Getting started](getting-started.md) | Install and get your first URL |
| [Live events](events.md) | `TabMonitor`, filters, async, `wait_for` |
| [Activity tracking](activity.md) | `TabHistory` and time per domain |
| [Command line](cli.md) | `browsermem active`, `watch`, `--json` |
| [API reference](api.md) | Every class and function |
| [How it works](how-it-works.md) | The memory technique, step by step |

!!! note "Responsible use"
    browsermem reads memory of processes that belong to your own user account. Monitoring
    other people's browsing without their knowledge and consent may be illegal where you live.
