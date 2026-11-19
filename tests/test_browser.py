"""End-to-end tests against a real Chromium browser (opt-in).

    BROWSERMEM_TEST_BROWSER=1 pytest -m browser

Launches Chrome or Edge with a throwaway profile, opens local pages, and checks
that browsermem recovers their URLs. Needs an interactive desktop session.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import time
from pathlib import Path
from typing import Callable, Iterator, Optional

import pytest

import browsermem
from browsermem import Browser, EventKind, TabMonitor

pytestmark = pytest.mark.browser

CANDIDATES = [
    (Browser.CHROME, r"%ProgramFiles%\Google\Chrome\Application\chrome.exe"),
    (Browser.CHROME, r"%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe"),
    (Browser.EDGE, r"%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe"),
    (Browser.EDGE, r"%ProgramFiles%\Microsoft\Edge\Application\msedge.exe"),
]


def _find_browser():
    for browser, path in CANDIDATES:
        exe = os.path.expandvars(path)
        if os.path.isfile(exe):
            return browser, exe
    pytest.skip("no Chrome or Edge installation found")


def _page(directory: Path, name: str, title: str) -> str:
    path = directory / f"{name}.html"
    path.write_text(f"<!doctype html><title>{title}</title><h1>{title}</h1>", encoding="utf-8")
    return path.as_uri()


def _wait_for(predicate: Callable[[], Optional[object]], timeout: float = 20) -> object:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.25)
    raise AssertionError("timed out")


@pytest.fixture
def browser(tmp_path) -> Iterator:
    kind, exe = _find_browser()
    profile = tmp_path / "profile"
    base = [exe, f"--user-data-dir={profile}", "--no-first-run", "--no-default-browser-check",
            "--disable-sync", "--new-window"]
    procs = []

    def open_url(url: str) -> None:
        procs.append(subprocess.Popen([*base, url]))

    try:
        yield kind, open_url
    finally:
        for p in procs:
            subprocess.run(["taskkill", "/PID", str(p.pid), "/T", "/F"], capture_output=True)
        time.sleep(1)
        shutil.rmtree(profile, ignore_errors=True)


def test_get_tab_resolves_local_page(tmp_path, browser):
    kind, open_url = browser
    title = "browsermem e2e page one"
    url = _page(tmp_path, "one", title)
    open_url(url)

    window = _wait_for(lambda: next(
        (w for w in browsermem.list_windows(kind) if w.title == title), None))
    tab = browsermem.get_tab(window)
    assert tab.url == url
    assert tab.browser == kind


def test_monitor_reports_tab_change(tmp_path, browser):
    kind, open_url = browser
    first = _page(tmp_path, "first", "browsermem e2e first")
    second = _page(tmp_path, "second", "browsermem e2e second")
    open_url(first)
    _wait_for(lambda: any(w.title == "browsermem e2e first"
                          for w in browsermem.list_windows(kind)))

    with TabMonitor(browsers=kind, all_windows=True) as monitor:
        open_url(second)  # opens a new tab in the running instance
        for tab in monitor.events(timeout=30):
            if tab.event in (EventKind.TAB_CHANGED, EventKind.WINDOW_ACTIVATED,
                             EventKind.SNAPSHOT) and tab.url == second:
                break
        else:
            pytest.fail("no event for the second page")
