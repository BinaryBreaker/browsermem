"""Exception hierarchy."""

from __future__ import annotations

from . import _native


class BrowserMemError(Exception):
    """Base class for all browsermem errors. ``status`` is the native status code."""

    def __init__(self, message: str, status: int = _native.ERR_INTERNAL):
        super().__init__(message)
        self.status = status


class NotFoundError(BrowserMemError):
    """No matching browser window or process."""


class AccessDeniedError(BrowserMemError):
    """The target process could not be opened or read (e.g. it runs elevated)."""


class StateError(BrowserMemError):
    """The call is not allowed in the current state."""


_BY_STATUS = {
    _native.ERR_INVALID_ARG: ValueError,
    _native.ERR_NOT_FOUND: NotFoundError,
    _native.ERR_ACCESS: AccessDeniedError,
    _native.ERR_STATE: StateError,
}


def check(status: int) -> None:
    """Raise the matching exception for a non-zero native status."""
    if status == _native.OK:
        return
    message = _native.last_error() or f"native call failed with status {status}"
    cls = _BY_STATUS.get(status, BrowserMemError)
    if cls is ValueError:
        raise ValueError(message)
    raise cls(message, status)
