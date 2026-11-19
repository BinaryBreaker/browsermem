from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, List, Optional, Sequence, Tuple

import pytest

HERE = Path(__file__).parent


def pytest_collection_modifyitems(config, items):
    if os.environ.get("BROWSERMEM_TEST_BROWSER") == "1":
        return
    skip = pytest.mark.skip(reason="set BROWSERMEM_TEST_BROWSER=1 to run real-browser tests")
    for item in items:
        if "browser" in item.keywords:
            item.add_marker(skip)


# ---- synthetic memory --------------------------------------------------------------


@dataclass
class Synthetic:
    title: str
    url: str
    decoy: str
    wide_url: str
    regions: List[Tuple[int, bytes]]


@pytest.fixture
def synthetic() -> Synthetic:
    """Memory laid out like a browser object: a struct holding a pointer to a
    UTF-16 title buffer and, nearby, a pointer to a UTF-8 URL buffer."""
    title = "Synthetic Tab Title For Testing"
    url = "https://synthetic.example/path/page?q=1"
    decoy = "https://decoy.example/unrelated/far-away"
    wide_url = "https://wide.example/utf16/string"

    base = 0x10000000
    a = bytearray(0x1000)
    t_off, u_off, s_off = 0x100, 0x400, 0x800
    tb = title.encode("utf-16-le")
    a[t_off:t_off + len(tb)] = tb
    a[u_off:u_off + len(url)] = url.encode()
    struct.pack_into("<Q", a, s_off + 0x10, base + t_off)  # e.g. title_ (std::u16string)
    struct.pack_into("<Q", a, s_off + 0x28, base + u_off)  # e.g. url_.spec_ (std::string)

    base2 = 0x20000000
    b = bytearray(0x1000)
    b[0x10:0x10 + len(decoy)] = decoy.encode()
    wb = wide_url.encode("utf-16-le")
    b[0x200:0x200 + len(wb)] = wb

    return Synthetic(title, url, decoy, wide_url, [(base, bytes(a)), (base2, bytes(b))])


def write_minidump_file(path: Path, regions: Sequence[Tuple[int, bytes]],
                        infos: Optional[Sequence[Tuple[int, int, int, int, int]]] = None) -> None:
    """Write a minimal MDMP with a Memory64ListStream (+ optional MemoryInfoListStream).

    ``infos`` entries: (base, size, state, protect, type).
    """
    n_streams = 2 if infos else 1
    cursor = 32 + 12 * n_streams
    mem64_rva, mem64_size = cursor, 16 + 16 * len(regions)
    cursor += mem64_size
    info_rva, info_size = cursor, (16 + 48 * len(infos)) if infos else 0
    cursor += info_size
    data_rva = cursor

    out = bytearray(struct.pack("<4sIIIIIQ", b"MDMP", 0xA793, n_streams, 32, 0, 0, 0))
    out += struct.pack("<III", 9, mem64_size, mem64_rva)
    if infos:
        out += struct.pack("<III", 16, info_size, info_rva)
    out += struct.pack("<QQ", len(regions), data_rva)
    for base, data in regions:
        out += struct.pack("<QQ", base, len(data))
    if infos:
        out += struct.pack("<IIQ", 16, 48, len(infos))
        for base, size, state, protect, mtype in infos:
            out += struct.pack("<QQIIQIIII", base, base, protect, 0, size, state, protect,
                               mtype, 0)
    for _, data in regions:
        out += data
    path.write_bytes(bytes(out))


# ---- a live process with a planted "tab" ---------------------------------------------


@dataclass
class PlantedProcess:
    pid: int
    title: str
    url: str


@pytest.fixture
def planted_process() -> Iterator[PlantedProcess]:
    proc = subprocess.Popen(
        [sys.executable, str(HERE / "_planted_child.py")],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
    )
    try:
        assert proc.stdout is not None
        info = json.loads(proc.stdout.readline())
        yield PlantedProcess(info["pid"], info["title"], info["url"])
    finally:
        proc.kill()
        proc.wait(timeout=10)
