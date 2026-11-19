"""ctypes bindings mirroring ``native/include/browsermem.h``. Internal module."""

from __future__ import annotations

import ctypes
import os
import sys
from ctypes import (
    CFUNCTYPE,
    POINTER,
    Structure,
    c_char_p,
    c_double,
    c_int,
    c_int32,
    c_uint32,
    c_uint64,
    c_void_p,
)

DLL_NAME = "browsermem.dll"

# Status codes
OK = 0
ERR_INVALID_ARG = -1
ERR_NOT_FOUND = -2
ERR_ACCESS = -3
ERR_STATE = -4
ERR_INTERNAL = -5


class bm_candidate(Structure):
    _fields_ = [("url", c_char_p), ("score", c_double)]


class bm_tab(Structure):
    _fields_ = [
        ("event", c_int32),
        ("browser", c_int32),
        ("browser_name", c_char_p),
        ("pid", c_uint32),
        ("candidate_count", c_uint32),
        ("hwnd", c_uint64),
        ("title", c_char_p),
        ("caption", c_char_p),
        ("url", c_char_p),
        ("confidence", c_double),
        ("timestamp_ms", c_uint64),
        ("scan_ms", c_uint32),
        ("reserved", c_uint32),
        ("candidates", POINTER(bm_candidate)),
    ]


class bm_window(Structure):
    _fields_ = [
        ("hwnd", c_uint64),
        ("pid", c_uint32),
        ("browser", c_int32),
        ("browser_name", c_char_p),
        ("caption", c_char_p),
        ("title", c_char_p),
        ("is_foreground", c_int32),
        ("reserved", c_int32),
    ]


class bm_url_entry(Structure):
    _fields_ = [("url", c_char_p), ("count", c_uint32), ("reserved", c_uint32)]


class bm_monitor_options(Structure):
    _fields_ = [
        ("struct_size", c_uint32),
        ("browser_mask", c_uint32),
        ("debounce_ms", c_uint32),
        ("rescan_ms", c_uint32),
        ("cache_ttl_ms", c_uint32),
        ("all_windows", c_int32),
        ("emit_unresolved", c_int32),
        ("reserved", c_int32),
    ]


TAB_CALLBACK = CFUNCTYPE(None, POINTER(bm_tab), c_void_p)


def _locate_dll() -> str:
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = [os.environ.get("BROWSERMEM_DLL"), os.path.join(here, DLL_NAME)]
    for path in candidates:
        if path and os.path.isfile(path):
            return os.path.abspath(path)
    raise ImportError(
        f"{DLL_NAME} was not found next to the browsermem package. Install a wheel "
        "(pip install browsermem), build from source (pip install .), or set "
        "BROWSERMEM_DLL to the DLL's full path."
    )


def _load() -> ctypes.CDLL:
    if sys.platform != "win32":
        raise ImportError("browsermem supports Windows only")
    lib = ctypes.CDLL(_locate_dll())

    def fn(name: str, restype, *argtypes) -> None:
        f = getattr(lib, name)
        f.restype = restype
        f.argtypes = list(argtypes)

    P = POINTER
    fn("bm_version", c_char_p)
    fn("bm_last_error", c_char_p)
    fn("bm_list_windows", c_int, c_uint32, P(P(bm_window)), P(c_uint32))
    fn("bm_windows_free", None, P(bm_window))
    fn("bm_get_active_tab", c_int, c_uint32, P(P(bm_tab)))
    fn("bm_get_tab", c_int, c_uint64, P(P(bm_tab)))
    fn("bm_tab_free", None, P(bm_tab))
    fn("bm_dump_urls", c_int, c_uint32, P(P(bm_url_entry)), P(c_uint32))
    fn("bm_urls_free", None, P(bm_url_entry))
    fn("bm_image_create", c_int, P(c_void_p))
    fn("bm_image_capture", c_int, c_uint32, P(c_void_p))
    fn("bm_image_add_region", c_int, c_void_p, c_uint64, c_void_p, c_uint64)
    fn("bm_image_info", c_int, c_void_p, P(c_uint64), P(c_uint32))
    fn("bm_image_urls", c_int, c_void_p, P(P(bm_url_entry)), P(c_uint32))
    fn("bm_image_resolve", c_int, c_void_p, c_char_p, c_int32, P(P(bm_tab)))
    fn("bm_image_free", None, c_void_p)
    fn("bm_monitor_options_init", None, P(bm_monitor_options))
    fn("bm_monitor_start", c_int, P(bm_monitor_options), TAB_CALLBACK, c_void_p, P(c_void_p))
    fn("bm_monitor_stop", c_int, c_void_p)
    return lib


lib = _load()


def last_error() -> str:
    raw = lib.bm_last_error()
    return raw.decode("utf-8", "replace") if raw else ""
