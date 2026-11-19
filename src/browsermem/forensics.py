"""Offline / volatile-memory forensics.

A :class:`MemoryImage` is a set of ``(base_address, bytes)`` regions. Build one
from a live process, a Windows minidump, or any acquisition you can turn into
regions, then list the URLs in it or resolve the URL that belongs to a tab
title -- no running browser needed::

    from browsermem.forensics import MemoryImage, write_minidump

    write_minidump(pid, "chrome.dmp")              # acquire
    with MemoryImage.from_minidump("chrome.dmp") as image:
        image.urls()[:20]                          # most frequent URLs
        image.resolve("Inbox - Gmail").url         # URL for a known tab title
"""

from __future__ import annotations

import contextlib
import ctypes
import mmap
import os
import struct
from ctypes import POINTER, byref, c_uint32, c_uint64, c_void_p, wintypes
from typing import Iterable, Iterator, List, Optional, Tuple, Union

from ._native import bm_tab, bm_url_entry, lib
from .api import _url_list
from .errors import AccessDeniedError, BrowserMemError, check
from .models import Browser, Tab, UrlCount, _tab_from_native

PathLike = Union[str, "os.PathLike[str]"]

__all__ = ["MemoryImage", "read_minidump_regions", "write_minidump"]


class MemoryImage:
    """A captured or reconstructed process memory image."""

    def __init__(self) -> None:
        """Create an empty image; add memory with :meth:`add_region`."""
        self._handle = c_void_p()
        check(lib.bm_image_create(byref(self._handle)))

    # -- construction -----------------------------------------------------------

    @classmethod
    def capture(cls, pid: int = 0) -> MemoryImage:
        """Copy the private read/write memory of a running process.

        ``pid=0`` picks the most recently active browser.
        """
        image = cls.__new__(cls)
        image._handle = c_void_p()
        check(lib.bm_image_capture(pid, byref(image._handle)))
        return image

    @classmethod
    def from_regions(cls, regions: Iterable[Tuple[int, bytes]]) -> MemoryImage:
        """Build an image from ``(base_address, data)`` pairs."""
        image = cls()
        for base, data in regions:
            image.add_region(base, data)
        return image

    @classmethod
    def from_minidump(cls, path: PathLike, *, private_only: bool = True) -> MemoryImage:
        """Load a Windows minidump (.dmp), e.g. from Task Manager → *Create dump file*,
        ProcDump, or :func:`write_minidump`. Needs a full-memory dump.

        With ``private_only`` (default) only private, writable memory is loaded when
        the dump records region types -- the same memory a live capture reads.
        """
        return cls.from_regions(read_minidump_regions(path, private_only=private_only))

    def add_region(self, base: int, data: Union[bytes, bytearray, memoryview]) -> None:
        """Add one memory region (copied). Regions must not overlap."""
        self._require_open()
        buf = (ctypes.c_char * len(data)).from_buffer_copy(data) if len(data) else None
        check(lib.bm_image_add_region(self._handle, base, buf, len(data)))

    # -- analysis ---------------------------------------------------------------

    @property
    def total_bytes(self) -> int:
        total, _ = self._info()
        return total

    @property
    def region_count(self) -> int:
        _, regions = self._info()
        return regions

    def urls(self) -> List[UrlCount]:
        """Every URL string in the image, most frequent first."""
        self._require_open()
        arr = POINTER(bm_url_entry)()
        count = c_uint32()
        check(lib.bm_image_urls(self._handle, byref(arr), byref(count)))
        return _url_list(arr, count.value)

    def resolve(self, title: str, browser: Optional[Union[Browser, str]] = None) -> Tab:
        """URL of the tab whose title is ``title``.

        If ``browser`` is given, ``title`` may also be a full window caption
        (``"Inbox - Gmail - Google Chrome"``). Check ``tab.resolved``.
        """
        self._require_open()
        b = int(Browser.parse(browser)) if browser is not None else 0
        ptr = POINTER(bm_tab)()
        check(lib.bm_image_resolve(self._handle, title.encode("utf-8"), b, byref(ptr)))
        try:
            return _tab_from_native(ptr.contents)
        finally:
            lib.bm_tab_free(ptr)

    # -- lifecycle --------------------------------------------------------------

    def close(self) -> None:
        if self._handle:
            lib.bm_image_free(self._handle)
            self._handle = c_void_p()

    def __enter__(self) -> MemoryImage:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def __repr__(self) -> str:
        if not self._handle:
            return "<MemoryImage closed>"
        total, regions = self._info()
        return f"<MemoryImage {regions} regions, {total / 1048576:.1f} MiB>"

    def _info(self) -> Tuple[int, int]:
        self._require_open()
        total, regions = c_uint64(), c_uint32()
        check(lib.bm_image_info(self._handle, byref(total), byref(regions)))
        return total.value, regions.value

    def _require_open(self) -> None:
        if not self._handle:
            raise BrowserMemError("MemoryImage is closed")


# ---- minidump reading -------------------------------------------------------------

_MEMORY_LIST = 5
_MEMORY64_LIST = 9
_MEMORY_INFO_LIST = 16
_MEM_COMMIT = 0x1000
_MEM_PRIVATE = 0x20000
_WRITABLE = 0x04 | 0x08 | 0x40 | 0x80  # RW, WRITECOPY, EXECUTE_RW, EXECUTE_WRITECOPY
_GUARD_OR_NOACCESS = 0x100 | 0x01


def read_minidump_regions(path: PathLike, *, private_only: bool = True) -> Iterator[
    Tuple[int, bytes]
]:
    """Yield ``(base_address, data)`` for the memory ranges stored in a minidump."""
    with open(path, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        if m[:4] != b"MDMP":
            raise BrowserMemError(f"{os.fspath(path)!r} is not a minidump")
        n_streams, dir_rva = struct.unpack_from("<II", m, 8)
        streams = {}
        for i in range(n_streams):
            stype, size, rva = struct.unpack_from("<III", m, dir_rva + 12 * i)
            streams[stype] = (rva, size)

        allowed = _private_writable_ranges(m, streams) if private_only else None

        def keep(base: int, size: int) -> bool:
            if allowed is None:
                return True
            return any(lo <= base < hi for lo, hi in allowed)

        if _MEMORY64_LIST in streams:
            rva, _ = streams[_MEMORY64_LIST]
            count, data_rva = struct.unpack_from("<QQ", m, rva)
            offset = data_rva
            for i in range(count):
                base, size = struct.unpack_from("<QQ", m, rva + 16 + 16 * i)
                if keep(base, size):
                    yield base, m[offset:offset + size]
                offset += size
        elif _MEMORY_LIST in streams:
            rva, _ = streams[_MEMORY_LIST]
            (count,) = struct.unpack_from("<I", m, rva)
            for i in range(count):
                base, size, data_rva = struct.unpack_from("<QII", m, rva + 4 + 16 * i)
                if keep(base, size):
                    yield base, m[data_rva:data_rva + size]
        else:
            raise BrowserMemError("minidump contains no memory (create a full-memory dump)")


def _private_writable_ranges(m: mmap.mmap, streams: dict) -> Optional[List[Tuple[int, int]]]:
    if _MEMORY_INFO_LIST not in streams:
        return None  # region types unknown: keep everything
    rva, _ = streams[_MEMORY_INFO_LIST]
    header_size, entry_size, count = struct.unpack_from("<IIQ", m, rva)
    ranges = []
    for i in range(count):
        base, _alloc, _aprot, _pad, size, state, protect, mtype = struct.unpack_from(
            "<QQIIQIII", m, rva + header_size + entry_size * i
        )
        if (state == _MEM_COMMIT and mtype == _MEM_PRIVATE and protect & _WRITABLE
                and not protect & _GUARD_OR_NOACCESS):
            ranges.append((base, base + size))
    ranges.sort()
    return ranges


# ---- minidump writing -------------------------------------------------------------

_PROCESS_QUERY_INFORMATION = 0x0400
_PROCESS_VM_READ = 0x0010
_MiniDumpWithFullMemory = 0x00000002
_MiniDumpWithFullMemoryInfo = 0x00000800


def write_minidump(pid: int, path: PathLike) -> None:
    """Write a full-memory minidump of a running process (like Task Manager's
    *Create dump file*), for later analysis with :meth:`MemoryImage.from_minidump`."""
    import msvcrt

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    dbghelp.MiniDumpWriteDump.restype = wintypes.BOOL
    dbghelp.MiniDumpWriteDump.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.HANDLE,
                                          wintypes.DWORD, c_void_p, c_void_p, c_void_p]

    process = kernel32.OpenProcess(_PROCESS_QUERY_INFORMATION | _PROCESS_VM_READ, False, pid)
    if not process:
        raise AccessDeniedError(f"OpenProcess({pid}) failed, Win32 error {ctypes.get_last_error()}")
    try:
        with open(path, "wb") as f:
            ok = dbghelp.MiniDumpWriteDump(
                process, pid, msvcrt.get_osfhandle(f.fileno()),
                _MiniDumpWithFullMemory | _MiniDumpWithFullMemoryInfo, None, None, None,
            )
            if not ok:
                raise AccessDeniedError(
                    f"MiniDumpWriteDump failed, Win32 error {ctypes.get_last_error()}"
                )
    except BaseException:
        with contextlib.suppress(OSError):
            os.remove(path)
        raise
    finally:
        kernel32.CloseHandle(process)
