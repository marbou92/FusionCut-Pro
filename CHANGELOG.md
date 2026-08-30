# Changelog

All notable changes to FusionCut Pro are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/).

## [0.4.0] - 2026-08-23

Milestone 4, phase 1: the editing core. The timeline is no longer a static
placeholder - it owns a real, frame-accurate model with clips, split, trim,
move, and delete; the UI renders and edits against it.

### Added
- `fc::TimelineModel` (pure C++, in fc_core): tracks, clips with source
  in/out points, timeline position, playback rate; operations addClip /
  removeClip / splitAt / moveClip / trimClipStart / trimClipEnd / clipAt /
  durationFrames. 64-check unit suite (`fc_timeline_tests`) covering
  tracks, add validation, split semantics, move + trim invariants, clipAt
  gaps, and duration aggregation.
- TimelinePanel now renders real clips from the model as colored blocks
  with labels and selection highlight; click-to-select, scrub, Ctrl+wheel
  zoom; razor-mode split signal (mouse-based razor UI lands in M4b).
- Editing actions wired through MainWindow: double-clicking a project
  item places a 5-second clip on V1 at the playhead; **Clip → Split at
  Playhead (C)** splits the selected clip (or the V1 clip under the
  playhead); **Delete / Backspace** removes the selected clip; sequence
  duration tracks the model after every edit.
- Track header L/M/S cells now reflect model state (read-only this phase;
  toggling arrives in M4b).

### Fixed (packaging, post-v0.3.0 portable)
- Portable build no longer needs `windeployqt` or `qmake`: plugins are
  located via `find -name qwindows.dll`, mirrored into both `dist/<cat>/`
  and the canonical `dist/plugins/<cat>/` layout, with core Qt DLLs also
  copied next to the platform plugin (defensive against plugin-dep DLL
  search quirks); `dist/qt.conf` pins the install root so Qt does not
  chase the MSYS2 absolute path baked into the runtime.
- `dist/run-console.bat` launches the app in a console so a startup crash
  prints Qt's diagnostic (missing DLL / plugin) instead of the bare
  0xc0000005 dialog.

## [0.3.0] - 2026-08-23

Milestone 3: the dual-mode UI shell. The app now shows **real decoded
frames** - import media, scrub, play, and generate 360p proxies, all
through the M2 engine on a background thread.

### Added
- **Pro Mode workspace** (Module 2.1): dockable panels with saveable
  layout (QSettings) - Project panel (import/remove/metadata/context menu,
  thumbnails captured from the first decoded frame), tabbed Effects
  browser (Module 7 seed catalog, drag-enabled for M4), bottom Timeline |
  Audio Mixer tabs, right Effect Controls parameter tree (Motion +
  Lumetri seeds), and a Source | Program monitor splitter.
- **Quick Mode page** (Module 2.2): CapCut-style simplified layout with
  large preview, prominent Import, aspect selector (16:9 / 9:16 / 1:1 /
  4:3), and the main + quick-actions toolbars (placeholders wired to M4-M7).
  One menu toggle switches modes; both share the same playback engine.
- **Playback engine**: `DecodeWorker` on a background QThread owning one
  `VideoDecoder` (sequential reads while playing, keyframe seek when
  scrubbing), `PreviewCanvas` custom-painted monitor with letterboxing
  and adaptive aspect, `TransportBar` with `fc::Timecode` readout, frame
  stepping, scrub slider, and Space/Left/Right shortcuts; timeline
  playhead is click/drag-scrubbable with Ctrl+wheel zoom.
- **Proxy workflow in-app**: right-click a clip (or Clip menu) generates
  a 360p proxy with live progress in the status bar; playback
  automatically prefers the proxy once generated.
- **Timeline shell**: fully custom-painted - `fc::Timecode` ruler, three
  placeholder tracks (V2/V1/A1) with functional lock/mute/solo header
  cells and colorblind-friendly accents (Module 10.4).
- Portable packaging: iterative `ldd` DLL sweep replaces the manual
  three-DLL copy, so the zip now includes Qt + FFmpeg + MinGW runtime
  dependency chains automatically.
- High-DPI attributes enabled in `main.cpp` (Module 10.4 UI scaling).

### Changed
- App now links `fc_core` + `fc_media`; building the app requires
  `FC_BUILD_MEDIA=ON` (guarded with a clear CMake error).
- About dialog reports the linked FFmpeg runtime version.

## [0.2.0] - 2026-08-22

Milestone 2: the FFmpeg-backed media I/O layer. The engine can now inspect,
decode, and proxy real video files; the portable/Windows legs of CI build and
run it on MinGW too.

### Added
- `fc_media` library (FFmpeg via pkg-config, dual API paths for FFmpeg
  4.4 and 5.1+/7.x - `AVChannelLayout` guarded on `LIBAVUTIL_VERSION_INT`):
  - `MediaProbe` - file inspection into a dependency-free `MediaInfo` model
    (video/audio streams, codec names, pixel/sample formats, duration,
    frame rate preferring `r_frame_rate` so CFR rates stay exact).
  - `VideoDecoder` - sequential and seeking decode pump converting every
    frame to packed RGBA; strips mp4 `AV_PKT_FLAG_DISCARD` so trailing
    frames are never silently dropped; seek resumes at the first frame with
    pts >= target (half-frame tolerance).
  - `ProxyGenerator` - 360p edit-proxy transcode (H.264/x264 with CRF +
    preset, MPEG-4 Part 2 fallback when no H.264 encoder exists; audio
    re-encoded to 48 kHz stereo AAC via swresample + audio FIFO); never
    upscales, keeps aspect with even dimensions; monotonic progress
    callback with cancellation; removes partial output on failure.
  - RAII ownership wrappers for every FFmpeg object used plus a C++-safe
    `fcError()` (no compound literals - MSVC friendly).
- Test layer: shared header-only harness, runtime synthetic media generator
  (no binary test assets in the repo), and a 317-check integration suite
  covering probe, sequential decode (color-order assertions), seek,
  proxy geometry/audio/cancellation, and no-upscale behavior.
- CI: new `media-tests` job (Ubuntu, FFmpeg 7.x) and
  `media-tests-ffmpeg44` job (Ubuntu 22.04 container, FFmpeg 4.4 - the
  legacy API generation the Windows 7 build targets); Qt job now builds
  with the media layer enabled; portable workflow installs
  `mingw-w64-x86_64-ffmpeg` + `pkgconf` and runs media tests on MinGW.

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

[0.4.0]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.4.0
[0.3.0]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.3.0
[0.2.0]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.2.0
[0.1.0]: https://github.com/marbou92/FusionCut-Pro/releases/tag/v0.1.0
