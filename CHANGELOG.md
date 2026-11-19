# Changelog

All notable changes are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.2.0] - 2026-11-19

### Changed
- **Up to ~100× faster lookups.** Memory is now streamed through reusable per-thread
  buffers on up to 8 threads instead of being copied wholesale, the title search uses SSE2,
  and only the pointers next to the title are checked instead of indexing every URL in
  memory. A full scan dropped from ~350 ms to ~45 ms on a typical Chrome profile.
- **Hot-region cache:** the regions that held a tab's objects are searched first on the
  next lookup (1–4 ms), with automatic fallback to a full scan.
- Offline `MemoryImage.resolve()` uses the same engine and no longer builds a full URL index.

### Fixed
- Favicon, image, script and style URLs stored next to the page URL could win over the page
  itself (seen on GitHub). They are now down-weighted.

### Added
- `browsermem_bench` developer tool (`-DBROWSERMEM_BUILD_TOOLS=ON`) with a per-step timing
  breakdown.

## [0.1.0] - 2026-11-19

First public release.

### Added
- `current_url()`, `get_active_tab()`, `get_tab()`, `get_all_tabs()`, `list_windows()`.
- `TabMonitor` live events: `SNAPSHOT`, `TAB_CHANGED`, `WINDOW_ACTIVATED`, `URL_CHANGED`,
  `WINDOW_CLOSED`. Callbacks, decorators, iterator and asyncio interfaces.
- Handler filters `on_domain()` and `on_url()`, and `wait_for()`.
- `TabHistory` / `Visit`: visits and time spent per domain.
- `Tab` helpers: `domain`, `host`, `scheme`, `is_web`, `matches_domain()`, `to_dict()`.
- `browsermem` command-line tool with JSON output.
- Forensics: `MemoryImage` (live capture, minidump, custom regions), `write_minidump()`.
- Support for Chrome, Edge, Brave, Opera, Vivaldi and Yandex; experimental Firefox.

[Unreleased]: https://github.com/BinaryBreaker/browsermem/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/BinaryBreaker/browsermem/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/BinaryBreaker/browsermem/releases/tag/v0.1.0
