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
| M4 - Editing core | Multi-track timeline, trim/split/ripple, audio mixer | Phase 1 shipped (v0.4.0) |
| M5 - Effects & color | Effects pipeline, 50+ effects, 30+ transitions, color panel | Planned |
| M6 - Text engine | Rich text, bundled color-emoji renderer, animations, captions | Planned |
| M7 - AI features | Face tracking, background removal, auto-captions | Planned |
| M8 - Optimization & polish | 1 GB RAM budget audit, shortcuts, export presets | Planned |

## System requirements (target)

| | Minimum | Recommended |
| --- | --- | --- |
| OS | Windows 7 SP1+ (32/64-bit) | Windows 10/11 (64-bit) |
| RAM | 1 GB | 4 GB |
| CPU | Intel Core 2 Duo / AMD Athlon 64 X2 | Intel i5 / AMD Ryzen 5 |
| Storage | 500 MB + project space | 2 GB SSD |
| Graphics | DirectX 9 compatible | DirectX 11 with GPU acceleration |

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

Two suites run: **core** (88 checks: rational frame rates, timecode
parse/format/math, LRU eviction, memory-pool ownership/alignment) and
**media** (317 checks: synthetic media is generated at runtime - no binary
assets in the repo - then probed, decoded frame-accurately with color-order
assertions, seeked, and transcoded to 360p proxies with geometry, audio,
progress, cancellation, and no-upscale verification).

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
