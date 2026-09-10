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
- **Text animations:** every text clip carries an entrance and an exit
  (fade, slide from any edge, pop with overshoot, typewriter with a
  cluster-aligned reveal, wipe along any edge; durations in frames,
  one shared direction). The state math is pure core, so preview and
  export evaluate identical animation states; animations persist
  additively in the project file (older files round-trip unchanged).
- **Captions:** strict SubRip (.srt) parser + canonical writer; import
  turns cues into styled text clips on the text track, export writes
  the timeline's text clips back out (markup-stripped, byte-stable).
- **Audio mixing:** the timeline's audio-track clips flatten into
  mixer spans (track gain/pan/mute/solo baked in, per-clip linear
  fades, seconds-mapped) and mix through a windowed, rolling-decoder
  engine at any requested rate/channel count. The Mixer panel carries
  real strips (faders, pan, mute/solo) plus a master fader; audio
  clips get a fade editor in Effect Controls. Playback streams the mix
  through the machine's audio device (WASAPI shared mode,
  event-driven; the device's consumed position is the playhead clock,
  scrubbing re-anchors); the export muxes the identical mix as an AAC
  track alongside the H.264 video. Importing a file with sound places
  video and audio clips together; audio-only files become audio clips.
  A source that will not decode mixes as silence and says so. Track
  strips, fades, and master persist additively in the project file.
- **Color emoji from the machine's own fonts:** no font is bundled.
  The app discovers the emoji-capable fonts installed on the PC -
  scanning the platform font directories AND the Windows font
  registrations (HKLM/HKCU), so relocated installs are found too. A
  face qualifies by mapping a representative battery of emoji
  codepoints (a font tagged with bitmap tables but empty behind them
  does not), and the picker lists each family exactly once (path,
  normalized-family-key, and byte-identical-content deduplication).
  Faces without bitmap tables but real emoji coverage - Segoe UI
  Symbol, Symbola, the outline Noto Emoji - list as *outline* picks:
  choosing one routes emoji clusters through that font's monochrome
  artwork via the platform text stack. Color faces
  (Segoe UI Emoji, Noto Color Emoji, JoyPixels... CBDT/CBLC+GSUB
  faces; Apple Color Emoji `.ttc` collections with sbix strikes) and
  the Text panel's emoji-font picker selects which one renders -
  switching fonts switches the emoji artwork set (Microsoft, Apple,
  Google). The selected file is parsed directly (bitmap strikes, cmap,
  GSUB ligatures, sbix records incl. 'dupe'/'flip' indirection), so
  full-color emoji appear on every Windows version, 7 included, with
  no platform emoji support required. Sequences without GSUB rules
  render member-by-member, never half a flag; with no emoji font
  installed, emoji fall back to the platform font stack. Clusters stay
  atomic in the layout engine and scale with the run's pixel size; the
  pick persists per machine (not per project).
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
- Playback survives source switches: crossing a cut into a clip from
  another file no longer stops the transport - the audio preview
  keeps running (it is the clock), the async open completes, and the
  queued seek re-resolves at the LIVE audio position; the frame the
  open emits (source frame 0) is cached for the thumbnail but never
  composited while the re-seek is pending, and a failed open drops
  the queued seek instead of wedging the switch state.
- The sequence is authoritative over media probes: loading media no
  longer re-times the project (the sequence fps is adopted from the
  FIRST probe of a fresh session - or from a loaded project - and
  then locked, so importing a 60 fps file into a 24 fps project does
  not retime every clip), probing no longer dirties an untouched
  project, and Open Project syncs the shell's fps to the model.
- Playback follows the SEQUENCE extent (timeline duration when clips
  exist, else the loaded media's): the clock, the restart-from-start
  check, and frame stepping clamp to the sequence, so a sequence
  longer than its first-opened source no longer stops early and a
  shorter one no longer plays into black.

### Tests

Ten ctest suites, ~40,000 checks total: core (88), timeline (130),
audio (1714), effects (2643), transitions (4531), project (131),
text (453), emoji (365), srt (83), media (29766). Synthetic media is
generated at runtime; the emoji suite pins its expectations against
hand-built synthetic font fixtures (nothing font-shaped lives in the
repo).

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
- `swr_alloc_set_opts2` takes the OUTPUT layout/format/rate FIRST -
  the proxy's resampler had them swapped. Invisible for years because
  every synthetic source was 48 kHz stereo (a pure pass-through hides
  any direction); found the moment the audio decoder fed a real
  conversion (a segfault inside libswresample).
- The emoji name-table decoder advanced a surrogate pair by three
  bytes instead of four, re-reading the pair's second byte as a fresh
  unit - every astral codepoint after the first corrupted. Invisible
  because no real emoji font carries astral characters in its FAMILY
  name; caught by a synthetic name-table test.
- Light font probing needs the REAL file length: table records
  routinely point past any short head, so offset bounds checks use
  the file size, not the bytes actually read.
- The portable (MinGW) build died on `audio_preview.cpp`, and the
  failure class explains why only it could: the WASAPI and `<mutex>`
  includes sat INSIDE `namespace fc`, pasting the entire headers into
  the project namespace (`fc::std::mutex`, `fc::IUnknown`), after
  which gcc resolves std names through `fc::std` first - where half
  of them do not exist. No Linux build could ever see it (the
  platform gate is closed there), and the file had never reached a
  MinGW compile before: it was the first new Windows-API source since
  the winmock offline-audit discipline existed, and that discipline
  was not extended to it. Fixed by hoisting every system include to
  global scope; the platform gate became `_WIN32` (the compiler's own
  macro) instead of `Q_OS_WIN` (which only a Qt header can define -
  the unused `QString` include that incidentally provided it is gone,
  and platform detection must not hang on an accidental include);
  winmock now carries the WASAPI surface so this file's Windows path
  is audited offline together with the crash-handler/diag/shim
  files, and a namespace-pollution check on the object file (zero
  `fc::std` / COM symbols) backs it.

### Known scope

- Quick Mode AI one-tap actions are placeholders.
- Audio crossfades across a CUT (dissolving two adjacent audio clips
  automatically) are not modeled yet; per-clip fades cover the
  click-free case.
- Audio effects (EQ, compression) do not exist yet - the effect stack
  is a video (RGBA) concept.
- No audio meters on the mixer strips yet.
