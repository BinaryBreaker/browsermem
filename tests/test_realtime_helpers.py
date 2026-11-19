"""Real-time convenience API: Tab helpers, handler filters, wait_for, TabHistory."""

from __future__ import annotations

import threading
from datetime import datetime, timedelta

import pytest

from browsermem import Browser, EventKind, Tab, TabHistory, TabMonitor


def make_tab(url, *, event=EventKind.TAB_CHANGED, hwnd=1, at=None, title="t"):
    return Tab(event, Browser.CHROME, "Google Chrome", 10, hwnd, title, f"{title} - Google Chrome",
               url, 0.9, at or datetime.now(), 1)


@pytest.mark.parametrize("url, scheme, host, domain, web", [
    ("https://www.github.com/BinaryBreaker", "https", "www.github.com", "github.com", True),
    ("http://docs.python.org/3/", "http", "docs.python.org", "docs.python.org", True),
    ("chrome://newtab/", "chrome", "newtab", "newtab", False),
    (None, None, None, None, False),
])
def test_tab_url_parts(url, scheme, host, domain, web):
    tab = make_tab(url)
    assert (tab.scheme, tab.host, tab.domain, tab.is_web) == (scheme, host, domain, web)


def test_matches_domain_includes_subdomains():
    tab = make_tab("https://gist.github.com/x")
    assert tab.matches_domain("github.com")
    assert tab.matches_domain("GIST.github.com")
    assert not tab.matches_domain("hub.com")
    assert not make_tab(None).matches_domain("github.com")


def test_on_url_and_on_domain_filter_events():
    monitor = TabMonitor()
    seen_url, seen_domain, seen_all = [], [], []
    monitor.on_url(r"/watch\?v=")(seen_url.append)
    monitor.on_domain("github.com")(seen_domain.append)
    monitor.on()(seen_all.append)

    for url in ["https://youtube.com/watch?v=abc", "https://github.com/a", "https://x.org/"]:
        monitor._dispatch(make_tab(url))

    assert [t.url for t in seen_url] == ["https://youtube.com/watch?v=abc"]
    assert [t.url for t in seen_domain] == ["https://github.com/a"]
    assert len(seen_all) == 3

    monitor.remove_handler(seen_domain.append)  # bound methods compare equal, not identical
    monitor.remove_handler(seen_url.append)
    monitor._dispatch(make_tab("https://github.com/watch?v=1"))
    assert len(seen_url) == 1 and len(seen_domain) == 1 and len(seen_all) == 4


def test_remove_filtered_handler():
    monitor = TabMonitor()
    hits = []

    def handler(tab):
        hits.append(tab)

    monitor.on_domain("github.com")(handler)
    monitor.remove_handler(handler)
    monitor._dispatch(make_tab("https://github.com/"))
    assert hits == []


def test_wait_for_returns_matching_tab():
    monitor = TabMonitor(browsers=Browser.FIREFOX)  # real monitor, no events expected
    target = make_tab("https://example.com/done")

    def feed():
        monitor._dispatch(make_tab("https://example.com/step-1"))
        monitor._dispatch(target)

    threading.Timer(0.3, feed).start()
    try:
        assert monitor.wait_for(r"/done$", timeout=10) is target
        assert monitor.wait_for(lambda t: False, timeout=0.5) is None
    finally:
        monitor.stop()


def test_history_tracks_visits_and_durations():
    t0 = datetime(2026, 1, 1, 12, 0, 0)
    history = TabHistory()
    history.record(make_tab("https://github.com/a", event=EventKind.SNAPSHOT, at=t0))
    history.record(make_tab("https://github.com/a", event=EventKind.WINDOW_ACTIVATED,
                            at=t0 + timedelta(seconds=5)))  # same page: no new visit
    history.record(make_tab("https://news.example/", at=t0 + timedelta(seconds=30)))
    history.record(make_tab(None, at=t0 + timedelta(seconds=40)))  # unresolved: ignored
    history.record(make_tab("https://github.com/b", at=t0 + timedelta(seconds=90)))
    history.record(make_tab("https://github.com/b", event=EventKind.WINDOW_CLOSED,
                            at=t0 + timedelta(seconds=100)))

    visits = history.visits
    assert [v.url for v in visits] == ["https://github.com/a", "https://news.example/",
                                       "https://github.com/b"]
    assert [v.duration.total_seconds() for v in visits] == [30, 60, 10]
    assert history.current is None
    assert history.time_by_domain() == {"news.example": timedelta(seconds=60),
                                        "github.com": timedelta(seconds=40)}
    assert history.to_list()[0]["seconds"] == 30.0
    assert len(history) == 3


def test_history_attaches_to_monitor():
    monitor = TabMonitor()
    history = TabHistory(monitor)
    monitor._dispatch(make_tab("https://example.com/"))
    assert history.current is not None
    assert history.current.url == "https://example.com/"
