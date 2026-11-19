"""Helper process: keeps a browser-like "tab object" in memory until stdin closes.

The layout mirrors what browsermem looks for in a real browser: a struct with
a pointer to a UTF-16 title buffer and, a few fields later, a pointer to a
NUL-terminated UTF-8 URL buffer.
"""

import ctypes
import json
import os
import sys
import uuid

token = uuid.uuid4().hex
title = f"browsermem planted tab {token}"
url = f"https://planted.example.test/{token}/page"

title_buf = ctypes.create_unicode_buffer(title)  # wchar_t == UTF-16 on Windows
url_buf = ctypes.create_string_buffer(url.encode())
tab_object = (ctypes.c_uint64 * 32)()
tab_object[3] = ctypes.addressof(title_buf)
tab_object[7] = ctypes.addressof(url_buf)

# Report our own pid: a venv's python.exe is a launcher that runs the real
# interpreter as a child process, so Popen.pid is not this process.
print(json.dumps({"pid": os.getpid(), "title": title, "url": url}), flush=True)
sys.stdin.read()
