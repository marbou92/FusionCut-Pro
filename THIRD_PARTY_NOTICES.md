# Third-party notices

FusionCut Pro is GPL-3.0 software (see `LICENSE`). It dynamically links
Qt 5 (LGPL-3.0, satisfied by dynamic linking) and the FFmpeg libraries
(LGPL-2.1+ / GPL depending on the build, dynamically linked and shipped
as separate DLLs in the portable distribution).

## Nothing is bundled inside this repository

The source tree carries no third-party assets: no fonts, no images, no
binary blobs. Test media is generated at runtime, and the emoji test
suite runs against hand-built synthetic font bytes (see
`tests/emoji_fixture.h`).

## Fonts read at RUNTIME (not distributed)

Color-emoji rendering reads font files that are ALREADY INSTALLED on
the user's machine - the app discovers the emoji-capable faces (Segoe
UI Emoji on Windows, Apple Color Emoji on macOS, Noto Color Emoji and
friends on Linux) and parses them in place. Those fonts are licensed
by their vendors to the user (Segoe UI Emoji ships with Windows; Apple
Color Emoji with macOS; Noto Color Emoji under the SIL Open Font
License 1.1 when installed separately) and are never copied,
redistributed, or modified by this software. Selecting one in the app's
emoji-font picker is no different from selecting it in any other
font-choosing application.

The portable distribution ships no font of its own; with no emoji font
installed on the machine, emoji in text clips fall back to the
platform's font stack.
