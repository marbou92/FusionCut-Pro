# Repository Settings

Canonical metadata for `marbou92/FusionCut-Pro`. Paste these into GitHub once:
**repo page -> gear icon (About) -> fill fields**.

## Description

```
FusionCut Pro - lightweight dual-mode video editor (Premiere-style Pro Mode + CapCut-style Quick Mode) targeting Windows 7+ with a 1 GB RAM budget. C++17 - Qt 5.15 - FFmpeg. Portable builds straight from GitHub Actions.
```

(218 characters - fits GitHub's 350-character limit.)

## Topics (tags)

Copy-paste all 20 into the About topics field (GitHub separates on spaces/enter,
accepts up to 20, lowercase only):

```
video-editor video-editing cpp cpp17 qt qt5 ffmpeg windows cross-platform lightweight portable premiere-alternative capcut-alternative timeline-editor color-grading face-tracking background-removal open-source gpl-v3 content-creators
```

## Recommended settings

| Setting | Value | Why |
| --- | --- | --- |
| Actions -> General | Allow all actions | Required for `msys2/setup-msys2` and `softprops/action-gh-release` |
| Actions -> Artifact retention | 14 days (or default) | Portable zips also live forever in Releases once tagged |
| Branches -> Add branch protection rule for `main` | Require status checks: `Core tests (ubuntu-latest)`, `Core tests (windows-latest)` | Keeps the engine green on both compilers before merge |
| Tags -> v* protection rule (optional) | Restrict to maintainers | Release tags drive the portable Release pipeline |
| Default branch | `main` | Already set |

## Badges (already wired in README)

- CI: `https://github.com/marbou92/FusionCut-Pro/actions/workflows/ci.yml/badge.svg?branch=main`
- Portable: `https://github.com/marbou92/FusionCut-Pro/actions/workflows/portable-build.yml/badge.svg`

## First release checklist (after CI is green)

1. Actions -> Portable Build -> Run workflow -> confirm the artifact builds.
2. `git tag v0.1.0 && git push origin v0.1.0` -> Release `v0.1.0` is created with the portable zip attached.
3. Release -> Edit -> paste the `0.1.0` entry from CHANGELOG.md as the release notes.
