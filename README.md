# FusionCut Pro

**A lightweight, dual-mode video editor for Windows 7 and later - engineered for a 1 GB RAM budget.**

[![CI](https://github.com/marbou92/FusionCut-Pro/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/marbou92/FusionCut-Pro/actions/workflows/ci.yml)
[![Portable Build](https://github.com/marbou92/FusionCut-Pro/actions/workflows/portable-build.yml/badge.svg)](https://github.com/marbou92/FusionCut-Pro/actions/workflows/portable-build.yml)
[![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-informational)](./LICENSE)

FusionCut Pro combines the professional panel workspace of a Premiere-style editor (**Pro Mode**)
with the streamlined, one-tap flow of a CapCut-style editor (**Quick Mode**) in a single native
application built with **C++17 and Qt 5.15** - the last Qt line that still runs on Windows 7.

Everything ships **through GitHub**: sources, tests, CI, and portable builds produced by
GitHub Actions. No installer, no registry writes.

## Highlights

- **Dual-mode workspace** - dockable Pro Mode panels vs. streamlined Quick Mode timeline
- **Legacy-friendly** - targets Windows 7 SP1+ (32/64-bit era hardware), 1 GB RAM minimum
- **Deterministic memory core** - fixed block pools and LRU frame-cache eviction, never guesswork
- **Portable-first distribution** - every build is a self-contained zip straight from CI
- **Fully tested engine primitives** - timecode, caching, and allocation units run in CI on
  every push (Ubuntu + Windows)

## Status

FusionCut Pro is in **pre-alpha**. The engineering foundation ships first; editing features
arrive milestone by milestone.

| Milestone | Scope | Status |
| --- | --- | --- |
| M1 - Engineering foundation | Build system, CI, portable pipeline, core primitives | Shipped (v0.1.0) |
| M2 - Media I/O | FFmpeg wrapper, decode pipeline, proxy generation | Shipped (v0.2.0) |
| M3 - Dual-mode UI | Pro Mode dockable panels, Quick Mode streamlined timeline | Shipped (v0.3.0) |
| M4 - Editing core | Multi-track timeline, trim/split/ripple, audio mixer | Shipped (v0.4.13) |
| M5 - Effects & color | Effects pipeline, 50+ effects, 30+ transitions, color panel | Shipped (v0.5.2) |
| M6 - Text engine | Rich text, bundled color-emoji renderer, animations, captions | Planned |
| M7 - AI features | Face tracking, background removal, auto-captions | Planned |
| M8 - Optimization & polish | 1 GB RAM budget audit, shortcuts, export presets | Planned |

> **Runtime crash reporting (v0.4.1+, attribution upgrade v0.5.0):** a
> built-in crash handler captures access violations, uncaught C++
> exceptions, CRT misuses, pure-virtual calls, and POSIX signals, then
> writes a structured report (`crash-logs/FusionCutPro-crash-<timestamp>.log`
> next to the executable) with the exception code, the **faulting
> module + offset** (one Toolhelp32 snapshot feeds the blame line, the
> per-frame module+RVA-annotated stack backtrace, the module list, and
> the crash dialog), the AV read/write/execute type + target address,
> the crashing thread id, an x64 register dump, and the boot trace -
> the dialog the user sees now names the culprit DLL directly.
> As of v0.4.3 the Windows VEH is registered at static-init time
> (before `main()`), which widens the VEH's coverage to runtime
> crashes that occur after `.CRT$XCU` but does **not** cover
> loader-phase failures (the Windows "0xc0000005 unable to start"
> dialog is the loader's own failure, shown before any user code
> runs).
>
> **Loader-phase diagnostic, baked in (v0.5.0):** the separate
> `fcp-loader-check.exe` of v0.4.4-v0.4.11 is RETIRED - its
> two-phase diagnostic now runs from the exe itself. Run
> `FusionCutPro.exe --diag` if the app will not start: phase 1 maps
> the exe's PE import tree from outside (no DllMain) and names any
> DLL the loader cannot map; phase 2 launches a fresh copy of the app
> under a built-in mini-debugger (Windows debug API, kernel32 only)
> and records every DLL load and every exception — including faults
> inside DllMain / static initializers that Windows Error Reporting
> never sees (which is why the "0xc0000005 unable to start" dialog
> produces no Event Viewer entry). It names the faulting module +
> offset and writes `FusionCutPro-diag-<timestamp>.log` with the full
> module load trail. `FusionCutPro.exe --crash-test` writes a
> synthetic runtime report to verify the in-process pipeline on a
> clean machine. v0.4.11 fixed the loader-phase root causes the
> watch exposed (see the Windows 7 / 8.x compatibility note above):
> the bundled `api-ms-win-core-synch-l1-2-0.dll` shim (WaitOnAddress
> api set) and the stubbed `librsvg-2-2.dll` (Windows 8+ import);
> v0.4.11 is CONFIRMED BOOTING end-to-end on the user's Windows 7
> machine (full seven-stage boot trace + workspace rendering), so
> `--diag` is the safety net rather than the daily driver.
>
> **Effects (v0.5.0, M5 Phase 1):** every timeline clip carries an
> effect stack processed in place on the decoded RGBA frame. 25 CPU
> effects ship in Phase 1 across Color (brightness, contrast,
> saturation, vibrance, hue, temperature, tint, exposure, gamma), Tone
> (levels, posterize, threshold, solarize, invert), Filter (black &
> white, sepia, vignette, deterministic film grain, pixelate, chromatic
> aberration), Blur & Sharpen (box, gaussian, unsharp sharpen) and
> Stylize (find edges, emboss). Browse them in the Effects panel
> (searchable), double-click to add to the selected clip, edit
> parameters live in Effect Controls - the program monitor re-renders
> instantly from the cached raw frame, and clips with effects show an
> amber `fx` badge in the timeline. Effects are deterministic
> (fixed-width integer math, seeded hash noise) and unit-tested with
> reference pixels (the `effects` ctest suite).
>
> **Effects catalog (v0.5.2, M5 Phase 3): 52 effects across 7
> categories.** 27 additions: Color Correction (the 9-knob combined
> grade), Channel Gain, Colorize, Duotone, Faded Film (Color);
> Highlights, Shadows, Gain, Bayer Dither (Tone); Mirror, Flip, Glow,
> Scanlines, Halftone (Filter); Motion Blur H/V, Radial Blur (Blur);
> Wave, Ripple, Fisheye, Tile (Distort, new category); Color Bars,
> Gradient, Grid, Noise (Generate, new category); Thermal, Glitch
> (Stylize).
>
> **Keyframes (v0.5.2, M5 Phase 3):** every Number parameter can carry
> a keyframe track in CLIP-RELATIVE frames. `paramAt` resolves linear
> interpolation inside the bracketing pair and clamps outside it; the
> program monitor, the export, and the panels all evaluate parameters
> at the same clip-relative position, so the value you scrub is the
> value that renders. Effect Controls shows a keyframe diamond on every
> Number param - click it at the playhead to toggle a keyframe; when a
> param is keyframed, sliders display the resolved value and edits
> write the keyframe at the playhead. Splitting a clip re-bases the
> right half's tracks with its new in-point (the left half keeps its
> full track non-destructively).
>
> **Transitions (v0.5.1, M5 Phase 2):** every cut between two adjacent
> clips can carry a transition - 36 of them across Dissolve (cross, dip
> to black/white, additive, film-grain, blur), Wipe (directional,
> corners, iris box/circle/diamond, clock, blinds, checker, barn
> doors), Slide, Push, and Zoom (in/out/through). Browse them in the
> new Transitions panel and double-click to drop one on the selected
> clip's cut (or press Ctrl+D for the default cross dissolve); the
> timeline shows each transition as a green X marker spanning its
> window - click it to edit the duration or remove it in Effect
> Controls. The program monitor composites the cut LIVE: the outgoing
> clip keeps playing while the incoming clip's first frame blends in
> (the no-overlap window model - nothing reflows, no content is lost,
> every transition is endpoint-exact and deterministic). The
> transitions engine + model semantics are unit-tested in the
> `transitions` ctest suite. The Phase 3 completions - keyframes,
> project persistence, export-side application, and the catalog
> expansion to 50+ effects - all shipped in v0.5.2 (see the callouts
> above and below).
>
> **Color panel (v0.5.2, M5 Phase 3):** a fourth left-dock tab - the
> colorist's grade view. Nine sliders (exposure, contrast, highlights,
> shadows, saturation, vibrance, temperature, tint, hue) edit the
> selected clip's combined Color Correction instance, auto-created on
> the first touch and re-rendered live from the cached raw frame.
> Reset Grade removes it. Keyframed corrector params display and edit
> at the playhead like every other effect.
>
> **Project persistence (v0.5.2, M5 Phase 3):** File > Open Project
> (Ctrl+O) / Save Project (Ctrl+S) / Save Project As (Ctrl+Shift+S).
> Projects are deterministic JSON (`.fcp`): tracks, clips with in/out
> points and effect stacks (parameters AND keyframes), cut transitions,
> the fps - media stays referenced by path (nothing is embedded, 1 GB
> budget). The parser is strict: a malformed or schema-violating file
> fails whole, never partially; ids, stacks, and transitions load back
> exactly; effects or transitions from a NEWER catalog round-trip
> untouched (they never process, they never crash). The window title
> tracks the dirty state; closing with unsaved changes prompts
> Save/Discard/Cancel.
>
> **Export (v0.5.2, M5 Phase 3):** File > Export Media (Ctrl+M) renders
> the timeline through the EXACT program-monitor pipeline - every
> effect (keyframes interpolating per frame) and every cut transition
> composited live - to an H.264 MP4 (MPEG-4 fallback encoder) at the
> project fps with CRF quality control and your choice of resolution.
> The job runs on a background thread with a progress dialog and
> Cancel; partial files are cleaned up on failure or cancellation.
> Video only in this phase: timeline audio mixing (multi-track summing,
> crossfades) ships with the audio milestone.

## System requirements (target)

| | Minimum | Recommended |
| --- | --- | --- |
| OS | Windows 7 SP1+ (64-bit; 32-bit untested) | Windows 10/11 (64-bit) |
| RAM | 1 GB | 4 GB |
| CPU | Intel Core 2 Duo / AMD Athlon 64 X2 | Intel i5 / AMD Ryzen 5 |
| Storage | 500 MB + project space | 2 GB SSD |
| Graphics | DirectX 9 compatible | DirectX 11 with GPU acceleration |

> **Windows 7 / 8.x compatibility:** the FFmpeg 8 runtime stack (via
> MSYS2) contains Rust-built DLLs (e.g. `librav1e.dll`) that import the
> `WaitOnAddress` futex API set by its literal api-set name, which
> pre-10 loaders cannot resolve. The portable build therefore ships a
> small compatibility shim (`api-ms-win-core-synch-l1-2-0.dll`, built
> from `src/app/api_set_synch.c` since v0.4.7) that provides REAL
> implementations of that API set on top of Windows-7-era kernel32
> primitives — so those Windows versions start AND run correctly.
> The DLL search order tries the application directory before
> System32, so the shim wins the bind on Win7/8.x; on Windows 10/11
> the OS resolves the name natively and the file is ignored. Leave it
> in place.
>
> **Windows 7 / librsvg:** the MSYS2 `librsvg-2-2.dll` in the FFmpeg
> dependency tree is built with a modern Rust toolchain that
> statically imports `kernel32!GetSystemTimePreciseAsFileTime`
> (Windows 8+ only) — on Windows 7 the loader aborts with
> `STATUS_ENTRYPOINT_NOT_FOUND` (0xC0000139). KERNEL32 is a
> KnownDLL, so no app-dir file can ever supply that export; instead
> the portable build replaces `librsvg-2-2.dll` with a generated stub
> exporting exactly the symbols `avcodec-62.dll` imports (all
> returning failure). Consequence: SVG image decoding is unavailable
> in the portable build; every video/audio codec is unaffected. A CI
> tripwire additionally scans every bundled binary for known
> Windows 8/10-only kernel32 imports and fails the build loudly if
> one appears.

## Getting a portable build

1. Open the **Actions** tab -> **Portable Build** -> **Run workflow** (or open the latest run).
2. Download the `FusionCutPro-<version>-win64-portable.zip` artifact.
3. Extract anywhere and run `FusionCutPro.exe`. Nothing is installed.

Pushing a tag named `v*` (e.g. `v0.1.0`) does the same and additionally attaches the zip to a
GitHub **Release**. The first run on a fresh runner takes ~15-25 minutes (Qt toolchain download);
subsequent runs reuse the MSYS2 cache.

## Building from source

**Ubuntu (Qt 5.15 + FFmpeg from apt):**

```bash
sudo apt-get install -y cmake g++ pkg-config qtbase5-dev \
  libavformat-dev libavcodec-dev libavutil-dev \
  libswscale-dev libswresample-dev
cmake -S . -B build -DFC_BUILD_APP=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

**Windows (MSYS2 MinGW64 - what the portable pipeline uses):**

```bash
pacman -S --needed mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-pkgconf mingw-w64-x86_64-qt5-base \
  mingw-w64-x86_64-qt5-tools mingw-w64-x86_64-ffmpeg
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFC_BUILD_APP=ON
cmake --build build --parallel
./build/src/app/FusionCutPro.exe
```

The core library and tests build with no third-party dependencies
(`-DFC_BUILD_APP=OFF -DFC_BUILD_MEDIA=OFF`), which is what the fast CI
matrix verifies on every push. The media layer (`-DFC_BUILD_MEDIA=ON`,
default ON) needs FFmpeg development libraries via pkg-config and compiles
against both the FFmpeg 4.4 and 5.1+/7.x API generations - CI runs its
integration suite on Ubuntu (FFmpeg 7.x), an Ubuntu 22.04 container
(FFmpeg 4.4, the Windows 7 target generation), and MinGW/Windows (portable
workflow).

## Tests

```bash
ctest --test-dir build --output-on-failure
```

Five suites run in the core-only configuration: **core** (88 checks:
rational frame rates, timecode parse/format/math, LRU eviction,
memory-pool ownership/alignment) and **timeline** (130 checks: clip
placement/split/trim/move semantics, cross-track moves with overlap
rejection, magnetic drop resolution, ripple delete/trim, rolling
boundary edits, topmost-clip lookup) plus **effects** (2600+ checks:
catalog integrity across the 52 entries, parameter clamping,
neutral-parameter identities, per-effect reference pixels, stack
order/disable/unknown semantics, split propagation, and the keyframe
battery - resolution, editing, rebase, time-aware application) and
**transitions** (4500+ checks: catalog integrity, endpoint exactness
for every kind, per-family reference pixels, and the transition model
battery - placement validation, window resolution, and invariant
pruning under every timeline mutation) and **project** (79 checks:
JSON codec strictness, full model round-trip with id/keyframe/stack
fidelity, byte-identical deterministic serialization, malformed-input
rejection, parse atomicity) and **media** (400 checks:
synthetic media is generated at runtime - no binary
assets in the repo - then probed, decoded frame-accurately with color-order
assertions, seeked, and transcoded to 360p proxies with geometry, audio,
progress, cancellation, and no-upscale verification, plus the export
pipeline: encode/probe/decode round-trips, per-frame progress, both
cancellation paths) when the media layer
is enabled.

## Project layout

```
.
├── .github/workflows/     # ci.yml (lint + core/media/Qt matrix) + portable-build.yml
├── cmake/                 # CMake templates (version.h.in)
├── docs/                  # repository metadata; specs and wireframes land here
├── src/
│   ├── app/               # Qt 5.15 desktop shell (Pro/Quick workspace host)
│   ├── core/              # dependency-free engine primitives (fc_core)
│   └── media/             # FFmpeg I/O layer (fc_media): probe, decode, proxy
├── tests/                 # core + media suites, shared harness, synthetic media generator
├── CMakeLists.txt
├── LICENSE                # GPL-3.0
└── VERSION                # single source of truth for the version number
```

## License

Copyright (C) 2026  FusionCut Pro contributors.

This program is free software: you can redistribute it and/or modify it under the terms of the
[GNU General Public License](./LICENSE) as published by the Free Software Foundation, either
version 3 of the License, or (at your option) any later version.

GPL-3.0 was chosen for forward compatibility with the FFmpeg ecosystem planned for Milestone 2.
A `THIRD_PARTY_NOTICES.md` ships with the first bundled dependency (Qt is dynamically linked,
which satisfies its LGPL-3.0 terms; the Noto/OFL emoji font planned for Milestone 6 is not
bundled yet).

## Repository settings

The canonical repo description, topics, and recommended settings live in
[docs/repo-settings.md](./docs/repo-settings.md).
