"""One-shot queries against running browsers."""

from __future__ import annotations

from ctypes import POINTER, byref, c_uint32
from typing import List, Optional, Union

from . import _native
from ._native import bm_tab, bm_url_entry, bm_window, lib  # noqa: F401
from .errors import NotFoundError, check
from .models import (
    BrowserFilter,
    BrowserWindow,
    Tab,
    UrlCount,
    _browser,
    _s,
    _tab_from_native,
    browser_mask,
)


def version() -> str:
    """Version of the native library."""
    return _s(lib.bm_version())


def list_windows(browsers: BrowserFilter = None) -> List[BrowserWindow]:
    """All browser windows, most recently active first. Does not read memory."""
    arr = POINTER(bm_window)()
    count = c_uint32()
    check(lib.bm_list_windows(browser_mask(browsers), byref(arr), byref(count)))
    try:
        return [
            BrowserWindow(
                hwnd=w.hwnd,
                pid=w.pid,
                browser=_browser(w.browser),
                browser_name=_s(w.browser_name),
                caption=_s(w.caption),
                title=_s(w.title),
                is_foreground=bool(w.is_foreground),
            )
            for w in (arr[i] for i in range(count.value))
        ]
    finally:
        if arr:
            lib.bm_windows_free(arr)


def get_active_tab(browsers: BrowserFilter = None) -> Optional[Tab]:
    """Active tab of the focused (or most recently focused) browser window.

    Returns ``None`` when no matching browser window is open.
    """
    ptr = POINTER(bm_tab)()
    status = lib.bm_get_active_tab(browser_mask(browsers), byref(ptr))
    if status == _native.ERR_NOT_FOUND:
        return None
    check(status)
    try:
        return _tab_from_native(ptr.contents)
    finally:
        lib.bm_tab_free(ptr)


def current_url(browsers: BrowserFilter = None) -> Optional[str]:
    """URL of the active tab, or ``None`` if no browser is open or it can't be resolved.

    Shortcut for ``get_active_tab().url``.
    """
    tab = get_active_tab(browsers)
    return tab.url if tab else None


def get_tab(window: Union[BrowserWindow, int]) -> Tab:
    """Active tab of a specific window (a :class:`BrowserWindow` or an HWND).

    Raises :class:`NotFoundError` if the handle is not a supported browser window.
    """
    hwnd = window.hwnd if isinstance(window, BrowserWindow) else int(window)
    ptr = POINTER(bm_tab)()
    check(lib.bm_get_tab(hwnd, byref(ptr)))
    try:
        return _tab_from_native(ptr.contents)
    finally:
        lib.bm_tab_free(ptr)


def get_all_tabs(browsers: BrowserFilter = None) -> List[Tab]:
    """The active tab of every open browser window."""
    tabs = []
    for window in list_windows(browsers):
        try:
            tabs.append(get_tab(window))
        except NotFoundError:  # window closed meanwhile
            continue
    return tabs


def dump_urls(pid: int = 0) -> List[UrlCount]:
    """Every URL string in a process's memory, most frequent first.

    ``pid=0`` picks the most recently active browser.
    """
    arr = POINTER(bm_url_entry)()
    count = c_uint32()
    check(lib.bm_dump_urls(pid, byref(arr), byref(count)))
    return _url_list(arr, count.value)


def _url_list(arr, count: int) -> List[UrlCount]:
    try:
        return [UrlCount(_s(arr[i].url), arr[i].count) for i in range(count)]
    finally:
        if arr:
            lib.bm_urls_free(arr)
