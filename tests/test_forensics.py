from __future__ import annotations

import pytest

from browsermem import BrowserMemError, MemoryImage
from browsermem.forensics import read_minidump_regions, write_minidump

from .conftest import write_minidump_file

MEM_COMMIT, MEM_PRIVATE, MEM_IMAGE, PAGE_READWRITE = 0x1000, 0x20000, 0x1000000, 0x04


# ---- synthetic images ------------------------------------------------------------------


def test_carves_utf8_and_utf16_urls(synthetic):
    with MemoryImage.from_regions(synthetic.regions) as image:
        urls = {u.url: u.count for u in image.urls()}
    assert urls[synthetic.url] == 1
    assert urls[synthetic.decoy] == 1
    assert urls[synthetic.wide_url] == 1


def test_resolves_url_next_to_title(synthetic):
    with MemoryImage.from_regions(synthetic.regions) as image:
        tab = image.resolve(synthetic.title)
    assert tab.resolved
    assert tab.url == synthetic.url
    assert tab.title == synthetic.title
    assert tab.confidence > 0.5
    assert tab.candidates[0][0] == synthetic.url
    assert synthetic.decoy not in [u for u, _ in tab.candidates]


def test_resolve_accepts_window_caption(synthetic):
    with MemoryImage.from_regions(synthetic.regions) as image:
        tab = image.resolve(f"{synthetic.title} - Google Chrome", browser="chrome")
    assert tab.url == synthetic.url
    assert tab.title == synthetic.title


def test_page_url_beats_nearby_favicon():
    """Browsers keep the favicon URL next to the page URL; the page must win."""
    import struct

    base = 0x30000000
    mem = bytearray(0x1000)
    title = "Favicon Regression Page Title"
    page = "https://example.com/dashboard"
    favicon = "https://cdn.example.com/static/favicon-dark.svg"
    tb = title.encode("utf-16-le")
    mem[0x100:0x100 + len(tb)] = tb
    mem[0x300:0x300 + len(favicon)] = favicon.encode()
    mem[0x400:0x400 + len(page)] = page.encode()
    struct.pack_into("<Q", mem, 0x800, base + 0x100)   # title
    struct.pack_into("<Q", mem, 0x808, base + 0x300)   # favicon: closest field
    struct.pack_into("<Q", mem, 0x840, base + 0x400)   # page URL: further away
    with MemoryImage.from_regions([(base, bytes(mem))]) as image:
        assert image.resolve(title).url == page


def test_unknown_title_is_unresolved(synthetic):
    with MemoryImage.from_regions(synthetic.regions) as image:
        tab = image.resolve("A title that is nowhere in memory")
    assert not tab.resolved
    assert tab.url is None


def test_image_info_and_repr(synthetic):
    image = MemoryImage.from_regions(synthetic.regions)
    assert image.region_count == 2
    assert image.total_bytes == 0x2000
    assert "2 regions" in repr(image)
    image.close()
    image.close()  # idempotent
    with pytest.raises(BrowserMemError):
        image.urls()


def test_empty_image():
    with MemoryImage() as image:
        assert image.urls() == []
        assert not image.resolve("anything").resolved


# ---- minidumps ---------------------------------------------------------------------


def test_minidump_roundtrip(tmp_path, synthetic):
    path = tmp_path / "synthetic.dmp"
    write_minidump_file(path, synthetic.regions)
    assert [b for b, _ in read_minidump_regions(path)] == [b for b, _ in synthetic.regions]
    with MemoryImage.from_minidump(path) as image:
        assert image.resolve(synthetic.title).url == synthetic.url


def test_minidump_private_only_filter(tmp_path, synthetic):
    (base_a, a), (base_b, b) = synthetic.regions
    infos = [
        (base_a, len(a), MEM_COMMIT, PAGE_READWRITE, MEM_PRIVATE),
        (base_b, len(b), MEM_COMMIT, PAGE_READWRITE, MEM_IMAGE),  # excluded by default
    ]
    path = tmp_path / "typed.dmp"
    write_minidump_file(path, synthetic.regions, infos)

    with MemoryImage.from_minidump(path) as image:
        assert image.region_count == 1
        assert synthetic.decoy not in [u.url for u in image.urls()]
    with MemoryImage.from_minidump(path, private_only=False) as image:
        assert image.region_count == 2


def test_rejects_non_minidump(tmp_path):
    path = tmp_path / "junk.dmp"
    path.write_bytes(b"not a dump" * 10)
    with pytest.raises(BrowserMemError):
        MemoryImage.from_minidump(path)


# ---- live processes --------------------------------------------------------------------


def test_capture_live_process(planted_process):
    with MemoryImage.capture(planted_process.pid) as image:
        assert image.total_bytes > 0
        tab = image.resolve(planted_process.title)
    assert tab.url == planted_process.url


def test_write_and_analyse_minidump(tmp_path, planted_process):
    path = tmp_path / "planted.dmp"
    write_minidump(planted_process.pid, path)
    assert path.stat().st_size > 0
    with MemoryImage.from_minidump(path) as image:
        assert image.resolve(planted_process.title).url == planted_process.url


def test_capture_missing_process():
    with pytest.raises(BrowserMemError):
        MemoryImage.capture(0xFFFFFFF0)
