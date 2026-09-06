# Research Notes — How This Was Figured Out

No official Adobe spec exists for the `.ffx` binary format. Everything in
`ffx_core/` was derived by hand-diffing real sample files. This document
records the derivation and, importantly, **the mistakes made along the
way** — several fixes looked correct in isolation but caused new failures,
and future contributors should read this before "simplifying" anything in
`pipeline.py`.

## Container format

`.ffx` files are RIFX (big-endian RIFF): `RIFX` + 4-byte big-endian size +
`FaFX` form tag, then a sequence of chunks. Each chunk is a 4-byte id + a
4-byte big-endian size + that many bytes of content, padded to an even
byte count. `LIST` chunks additionally have a 4-byte form tag before their
nested children (same nesting rules as top-level `RIFX`).

## The `head` chunk and the version gate

16 bytes, 4 big-endian uint32 fields: `[3, VERSION, X, 0x01000000]`. The
2nd field gates which AE version will open the file.

- **CS5.5 confirmed value: 78 (0x4E)** — derived by diffing the user's own
  native-CS5.5 preset (`Good_Quality_CC.ffx`, despite the misleading "CC"
  in its filename — CC there stood for "Color Correction", not the AE
  version) against a CC-saved preset. The CC file's value was 95 (0x5F);
  swapping only this byte was the first fix attempted.
- CC-era files in this project showed values 93, 94, and 95 — evidently
  varies per CC sub-version/build, not a single constant.
- **Mistake #1**: patching only this byte was not sufficient — the
  resulting file crashed AE on load. The version gate wasn't the only
  format difference between CC and CS5.5; see the string-encoding section
  below.

## Effect index (`besc` → `tdsp`/`tdsn`) and parameter blocks (`sspc`)

Discovered via full recursive chunk-tree dumps, then cross-referencing
against `strings`-style greps for known effect names.

- `LIST besc` is the single top-level container holding effectively
  everything: a `beso` header, then repeating `(LIST tdsp, tdsn)` pairs
  (the effect index), then a run of `LIST sspc` blocks (the actual
  parameter data, one per non-sentinel effect, in the same order as the
  index).
- `LIST tdsp` is **always exactly 172 bytes**, regardless of the effect's
  name length. This was the first clue that some fields inside it are
  fixed-width, not length-prefixed — which mattered a lot later (see
  Mistake #3).
  - Contains two `tdmn` chunks: `tdmn[0]` is always the literal string
    `"ADBE Effect Parade"`; `tdmn[1]` is the effect's real match-name
    (e.g. `"S_Sharpen"`). Both are **fixed 40-byte, null-padded, in both
    CC and CS5.5 format** — this field never needed conversion.
  - Contains two `tdix` chunks (4-byte uint32 each): `tdix[0]` is always
    `0xFFFFFFFF` (an unused marker); `tdix[1]` is a **sequential index**
    that ties this entry to its `sspc` parameter block by position.
- `tdsn` (a sibling immediately after each `tdsp`) holds the effect's
  custom/display name (e.g. `"S_FilmDamage 2"` for a second instance of
  the same effect on a layer).
- `LIST sspc` blocks hold the actual parameter data. `fnam` (a direct
  child) holds the effect's short name (e.g. `"Looks"`, distinct from the
  match-name `"MB LookSuite3"`).
- A final sentinel `tdsp` entry (`tdmn` = `"ADBE End of path sentinel"`,
  single `tdix` = `0xFFFFFFFF`) terminates the index. Never touch it.

## The Magic Bullet Looks crash, and effect removal

The user's first test file crashed AE even after the version patch. Effect
listing (via `tdmn`) revealed it used `MB LookSuite3` (Magic Bullet Looks)
and three Boris FX Sapphire effects (`S_FilmDamage`, `S_MathOps`,
`S_Sharpen`) plus native effects. The user confirmed Sapphire was
installed but Magic Bullet Looks was not — a plausible crash cause, since
a missing plugin binary can crash AE outright on project load rather than
just showing a red "missing effect" warning.

Removing an effect requires deleting **three** things, not just its
name:
1. The `tdsp`+`tdsn` pair in the index (found via `tdmn[1]` match-name).
2. The corresponding `sspc` parameter block, matched **by position**
   (order in the `sspc` sequence corresponds to the order of non-sentinel
   `tdsp` entries) — matching by `fnam`'s short name alone is unreliable,
   since short names like `"Looks"` aren't guaranteed unique.
3. **Mistake #2**: after deleting entries, the surviving `tdix[1]` values
   are left with gaps (e.g. `1, 2, 3, 4, 5, 7, 8...` after removing
   indices 0 and 6). AE uses `tdix` to look up each effect's own
   parameter block; a gap causes it to read the *wrong* block for later
   effects — this manifested as displayed names/parameters not matching
   their actual effect, `not a crash`. Fixed by renumbering every
   remaining `tdix[1]` to be contiguous `0..N-1` after any removal.

## The string-encoding difference (the "Utf1/Utf2" bug)

Even after fixing the crash and the `tdix` gap, effect and parameter names
displayed as garbled placeholder text (`Utf`, `Utf1`, `Utf2`...) — AE's own
auto-dedupe suffixing kicking in because every name was literally reading
back as the same string.

Root cause, found by diffing a **CS5.5-native** reference file (built
directly by the user in CS5.5, never touched by CC) against the CC file:

- CC's `tdsn` and `fnam` chunks are encoded as `"Utf8"` (4 bytes) + a
  4-byte big-endian length + the string — a self-describing, variable-size
  format.
- CS5.5's native `tdsn` is **plain text + a single null terminator, no
  prefix, no length field, variable size**.
- CS5.5's native `fnam` is **plain text, null-padded to a fixed 48 bytes,
  no prefix**. This is *not* the same treatment as `tdsn` — `fnam` sits at
  a fixed byte offset inside `sspc`, and leaving it variable-length (as
  `tdsn` correctly is) shifts every field positioned after it, corrupting
  the rest of the block.
- **Mistake #3**: the first attempt stripped the `Utf8` prefix from every
  matching chunk uniformly (treating `fnam` the same as `tdsn`) — this
  produced a **hard crash**, worse than the cosmetic naming bug it was
  meant to fix, because it broke `sspc`'s fixed-offset internal layout.
  The fix required distinguishing `fnam` (fixed 48-byte, must pad) from
  `tdsn`/`pdnm` (variable, just strip-and-null-terminate).
- A third field type, `pdnm` (parameter display names — e.g. "Opacity",
  or a pipe-delimited dropdown option list like
  `"Off|Side By Side|Compare..."`), was found later via a full leaf-chunk
  scan for anything still carrying `Utf8`. It follows the same
  variable-length treatment as `tdsn`.

## Keyframes and third-party plugin data — deliberately untouched

`lhd3` (keyframe header: count + flags) and `ldat` (keyframe data: time,
value, and bezier tangent doubles for Graph Editor easing) were **never
modified** by any step in the pipeline, and verified byte-identical
before/after in every test file. This turned out to be correct — no
conversion was needed for keyframe data at all, only for the container's
name-string encoding.

Third-party plugins may carry their own **proprietary** parameter blob —
confirmed with RE:Vision Effects' Twixtor, which stores its internal speed
/ time-remap graph curve in an `sdat` chunk with no publicly known format.
Two attempts to reverse-engineer this were considered and explicitly
**not** attempted:
- No CC-vs-CS5.5 pair of the *same* Twixtor curve was available to diff
  (the only CS5.5-native Twixtor sample was built directly in CS5.5, with
  no CC-side equivalent to compare against).
- Blind-patching a plugin's private binary format without a way to verify
  the result carries real risk of silently corrupting the curve rather
  than failing loudly — worse than doing nothing.

The pipeline leaves these blobs completely untouched, and the verification
pass explicitly checks that they remain byte-identical after conversion.
This has been sufficient in every real test case so far — Twixtor's own
graph curve transferred correctly with zero modification needed, once the
container-level version/string fixes were in place.

## Summary of what NOT to do (learned the hard way)

- Don't patch only the version byte and assume that's sufficient.
- Don't remove an effect's index entry without also removing its matching
  `sspc` block and renumbering `tdix`.
- Don't treat `fnam` the same as `tdsn`/`pdnm` — it's fixed-width, they're
  not.
- Don't attempt to reverse-engineer a third-party plugin's own parameter
  blob without a same-curve CC/target-version pair to diff against.
- Always run the full verification pass (zero `Utf8` tags, contiguous
  `tdix`, unchanged keyframe/blob data) — several of the above mistakes
  produced a file that "looked" fine (parsed without error) but was wrong
  in ways only visible by actually opening it in AE.

## Keyframe records are per-DIMENSION (lhd3 field [3]) — round-23 graph fix

The graph pane's "spike then decay" artifact on some effects (BCC points
especially) was the keyframe reader treating a 2D stream as 1D:

- `lhd3` is 52 bytes of big-endian uint32s. Proven layout:
  `[0] 0x00D00BEE` magic, `[2]` keyframe count, `[3]` **dimension count**,
  `[4]` record size (48), the rest zeros/flags on current samples. Every
  stream in `sample_1.ffx` is 1D and carries `[3] = 1`.
- `ldat` holds `count x dims` records, interleaved per keyframe
  (dim0, dim1, dim0, dim1, ...), each record the proven 48-byte layout
  (time, interp in/out, value, in-slope, in-influence, out-slope,
  out-influence, 2 bytes pad).
- Reading a 2D stream as 1D plots X and Y as if they were consecutive
  keyframes — the value graph zig-zags between the dimensions and the
  speed graph spikes and decays; shapes AE never draws.
- The dimension declaration is only trusted after two checks: the record
  count must tile the ldat exactly, and every dimension of one keyframe
  must share its time (the structural fingerprint of interleaving).
  Anything else falls back to the 1D read. `[3] = 0` (synthetic test
  files) reads as 1.
- Round 23 plotted only dimension 0 (the row/tooltip named 2D streams
  "X of 2D (Y = ...)"); round 25 decodes dimension 1's value AND its
  own tangent block, so the value graph draws AE's X+Y curve pair and
  the speed graph draws the combined magnitude (round-25 section below).
- **Padded records carry the tangents too (round 26).** Every tangent
  offset lives in the record's FIRST 48 bytes; a writer that pads the
  record beyond byte 48 keeps the proven layout intact, and the padding
  proves it: when every byte past byte 48 is zero, the tangent block
  decodes with the same confidence as a 48-byte record. Round 25 and
  earlier decoded tangents ONLY at recSize == 48, so a padded stream
  silently drew clean straight lines where AE shows eased curves — the
  "the curve math doesn't look like AE" report that no presentation fix
  could reach. A record whose tail is NOT zero (an unknown layout whose
  +16 could be a second value double) still takes the honest linear
  read; the fixture's 48-byte streams are untouched (byte-identical
  decode, proven old-vs-new).

## The pard param-flags word (+4) and AE-hidden rows — round-23 panel fix

The 148-byte `pard` descriptor in `LIST parT` starts with a big-endian
uint32 **flags word at offset +4** (the control kind is the uint32 at +12,
its low byte at +15):

- **Bit 0x200 hides a parameter.** Proven on `sample_1.ffx`: BCC's three
  "placeholder" rows carry 0x220, Sapphire's opaque "mocha" blob 0x208 —
  and every visibly rendered parameter of all three vendors leaves bit
  0x200 clear. Visible rows carry 0x8 / 0x20 / 0x2 freely, so only 0x200
  may hide a row.
- BCC's own "Hidden" slider carries 0x8 like visible sliders, so the flag
  alone cannot identify it; it is hidden by its exact display name, along
  with the "placeholder" padders.
- ARB_DATA parameters (kind 11) render no UI in AE at all — hidden
  unconditionally (Sapphire "mocha", BCC "Mocha Data0").
- Net effect on the fixture: BCC's Effect Controls drop 5 junk rows
  (3x placeholder, "Hidden", "Mocha Data0") and Sapphire's mocha blob row,
  matching what AE's own panel shows.

## Nested tdgp groups: the naming order, and the tdmn BEFORE the group (round 26)

A nested `LIST tdgp` usually carries its display name in a `tdsn` leaf.
Round 26 established the FULL naming order AE's own writer uses:

1. the `tdsn` inside the group;
2. **the `tdmn` directly BEFORE the group** — the group's match name.
   The fixture pairs `'ADBE Effect Built In Params'` with the
   "Compositing Options" tdgp and `'ADBE Effect Mask Parade'` with its
   internal wrapper six times, always as the immediately preceding
   sibling. That tdmn belongs to the GROUP, not to the next parameter:
   leaving it pending mis-paired the next `tdbs` that carried no tdmn of
   its own (its parT kind/flags/menu then came from the group's
   descriptor — a visible row could inherit the group's 0x200 hidden bit
   and vanish, or a GROUP marker misfire), and an unnamed group hoisted
   every parameter inside it one level up. The walker now consumes the
   tdmn at the tdgp, uses it as a name candidate, and clears it.
3. a `tdmn` INSIDE the group resolving to the parT descriptor's display
   name (round 23's fallback — and the last resort: on a tdsn-less group
   it used to name the GROUP after its first inner PARAMETER, a header
   AE never shows).

Anonymous wrappers (no name anywhere) keep the parent path — they ARE
the parent visually.

## Graph Editor rendering references (round-23; corrected rounds 24/25)

The round-23 reading of a "light editor" with unselected direction
lines and a faint speed fill was wrong - the round-24 screenshot pair
(value graph, speed graph) shows AE's REFERENCE shots wearing one dark
skin. Round 25 settles the theme question the way AE itself does: AE's
Graph Editor follows AE's UI brightness preference, so OUR editor
follows the app theme - dark app = AE's dark palette, light app = the
app's own light tokens. The round-24 shape grammar stands:

- dark editor (BOTH modes): field #656565, grid #595959, zero line
  brighter, curve #CBCBCB, picked key #FFEE00, current-time line
  #FC0000, ruler labels bare numbers - the readout above the plot
  carries the unit ("N units" / "N units/sec", AE's own grammar).
- light editor (BOTH modes, round 25): the app's own tokens - B.Surface
  field, B.OutlineVariant grid, B.Primary curve and key fill,
  B.OnSurfaceVariant labels; AE's #FFEE00 picked key and #FC0000
  current-time line stay fixed (they read on any brightness).
- value graph: direction lines exist ONLY on the picked key
  (unselected keys show none; round-23 drew every bezier key's lines -
  disproven by the round-24 screenshot). The value carry beyond the
  keyed span is DASHED (AE's dashed stub after the last key). No glow,
  no fill: a plain 2px line.
- speed graph: SIGNED derivative with NO area fill (the round-23 faint
  fill was wrong - AE's field stays uniformly dark under the curve).
  Each segment renders as ONE analytic arc (dv/dt evaluated straight
  from the segment's control points); keyframe speed discontinuities
  get TRUE VERTICAL jump lines at the key's time (the reference's
  vertical plunge at the left edge and rise at the right edge are the
  carry-to-segment and segment-to-carry jumps); zero carries fill the
  window outside the keyed span. Ruler: ~6 lines, bare numbers.
- speed-editor key icons are shaped by interpolation (circle = bezier /
  easy ease, hollow square = linear, half square = hold); the value
  editor draws squares regardless. The PICKED key's icon is painted
  editor yellow; AE draws no selection ring around it.


## Multidimensional graphs draw AE's per-dimension pair (round 25)

Round-25 research pins AE's Graph Editor behavior for a 2D property
(Position, a POINT control):

- VALUE graph: ONE CURVE PER DIMENSION - "when you animate Position,
  the value graph shows two separate lines, X and Y" (the two curves
  overlay in one editor; Separate Dimensions splits them apart). The
  Y curve needs Y's OWN tangent block - the 48-byte ldat record
  carries it one record after dimension 0's - so the parser decodes
  dimension 1's slopes/influences/interpolation too, and PresetCurve
  builds Y segments through the same proven geometry.
- SPEED graph: ONE COMBINED curve - "the speed graph combines them
  into one single line representing the object's overall speed", the
  magnitude sqrt(vx^2 + vy^2) of the per-dimension signed rates. 1D
  streams keep the signed speed (the -500 dip reference).
- Dimension 1's tangent block gets the same garbage envelope as
  dimension 0 (ClampHandle2), so one absurd Y slope cannot bend the
  Y curve into an arc AE never draws.

## The ldat time unit is 1/1024 of a 30 fps frame — 30720 ticks per second (round 28)

The worst kind of wrong constant is one that produces *plausible* graphs,
and the tick base was exactly that: an early revision read ldat's int32
keyframe times as 1/1024 SECOND, "validated" by fixture times that land on
round numbers when divided by 1024. Every value graph looked right anyway
(value shape is scale-free — only the *relative* keyframe times matter),
and endpoint speeds read the stored slope doubles directly, which are
value-per-second and independent of any time base. Only the interior of
the speed graph betrayed the stretch: interior speed is bezier geometry
divided by real seconds, so a 30× time stretch squashes interior speeds to
1/30 while endpoints stay put — the lopsided speed curves that "still
don't look like AE" no matter how the editor was restyled.

An AE-authored preset contributed for side-by-side comparison pinned the
truth three independent ways (the file stays out of the repository; this
note records just the method and the numbers):

1. **The file states its base.** The `beso` (basic settings) chunk carries
   the uint32 30720 — 30 fps × 1024 — alongside its pixel dimensions.
2. **The area law.** A speed graph plots dv/dt, so integrating AE's own
   drawn curve must reproduce the stream's total value travel
   (∫ dv/dt dt = Δv). Column-by-column integration of AE's pixels gives
   Δv = -365.88 only when the keyed span 13312 ticks = 0.4333 s
   (= 13312/30720). The old reading (13.0 s) overshoots the travel ~30×;
   no other candidate base (1024, 65536, 32768) lands within 5%.
3. **Pixel-exact curve recovery.** With 0.4333 s spans the decoded
   tangents reproduce AE's speed curve outright: the early notch bottoms
   at -1636/s (AE's pixels: -1637) and the main valley at -3215/s (AE:
   -3226), and the keyframe x-ratios (7006 : 6306 = 1.111) match AE's
   marker positions (93.5 px : 84 px = 1.113).

The old fixture evidence re-reads cleanly under the same constant: its
times 1877 / 5631 / 11264 / 21504 are whole-or-half FRAMES in
1024-ticks-per-frame units (1.833 / 5.5 / 11 / 21 frames — 11264 and
21504 are exactly 11 and 21), which the earlier revision mislabeled as
seconds. `PresetCurve.TicksPerSecond` is now 30720.0; nothing in the tool
rewrites ldat bytes, so this remains a display-time conversion only.

## The pard flag 0x8 is the writer's "not in AE's Effect Controls" bit (round 29)

Round 23 concluded that BCC's 'Hidden' row "carries 0x8 like visible
sliders, so it hides by NAME instead" — that retraction goes the other
way now. The full BCC Directional Blur descriptor walk (side-by-side EC
screenshots contributed for comparison) shows a 23-row superseded legacy
PixelChooser block — Legacy PixelChooser, Apply PixelChooser, PC
Intensity, Mask, Shape, Point 1/2 and the matte controls From, To,
Scale, Stretch/Direction, Region Blend, Reverse Range, Channel, Matte
Layer, Type, Black/Threshold/From, White/To, Matte Softness, Color,
Blur Matte, Choke Matte, Invert Matte — every row carrying 0x8, and AE's
own Effect Controls drawing NONE of them, while every row AE does draw
carries 0x0 (rows inside groups carry 0x20, which is therefore not a
visibility bit). 'Hidden' (0x8), Sapphire's 'mocha' (0x208) and BCC's
'Mocha Data0' (0x8, kind 11) all fall under the same rule; Adobe's own
effects never set 0x8. IsHiddenParam now tests 0x200 OR 0x8 — the
round-23/26 name-based and 0x200-based hiding still holds, they just
were reading two edges of one bit.

## The no-sspc preset class: property/animator presets (round 30)

A contributed pack of 27 real-world presets (kept out of the repository)
contains six files that decode to ZERO effects under the previous
reader: two single-property value snaps and four text-animator presets.
Their `besc` carries no `sspc` snapshot at all — they are AE's "apply a
preset to a PROPERTY selection" class. Proven structure, consistent
across all six:

    besc children = [beso,
                     (LIST tdsp target-path, tdsn display-name) × N,
                     LIST tdsp containing only 'ADBE End of path sentinel',
                     tdgp-or-tdbs data block × N]

Each real `tdsp` is ONE property group: its tdmn chain is the target path
('ADBE Effect Parade' → 'S_Shake' → 'S_Shake-0050' for a Sapphire
Amplitude snap; 'ADBE Text Properties' → 'ADBE Text Animators' →
'ADBE Text Animator' for the text presets), the `tdsn` leaf immediately
after the path names the group ('Amplitude', 'Frequency', 'Path Options',
'More Options', 'Animator 1'), and the i-th tdgp/tdbs after the sentinel
carries that group's values. The panning presets hold one bare `tdbs`
(static cdat + tdum/tduM bounds); the text presets hold tdgp trees with
nested 'ADBE Text Selectors'/'ADBE Text Range Advanced' groups. None of
the six carries lhd3/ldat — the values are static; the animation comes
from AE's own animator semantics, not from keyframes in the file.

Three consequences, all verified against the pack:

1. `tdix` values in these path entries are NOT sspc indexes: they are
   0xFFFFFFFF sentinels and small path markers (0, 4, 5). Renumbering
   them 0..N-1 (the whole-effect invariant) would rewrite the property's
   own identity, and the contiguity check would false-fail. All three
   pipeline steps (removal, renumbering, verify) now branch on
   `sspc count == 0`.
2. Removing "effects" from such a file has no meaning (there is no
   snapshot to remove; removing a path entry would orphan its data
   block), so removal is a no-op and the caller's not-found warning
   explains the request.
3. Inspection yields one entry per real tdsp, in path order, so the
   effect list's N rows keep pairing with the inspection by position.

## parT is written once per repeated effect (round 30)

The same pack shows multi-effect presets where the same effect appears
two or four times (S_Sharpen ×2, MB LookSuite3 ×4, BCC Unsharp Mask ×2,
Wave Warp ×2, Drop Shadow ×2). AE writes the parT descriptor tree only
on the FIRST sspc of that effect; every later copy carries none. Under
the old reader each later copy degraded to Unknown kinds — which
un-grouped BCC's parameter tree (a BCC Unsharp Mask copy showed 89 flat
rows where the first copy shows 61 grouped ones), leaked AE-hidden rows
past the 0x8/0x200 rules (no flags at all on Unknown rows) and stripped
popup menus. The reader now caches the parT map by match name and gives
an empty map the first copy's map; a later copy that does carry its own
parT keeps it.

## parT kind 10 is AE's own bounded slider (round 30)

Across the pack, kind 10 rows are Exposure/Offset/Gamma (Adobe
Exposure2), the six Reverb and three High-Low Pass rows (Adobe audio),
Deep Glow's Radius/Exposure/Threshold/Spread, CSpice Glitchify's
Amount/Speed, omino's sliders and MB 'Strength' — all flags 0x0, all
with tdum/tduM bounds in the file. Kind 10 joins Slider(1) /
FixedSlider(2) / FloatSlider(9) as a bounded-slider kind
(`PresetParamKind.BoundedSlider`); the value slot already renders it
through the shared slider path.

## The update check is one version file, the batch jobs are one page (round 31)

**Check for Updates.** The About page's placeholder button goes live
with the smallest possible scheme: the repository root carries a
one-line `VERSION.txt`, GitHub serves its raw bytes for the default
branch at a stable URL, and the app fetches exactly that one file
(HttpWebRequest, TLS 1.2 pinned for Windows 7, 6 s timeout, no
telemetry). The body must be strictly numeric-dotted ("1.0.31", an
optional leading v tolerated) — a 404 page or captive-portal HTML
disqualifies itself and surfaces as an error instead of parsing as a
version. Comparison is numeric per part (Version.CompareTo), never a
string compare, so 1.0.9 < 1.0.31. Bumping the version for a new round
is a one-line edit of the same file, committed with the code it
announces.

**Batch.** One page, two jobs, one folder in:

- *Inspect* mirrors the effect lister's deep read (PresetInspector
  with the errors list) into one summary row per file: status, effect /
  parameter / animated counts, size, first decode notes. A file whose
  effects all fail to decode shows FAILED; a file that yields zero
  effects or carries decode warnings shows WARN — round 30 proved both
  states are legitimate file shapes, never reasons to abort the run.
- *Convert* runs Pipeline.Convert per file: version patch to the
  chosen target, optional removal of effects whose match name resolves
  (system scan first, reference tables second — the same recognition
  chain as the single-file page) as not-installed under the active
  profile. Three explicit output modes: a "converted" subfolder inside
  the source, a version suffix beside each original, or an in-place
  overwrite that demands a Yes/No first. Derived outputs overwrite
  their own previous result (re-runs stay idempotent); the one guarded
  edge is a derived name colliding with the INPUT file, which falls
  back to a numbered suffix instead of destroying the source.

The folder walk is hand-rolled (per-directory try/catch) because
.NET Framework's Directory.EnumerateFiles with AllDirectories throws
mid-enumeration on the first locked subtree and loses every file after
it. Per-file work runs on one worker thread with a cancellation token
checked between files; UI rows marshal through the dispatcher; progress
is a determinate 0..N fraction. The CSV report quotes every field
(doubled quotes), UTF-8 with BOM so Excel reads it correctly.

## Round 32 — the startup crash, the 0.1.0 release scheme, the nightly channel

**The BAML parse-order crash.** WPF raises `ToggleButton.OnIsCheckedChanged`
*inside* the BAML parse the moment the parser applies `IsChecked="True"` —
before `InitializeComponent()` returns, and before any control declared
further down the XAML exists. BatchPage's pre-checked "Include subfolders"
checkbox fired its `Checked` handler mid-parse; the handler's guard checked
`SourceCountText` (declared *before* the checkbox in the XAML, so non-null)
and passed, and `UpdateCta` then dereferenced `RunBtn` (declared *after*,
still null). NullReferenceException in `BatchPage.UpdateCta`, thrown from
`BatchPage..ctor` inside `MainWindow..ctor` inside `App.OnStartup` — a
deterministic crash on every machine, on every 1.0.31 start. The crash
funnel did its job (readable problem report, full trace in
%LOCALAPPDATA%\FFXCompatibilityTool\crash.log) — which is how the fix was
located without a repro machine. The guard that cannot be defeated by
declaration order is `FrameworkElement.IsInitialized`: false for the whole
parse, true only after EndInit — by the time `InitializeComponent()`
returns. All three parse-time-firing handlers in the app (BatchPage's
ModeNav + SourceOption, SettingsPage's SubNav — `IsSelected="True"` on the
first ListBoxItem raises SelectionChanged the same way) now guard on it,
and the initial state each of them would compute is already applied
without them (the constructor's `ApplyModeVisuals()` for BatchPage; the
XAML's own static visibilities for SettingsPage), so behavior is exactly
what it would have been without the crash. Audit of every other
event-wired control: `IsChecked="True" Click="…"` (the Effect Lister's
three segmented toggles) cannot fire during parse — Click is a
user-gesture event, only Checked/Unchecked ride OnIsCheckedChanged; the
remaining `{Binding …}` toggles are runtime bindings. That closes the
class, not just the instance.

**The version scheme.** The internal round counter (1.0.31) becomes the
release scheme 0.1.0: the assembly `<Version>` and the About / crash
report / session-log stamps all read AppInfo (one source), and VERSION.txt
— the live update endpoint — moves to 0.1.0 in the same commit, so a fresh
release install compares 0.1.0 vs 0.1.0 and reports up-to-date. The
comparison is numeric per part (System.Version), so 0.1.9 < 0.1.10 when
the first patch lands. The same-commit rule is non-negotiable here:
VERSION.txt is served raw from the default branch the moment the commit
lands, while installed builds only see it when the user checks.

**The nightly channel.** A new workflow publishes the rolling test build
the release is shipped through instead of hand-made portable zips: every
push to main (i.e. every uploaded round), plus a 02:30 UTC daily schedule,
plus manual dispatch. Same toolchain as the release workflow
(windows-latest's 4.8 targeting pack, .NET 8 SDK driving). Tests run
before packaging — a red test run stops the pipeline and the previous
nightly stays published. The artifact is the Release output folder zipped
whole (exe + dependency DLLs + data/plugin_table.json), named with the UTC
date and the short commit SHA, kept 14 days. Publishing replaces one
rolling pre-release tagged `nightly`: the previous release and tag are
deleted, the tag is re-pointed at the commit that was just built and
tested, and a new pre-release is created with the zip attached — tagged
v* releases keep their separate "Latest" slot. A workflow-level
concurrency group serializes runs so two pushes landing close together
cannot race the release replacement.

## Round 33 — the release-notes bot, and the Batch section dissolves into its users

**The tag workflow's description bot.** The release workflow now writes
the release description itself before publishing: it walks the commit
subjects between the previous tag and the new one (full clone — a
shallow checkout has nothing to read; the first tag ever reads the
whole history, which is honest for a first release), splits the repo's
multi-entry commit messages at their " and <type>(" joins so every
change becomes its own bullet, and appends install steps (unzip
anywhere, run the exe; .NET Framework 4.8 — preinstalled on Windows
10/11, a one-time install on Windows 7 SP1), the SHA-256 of the
attached zip, and the Check-for-Updates note. GitHub's generated notes
(the compare link) ride on top of that body. A manual dispatch — no
tag — skips both steps and just produces the artifact.

**Batch dissolves.** The Batch section is gone from the nav; its two
jobs return to where their results are actually used:

- *Convert* accepts a whole folder ("Open folder…" beside the status
  chip, folder drops, and a multi-select file dialog — one entry point
  each for the queue). The queue card carries the three settings the
  batch page had (include subfolders / remove effects missing from the
  profile / the three output modes) and the CTA converts every .ffx in
  one pass, streaming one console line per file and reusing the save
  banner as a batch-completion handoff to the output folder. The
  single-preset checklist flow is untouched — a single file behaves
  exactly as before, and loading one file while a queue is showing
  returns to the checklist.
- *Effect Lister* takes the same folder inputs and turns them into a
  queue: a combo in the header lists the folder's presets, picking one
  deep-reads it with the exact single-preset anatomy, and the file chip
  reads "3 / 27 — name.ffx". The batch-inspect table survives as the
  "Folder report" flyout: every preset deep-read into one row (status,
  effect/parameter/animated counts, size, decode notes), exportable as
  the same quoted UTF-8-BOM CSV.

The folder walk and the size formatter moved to one shared helper
(FolderScan) so the two pages cannot drift; the per-directory
try/catch stands (one locked subtree contributes nothing). Loading a
new queue bumps a scan generation so an in-flight report can never mix
the old folder's rows into the new one. Every new state-event handler
guards on IsInitialized — the round-32 lesson is now house style: the
queue's pre-checked subfolder box fires during the BAML parse, exactly
like its BatchPage ancestor did.
