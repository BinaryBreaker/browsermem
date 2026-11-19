"""Live, event-driven tab monitoring."""

from __future__ import annotations

import asyncio
import contextlib
import logging
import queue
import re
import threading
import time
from collections import defaultdict
from ctypes import byref, c_void_p, sizeof
from typing import (
    AsyncIterator,
    Callable,
    Dict,
    Iterator,
    List,
    Optional,
    Pattern,
    Set,
    Tuple,
    Union,
)

from ._native import TAB_CALLBACK, bm_monitor_options, lib
from .errors import check
from .models import BrowserFilter, EventKind, Tab, _tab_from_native, browser_mask

log = logging.getLogger("browsermem")

Handler = Callable[[Tab], None]
_STOP = object()


class _Filtered:
    """A handler that only fires when ``accept(tab)`` is true."""

    def __init__(self, wrapped: Handler, accept: Callable[[Tab], bool]) -> None:
        self.wrapped = wrapped
        self.accept = accept

    def __call__(self, tab: Tab) -> None:
        if self.accept(tab):
            self.wrapped(tab)


class TabMonitor:
    """Watches browser windows and reports tab changes together with their URL.

    Consume events in whichever style fits your program (they can be combined)::

        # callback
        TabMonitor(lambda tab: print(tab.url)).run_forever()

        # decorators, per event kind
        monitor = TabMonitor()

        @monitor.on(EventKind.TAB_CHANGED)
        def changed(tab): ...

        # blocking iterator
        with TabMonitor() as monitor:
            for tab in monitor.events():
                ...

        # asyncio
        async with TabMonitor() as monitor:
            async for tab in monitor.aevents():
                ...

    Callbacks run on a native worker thread: keep them short, or hand work off
    to your own thread / event loop.
    """

    def __init__(
        self,
        callback: Optional[Handler] = None,
        *,
        browsers: BrowserFilter = None,
        all_windows: bool = False,
        debounce_ms: int = 600,
        rescan_ms: int = 0,
        cache_ttl_ms: int = 30_000,
        emit_unresolved: bool = True,
        queue_size: int = 1000,
    ) -> None:
        """
        Args:
            callback: Called for every event.
            browsers: Only watch these browsers (default: all supported).
            all_windows: Also report changes in background browser windows.
            debounce_ms: How long a window caption must be stable before it is reported.
            rescan_ms: Periodically rescan the focused tab to catch navigations
                that keep the same title (0 = off; each rescan costs ~0.1-0.6 s CPU).
            cache_ttl_ms: Reuse a result while a window's caption is unchanged.
            emit_unresolved: Deliver events even when no URL could be resolved.
            queue_size: Capacity of the buffer behind :meth:`events`.
        """
        self._options = bm_monitor_options()
        lib.bm_monitor_options_init(byref(self._options))
        self._options.struct_size = sizeof(bm_monitor_options)
        self._options.browser_mask = browser_mask(browsers)
        self._options.all_windows = int(all_windows)
        self._options.debounce_ms = debounce_ms
        self._options.rescan_ms = rescan_ms
        self._options.cache_ttl_ms = cache_ttl_ms
        self._options.emit_unresolved = int(emit_unresolved)

        self._handlers: Dict[Optional[EventKind], List[Handler]] = defaultdict(list)
        if callback:
            self._handlers[None].append(callback)
        self._queue: queue.Queue = queue.Queue(maxsize=queue_size)
        self._async_queues: Set[Tuple[asyncio.AbstractEventLoop, asyncio.Queue]] = set()
        self._handle = c_void_p()
        self._native_cb = TAB_CALLBACK(self._on_native)  # must outlive the native monitor
        self._callback_thread: Optional[int] = None
        self._lock = threading.RLock()

    # -- handlers ------------------------------------------------------------

    def on(self, *kinds: EventKind) -> Callable[[Handler], Handler]:
        """Decorator registering a handler for the given event kinds (all if none)."""

        def register(fn: Handler) -> Handler:
            self.add_handler(fn, *kinds)
            return fn

        return register

    def add_handler(self, fn: Handler, *kinds: EventKind) -> None:
        with self._lock:
            for kind in kinds or (None,):
                self._handlers[kind].append(fn)

    def on_url(self, pattern: Union[str, Pattern[str]],
               *kinds: EventKind) -> Callable[[Handler], Handler]:
        """Decorator: call the handler only when the tab URL matches a regex (``re.search``)::

            @monitor.on_url(r"youtube\\.com/watch")
            def video(tab): ...
        """
        regex = re.compile(pattern) if isinstance(pattern, str) else pattern

        def register(fn: Handler) -> Handler:
            self.add_handler(_Filtered(fn, lambda t: bool(t.url and regex.search(t.url))), *kinds)
            return fn

        return register

    def on_domain(self, *domains: str) -> Callable[[Handler], Handler]:
        """Decorator: call the handler only for tabs on these domains (and subdomains)::

            @monitor.on_domain("github.com", "gitlab.com")
            def code_hosting(tab): ...
        """

        def register(fn: Handler) -> Handler:
            self.add_handler(_Filtered(fn, lambda t: t.matches_domain(*domains)))
            return fn

        return register

    def wait_for(
        self,
        match: Union[str, Pattern[str], Callable[[Tab], bool], None] = None,
        timeout: Optional[float] = None,
    ) -> Optional[Tab]:
        """Block until a tab event matches, then return it (``None`` on timeout).

        ``match`` is a URL regex, a predicate, or ``None`` for the next event with a
        URL. Starts the monitor if needed::

            tab = monitor.wait_for(r"/checkout/complete", timeout=300)
        """
        if match is None:
            predicate: Callable[[Tab], bool] = lambda t: t.resolved  # noqa: E731
        elif callable(match):
            predicate = match
        else:
            regex = re.compile(match) if isinstance(match, str) else match
            predicate = lambda t: bool(t.url and regex.search(t.url))  # noqa: E731

        found: List[Tab] = []
        done = threading.Event()

        def handler(tab: Tab) -> None:
            if not done.is_set() and tab.event != EventKind.WINDOW_CLOSED and predicate(tab):
                found.append(tab)
                done.set()

        self.add_handler(handler)
        try:
            self.start()
            deadline = None if timeout is None else time.monotonic() + timeout
            while not done.wait(0.25):  # short waits keep Ctrl+C responsive
                if deadline is not None and time.monotonic() >= deadline:
                    return None
            return found[0]
        finally:
            self.remove_handler(handler)

    def remove_handler(self, fn: Handler) -> None:
        """Unregister a handler (including ones added with on_url / on_domain)."""
        with self._lock:
            for handlers in self._handlers.values():
                handlers[:] = [h for h in handlers
                               if h != fn and getattr(h, "wrapped", None) != fn]

    # -- lifecycle -----------------------------------------------------------

    @property
    def running(self) -> bool:
        return bool(self._handle.value)

    def start(self) -> TabMonitor:
        """Start monitoring (idempotent). The current tab is reported as a SNAPSHOT event."""
        with self._lock:
            if not self.running:
                check(lib.bm_monitor_start(byref(self._options), self._native_cb, None,
                                           byref(self._handle)))
        return self

    def stop(self) -> None:
        """Stop monitoring (idempotent). Safe to call from inside a handler."""
        if threading.get_ident() == self._callback_thread:
            # The native worker cannot join itself: stop from another thread.
            threading.Thread(target=self.stop, name="browsermem-stop", daemon=True).start()
            return
        with self._lock:
            if not self.running:
                return
            handle, self._handle = self._handle, c_void_p()
            check(lib.bm_monitor_stop(handle))
            async_queues = list(self._async_queues)
        self._put(_STOP)
        for loop, q in async_queues:
            loop.call_soon_threadsafe(q.put_nowait, _STOP)

    def __enter__(self) -> TabMonitor:
        return self.start()

    def __exit__(self, *exc: object) -> None:
        self.stop()

    async def __aenter__(self) -> TabMonitor:
        return self.start()

    async def __aexit__(self, *exc: object) -> None:
        await asyncio.get_running_loop().run_in_executor(None, self.stop)

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.stop()

    # -- consumption ---------------------------------------------------------

    def events(self, timeout: Optional[float] = None) -> Iterator[Tab]:
        """Yield events until :meth:`stop` is called, or until ``timeout`` seconds
        pass without an event. Ctrl+C works while waiting."""
        while True:
            waited = 0.0
            while True:
                try:
                    item = self._queue.get(timeout=0.25)
                    break
                except queue.Empty:
                    waited += 0.25
                    if timeout is not None and waited >= timeout:
                        return
            if item is _STOP:
                return
            yield item

    async def aevents(self) -> AsyncIterator[Tab]:
        """Async iterator over events, delivered on the running event loop."""
        loop = asyncio.get_running_loop()
        q: asyncio.Queue = asyncio.Queue()
        entry = (loop, q)
        with self._lock:
            self._async_queues.add(entry)
        try:
            while True:
                item = await q.get()
                if item is _STOP:
                    return
                yield item
        finally:
            with self._lock:
                self._async_queues.discard(entry)

    def run_forever(self) -> None:
        """Start, dispatch events to handlers until Ctrl+C, then stop."""
        self.start()
        try:
            for _ in self.events():
                pass
        except KeyboardInterrupt:
            pass
        finally:
            self.stop()

    # -- native bridge -------------------------------------------------------

    def _put(self, item: object) -> None:
        try:
            self._queue.put_nowait(item)
        except queue.Full:  # drop the oldest event rather than block the native thread
            with contextlib.suppress(queue.Empty):
                self._queue.get_nowait()
            self._queue.put_nowait(item)

    def _on_native(self, ptr, _user) -> None:  # runs on the native worker thread
        self._callback_thread = threading.get_ident()
        try:
            tab = _tab_from_native(ptr.contents)
        except Exception:
            log.exception("browsermem: failed to decode a native event")
            return
        self._dispatch(tab)

    def _dispatch(self, tab: Tab) -> None:
        self._put(tab)
        with self._lock:
            handlers = [*self._handlers[None], *self._handlers[tab.event]]
            async_queues = list(self._async_queues)
        for loop, q in async_queues:
            with contextlib.suppress(RuntimeError):  # the event loop was closed
                loop.call_soon_threadsafe(q.put_nowait, tab)
        for handler in handlers:
            try:
                handler(tab)
            except Exception:
                log.exception("browsermem: handler %r raised", handler)
