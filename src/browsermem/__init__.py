"""browsermem -- real-time browser tab URLs and titles on Windows.

Get the URL of the active tab::

    >>> import browsermem
    >>> browsermem.current_url()
    'https://github.com/BinaryBreaker/browsermem'

React to tab changes as they happen::

    >>> monitor = browsermem.TabMonitor()
    >>> @monitor.on_domain("youtube.com")
    ... def watching(tab):
    ...     print("Now watching:", tab.title)
    >>> monitor.run_forever()

See https://github.com/BinaryBreaker/browsermem for the full documentation.
"""

from ._native import lib as _lib

__version__: str = _lib.bm_version().decode()

from .api import (  # noqa: E402
    current_url,
    dump_urls,
    get_active_tab,
    get_all_tabs,
    get_tab,
    list_windows,
    version,
)
from .errors import (  # noqa: E402
    AccessDeniedError,
    BrowserMemError,
    NotFoundError,
    StateError,
)
from .forensics import MemoryImage  # noqa: E402
from .history import TabHistory, Visit  # noqa: E402
from .models import Browser, BrowserWindow, EventKind, Tab, UrlCount  # noqa: E402
from .monitor import TabMonitor  # noqa: E402

__all__ = [
    "AccessDeniedError",
    "Browser",
    "BrowserMemError",
    "BrowserWindow",
    "EventKind",
    "MemoryImage",
    "NotFoundError",
    "StateError",
    "Tab",
    "TabHistory",
    "TabMonitor",
    "UrlCount",
    "Visit",
    "__version__",
    "current_url",
    "dump_urls",
    "get_active_tab",
    "get_all_tabs",
    "get_tab",
    "list_windows",
    "version",
]
