"""Acquire a browser's memory as a minidump, then analyse it offline.

    python forensics_minidump.py            # dump the active browser, then analyse
    python forensics_minidump.py chrome.dmp # analyse an existing dump
"""

import sys

import browsermem
from browsermem.forensics import MemoryImage, write_minidump

if len(sys.argv) > 1:
    path, title = sys.argv[1], None
else:
    windows = browsermem.list_windows()
    if not windows:
        sys.exit("No browser window open.")
    path, title = "browser.dmp", windows[0].title
    print(f"Dumping {windows[0].browser_name} (pid {windows[0].pid}) to {path} ...")
    write_minidump(windows[0].pid, path)

with MemoryImage.from_minidump(path) as image:
    print(image)
    print("\nMost frequent URLs:")
    for url, count in image.urls()[:15]:
        print(f"{count:6}  {url}")
    if title:
        tab = image.resolve(title)
        print(f"\nTab {title!r} -> {tab.url} (confidence {tab.confidence:.2f})")
