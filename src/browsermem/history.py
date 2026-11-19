"""Browsing timeline built from live tab events."""

from __future__ import annotations

import threading
from collections import defaultdict
from dataclasses import dataclass
from datetime import datetime, timedelta
from typing import Any, Dict, List, Optional

from .models import EventKind, Tab
from .monitor import TabMonitor


@dataclass
class Visit:
    """A stretch of time during which one URL was the active tab of a window."""

    tab: Tab
    started: datetime
    ended: Optional[datetime] = None

    @property
    def url(self) -> Optional[str]:
        return self.tab.url

    @property
    def duration(self) -> timedelta:
        """Time on the page so far (or in total, once the visit has ended)."""
        return (self.ended or datetime.now()) - self.started

    def to_dict(self) -> Dict[str, Any]:
        return {
            "url": self.url,
            "title": self.tab.title,
            "domain": self.tab.domain,
            "browser": self.tab.browser.name.lower(),
            "started": self.started.isoformat(timespec="seconds"),
            "ended": self.ended.isoformat(timespec="seconds") if self.ended else None,
            "seconds": round(self.duration.total_seconds(), 1),
        }


class TabHistory:
    """Records which URL was active, when, and for how long::

        monitor = TabMonitor()
        history = TabHistory(monitor)
        monitor.start()
        ...
        for domain, spent in history.time_by_domain().items():
            print(domain, spent)

    A new visit starts whenever the active URL changes (tab switch, navigation,
    switching to another browser window). Time spent outside the browser is
    counted towards the last visit until the browser reports a new event, so
    durations are an upper bound.
    """

    def __init__(self, monitor: Optional[TabMonitor] = None, *, max_visits: int = 10_000):
        self._lock = threading.Lock()
        self._visits: List[Visit] = []
        self._max = max_visits
        if monitor is not None:
            self.attach(monitor)

    def attach(self, monitor: TabMonitor) -> None:
        """Start recording events from ``monitor``."""
        monitor.add_handler(self.record)

    def record(self, tab: Tab) -> None:
        """Feed one event (called automatically when attached to a monitor)."""
        with self._lock:
            current = self._visits[-1] if self._visits else None
            if tab.event == EventKind.WINDOW_CLOSED:
                if current and current.ended is None and current.tab.hwnd == tab.hwnd:
                    current.ended = tab.timestamp
                return
            if not tab.resolved:
                return
            if (current and current.ended is None and current.url == tab.url
                    and current.tab.hwnd == tab.hwnd):
                return  # same page, e.g. returning to the window
            if current and current.ended is None:
                current.ended = tab.timestamp
            self._visits.append(Visit(tab, tab.timestamp))
            if len(self._visits) > self._max:
                del self._visits[: len(self._visits) - self._max]

    # -- queries --------------------------------------------------------------

    @property
    def visits(self) -> List[Visit]:
        """All recorded visits, oldest first (a copy)."""
        with self._lock:
            return list(self._visits)

    @property
    def current(self) -> Optional[Visit]:
        """The visit in progress, if any."""
        with self._lock:
            last = self._visits[-1] if self._visits else None
            return last if last and last.ended is None else None

    def time_by_domain(self) -> Dict[str, timedelta]:
        """Total time per domain, longest first."""
        totals: Dict[str, timedelta] = defaultdict(timedelta)
        for visit in self.visits:
            totals[visit.tab.domain or visit.tab.scheme or "unknown"] += visit.duration
        return dict(sorted(totals.items(), key=lambda kv: kv[1], reverse=True))

    def to_list(self) -> List[Dict[str, Any]]:
        """JSON-friendly list of visits."""
        return [v.to_dict() for v in self.visits]

    def clear(self) -> None:
        with self._lock:
            self._visits.clear()

    def __len__(self) -> int:
        with self._lock:
            return len(self._visits)
