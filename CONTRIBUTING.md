# Contributing to browsermem

Thanks for helping! Bug reports, browser-compatibility reports and pull requests are all
welcome.

## Development setup

You need Windows 10/11 x64, Python 3.9+, and **Visual Studio 2022 or newer with the
"Desktop development with C++" workload**. CMake and Ninja are installed automatically by
the build.

```bash
git clone https://github.com/BinaryBreaker/browsermem
cd browsermem
python -m venv .venv
.venv\Scripts\activate
pip install -e ".[dev]"      # builds the DLL and installs in editable mode
pytest
```

After changing C++ code, run `pip install -e .` again to rebuild the DLL.

## Tests

| Command | What runs |
|---|---|
| `pytest` | unit, forensics and live-capture tests (no browser needed) |
| `set BROWSERMEM_TEST_BROWSER=1 && pytest -m browser` | end-to-end tests with a real Chrome or Edge (opens browser windows) |

The forensics tests build synthetic memory, and they also start a helper process that
keeps a browser-like "tab object" in memory, so the matching engine can be tested
deterministically.

## Code style

- Python: `ruff check .` (configuration in `pyproject.toml`).
- C++: C++17, `/W4 /WX` must stay warning-free. Follow the existing style (Google-like,
  100 columns).
- Public Python API changes need docstrings, a docs update in `docs/`, and a
  `CHANGELOG.md` entry under *Unreleased*.
- Keep `native/include/browsermem.h` and `src/browsermem/_native.py` in sync; a test checks
  that the enum values match.

## Adding a browser

Most Chromium-based browsers only need a new entry in `AllProfiles()` in
`native/src/browsers.cpp` (executable name, window class, caption suffix), a value in the
`Browser` enum in both the header and `models.py`, and a line in the README table.

## Pull requests

1. Fork the repository and create a branch from `main`.
2. Add tests for your change.
3. Make sure `pytest` and `ruff check .` pass.
4. Open a PR describing what changed and why.

## Releasing (maintainers)

1. Update `version` in `pyproject.toml` and move the *Unreleased* changelog entries under
   the new version.
2. Commit, then tag: `git tag v0.2.0 && git push --tags`.
3. The *Release* workflow builds the wheel and sdist, tests them, publishes to PyPI with
   Trusted Publishing, and creates a GitHub release.
