# Changelog

## 0.1.0 (unreleased - pre-alpha)

The full feature set developed so far. Nothing is released yet: the
version stays at 0.1.0 until the first public build.

### Engine

- **Media I/O (FFmpeg):** probe, frame-accurate decode, 360p proxy
  generation for smooth low-RAM playback. Compiles against the FFmpeg
  4.4 and 5.1+/7.x API generations; the portable build ships the
  FFmpeg 8 runtime. The proxy generator now rescales packet
  timestamps against the STREAM's time base at mux time (the same
  bug class the exporter fixed: the muxer may pick a different time
  base in write_header, which made proxies probe at a fraction of
  their real duration), and checks the returns it silently ignored
  before (stream-info discovery, encoder/decoder flushes, frame
  writability, FIFO reads) so a failing step reports a real error
  instead of limping on. The export frame callback receives the full
  int64_t frame index its signature declares - the loop used to narrow
  the counter to int, silently wrapping past 2^31 frames. The proxy
  now owns its output AVIOContext through RAII (every error path used
  to leak the handle, which also made the partial-file cleanup fail
  on Windows), silence-pads its audio tail so the last partial AAC
  frame flushes, checks its FIFO growth (a failed realloc used to be
  ignored and limped on), and its partial-file cleanup removes UTF-8
  paths through the wide API on Windows (the narrow CRT reads bytes in
  the system code page and silently missed non-ASCII destinations). The
  probe ignores ATTACHED_PIC video streams (a music file's embedded
  cover art used to classify the whole file as a one-frame video and
  route it into the video pipeline), maps a NOPTS container duration
  to unknown instead of a garbage negative, no longer builds a
  std::string from a NULL sample-format name (undefined behavior on
  streams whose format was never resolved), and reads the pixel format
  of YUV420P streams correctly (format index 0 - the single most
  common pixel format - was mistaken for "no format"; the unknown case
  is the negative default, not zero). The video decoder probes the
  context it already opened (a full open + find_stream_info pass ran
  twice per source; find_stream_info is the expensive one - it reads
  and parses packets). Encoders now set AV_CODEC_FLAG_GLOBAL_HEADER
  for MP4 targets: without it libx264 keeps its headers in-band and
  file validity silently depends on movenc self-healing at trailer
  time, while the MPEG-4 fallback writes an esds with an EMPTY
  DecoderSpecificInfo - a malformed file some players reject outright
  (the exporter, the proxy generator, and the test fixtures all set
  it now). The proxy generator guards its unchecked allocations (a
  null output stream or FIFO dereferenced right away - the exporter
  already checked its own), refuses an encoder-reported frame_size of
  0 (the FIFO drain loop would spin forever; the 1024 fallback the
  exporter uses applies), and treats any encoder receive error other
  than EAGAIN/EOF as the real failure it is - a dying encoder used to
  end the receive loop "cleanly", silently truncating the output file
  and still committing it. Its per-frame resample buffer is pooled
  across the job (a malloc/free pair per decoded AAC frame was pure
  churn), its stream-info failure names the source file, and a
  caller cancel now reports an empty error - the same contract as the
  exporter - so the app can name "cancelled" instead of a bare
  "Proxy failed:" line. The audio decoder's "stream discovery
  failed" error carries the path and the FFmpeg error string. The
  audio decoder stamps each chunk's pts on the OUTPUT timeline -
  anchored at the source pts but never allowed to run backwards:
  resampling upward makes one input frame's output span slightly
  LONGER than that frame's own pts step (1024 samples at 44.1 kHz
  turn into 1114-1115 at the 48 kHz mixer target), so raw source pts
  started the next chunk up to ~9 us before the previous one ended -
  backwards jumps on a large fraction of chunks that broke the
  chunk-to-chunk continuity the window mixer's coverage logic and
  the upsample regression battery rely on.
- **Deterministic core primitives:** rational frame rates and
  timecode math, fixed-block memory pools, LRU frame-cache eviction -
  integer math and pinned unit tests, identical on every platform.
  Timecode equality compares the FULL rate identity: a drop-frame
  timecode no longer equals its non-drop twin (same ratio, same frame
  count) even though the two present differently (";FF" vs ":FF").
- **Timeline model:** multi-track video/audio/text lanes, trim, split,
  roll, ripple delete, magnetic move with overlap rejection,
  topmost-wins compositing resolution. Every mutation preserves the
  transition invariants (adjacency, duration clamps, re-targeting on
  split). Setting a track's lock/mute/solo to the values it already
  holds is a true no-op - the document revision stays put, so a
  keyboard tour over the tracks no longer marks an untouched project
  dirty. Trims, rolls, and clip additions reject mutations that would
  collapse a clip's timeline duration below one frame: at a rate > 1
  a positive SOURCE extent can round down to a 0-frame "zombie" that
  stays in the model invisible, un-splittable and un-movable (a
  hand-edited rate-3 file could create one). addClip enforces the
  lane's non-overlap invariant exactly like moveClipTo and
  addTextClip always have, so no caller can build a state the parser
  rejects. setClipRate changes a placed clip's playback speed: the
  timeline start stays anchored while the duration rescales from the
  UNCHANGED source extent - faster rates shrink the clip and leave a
  gap, slower ones grow it into free space (an overlap with a
  neighbor is rejected exactly like any placement), and transitions
  on a boundary the change breaks are pruned like a trim would.
  Whole-model snapshots power the undo/redo: snapshot()
  copies the plain-value model and restoreSnapshot() re-assigns it
  with the revision bumped past anything the instance ever handed
  out, so a restore is always visible to revision-watching views
  even when the restored state is the older one.
- **Effects:** 52 CPU effects across 7 categories (Color, Tone,
  Filter, Blur & Sharpen, Distort, Generate, Stylize), per-clip
  stacks, per-parameter keyframe tracks with linear interpolation
  evaluated identically in preview, export, and panels. Parameter
  writes clamp non-finite values deterministically: NaN cannot be
  clamped (every comparison fails) and used to slip through min/max
  into the pixel loops (lround(NaN * 255) is undefined behavior); it
  now lands on the descriptor default, and ±inf clamps like any
  out-of-range number. The box-blur family (box blur, horizontal and
  vertical motion blur) runs sliding-window running sums - O(w·h)
  per pass instead of O(w·h·r), bit-identical output (exact integer
  arithmetic; the dropped and entering taps of the clamped window
  cancel to the same sums) - radius-32 motion blur on 1080p drops
  from ~166 ms to ~30 ms per frame, and an independent naive
  reference pins the results byte-exact across shapes and radii.
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
  The resampler's output budget is now computed in OUTPUT samples via
  swr_get_out_samples (the old formula summed input-side quantities:
  every upsampling source - 44.1 kHz music against the 48 kHz mixer
  rate, the single most common input class - came out chunked, with
  ~1 ms dropouts 44 times a second, cumulative audio-ahead drift, and
  an unboundedly growing internal buffer; the 1:1 test sources could
  never see it). The resampler is flushed at end of stream by the
  decoder and the proxy generator, so the low-pass filter tail (and
  any input stranded before the fix) is emitted instead of dropped -
  the last ~21 ms of every file used to vanish. The mixer no longer
  blends samples that sit BEFORE a clip's in-point into a pull window
  (a BACKWARD seek lands on a packet that starts early; its lead-in
  leaked into exports and the preview as a click on every clip head
  without a fadeIn). The rolling decode's seek decision now reads the
  HELD chunks instead of the raw decoder position: decoded chunks
  legitimately overhang the ~10 ms pull window (AAC 21 ms, MP3 26 ms,
  FLAC ~85 ms chunks), so the old position-based rule fired a
  clear + demuxer-seek + flush + re-decode cycle on EVERY audio
  callback - correct output, real CPU burn, and a glitch risk on
  FLAC sources and fast export pulls alike. A jump too far forward
  to decode through (250 ms) still seeks. Audio spans now carry
  their clip's playback rate: a sped-up clip consumes source seconds
  proportionally faster (the tone's pitch shifts with it - the
  speed == 1 blend stays bit-identical to the pinned integer-stride
  scatter, any other rate linearly interpolates between output
  samples), so audio keeps step with its video sibling at any speed.
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
  no platform emoji support required. Collection files parse each
  sub-font from a clean slate: a face that fails mid-parse (an sbix
  face missing its metrics, a CBDT face with a broken cmap) no longer
  leaves its strikes, cmap ranges, or advances behind for the next
  face in the collection to trip over, so the surviving face's glyphs
  resolve through ITS OWN tables. Sequences without GSUB rules
  render member-by-member, never half a flag; with no emoji font
  installed, emoji fall back to the platform font stack. Clusters stay
  atomic in the layout engine and scale with the run's pixel size; the
  pick persists per machine (not per project).
- **Project persistence:** strict deterministic JSON (`.fcp`) with
  byte-identical round-trips; ids, effect stacks, keyframes,
  transitions, and text documents load back exactly; newer-catalog
  ids round-trip untouched. The loader now rejects transitions
  declared on a TEXT track (the runtime model has always refused to
  create them there - the compositor has no decoded stream to hold
  for the incoming side), closing the gap where a hand-edited or
  foreign file could smuggle one in. Saving is atomic (QSaveFile:
  temp file + rename on commit - a crash mid-write can no longer
  truncate the only copy), loading tolerates a UTF-8 BOM (Win7-era
  Notepad writes one; the parser used to reject the file as a bad
  number), and Save As only retargets the session once the write
  actually succeeded. The loader now also rejects per-track
  OVERLAPPING clips (the non-overlap invariant is a format property:
  the model mutators enforce it, the parser didn't, so a
  hand-edited file could create the order-dependent geometry the
  rest of the engine assumes away) and media clips whose duration
  collapses below one frame at their rate; effect parameter values
  and keyframe values are re-clamped to the descriptor ranges on
  load - the writer's clamp is no longer trusted (1e300 used to
  reach the pixel math as-is). Lone UTF-16 surrogates in string
  escapes now decode to U+FFFD instead of CESU-8 bytes (a surrogate
  is not a scalar value; a broken escape no longer corrupts the
  following one - the parser backs out so it decodes on its own,
  and real surrogate pairs still combine). Numbers serialize through
  a genuine shortest-round-trip loop (the first %g precision from 1
  to 17 that strtod reads back bit-exactly): NTSC rates (30000/1001
  & co.) need 16-17 digits, and the old fixed %.10g silently broke
  the header's shortest-exact promise on exactly the rates editors
  ship at.
- **Export:** H.264 MP4 (MPEG-4 fallback) through the exact program
  pipeline - effects with per-frame keyframe interpolation, live
  transitions, text/emoji compositing - with CRF control, progress,
  and cancellation. A hard source-decode failure mid-export (a
  corrupt or unsupported frame) now aborts with an honest
  "source decode failed (...): path" message instead of either
  rendering silent black frames or masquerading as "Export
  cancelled"; a clean end of source still tolerates (the tail
  renders black, exactly like broken audio mixes as silence).
- **Crash reporting and the baked-in loader diagnostic:** a VEH-based
  runtime crash reporter with faulting-module attribution and a boot
  trace, plus the `--diag` two-phase loader/startup diagnostic (PE
  import-tree walk + debug-API watch) and the `--crash-test` synthetic
  report. Confirmed booting end-to-end on a real Windows 7 machine.
  A stack overflow now takes an allocation-free emergency path (a
  pre-committed static buffer, snprintf into fixed storage, raw
  Win32 file writes) instead of growing std::string on an exhausted
  stack - the report survives where it previously almost never
  could - and the diagnostic supervisor closes every debug-event
  handle it is handed, bounds the import-descriptor walk by the
  data-directory size, and gives up on an unkillable process after
  a bounded wait instead of spinning forever.

### Application

- **Clip Speed / Duration (Clip menu, Ctrl+R):** a dialog edits the
  selected clip's playback rate - the timeline length rescales from
  the unchanged source extent (live preview in frames and seconds;
  OK is disabled when the rate would collapse the clip below one
  frame), and the preview monitor, the export, and the audio mix all
  follow the same rate-aware timeline-to-source mapping. Audio clips
  a video+audio import linked to the edited clip (same source,
  source range, placement, and current speed) can ride along so
  picture and sound stay in step; a slow-down that would not fit on
  the track is rejected with an explanation instead of overlapping
  a neighbor, and a rescaled clip sheds or clamps transitions exactly
  like a trim would.
- **Undo / redo (Edit menu, Ctrl+Z / Ctrl+Shift+Z):** every discrete
  timeline edit pushes a whole-model snapshot before it mutates (up
  to 50 steps) and undo/redo restores it with every panel and
  selection-dependent editor re-synced exactly like a project load.
  Covered: import (video+audio rider or audio-only), text-clip add,
  caption import (one step for the whole file), split, move, trim,
  roll, delete/ripple-delete, transition add/remove, track
  lock/mute/solo. A rejected edit pops its own snapshot; a lazily
  created track counts as part of the change (undo removes the empty
  lane); any new edit clears the redo branch; loading a different
  project clears the history. Continuous value-scrubbing (effect and
  color sliders, fades, transition duration, text typing) stays
  live-only for now - one snapshot per slider tick would flood the
  stack, and a coalescing pass is future work.
- The play clock advances by MEASURED wall time when no audio device
  is running: the timer interval rounds to whole milliseconds
  (1000/24 -> 41 ms), and the old fixed frame-step per tick made
  silent timelines play ~1.7% fast; a stalled tick (system sleep,
  debugger) clamps to a quarter second instead of teleporting the
  playhead on resume.
- Unmuting or un-soloing the only audible track mid-playback brings
  the mix up even when playback started with zero audio spans (the
  device used to never open, leaving silence for the whole run).
- Timeline interaction polish: the ruler seeks on click and drag;
  drags can no longer START on locked tracks (the press falls
  through to a scrub); clips narrower than three trim-edge zones
  keep a grabbable body; a lost mouse grab (system gesture, window
  switch) commits the pending drag exactly as a release would
  instead of ghosting forever; swapping the model clears stale
  selection highlights; ruler timecode labels no longer overlap at
  small zoom steps; Quick Mode's play button tracks its state in a
  property instead of comparing translated button labels, and the
  inert "Custom" aspect entry is disabled with a tooltip.
- Persistence and library hygiene: subtitle export writes atomically
  through QSaveFile (the same discipline as project save - a crash
  mid-write leaves the previous .srt intact); the media library
  hands out stable item pointers (items are heap-owned, so nothing
  invalidates on add) and Remove no longer leaks a QListWidgetItem;
  effect-controls sliders guard degenerate zero-span descriptors;
  a track-state toggle that changes nothing no longer marks the
  project dirty; and a project open's monitor-load probe no longer
  overwrites the "Project loaded" status message.
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
  queued seek re-resolves at the LIVE audio position; the switch opens
  the source QUIETLY (no open-time frame-0 decode - that first frame
  used to arrive before the re-seek guard could arm and flash at every
  cut), so the re-resolved frame is the only thing that paints, and a
  failed open drops the queued seek instead of wedging the switch
  state.
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
- The export job reads FROZEN state: before the job is queued, the
  GUI thread snapshots the timeline model, the sequence fps, the
  source->decode-path map (the media library is never touched from
  the worker thread), and the emoji font path; the audio note comes
  back captured in the completion call instead of through a member
  shared across threads. The old safety argument ("the modal dialog
  blocks all input, so the model is effectively frozen") is retired.
- The Effect Controls boolean checkbox no longer indexes the effect
  stack through an unvalidated list row (a cleared list made
  currentRow() -1 and the write became a wild index); it now uses
  the same captured-row guard as the slider and keyframe paths.
- Quick Mode is wired to the same engine as Pro Mode: the Import
  button opens the media dialog, the position slider scrubs the
  program (enabled once a real duration exists), |< and >| step one
  frame, and the slider + a timecode readout follow the playhead.
  The previously dead signals (seekRequested / stepRequested) carry
  real traffic now.
- The timeline scrolls horizontally: a scrollbar under the lanes
  (the track header column and the tool/zoom rows stay fixed), plain
  wheel scrolls while Ctrl+wheel zooms, playback keeps the playhead
  in view, and zooming anchors on the playhead instead of the left
  edge. Previously a sequence longer than the window was unreachable
  past the right border. The scroll extent is now computed in the
  panel's real units - the zoom scale is pixels-per-FRAME, and the
  old extent multiplied duration (seconds) by that scale, which kept
  the scrollbar dead at default zoom and clipped the playhead, the
  ruler ticks, and every clip past the first seconds out of view;
  transition markers hit-test scroll-aware (a scrolled click used to
  select a transition living a whole viewport away), the ruler tick
  spacing is fps-scaled to match its 70 px target, Ctrl+wheel with a
  purely horizontal delta no longer zooms out, and an fps change
  re-ranges the scroll bar.
- The export progress dialog cancels the job when closed with the
  system X or Esc (the dialog used to hide while the encode kept
  running invisibly), and quitting the app while an export runs now
  sets the cancel flag and joins the worker without a timeout - the
  old 3 s wait timed out, destroyed the thread's owner under it, and
  aborted the process on exit (a cancel-less proxy job now simply
  finishes before exit).
- Both transports keep their position display across model edits:
  every mutation funnels through the duration refresh, whose setMedia
  call used to zero the slider and timecode while paused (no worker
  round trip was coming to restore it), so deleting or trimming a
  clip no longer makes the readout jump to 00:00:00:00.
- Clip placement resolves the FIRST video lane instead of assuming
  the default layout: adding a text track inserts lanes ABOVE the
  video ones (shifting V1 off index 1), and the old hardcoded track
  index silently dropped the clip once that happened (split-at-
  playhead's no-selection default resolved the same way).
- The Project panel's metadata readout is live from import: every
  library item carries its probed summary (dimensions, codec, fps,
  duration), the duration line shows the source fps, and the tooltip
  reads the same summary. The one-liner formatter is shared with the
  decode worker's status report so the two can never drift.
- The Effects and Transitions panels share one catalog-tree builder
  (search + category grouping + filter); the panels were copy-paste
  twins of that logic and only their wording differed.
- Import lengths are computed in SEQUENCE frames from the container
  duration: the raw stream frame count is counted in the SOURCE's own
  rate (300 frames of a 30 fps file are 12.5 s at a 24 fps sequence),
  and using it verbatim made every non-24 fps MP4 import run long -
  the picture froze on the last decoded frame with silent audio while
  the playhead kept advancing (1.25x too long at 30 fps, 2.5x at 60,
  truncated at 15). The count is only a fallback now, converted
  through the probe's own fps when the container reports no duration.
- File > Exit goes through close() like the window X: QApplication::quit
  ended the event loop directly, so the unsaved-changes prompt and the
  layout save never ran and a dirty project was destroyed silently.
- The rendered text-layer caches are bounded (LRU in the program
  monitor, a small rotating map in the export job): one static text
  clip holds a full-resolution RGBA layer for its whole life, Import
  Subtitles creates one clip PER CUE, and a caption-heavy project used
  to hold hundreds of MB after a single playthrough (past 1 GB inside
  long exports - swap-death on the 1 GB target).
- A dead audio device no longer freezes the transport: the render
  thread's failure exits (endpoint removed, format change, wedged
  buffer submission) raise a failed flag the play clock checks, so
  playback falls back to the system wall clock and the status bar
  says so once; the next play attempt re-probes and recovers when the
  device is back. Previously the thread died while running() stayed
  true and every tick re-read a frozen position until Stop.
- Worker B (the transition held-frame fetcher) joins without a timeout
  at shutdown, matching the main decode thread - its old 3 s cap was
  the exact destroy-under-a-slow-open abort the main thread had
  already fixed.
- Proxy generation runs on its own worker thread: the transcode used
  to occupy the shared decode worker for minutes, freezing the program
  monitor (every frame request queued behind the job) and stalling
  library loads. A second Ctrl+P while a job runs is now a friendly
  "already running" note instead of two jobs cross-wiring their
  results, and the job's progress/done signals follow the right
  source.
- A library double-click arms a request token that rides the open
  through to its probe: only the mediaInfo carrying THAT token places
  the clip. Rapid clicks (or an import landing while a probe was in
  flight) used to let the first item's probe place the second item's
  clip with the first item's length.
- Both transports seek from groove clicks and keyboard input, not
  just handle drags (a groove click moved the handle, emitted nothing,
  and snapped back on the next programmatic update), and a USER seek
  while playing always re-anchors the audio stream - the 0.30 s drift
  guard used to treat small nudges as clock noise and snap the
  playhead back.
- Clicking a clip (select without drag) no longer dirties the project:
  the release handler emitted the origin-position move, the model
  wrote identical values and bumped the revision, and Alt+F4 on a
  freshly opened project asked to save.
- Track header L/M/S toggles mark the project dirty (the state is
  persisted - close used to lose it without a prompt) and sync the
  Mixer strips both ways (a mixer mute/solo now re-dims the timeline).
- Deleting a clip clears the Color panel too (it kept showing the dead
  clip's grade next to the already-cleared effect and text editors).
- Open Project and the export-size suggestion skip text clips when
  picking "the first clip": a title/caption carries no source, and
  opening it loaded "" into the monitor (error until the first scrub)
  or silently dropped the "Match source" export size.
- The export fetch loop skips decoded frames forward to the requested
  time: a source whose fps differs from the sequence (60 fps footage
  in a 24 fps timeline) used to trail further behind every frame
  until the seek band forced a periodic snap-forward - visible
  stutter in exports.
- A rejected transition-duration edit no longer dirties the project
  (the status message fired but the write never happened).

### Tests

Ten ctest suites, ~46,500 checks total: core (102), timeline (273),
audio (1714), effects (3833), transitions (4531), project (190),
text (453), emoji (374), srt (83), media (34,994, including the
upsample and span-rate batteries). Synthetic media is generated at runtime; the emoji suite
pins its expectations against hand-built synthetic font fixtures
(nothing font-shaped lives in the repo).

The regression armor added alongside the round-2 fixes: the drop-frame
timecode equality and the track-state no-op revision are pinned;
rate-collapse ("zombie") trims/rolls/adds and their parser-level
counterparts are rejected by test; setClipRate carries its own
battery (llround duration pins, failure pairs that leave both the
rate and the revision untouched, the extreme-rate zombie guard,
overlap rejection in both grow directions, transition pruning vs.
survival, rate-2 split/trim interplay, snapshot/restore round-trip),
the project reader/writer round-trips clip rates bitwise while
rejecting 0 and >100, and the audio mixer pins a rate-2 span by its
880 Hz Goertzel peak (with the absent 440), a rate-0.5 span by its
220 Hz peak, the timeline-extent leak check past a sped-up span's
end, a clean EOF mid-coverage, the explicit-1.0 integer path, and
defensive 0/NaN rates mixing as 1.0; per-track clip overlaps, zero-frame
clips, and text-track transitions are pinned at the PARSER level; a
TTC whose first face fails mid-parse (a ppem-200 strike with no cmap)
no longer hijacks the surviving face's bitmaps - the leak the reset
fix closed is now observable in both directions; a 44.1 kHz source
decoded at 48 kHz exercises the resampler budget's UPSAMPLING
direction (the old input-side formula only ever failed upward, and
every other test source was 48 kHz) with sample-count continuity at
±20 ms, monotone chunk pts, and a Goertzel tone check; the audio
duration assert tightened from ±0.3 s to ±50 ms (the encoded track is
whole AAC frames, so ±20 ms there would fight encoder priming); the
effects suite sweeps the entire catalog across ±1e30/±inf/NaN
parameters asserting finite, in-range storage and the header's
"alpha preserved by every effect" contract, plus single-keyframe
paramAt resolution; a size-mismatched blur-dissolve fixture that
overread its 8×8 second input as 16×16 (silently, in Release) was
caught by the new sanitizer leg and fixed. The proxy's cancellation
contract is pinned to the exporter's: false with an empty error (the
app maps that to "cancelled by caller") and no partial output left
behind. The undo/redo snapshot API is pinned by a roundtrip battery
(deep-copy independence, exact state restore, revision monotonicity
across repeated restores, audio-strip ride-along); the rewritten box
blur is pinned byte-exact against an independent naive reference
across 7 shapes x 4 radii and motion H/V x 5 radii up to 32 (1x1,
1x7, 7x1, and r >= w included); lone surrogates decode to U+FFFD
while real pairs still combine; NTSC rates write-read bit-exactly
with the shortest spelling pinned for plain 24/12.5; and absurd
timecode inputs saturate at the exact representable cap instead of
truncating.

### Build & CI

- ci.yml gained a Windows media-tests leg (windows-latest, MSYS2
  MinGW64 + FFmpeg - the exact toolchain the portable zip ships).
  The FFmpeg layer previously compiled on Linux only in CI, so a
  Windows-only break surfaced only in the manual/tag-triggered
  portable workflow, long after the commit that caused it.
- ci.yml gained a core ASan+UBSan leg (ubuntu, Debug, sanitizer
  flags on the dependency-free core) - the cheapest job in the
  matrix catching the most expensive bugs; its first run surfaced a
  real heap overread in a transitions test fixture. Every job now
  carries timeout-minutes (five of six used to default to the
  runner's 6-hour hang burn), and the media suite carries a ctest
  TIMEOUT property so a hung FFmpeg call fails the job in minutes.
- The core and its test suites build with -Wall -Wextra -Wpedantic
  -Wshadow through an fc_warnings interface target (MSVC: /W4) - the
  set that surfaced the dead SRT helper at zero cost. The media and
  app layers stay on toolchain defaults so third-party header noise
  does not bury our warnings; nothing is promoted to -Werror.
- The portable workflow stamps PORTABLE.txt's version header from the
  VERSION file - the single source CMake's project() and the zip name
  already read - instead of a hard-coded v0.1.0 literal that silently
  went stale the moment VERSION moved. The portable zip now ships the
  license text and third-party notices alongside the binaries (GPL
  distribution convention).
- Third-party actions are pinned to commit SHAs (checkout,
  setup-msys2, cache, upload/download-artifact, gh-release),
  superseded pushes cancel through a concurrency group, the Qt app
  job compiles through ccache (month-rotated cache), and the lint
  glob covers .c files too. The portable release step moved to a
  separate least-privilege job: the build runs with contents: read,
  and only the tag-triggered release job - which executes no
  repository code - holds contents: write. Windows builds compile
  with _WIN32_WINNT=0x0601 (the Win7 gate exists at compile time,
  not only as the runtime tripwires) and MinGW links the C/C++
  runtimes statically, so the portable exe needs no
  libgcc/libstdc++ DLLs beside it. CMakePresets.json (core / media /
  app presets) and a CONTRIBUTING.md document the build, test, and
  formatting workflow.

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
