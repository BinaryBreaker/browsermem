"""Public data types."""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime
from enum import IntEnum
from typing import Any, Iterable, NamedTuple, Optional, Tuple, Union
from urllib.parse import urlsplit

from ._native import bm_tab


class Browser(IntEnum):
    """Supported browsers (values match ``BM_BROWSER_*``)."""

    UNKNOWN = 0
    CHROME = 1
    EDGE = 2
    BRAVE = 3
    OPERA = 4
    VIVALDI = 5
    YANDEX = 6
    FIREFOX = 7  #: experimental

    @classmethod
    def parse(cls, value: Union[str, int, Browser]) -> Browser:
        """``Browser.parse("edge")`` -> ``Browser.EDGE``."""
        if isinstance(value, str):
            return cls[value.strip().upper()]
        return cls(value)


class EventKind(IntEnum):
    """Why a :class:`Tab` was produced (values match ``BM_EVENT_*``)."""

    SNAPSHOT = 0          #: one-shot query, or a monitor's initial state
    TAB_CHANGED = 1       #: tab switch, new tab, or navigation that changed the title
    WINDOW_ACTIVATED = 2  #: user switched to a browser window
    URL_CHANGED = 3       #: periodic rescan found a new URL with the same title
    WINDOW_CLOSED = 4     #: browser window closed; fields hold the last known tab


BrowserFilter = Union[Browser, str, Iterable[Union[Browser, str]], None]


def browser_mask(browsers: BrowserFilter) -> int:
    """Convert a browser filter to the native bit mask (``None`` = all browsers)."""
    if browsers is None:
        return 0xFFFFFFFF
    if isinstance(browsers, (Browser, str, int)):
        browsers = [browsers]  # type: ignore[list-item]
    mask = 0
    for b in browsers:  # type: ignore[union-attr]
        mask |= 1 << int(Browser.parse(b))
    return mask


class UrlCount(NamedTuple):
    """A URL found in memory and how many copies of it exist."""

    url: str
    count: int


@dataclass(frozen=True)
class Tab:
    """The active tab of a browser window, as recovered from memory."""

    event: EventKind
    browser: Browser
    browser_name: str
    pid: int
    hwnd: int
    title: str
    caption: str
    url: Optional[str]
    confidence: float
    timestamp: datetime
    scan_ms: int
    candidates: Tuple[Tuple[str, float], ...] = field(default=(), repr=False)

    @property
    def resolved(self) -> bool:
        """True if a URL was found."""
        return self.url is not None

    @property
    def scheme(self) -> Optional[str]:
        """``"https"``, ``"chrome"``, ``"file"``, ... (``None`` if unresolved)."""
        return urlsplit(self.url).scheme or None if self.url else None

    @property
    def host(self) -> Optional[str]:
        """Lower-cased host name, e.g. ``"www.github.com"``."""
        return urlsplit(self.url).hostname if self.url else None

    @property
    def domain(self) -> Optional[str]:
        """Host name without a leading ``www.``, e.g. ``"github.com"``."""
        host = self.host
        return host[4:] if host and host.startswith("www.") else host

    @property
    def is_web(self) -> bool:
        """True for ``http``/``https`` pages (False for ``chrome://newtab``, files, ...)."""
        return self.scheme in ("http", "https")

    def matches_domain(self, *domains: str) -> bool:
        """True if the tab is on one of ``domains`` or a subdomain of one."""
        host = self.host
        if not host:
            return False
        return any(host == d.lower() or host.endswith("." + d.lower()) for d in domains)

    def to_dict(self) -> dict[str, Any]:
        """JSON-friendly representation."""
        return {
            "domain": self.domain,
            "event": self.event.name.lower(),
            "browser": self.browser.name.lower(),
            "browser_name": self.browser_name,
            "pid": self.pid,
            "hwnd": self.hwnd,
            "title": self.title,
            "caption": self.caption,
            "url": self.url,
            "confidence": round(self.confidence, 4),
            "timestamp": self.timestamp.isoformat(timespec="milliseconds"),
            "scan_ms": self.scan_ms,
            "candidates": [{"url": u, "score": round(s, 4)} for u, s in self.candidates],
        }


@dataclass(frozen=True)
class BrowserWindow:
    """A top-level browser window (found without reading memory)."""

    hwnd: int
    pid: int
    browser: Browser
    browser_name: str
    caption: str
    title: str
    is_foreground: bool

    def to_dict(self) -> dict[str, Any]:
        return {
            "hwnd": self.hwnd,
            "pid": self.pid,
            "browser": self.browser.name.lower(),
            "browser_name": self.browser_name,
            "caption": self.caption,
            "title": self.title,
            "is_foreground": self.is_foreground,
        }


# ---- conversions from native structs (internal) -------------------------------


def _s(raw: Optional[bytes]) -> str:
    return raw.decode("utf-8", "replace") if raw else ""


def _browser(value: int) -> Browser:
    try:
        return Browser(value)
    except ValueError:
        return Browser.UNKNOWN


def _tab_from_native(t: bm_tab) -> Tab:
    candidates = tuple(
        (_s(t.candidates[i].url), float(t.candidates[i].score)) for i in range(t.candidate_count)
    )
    return Tab(
        event=EventKind(t.event),
        browser=_browser(t.browser),
        browser_name=_s(t.browser_name),
        pid=t.pid,
        hwnd=t.hwnd,
        title=_s(t.title),
        caption=_s(t.caption),
        url=_s(t.url) or None,
        confidence=float(t.confidence),
        timestamp=datetime.fromtimestamp(t.timestamp_ms / 1000),
        scan_ms=t.scan_ms,
        candidates=candidates,
    )
