# Contributing to FusionCut Pro

Short version: build the core, keep the tree byte-identical under the pinned
clang-format, ship small focused commits.

## Prerequisites

| Layer | Needs |
| --- | --- |
| Core library + 9 test suites | Any C++17 compiler (GCC / Clang / MSVC), CMake >= 3.16 - no third-party dependencies |
| Media layer + media suite | FFmpeg dev libraries via pkg-config (`libavformat`, `libavcodec`, `libavutil`, `libswscale`, `libswresample`); the code compiles against both the FFmpeg 4.4 and 5.1+/7.x API generations |
| Qt desktop app | Qt 5.12+ (5.15 is what CI and the portable build use), Widgets component |

Windows: MSYS2 MinGW64 is the supported toolchain (it is what the portable
zip ships); see "Building from source" in the README for the package list.

## Build

Plain CMake:

```bash
cmake -S . -B build -DFC_BUILD_APP=ON -DFC_BUILD_MEDIA=ON   # or OFF to skip Qt/FFmpeg
cmake --build build --parallel
```

Or the presets in `CMakePresets.json` (need CMake >= 3.21) - all three
configure into `build/` in Release:

```bash
cmake --preset core    # dependency-free core + core test suites (no FFmpeg/Qt)
cmake --build --preset core
cmake --preset media   # core + media layer (needs FFmpeg dev)
cmake --preset app     # everything (needs Qt 5.15 + FFmpeg)
```

`FC_BUILD_APP=ON` requires `FC_BUILD_MEDIA=ON` (the app links fc_media).

## Tests

```bash
ctest --test-dir build --output-on-failure        # or: ctest --preset core
```

The core suites (`core`, `timeline`, `effects`, `transitions`, `project`,
`text`, `emoji`, `srt`, `audio`) build and run with no FFmpeg installed -
run them before every commit. The `media` suite needs FFmpeg at build time;
it generates its media at runtime, so the repo carries no binary fixtures.

## Formatting (CI enforces this byte for byte)

The tree is formatted with **clang-format 22.1.8 exactly** - other versions
produce different output, so CI and local runs must use the same one:

```bash
pip install --user --break-system-packages clang-format==22.1.8

# the exact command CI runs (any byte difference fails the job):
find src tests -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.c' \) -print0 \
  | xargs -0 -r clang-format --dry-run --Werror

# ...and the -i form to format in place before committing
```

## Continuous integration

`.github/workflows/ci.yml` runs 7 jobs on every push/PR: format check; core
tests on Ubuntu and Windows; core tests under ASan+UBSan (Debug); media
tests on Ubuntu (FFmpeg 6.1), in an Ubuntu 22.04 container (FFmpeg 4.4 - the
API generation the Windows 7 build ships with), and on Windows MinGW64
(MSYS2 FFmpeg - the portable toolchain); plus the full Qt app build on
Ubuntu (ccache-cached). Third-party actions are pinned to commit SHAs.

`.github/workflows/portable-build.yml` packages the Windows x64 portable
zip. Its build job runs with a read-only token; a separate release job - the
only one with `contents: write` - attaches the zip to a GitHub Release on
`v*` tags.

## Commit discipline

- Small, focused commits - one logical change each. (A delivery may still
  contain several commits when the sets of touched files are disjoint.)
- Conventional, lowercase-imperative subjects with a scope:
  `fix(media): ...`, `feat(app): ...`, `test(core): ...`, `chore(ci): ...`,
  `docs: ...`. The subject states WHAT changed, not where you got stuck.
- Never mix reformat-only edits into a functional change.
- CI must pass before delivery; the two workflows are the project's gate,
  not a suggestion.
