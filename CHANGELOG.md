# Changelog

All notable changes are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

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

[Unreleased]: https://github.com/BinaryBreaker/browsermem/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/BinaryBreaker/browsermem/releases/tag/v0.1.0
