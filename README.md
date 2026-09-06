# FFX Compatibility Tool — C# / .NET Framework 4.8 port

A from-scratch port of the Python `ffx_core` engine to C#, targeting
**.NET Framework 4.8** specifically for real Windows 7 compatibility —
see the repo's earlier history for why: Python 3.9+, Qt6 (PySide6), and
even PySide2's available wheel range all independently stopped supporting
Win7, and chasing each one individually stopped being productive.

## Important: what's verified and what isn't yet

**I could not compile or run this C# code myself** — this sandbox has no
.NET SDK installed and no way to install one (network access here is
locked to a handful of package registries, not Microsoft's). Every line
was written by careful manual translation from the Python version that
*was* fully tested against your real preset files across this whole
project, but **the C# port itself has not been executed by anyone yet.**

`.github/workflows/test.yml` is set up to build and run the full test
suite (a port of every meaningful test from `tests/test_riff.py` and
`tests/test_pipeline.py`, including a real-file round-trip using the same
`sample_1.ffx` fixture) on `windows-latest` the moment you push this. That
CI run is the actual first real test of this code — please check it
before trusting the logic, the same discipline the Python version went
through before any of it got called "confirmed."

If it fails, the most likely culprits, roughly in order of likelihood:
1. A typo or off-by-one in the manual translation (most likely — this is
   hand-ported, not machine-translated)
2. `System.Text.Json` version pin needing adjustment for net48 compat
3. Something about the `<None Include>` linked-file paths for
   `plugin_table.json` / the `.ffx` fixture not resolving the way I
   expect across `dotnet build`'s output structure

None of these would be surprising for a first-pass port — flag whatever
the CI output shows and I'll fix it directly rather than guess further.

## Structure

```
FfxTool.Core/              # port of ffx_core — RiffNode.cs, Pipeline.cs, PluginLookup.cs
FfxTool.Core.Tests/         # xUnit port of test_riff.py / test_pipeline.py (incl. fixtures/sample_1.ffx)
FfxTool.Gui/               # WPF GUI — MainWindow, ConvertPage (single preset or a whole folder), ListerPage (single preset or a folder queue + report), ProfilePage, SettingsPage + MD3 theme
data/plugin_table.json      # shared verbatim — copied to output via <None Include Link> (Core + Gui)
.github/workflows/test.yml  # dotnet build + test on windows-latest (Core + Gui)
.github/workflows/build.yml # Release zip of FfxTool.Gui.exe + dependencies + data/plugin_table.json
.github/workflows/nightly.yml # rolling "nightly" pre-release of main (every push + daily) for feature testing
```

`FfxTool.sln` includes all three projects (`Core`, `Core.Tests`, `Gui`) so a single `dotnet build FfxTool.sln` builds the entire repo. Each csproj links `../data/plugin_table.json` with `CopyToOutputDirectory=PreserveNewest`; `Core.Tests` additionally links `fixtures/*.ffx`.

## What was deliberately preserved from the Python version

Every hard-won detail from `RESEARCH_NOTES.md` carried over as-is:
- `fnam` chunks get padded to a fixed 48 bytes; `tdsn`/`pdnm` stay
  variable-length — these are NOT the same treatment (this distinction
  was Mistake #3 in the original derivation; getting it wrong crashes AE).
- Effect removal matches `sspc` blocks to `tdsp` entries by **position**,
  not name, and always renumbers `tdix` afterward.
- Keyframe (`lhd3`/`ldat`) and third-party plugin blob data is never
  touched by any pipeline step, and `Pipeline.Verify()` checks this holds
  after every conversion — same verification discipline as the Python
  version, not weakened for the port.

## Running locally

```bash
dotnet restore FfxTool.sln
dotnet build FfxTool.sln --configuration Release
dotnet test FfxTool.sln --configuration Release
# GUI: FfxTool.Gui\bin\Release\net48\FfxTool.Gui.exe (+ data\plugin_table.json alongside it)
```
