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
- **Deterministic engine core** - fixed block pools and LRU frame-cache eviction, never guesswork
- **Full-color emoji in titles** - a bundled Noto Color Emoji font renders through the app's
  own bitmap-table parser, so emoji look identical on Windows 7 through 11
- **Portable-first distribution** - every build is a self-contained zip straight from CI
- **Fully tested engine primitives** - timecode, caching, effects, transitions, text, emoji,
  and allocation units run in CI on every push (Ubuntu + Windows)

## Status

FusionCut Pro is in **pre-alpha** (version 0.1.0, unreleased). The engineering foundation and
the core editing feature set are in place and under active development:

- Media I/O (FFmpeg probe/decode/proxy), the dual-mode UI, and the multi-track editing core
  (trim/split/ripple/roll, magnetic moves, audio mixer)
- 52 CPU effects with per-parameter keyframes, 36 cut transitions, the Color panel
- Deterministic project files (`.fcp`), export to H.264 MP4 through the exact program pipeline
- Rich-text titles with word wrap, alignment, background boxes, effect stacks - and
  full-color emoji

Planned next: text animations, caption import (SRT), timeline audio mixing, AI-assisted
features (face tracking, background removal, auto-captions), and the 1 GB RAM budget audit.

> **Runtime crash reporting:** a built-in crash handler captures access violations, uncaught
> C++ exceptions, CRT misuses, pure-virtual calls, and POSIX signals, then writes a structured
> report (`crash-logs/FusionCutPro-crash-<timestamp>.log` next to the executable) with the
> exception code, the **faulting module + offset** (one Toolhelp32 snapshot feeds the blame
> line, the per-frame module+RVA-annotated stack backtrace, the module list, and the crash
> dialog), the AV read/write/execute type + target address, the crashing thread id, an x64
> register dump, and the boot trace - the dialog the user sees names the culprit DLL
> directly. The Windows VEH is registered at static-init time (before `main()`), which
> widens the VEH's coverage to runtime crashes that occur after `.CRT$XCU` but does **not**
> cover loader-phase failures (the Windows "0xc0000005 unable to start" dialog is the
> loader's own failure, shown before any user code runs).
>
> **Loader-phase diagnostic, baked in:** the separate `fcp-loader-check.exe` is RETIRED -
> its two-phase diagnostic now runs from the exe itself. Run
> `FusionCutPro.exe --diag` if the app will not start: PHASE 1 maps the exe's PE import
> tree from outside (no DllMain) and names any DLL the loader cannot map; PHASE 2 launches
> a fresh copy of the app under a built-in mini-debugger (Windows debug API, kernel32
> only) and records every DLL load and every exception - including faults inside DllMain /
> static initializers that Windows Error Reporting never sees (which is why the "0xc0000005
> unable to start" dialog produces no Event Viewer entry). It names the faulting module +
> offset and writes `FusionCutPro-diag-<timestamp>.log` with the full module load trail.
> `FusionCutPro.exe --crash-test` writes a synthetic runtime report to verify the
> in-process pipeline on a clean machine. The loader-phase root causes the watch exposed
> are fixed (see the Windows 7 / 8.x compatibility note below): the bundled
> `api-ms-win-core-synch-l1-2-0.dll` shim (WaitOnAddress api set) and the stubbed
> `librsvg-2-2.dll` (Windows 8+ import); the build is CONFIRMED BOOTING end-to-end on a
> real Windows 7 machine (full seven-stage boot trace + workspace rendering), so `--diag`
> is the safety net rather than the daily driver.
>
> **Effects:** every timeline clip carries an effect stack processed in place on the decoded
> RGBA frame. 52 CPU effects ship across Color (brightness, contrast, saturation, vibrance,
> hue, temperature, tint, exposure, gamma, the 9-knob Color Correction grade, channel gain,
> colorize, duotone, faded film), Tone (levels, posterize, threshold, solarize, invert,
> highlights, shadows, gain, Bayer dither), Filter (black & white, sepia, vignette,
> deterministic film grain, pixelate, chromatic aberration, mirror, flip, glow, scanlines,
> halftone), Blur & Sharpen (box, gaussian, unsharp sharpen, motion blur H/V, radial blur),
> Distort (wave, ripple, fisheye, tile), Generate (color bars, gradient, grid, noise) and
> Stylize (find edges, emboss, thermal, glitch). Browse them in the Effects panel
> (searchable), double-click to add to the selected clip, edit parameters live in Effect
> Controls - the program monitor re-renders instantly from the cached raw frame, and clips
> with effects show an amber `fx` badge in the timeline. Effects are deterministic
> (fixed-width integer math, seeded hash noise) and unit-tested with reference pixels (the
> `effects` ctest suite).
>
> **Keyframes:** every Number parameter can carry a keyframe track in CLIP-RELATIVE
> frames. `paramAt` resolves linear interpolation inside the bracketing pair and clamps
> outside it; the program monitor, the export, and the panels all evaluate parameters at
> the same clip-relative position, so the value you scrub is the value that renders.
> Effect Controls shows a keyframe diamond on every Number param - click it at the
> playhead to toggle a keyframe; when a param is keyframed, sliders display the resolved
> value and edits write the keyframe at the playhead. Splitting a clip re-bases the right
> half's tracks with its new in-point (the left half keeps its full track
> non-destructively).
>
> **Transitions:** every cut between two adjacent clips can carry a transition - 36 of
> them across Dissolve (cross, dip to black/white, additive, film-grain, blur), Wipe
> (directional, corners, iris box/circle/diamond, clock, blinds, checker, barn doors),
> Slide, Push, and Zoom (in/out/through). Browse them in the Transitions panel and
> double-click to drop one on the selected clip's cut (or press Ctrl+D for the default
> cross dissolve); the timeline shows each transition as a green X marker spanning its
> window - click it to edit the duration or remove it in Effect Controls. The program
> monitor composites the cut LIVE: the outgoing clip keeps playing while the incoming
> clip's first frame blends in (the no-overlap window model - nothing reflows, no content
> is lost, every transition is endpoint-exact and deterministic). The transitions engine
> + model semantics are unit-tested in the `transitions` ctest suite.
>
> **Color panel:** a fourth left-dock tab - the colorist's grade view. Nine sliders
> (exposure, contrast, highlights, shadows, saturation, vibrance, temperature, tint, hue)
> edit the selected clip's combined Color Correction instance, auto-created on the first
> touch and re-rendered live from the cached raw frame. Reset Grade removes it. Keyframed
> corrector params display and edit at the playhead like every other effect.
>
> **Project persistence:** File > Open Project (Ctrl+O) / Save Project (Ctrl+S) / Save
> Project As (Ctrl+Shift+S). Projects are deterministic JSON (`.fcp`): tracks, clips with
> in/out points and effect stacks (parameters AND keyframes), cut transitions, text
> documents, the fps - media stays referenced by path (nothing is embedded, 1 GB budget).
> The parser is strict: a malformed or schema-violating file fails whole, never partially;
> ids, stacks, and transitions load back exactly; effects or transitions from a NEWER
> catalog round-trip untouched (they never process, they never crash). The window title
> tracks the dirty state; closing with unsaved changes prompts Save/Discard/Cancel.
>
> **Export:** File > Export Media (Ctrl+M) renders the timeline through the EXACT
> program-monitor pipeline - every effect (keyframes interpolating per frame), every cut
> transition, and every text clip composited live - to an H.264 MP4 (MPEG-4 fallback
> encoder) at the project fps with CRF quality control and your choice of resolution.
> The job runs on a background thread with a progress dialog and Cancel; partial files
> are cleaned up on failure or cancellation. Video only for now: timeline audio mixing
> (multi-track summing, crossfades) ships later.
>
> **Text:** titles are first-class timeline citizens. Text tracks (T1, created at the very
> top of the timeline when you add your first title) host TEXT clips - generated frames,
> no source media. The clip's rich-text document (a flat sequence of styled UTF-8 runs) is
> laid out by a deterministic integer-math engine - word wrap, hard splits for over-wide
> words, per-line baseline alignment across mixed sizes, left/center/right block
> alignment, an optional background box - and rasterized into a transparent layer that
> composites ON TOP of the video stack in the program monitor, Quick Mode, and the export
> (per-pixel source-over blending, also integer math). The Text panel is a real rich-text
> editor: type, then style the SELECTION (family / pixel size / bold / italic /
> underline / color - no selection styles what you type next), align the block, place it
> with X / Y / Width, toggle the background. Add titles with Title > Add Text Clip
> (Ctrl+T); every edit re-renders the program monitor instantly. Text clips carry effect
> stacks and keyframes like every other clip (glow on a title works - the stack runs on
> the text layer), they split/trim/roll/move/ripple-delete like every other clip, and
> they save/load in the project file (format 1, additive: older projects load unchanged).
> The engine is unit-tested in the `text` ctest suite (layout numbers are pinned with
> synthetic font metrics; only glyph pixels are platform-dependent).
>
> **Color emoji:** emoji in text render from the bundled **Noto Color Emoji** font
> (SIL OFL 1.1 - see `THIRD_PARTY_NOTICES.md`), parsed directly from its CBDT/CBLC
> bitmap tables by the app's own engine - no platform emoji support required, so the
> same full-color bitmaps appear on Windows 7, 8, 10, and 11. Sequences shape through
> the font's own GSUB rules: ZWJ chains (families, professions, couples), flags,
> keycaps, and skin-tone modifiers all render as single glyphs; U+FE0F selects the
> emoji presentation; hearts and other text-default symbols render as text unless
> followed by U+FE0F (Unicode's own rule). Emoji are layout citizens like any glyph -
> word wrap keeps them whole, they scale with the run's pixel size, and their metrics
> fold into the line height. The font file ships next to `FusionCutPro.exe` in the
> portable zip; without it the app still runs, and emoji fall back to the platform
> font. The parser and shaping policy are unit-tested in the `emoji` ctest suite.

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
> from `src/app/api_set_synch.c`) that provides REAL implementations of
> that API set on top of Windows-7-era kernel32 primitives - so those
> Windows versions start AND run correctly. The DLL search order tries
> the application directory before System32, so the shim wins the bind
> on Win7/8.x; on Windows 10/11 the OS resolves the name natively and
> the file is ignored. Leave it in place.
>
> **Windows 7 / librsvg:** the MSYS2 `librsvg-2-2.dll` in the FFmpeg
> dependency tree is built with a modern Rust toolchain that
> statically imports `kernel32!GetSystemTimePreciseAsFileTime`
> (Windows 8+ only) - on Windows 7 the loader aborts with
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

Seven suites run in the core-only configuration: **core** (88 checks:
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
rejection, parse atomicity) and **text** (381 checks: UTF-8 decoding
across the valid and invalid classes, layout placement math on
synthetic metrics - wrap/alignment/baseline/box, source-over
reference pixels, the text-clip timeline mutator battery, and the
text-document project round-trip) and **emoji** (218 checks: the
bundled font's strike/cmap/GSUB tables decoded against pinned
byte-stable facts, cluster shaping - singles, FE0F presentation,
flags, keycaps, skin tones, ZWJ chains - bitmap record decoding with
PNG-signature verification, strike-scaling math, malformed-input
robustness, and the layout engine's cluster-atomicity rule) and
**media** (400 checks: synthetic media is generated at runtime - no
binary assets in the repo beyond the font - then probed, decoded
frame-accurately with color-order assertions, seeked, and transcoded
to 360p proxies with geometry, audio, progress, cancellation, and
no-upscale verification, plus the export pipeline: encode/probe/decode
round-trips, per-frame progress, both cancellation paths) when the
media layer is enabled.

## Project layout

```
.
├── .github/workflows/     # ci.yml (lint + core/media/Qt matrix) + portable-build.yml
├── cmake/                 # CMake templates (version.h.in)
├── docs/                  # repository metadata; specs and wireframes land here
├── resources/fonts/       # NotoColorEmoji.ttf - bundled color-emoji font (SIL OFL 1.1)
├── src/
│   ├── app/               # Qt 5.15 desktop shell (Pro/Quick workspace host)
│   ├── core/              # dependency-free engine primitives (fc_core)
│   └── media/             # FFmpeg I/O layer (fc_media): probe, decode, proxy
├── tests/                 # core + media suites, shared harness, synthetic media generator
├── CMakeLists.txt
├── LICENSE                # GPL-3.0
├── THIRD_PARTY_NOTICES.md # bundled-component licenses (Noto Color Emoji / OFL)
└── VERSION                # single source of truth for the version number
```

## License

Copyright (C) 2026  FusionCut Pro contributors.

This program is free software: you can redistribute it and/or modify it under the terms of the
[GNU General Public License](./LICENSE) as published by the Free Software Foundation, either
version 3 of the License, or (at your option) any later version.

GPL-3.0 was chosen for forward compatibility with the FFmpeg ecosystem (dynamically linked,
which also satisfies Qt's LGPL-3.0 terms). The bundled Noto Color Emoji font is licensed
separately under the SIL Open Font License 1.1 - see
[THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md).

## Repository settings

The canonical repo description, topics, and recommended settings live in
[docs/repo-settings.md](./docs/repo-settings.md).
