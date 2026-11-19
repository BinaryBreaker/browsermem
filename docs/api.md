# API reference

## Functions

::: browsermem.current_url

::: browsermem.get_active_tab

::: browsermem.get_tab

::: browsermem.get_all_tabs

::: browsermem.list_windows

::: browsermem.dump_urls

::: browsermem.version

## Live monitoring

::: browsermem.TabMonitor

## Activity tracking

::: browsermem.TabHistory

::: browsermem.Visit

## Data types

::: browsermem.Tab

::: browsermem.BrowserWindow

::: browsermem.UrlCount

::: browsermem.Browser

::: browsermem.EventKind

## Forensics

::: browsermem.MemoryImage

::: browsermem.forensics.write_minidump

::: browsermem.forensics.read_minidump_regions

## Exceptions

::: browsermem.BrowserMemError

::: browsermem.NotFoundError

::: browsermem.AccessDeniedError

::: browsermem.StateError

## C API

The native library (`browsermem.dll`) has a plain C interface, so you can also use it from
C, C++, C#, Rust or Go. The full contract is in
[`native/include/browsermem.h`](https://github.com/BinaryBreaker/browsermem/blob/main/native/include/browsermem.h).
