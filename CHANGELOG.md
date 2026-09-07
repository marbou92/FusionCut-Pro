# Changelog

## 0.1.0 (unreleased - pre-alpha)

The full feature set developed so far. Nothing is released yet: the
version stays at 0.1.0 until the first public build.

### Engine

- **Media I/O (FFmpeg):** probe, frame-accurate decode, 360p proxy
  generation for smooth low-RAM playback. Compiles against the FFmpeg
  4.4 and 5.1+/7.x API generations; the portable build ships the
  FFmpeg 8 runtime.
- **Deterministic core primitives:** rational frame rates and
  timecode math, fixed-block memory pools, LRU frame-cache eviction -
  integer math and pinned unit tests, identical on every platform.
- **Timeline model:** multi-track video/audio/text lanes, trim, split,
  roll, ripple delete, magnetic move with overlap rejection,
  topmost-wins compositing resolution. Every mutation preserves the
  transition invariants (adjacency, duration clamps, re-targeting on
  split).
- **Effects:** 52 CPU effects across 7 categories (Color, Tone,
  Filter, Blur & Sharpen, Distort, Generate, Stylize), per-clip
  stacks, per-parameter keyframe tracks with linear interpolation
  evaluated identically in preview, export, and panels.
- **Transitions:** 36 endpoint-exact cut transitions (dissolves,
  wipes, slides, pushes, zooms) on the no-overlap window model: the
  outgoing clip plays live while the incoming clip's held first frame
  blends in - no reflow, no content loss.
- **Text engine:** rich-text documents (styled UTF-8 runs), greedy
  word wrap with hard splits, per-line baseline alignment, block
  alignment, background boxes - all integer-math and deterministic.
  Text tracks and generated text clips behave as full timeline
  citizens (split/trim/roll/move/ripple, effect stacks, keyframes).
- **Color emoji:** the bundled Noto Color Emoji font (SIL OFL 1.1) is
  parsed directly - CBLC/CBDT bitmap strikes, cmap, and the GSUB
  ligature rules - so emoji sequences (families, flags, keycaps, skin
  tones, VS16 presentation) render as full-color bitmaps on every
  Windows version, 7 included. Emoji clusters are atomic in the
  layout engine and scale with the run's pixel size.
- **Project persistence:** strict deterministic JSON (`.fcp`) with
  byte-identical round-trips; ids, effect stacks, keyframes,
  transitions, and text documents load back exactly; newer-catalog
  ids round-trip untouched.
- **Export:** H.264 MP4 (MPEG-4 fallback) through the exact program
  pipeline - effects with per-frame keyframe interpolation, live
  transitions, text/emoji compositing - with CRF control, progress,
  and cancellation.
- **Crash reporting and the baked-in loader diagnostic:** a VEH-based
  runtime crash reporter with faulting-module attribution and a boot
  trace, plus the `--diag` two-phase loader/startup diagnostic (PE
  import-tree walk + debug-API watch) and the `--crash-test` synthetic
  report. Confirmed booting end-to-end on a real Windows 7 machine.

### Application

- Dual-mode workspace: Pro Mode dockable panels (Project, Effects,
  Transitions, Color, Text, Effect Controls, Mixer) vs. Quick Mode
  one-tap flow.
- Timeline panel with drag-move ghost, magnetic snapping, razor,
  ripple toggle, fx badges, text T-badges, and clickable transition
  markers.
- Program monitor composites the full pipeline live (effects,
  transitions, text-over-video, text-over-black); the export renders
  the identical stack.
- File > Open/Save/Save As project, Export Media, Title > Add Text
  Clip, Ctrl+D default transition; dirty-state title and close prompt.

### Tests

Eight ctest suites, ~8500 checks total: core (88), timeline (130),
effects (2643), transitions (4531), project (79), text (381),
emoji (218), media (400). Synthetic media is generated at runtime;
the emoji suite pins its expectations against the committed font
bytes.

### Notable engineering finds along the way

- A dangling reference after `clips_.insert` silently dropped
  re-targeted transitions on split (caught by the model battery).
- The exporter wrote packets unrescaled into the mov muxer's own
  timescale, making a 2.5 s export probe as 0.002 s (fixed with
  `av_packet_rescale_ts`).
- The emoji font's strike advances (136 px at ppem 109) match its
  hmtx math (2550/2048 em) exactly - the bitmap scaler and the font's
  own metrics agree by construction.
- Index-subtable format 1 keeps its first glyph exactly at
  `sbitOffset`; the PNG signature check is what separates "missing
  glyph" from corruption in that convention.

### Known scope

- Video-only export; timeline audio mixing (multi-track summing,
  crossfades) ships later.
- Text animations and caption (SRT) import are planned.
- Quick Mode AI one-tap actions are placeholders.
