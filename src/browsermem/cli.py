"""``browsermem`` command-line interface."""

from __future__ import annotations

import argparse
import contextlib
import json
import sys
from typing import List, Optional, Sequence

from . import __version__
from .api import dump_urls, get_active_tab, list_windows
from .errors import BrowserMemError
from .forensics import MemoryImage, write_minidump
from .models import Browser, EventKind, Tab, UrlCount
from .monitor import TabMonitor

_EVENT_LABEL = {
    EventKind.SNAPSHOT: "snapshot",
    EventKind.TAB_CHANGED: "tab-changed",
    EventKind.WINDOW_ACTIVATED: "activated",
    EventKind.URL_CHANGED: "url-changed",
    EventKind.WINDOW_CLOSED: "closed",
}


def _print_json(obj: object) -> None:
    print(json.dumps(obj, ensure_ascii=False), flush=True)


def _print_tab(tab: Tab, as_json: bool, verbose: bool) -> None:
    if as_json:
        _print_json(tab.to_dict())
        return
    print(f"[{tab.timestamp:%H:%M:%S}] {_EVENT_LABEL[tab.event]:<11} "
          f"{tab.browser_name:<15} {tab.title}")
    print(f"           {tab.url or '<unresolved>'}  "
          f"(confidence {tab.confidence:.2f}, {tab.scan_ms} ms)", flush=True)
    if verbose:
        for url, score in tab.candidates:
            print(f"             {score:6.2f}  {url}")


def _print_urls(urls: List[UrlCount], limit: Optional[int], as_json: bool) -> None:
    urls = urls[:limit] if limit else urls
    if as_json:
        _print_json([u._asdict() for u in urls])
    else:
        for u in urls:
            print(f"{u.count:7}  {u.url}")


def _browsers(values: Optional[List[str]]) -> Optional[List[Browser]]:
    return [Browser.parse(v) for v in values] if values else None


def _cmd_active(a: argparse.Namespace) -> int:
    tab = get_active_tab(_browsers(a.browser))
    if tab is None:
        print("No browser window found.", file=sys.stderr)
        return 1
    _print_tab(tab, a.json, a.verbose)
    return 0


def _cmd_windows(a: argparse.Namespace) -> int:
    windows = list_windows(_browsers(a.browser))
    if a.json:
        _print_json([w.to_dict() for w in windows])
    else:
        for w in windows:
            mark = "*" if w.is_foreground else " "
            print(f"{mark} {w.browser_name:<15} pid={w.pid:<6} hwnd=0x{w.hwnd:x}  {w.title}")
    return 0


def _cmd_watch(a: argparse.Namespace) -> int:
    monitor = TabMonitor(
        lambda tab: _print_tab(tab, a.json, a.verbose),
        browsers=_browsers(a.browser),
        all_windows=a.all,
        rescan_ms=a.rescan,
        debounce_ms=a.debounce,
    )
    if not a.json:
        print("Watching browser tabs... (Ctrl+C to stop)", flush=True)
    monitor.run_forever()
    return 0


def _cmd_urls(a: argparse.Namespace) -> int:
    _print_urls(dump_urls(a.pid), a.limit, a.json)
    return 0


def _cmd_minidump(a: argparse.Namespace) -> int:
    write_minidump(a.pid, a.output)
    print(f"Wrote {a.output}", file=sys.stderr)
    return 0


def _cmd_analyze(a: argparse.Namespace) -> int:
    with MemoryImage.from_minidump(a.dump, private_only=not a.all_memory) as image:
        if not a.json:
            print(f"Loaded {image!r}", file=sys.stderr)
        if a.title:
            browser = a.browser[0] if a.browser else None
            _print_tab(image.resolve(a.title, browser), a.json, a.verbose)
        else:
            _print_urls(image.urls(), a.limit, a.json)
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="browsermem",
        description="Recover browser tab URLs and titles from browser process memory.",
    )
    p.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    sub = p.add_subparsers(dest="command", required=True, metavar="COMMAND")

    def common(sp: argparse.ArgumentParser, browser: bool = True) -> None:
        sp.add_argument("--json", action="store_true", help="machine-readable JSON output")
        sp.add_argument("-v", "--verbose", action="store_true", help="show URL candidates")
        if browser:
            sp.add_argument("-b", "--browser", action="append", metavar="NAME",
                            help="limit to a browser (chrome, edge, brave, ...); repeatable")

    sp = sub.add_parser("active", help="URL of the active tab")
    common(sp)
    sp.set_defaults(func=_cmd_active)

    sp = sub.add_parser("windows", help="list browser windows")
    common(sp)
    sp.set_defaults(func=_cmd_windows)

    sp = sub.add_parser("watch", help="stream tab-change events")
    common(sp)
    sp.add_argument("--all", action="store_true", help="include background windows")
    sp.add_argument("--rescan", type=int, default=0, metavar="MS",
                    help="rescan the focused tab every MS milliseconds")
    sp.add_argument("--debounce", type=int, default=600, metavar="MS")
    sp.set_defaults(func=_cmd_watch)

    sp = sub.add_parser("urls", help="every URL in a live process's memory")
    common(sp, browser=False)
    sp.add_argument("--pid", type=int, default=0, help="process id (default: active browser)")
    sp.add_argument("-n", "--limit", type=int, help="show at most N URLs")
    sp.set_defaults(func=_cmd_urls)

    sp = sub.add_parser("minidump", help="write a full-memory dump of a process")
    sp.add_argument("pid", type=int)
    sp.add_argument("output")
    sp.set_defaults(func=_cmd_minidump)

    sp = sub.add_parser("analyze", help="analyse a minidump offline")
    common(sp)
    sp.add_argument("dump", help="path to a .dmp file")
    sp.add_argument("-t", "--title", help="resolve the URL for this tab title / caption")
    sp.add_argument("-n", "--limit", type=int, help="show at most N URLs")
    sp.add_argument("--all-memory", action="store_true",
                    help="scan every region, not just private writable memory")
    sp.set_defaults(func=_cmd_analyze)
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        with contextlib.suppress(AttributeError, ValueError):  # e.g. replaced streams
            stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (BrowserMemError, OSError, KeyError) as e:
        print(f"browsermem: error: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
