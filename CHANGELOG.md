# Changelog

All notable changes to FusionCut Pro are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/).

## [0.1.0] - 2026-08-22

First public baseline: engineering foundation only (no editing features yet).

### Added
- CMake build system (C++17) driven by a single `VERSION` file, with
  `FC_BUILD_APP` / `FC_BUILD_TESTS` options.
- Core engine primitives (dependency-free, 88 unit checks in CI):
  - `fc::FrameRate` + `fc::Timecode` - rational frame rates (24/25/30/50/60 and
    NTSC 23.976/29.97/59.94), non-drop timecode parse/format/arithmetic.
  - `fc::LruCache` - LRU cache with hit/miss statistics (frame-cache policy core).
  - `fc::MemoryPool` - fixed-capacity, 64-byte-aligned block pool with ownership
    validation and double-release detection (deterministic frame allocation).
- Qt 5.15 desktop shell (`FusionCutPro.exe`): Module 2 menu structure, FusionCut
  dark theme (charcoal #1E1E1E / panels #252525 / accent #00A8FF), and the
  Pro Mode / Quick Mode workspace toggle placeholder.
- GitHub Actions CI: blocking format check (pinned clang-format), core unit
  tests on Ubuntu and Windows (MSVC), Qt app build on Ubuntu (Qt 5.15).
- GitHub Actions `Portable Build` workflow: on-demand Windows x64 portable zip
  (MSYS2 MinGW64 + windeployqt, tests run before packaging); tag pushes (`v*`)
  attach the zip to a GitHub Release.
- Project documentation: README, changelog, repository settings
  (description + topics), GPL-3.0 license.

### Fixed
- Qt build: replaced `QWidget::addAction(text, shortcut, receiver, functor)`
  calls (a Qt 6.3+ overload) with explicit action creation that compiles
  against Qt 5.15 - fixes the Ubuntu Qt CI job; shortcuts now live on the
  actions themselves. Bonus: Clip/Sequence/Effects menus gained their
  Module 6 default shortcuts (C, Ctrl+R, Ctrl+L, Ctrl+G, Ctrl+Shift+G,
  Ctrl+D, Enter, +/-).
- Format check: `.clang-format` now pins Qt-convention right pointer
  alignment (`PointerAlignment: Right`) matching the codebase, all sources
  were formatted with clang-format 22.1.8, and CI installs that exact
  pinned version - the check is now blocking and reproducible.

[0.1.0]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.1.0
