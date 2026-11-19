from __future__ import annotations

import asyncio
import json
import re
import threading
import time
from datetime import datetime
from importlib.metadata import PackageNotFoundError
from importlib.metadata import version as dist_version
from pathlib import Path

import pytest

import browsermem
from browsermem import Browser, BrowserWindow, EventKind, NotFoundError, Tab, TabMonitor
from browsermem.cli import main as cli_main
from browsermem.models import browser_mask

from .conftest import write_minidump_file

HEADER = Path(__file__).parents[1] / "native" / "include" / "browsermem.h"


def test_version_matches_distribution():
    try:
        installed = dist_version("browsermem")
    except PackageNotFoundError:
        pytest.skip("running from a source tree without an installed distribution")
    assert browsermem.__version__ == browsermem.version() == installed


@pytest.mark.skipif(not HEADER.exists(), reason="source checkout only")
@pytest.mark.parametrize("enum, prefix", [(Browser, "BM_BROWSER_"), (EventKind, "BM_EVENT_")])
def test_enums_match_c_header(enum, prefix):
    defined = {
        name: int(value)
        for name, value in re.findall(rf"#define {prefix}(\w+)\s+(-?\d+)\b", HEADER.read_text())
    }
    assert defined == {m.name: m.value for m in enum}


def test_browser_mask():
    assert browser_mask(None) == 0xFFFFFFFF
    assert browser_mask(Browser.CHROME) == 1 << 1
    assert browser_mask(["chrome", "Edge"]) == (1 << 1) | (1 << 2)
    with pytest.raises(KeyError):
        browser_mask("netscape")


def test_list_windows_shape():
    windows = browsermem.list_windows()
    assert isinstance(windows, list)
    assert all(isinstance(w, BrowserWindow) for w in windows)


def test_get_tab_rejects_non_browser_window():
    with pytest.raises(NotFoundError):
        browsermem.get_tab(0)


def test_tab_to_dict_is_json_serialisable():
    tab = Tab(EventKind.TAB_CHANGED, Browser.EDGE, "Microsoft Edge", 1, 2, "t", "c",
              "https://x.example/", 0.9, datetime(2026, 1, 1), 5, (("https://x.example/", 1.0),))
    d = json.loads(json.dumps(tab.to_dict()))
    assert d["event"] == "tab_changed"
    assert d["browser"] == "edge"
    assert d["candidates"][0]["url"] == "https://x.example/"


# ---- monitor lifecycle (no browser needed) ----------------------------------------------


def test_monitor_start_stop_idempotent():
    monitor = TabMonitor(browsers=Browser.FIREFOX)
    assert not monitor.running
    monitor.start()
    monitor.start()
    assert monitor.running
    monitor.stop()
    monitor.stop()
    assert not monitor.running


def test_monitor_events_end_on_stop():
    with TabMonitor(browsers=Browser.FIREFOX) as monitor:
        threading.Timer(0.3, monitor.stop).start()
        started = time.monotonic()
        events = list(monitor.events(timeout=10))
        assert time.monotonic() - started < 5  # ended by stop(), not by the timeout
    assert all(isinstance(e, Tab) for e in events)
    assert not monitor.running


def test_monitor_async_events_end_on_stop():
    async def run():
        async with TabMonitor(browsers=Browser.FIREFOX) as monitor:
            asyncio.get_running_loop().call_later(0.3, monitor.stop)
            return [tab async for tab in monitor.aevents()]

    assert isinstance(asyncio.run(asyncio.wait_for(run(), timeout=10)), list)


def test_monitor_handler_registration():
    monitor = TabMonitor()

    @monitor.on(EventKind.TAB_CHANGED)
    def handler(tab):
        pass

    assert handler in monitor._handlers[EventKind.TAB_CHANGED]
    monitor.remove_handler(handler)
    assert handler not in monitor._handlers[EventKind.TAB_CHANGED]


# ---- CLI ---------------------------------------------------------------------------


def test_cli_windows_json(capsys):
    assert cli_main(["windows", "--json"]) == 0
    assert isinstance(json.loads(capsys.readouterr().out), list)


def test_cli_analyze(tmp_path, capsys, synthetic):
    path = tmp_path / "s.dmp"
    write_minidump_file(path, synthetic.regions)

    assert cli_main(["analyze", str(path), "--title", synthetic.title, "--json"]) == 0
    assert json.loads(capsys.readouterr().out)["url"] == synthetic.url

    assert cli_main(["analyze", str(path), "--json", "-n", "5"]) == 0
    urls = [u["url"] for u in json.loads(capsys.readouterr().out)]
    assert synthetic.url in urls


def test_cli_reports_errors(capsys):
    assert cli_main(["urls", "--pid", str(0xFFFFFFF0)]) == 1
    assert "error" in capsys.readouterr().err
