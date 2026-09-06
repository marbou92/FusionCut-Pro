using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;
// System.IO and System.Windows.Shapes both define `Path` - bind the bare
// name to the shape once so the graph code stays unambiguous
using Path = System.Windows.Shapes.Path;
using FfxTool.Core;
using Microsoft.Win32;

namespace FfxTool.Gui
{
    /// <summary>
    /// Effect Lister: read-only compatibility view of a preset's effects —
    /// filter/sort toolbar, status-colored rows in a unified list card,
    /// friendly empty state, drag-overlay language shared with Convert,
    /// and a recent-files flyout over the full 5-entry history.
    ///
    /// The workspace opens into AE's Effect Controls panel — every effect
    /// as a collapsible block of real AE property lines (keyframe-navigator
    /// gutter, stopwatch, fixed name column, hover-underlined value, nested
    /// parameter groups, the About link) — with a
    /// switcher to the split compatibility list + tabbed inspector, whose
    /// parameter rows are the simple read (stopwatch, name, value under a
    /// quiet group caption). Both read the same decoded data and share
    /// keyframe selection state.
    ///
    /// The right-hand inspector reads like AE's timeline: parameter rows
    /// carry the stopwatch mark (lit = time-varying) and a keyframe
    /// navigator, the Keyframes tab shows AE-style timecodes with slope
    /// and influence numbers for the selected keyframe, and the Graph tab
    /// draws the Graph Editor pair — a value graph with tangent handles
    /// and a speed graph — with a hover probe. Everything is read-only
    /// (PresetInspector reads, never writes; the pipeline's keyframes are
    /// untouched).
    /// </summary>
    public partial class ListerPage : UserControl, ISection
    {
        public class EffectRowVm
        {
            public string Name { get; set; }
            /// <summary>Raw match name for the row tooltip when the visible
            /// name is the human display name (null when they are equal).</summary>
            public string MatchTip { get; set; }
            public string VendorLabel { get; set; }
            public string Status { get; set; }
            // animated-parameter count for the list row's badge — the
            // clear "this effect moves" marker the plain rows lacked
            public int Animated { get; set; }
            public Visibility HasAnimated =>
                Animated > 0 ? Visibility.Visible : Visibility.Collapsed;
            public string AnimText => Animated + " animated";
            // position of this effect among the file's non-sentinel effects —
            // the stable key that ties a (sorted/filtered) row back to its
            // PresetEffectDetails entry
            public int EffectIndex { get; set; }
        }

        public class RecentRow
        {
            public string FileName { get; set; }
            public string Meta { get; set; }
            public string Path { get; set; }
            public bool Exists { get; set; }
        }

        /// <summary>
        /// One row of the Parameters tab / Effect Controls body, shaped
        /// like an AE property line: stopwatch state, name + stream
        /// summary, the value slot, and the keyframe navigator targets.
        /// Brushes stay in the templates (via DynamicResource triggers
        /// keyed on IsAnimated) so theme swaps keep working — the VM only
        /// carries flags, text and counts. The Effect Controls template
        /// renders the navigator gutter; the inspector's simple row drops
        /// it in favor of a click-through to the Keyframes tab.
        /// </summary>
        public class ParamRowVm
        {
            public string Name { get; set; }
            public string Detail { get; set; }
            public string MatchName { get; set; }
            public bool IsAnimated { get; set; }
            public double StopwatchOpacity { get; set; }
            public string StopwatchTip { get; set; }
            public string ValueText { get; set; }
            public Visibility ValueVisible { get; set; }
            public string ValueTip { get; set; }
            public Visibility NavVisible { get; set; }
            public string KeyCountTip { get; set; }
            public Cursor RowCursor { get; set; }
            public double RowOpacity { get; set; }
            // the decoded parameter behind the row — what the navigator
            // buttons and the row click resolve to
            public PresetParameter ParamRef { get; set; }

            public ParamRowVm(PresetParameter p)
            {
                ParamRef = p;
                Name = p.Name;
                MatchName = p.MatchName ?? p.Name;
                IsAnimated = p.IsAnimated;
                RowCursor = p.IsAnimated ? Cursors.Hand : Cursors.Arrow;

                // the Count check is belt-and-braces: the decoder only sets
                // IsAnimated after storing at least one keyframe, but this
                // row builder must never be able to throw on any input
                if (p.IsAnimated && p.Keyframes.Count > 0)
                {
                    // stream summary: keys · value travel · time span
                    double vMin = p.Keyframes.Min(k => k.Value);
                    double vMax = p.Keyframes.Max(k => k.Value);
                    double span = PresetCurve.Seconds(
                        p.Keyframes[p.Keyframes.Count - 1].Time - p.Keyframes[0].Time);
                    string travel = Math.Abs(vMax - vMin) < 1e-9
                        ? $"flat at {Fmt(vMin)}"
                        : $"{Fmt(vMin)} → {Fmt(vMax)}";
                    Detail = $"animated · {travel} · {span.ToString("0.##")} s span";
                    StopwatchOpacity = 1.0;
                    StopwatchTip = "Time-varying: ON — this property carries a keyframe stream (read-only inspector)";

                    // the value slot shows the value at the first keyframe,
                    // the way AE shows the value at the playhead
                    ValueText = Fmt(p.Keyframes[0].Value);
                    ValueVisible = Visibility.Visible;
                    ValueTip = $"value at the first keyframe · range {Fmt(vMin)} … {Fmt(vMax)} · {p.Keyframes.Count} keyframes";
                    NavVisible = Visibility.Visible;
                    KeyCountTip = $"{p.Keyframes.Count} keyframe{(p.Keyframes.Count == 1 ? "" : "s")} · click to open the Keyframes tab";
                }
                else
                {
                    string range = p.Min.HasValue && p.Max.HasValue
                        ? $" · range {Fmt(p.Min)} … {Fmt(p.Max)}" : "";
                    Detail = "static value" + range;
                    StopwatchOpacity = 0.4;
                    StopwatchTip = "Time-varying: OFF — static value (read-only inspector)";

                    if (p.StaticValue.HasValue)
                    {
                        ValueText = Fmt(p.StaticValue);
                        ValueVisible = Visibility.Visible;
                        ValueTip = p.Min.HasValue && p.Max.HasValue
                            ? $"static value · range {Fmt(p.Min)} … {Fmt(p.Max)}"
                            : "static value";
                    }
                    else
                    {
                        ValueText = "";
                        ValueVisible = Visibility.Collapsed;
                        ValueTip = "";
                    }
                    NavVisible = Visibility.Collapsed;
                    KeyCountTip = "";
                }
            }

            static string Fmt(double? v) => v?.ToString("0.###") ?? "—";
        }

        /// <summary>
        /// One row of the EFFECT CONTROLS panel — deliberately a separate
        /// type from the inspector's ParamRowVm so the two parameter UIs
        /// stay independent: this one carries the control kind decoded from
        /// the preset's parT tree (checkbox, popup, color, angle, point,
        /// layer, button...) and the value column renders the matching
        /// AE-style visual per kind. Read-only, like AE's panel.
        /// </summary>
        public class EcParamVm
        {
            public string Name { get; set; }
            public string Detail { get; set; }
            public string MatchName { get; set; }
            public bool IsAnimated { get; set; }
            public double StopwatchOpacity { get; set; }
            public string StopwatchTip { get; set; }
            public string ValueText { get; set; }
            public Visibility ValueVisible { get; set; }
            public string ValueTip { get; set; }
            public Visibility NavVisible { get; set; }
            public string KeyCountTip { get; set; }
            public Cursor RowCursor { get; set; }

            // --- kind-driven view state (Effect Controls value visuals) ---
            public bool IsCheckbox { get; set; }
            public bool IsPopup { get; set; }
            public bool IsColor { get; set; }
            public bool Checked { get; set; }
            public string PopupText { get; set; }
            public Brush ColorBrush { get; set; }
            // ranged slider rendering (AE's track + number): the
            // fill comes from the parT min/max decoded out of the
            // preset; a visual read of the stored value, never an
            // editor
            public bool IsSlider { get; set; }
            public double SliderFillW { get; set; }
            public Visibility SliderVisible { get; set; } = Visibility.Collapsed;
            // which value control this row renders — computed here so the
            // templates bind Visibility straight to the VM (style triggers
            // could never override a local Visibility binding)
            public Visibility TextVisible { get; set; } = Visibility.Collapsed;
            public Visibility CheckboxVisible { get; set; } = Visibility.Collapsed;
            public Visibility PopupVisible { get; set; } = Visibility.Collapsed;
            public Visibility SwatchVisible { get; set; } = Visibility.Collapsed;
            // the decoded parameter behind the row — what the navigator
            // buttons and the row click resolve to
            public PresetParameter ParamRef { get; set; }

            public EcParamVm(PresetParameter p)
            {
                ParamRef = p;
                Name = p.Name;
                MatchName = p.MatchName ?? p.Name;
                IsAnimated = p.IsAnimated;
                RowCursor = p.IsAnimated ? Cursors.Hand : Cursors.Arrow;
                int kind = p.Kind;

                IsCheckbox = kind == PresetParamKind.Checkbox;
                IsPopup = kind == PresetParamKind.Popup;
                IsColor = kind == PresetParamKind.Color && !p.IsAnimated;

                // row-level state: the stream summary lives in tooltips, the
                // stopwatch mark reads ON/OFF, the navigator gutter is
                // reserved on every row so the NAME columns align like AE's
                if (p.IsAnimated && p.Keyframes.Count > 0)
                {
                    double vMin = p.Keyframes.Min(k => k.Value);
                    double vMax = p.Keyframes.Max(k => k.Value);
                    double span = PresetCurve.Seconds(
                        p.Keyframes[p.Keyframes.Count - 1].Time - p.Keyframes[0].Time);
                    string travel = Math.Abs(vMax - vMin) < 1e-9
                        ? $"flat at {Fmt(vMin)}"
                        : $"{Fmt(vMin)} → {Fmt(vMax)}";
                    Detail = $"animated · {travel} · {span.ToString("0.##")} s span";
                    StopwatchOpacity = 1.0;
                    StopwatchTip = "Time-varying: ON — this property carries a keyframe stream (read-only panel)";
                    NavVisible = Visibility.Visible;
                    KeyCountTip = $"{p.Keyframes.Count} keyframe{(p.Keyframes.Count == 1 ? "" : "s")} · click to open the Keyframes tab";
                }
                else
                {
                    string range = p.Min.HasValue && p.Max.HasValue
                        ? $" · range {Fmt(p.Min)} … {Fmt(p.Max)}" : "";
                    Detail = "static value" + range;
                    StopwatchOpacity = 0.4;
                    StopwatchTip = "Time-varying: OFF — static value (read-only panel)";
                    NavVisible = Visibility.Collapsed;
                    KeyCountTip = "";
                }

                // value slot: the control AE draws for this kind. The value
                // read is the static cdat value, or the first keyframe's the
                // way AE shows the value at the playhead.
                double v = 0;
                bool hasV = (p.IsAnimated && p.Keyframes.Count > 0) || p.StaticValue.HasValue;
                if (hasV)
                    v = p.IsAnimated && p.Keyframes.Count > 0
                        ? p.Keyframes[0].Value : p.StaticValue.Value;

                if (IsCheckbox)
                {
                    // AE draws a real checkbox in the value column
                    Checked = hasV && v >= 0.5;
                    CheckboxVisible = Visibility.Visible;
                    ValueText = "";
                    ValueVisible = Visibility.Collapsed;
                    ValueTip = (Checked ? "On" : "Off") + " — from the preset (read-only panel)";
                }
                else if (IsPopup)
                {
                    // AE draws a popup whose label is the selected entry
                    PopupText = PopupLabel(p, hasV ? v : 0);
                    PopupVisible = Visibility.Visible;
                    ValueText = "";
                    ValueVisible = Visibility.Collapsed;
                    var menu = p.MenuItems;
                    ValueTip = menu != null
                        ? $"menu selection {Math.Max(1, (int)Math.Round(hasV ? v : 1))} of {menu.Length} · read-only panel"
                        : "menu selection · read-only panel";
                }
                else if (IsColor)
                {
                    // AE draws a color swatch in the value column
                    ColorBrush = ColorOf(p);
                    SwatchVisible = Visibility.Visible;
                    ValueText = "";
                    ValueVisible = Visibility.Collapsed;
                    ValueTip = "color from the preset (read-only panel)";
                }
                else if (kind == PresetParamKind.Button || kind == PresetParamKind.FloatSlider ||
                         kind == PresetParamKind.ArbitraryData)
                {
                    // command rows and plugin data blobs: AE shows no value
                    ValueText = "";
                    ValueVisible = Visibility.Collapsed;
                    ValueTip = "";
                }
                else if (kind == PresetParamKind.Point && !p.IsAnimated && p.StaticValue2.HasValue)
                {
                    ValueText = $"({Num(p.StaticValue ?? 0)}, {Num(p.StaticValue2 ?? 0)})";
                    ValueVisible = Visibility.Visible;
                    TextVisible = Visibility.Visible;
                    ValueTip = "point (X, Y) from the preset · " + Detail;
                }
                else if ((kind == PresetParamKind.Layer || kind == PresetParamKind.Path))
                {
                    ValueText = !hasV || v == 0
                        ? "None"
                        : (kind == PresetParamKind.Layer ? $"Layer {v.ToString("0")}" : $"Mask {v.ToString("0")}");
                    ValueVisible = Visibility.Visible;
                    TextVisible = Visibility.Visible;
                    ValueTip = "selection from the preset (read-only panel)";
                }
                else if (hasV)
                {
                    // sliders, angles, percents — AE's right-aligned number
                    ValueText = kind == PresetParamKind.Angle ? Num(v) + "°" : Num(v);
                    ValueVisible = Visibility.Visible;
                    TextVisible = Visibility.Visible;
                    // a stated range renders AE's slider track: the
                    // filled proportion of (v - min) / (max - min),
                    // the number riding at the right of the track
                    if (p.Min.HasValue && p.Max.HasValue && p.Max.Value > p.Min.Value)
                    {
                        double frac = (v - p.Min.Value) / (p.Max.Value - p.Min.Value);
                        if (frac < 0) frac = 0;
                        if (frac > 1) frac = 1;
                        IsSlider = true;
                        SliderFillW = 4.0 + frac * 84.0; // 92px track, 4px nub at both extremes
                        SliderVisible = Visibility.Visible;
                        TextVisible = Visibility.Collapsed; // the number lives inside the slider block
                    }
                    if (p.IsAnimated)
                    {
                        double vMin = p.Keyframes.Min(k => k.Value);
                        double vMax = p.Keyframes.Max(k => k.Value);
                        ValueTip = $"value at the first keyframe · range {Fmt(vMin)} … {Fmt(vMax)} · {p.Keyframes.Count} keyframes";
                    }
                    else
                    {
                        ValueTip = p.Min.HasValue && p.Max.HasValue
                            ? $"static value · range {Fmt(p.Min)} … {Fmt(p.Max)}"
                            : "static value";
                    }
                }
                else
                {
                    ValueText = "";
                    ValueVisible = Visibility.Collapsed;
                    ValueTip = "";
                }
            }

            /// <summary>AE formats values with a fixed decimal read.</summary>
            static string Num(double v) => v.ToString("0.0##");

            static string Fmt(double? v) => v?.ToString("0.###") ?? "—";

            /// <summary>
            /// Popup label for a 1-based stored index ("No|Tile|Reflect" with
            /// 3.0 → "Reflect"); out-of-range presets fall back to Option N.
            /// </summary>
            static string PopupLabel(PresetParameter p, double idx)
            {
                var menu = p.MenuItems;
                int i = (int)Math.Round(idx) - 1;
                if (menu != null && i >= 0 && i < menu.Length && menu[i].Length > 0)
                    return menu[i];
                return "Option " + Math.Max(1, i + 1);
            }

            /// <summary>
            /// Swatch brush from the stored RGB(A) doubles; presets store
            /// either the 0-1 or the 0-255 scale, told apart per channel.
            /// </summary>
            static Brush ColorOf(PresetParameter p)
            {
                double r = p.StaticValue ?? 0, g = p.StaticValue2 ?? 0, b = p.StaticValue3 ?? 0;
                if (r > 1 || g > 1 || b > 1) { r /= 255.0; g /= 255.0; b /= 255.0; }
                byte R = (byte)Math.Round(Math.Max(0, Math.Min(1, r)) * 255);
                byte G = (byte)Math.Round(Math.Max(0, Math.Min(1, g)) * 255);
                byte B = (byte)Math.Round(Math.Max(0, Math.Min(1, b)) * 255);
                var br = new SolidColorBrush(Color.FromRgb(R, G, B));
                br.Freeze();
                return br;
            }
        }

        /// <summary>One keyframe row: AE timecode, frame math, easing chip.</summary>
        public class KfRowVm
        {
            public string Index { get; set; }
            public int KfIndex { get; set; }
            public string TimeSec { get; set; }
            public string Sub { get; set; }
            public string Value { get; set; }
            public string Interp { get; set; }
            public string Tip { get; set; }
            public bool Selected { get; set; }

            public KfRowVm(int index, PresetKeyframe kf, PresetKeyframe prev)
            {
                KfIndex = index - 1;
                Index = index.ToString();
                // ticks → seconds via PresetCurve's empirically derived
                // timebase (1 tick = 1/1024 s); raw ticks stay in the tooltip
                double sec = PresetCurve.Seconds(kf.Time);
                TimeSec = Timecode(sec);
                int frame = (int)Math.Round(sec * Fps);
                if (prev == null)
                {
                    Sub = $"frame {frame} · {sec.ToString("0.##")} s";
                }
                else
                {
                    int prevFrame = (int)Math.Round(PresetCurve.Seconds(prev.Time) * Fps);
                    double ds = sec - PresetCurve.Seconds(prev.Time);
                    Sub = $"frame {frame} · +{frame - prevFrame}f · +{ds.ToString("0.##")}s";
                }
                Value = kf.Value.ToString("0.###");
                Interp = kf.InterpLabel;
                Tip = $"t = {sec.ToString("0.###")} s · raw time {kf.Time} ticks" +
                      $" · in influence {kf.InInfluence.ToString("0.##")}" +
                      $" · out influence {kf.OutInfluence.ToString("0.##")}";
            }
        }

        /// <summary>
        /// One effect block of the Effect Controls view: header data plus
        /// the AE property rows (the same ParamRowVm anatomy the inspector
        /// uses, so stopwatch/navigator behavior is identical in both views).
        /// </summary>
        public class EcGroupVm : System.ComponentModel.INotifyPropertyChanged
        {
            public string Title { get; set; }
            public string Sub { get; set; }
            // INPC: disclosure flips fold the block IN PLACE — the old
            // rebuild-per-toggle reset the scroll and could leave blocks
            // visually stuck (the "sometimes it won't collapse" report)
            bool _open;
            public bool Open
            {
                get { return _open; }
                set { _open = value; Fire(); }
            }
            public Visibility BodyVisible => Open ? Visibility.Visible : Visibility.Collapsed;
            public event System.ComponentModel.PropertyChangedEventHandler PropertyChanged;
            void Fire()
            {
                var h = PropertyChanged;
                if (h == null) return;
                h(this, new System.ComponentModel.PropertyChangedEventArgs("Open"));
                h(this, new System.ComponentModel.PropertyChangedEventArgs("BodyVisible"));
            }
            // body tree: EcParamVm property lines and EcSubGroupVm group
            // nodes, in the preset's document order
            public List<object> Items { get; set; }
            public int EffectIndex { get; set; }
            // header chip: how many keyframe streams this effect carries
            public int Animated { get; set; }
            public Visibility AnimVisible => Animated > 0 ? Visibility.Visible : Visibility.Collapsed;
            public string AnimText => Animated + " animated";
        }

        /// <summary>One row of the folder report (the baked-in batch
        /// inspect): status, counts, size, first decode notes.</summary>
        public class ScanRowVm
        {
            public string FileName { get; set; }
            public string Status { get; set; }
            public string Effects { get; set; }
            public string Params { get; set; }
            public string Animated { get; set; }
            public string Size { get; set; }
            public string Note { get; set; }
        }

        private readonly PluginProfile _profile;
        private List<Pipeline.EffectInfo> _currentEffects = new List<Pipeline.EffectInfo>();
        private List<PresetEffectDetails> _details = new List<PresetEffectDetails>();
        // human-readable decode problems from the last load ("effect #2 ..."):
        // surfaced on the panel and in the log, so a preset that half-decodes
        // never fails in silence
        private List<string> _inspectErrors = new List<string>();
        // last plugin-table failure reason already logged (null = none) —
        // so a persistent failure is logged once, not again on every Refresh
        private string _lastTableError;
        // match-name → display name/category table (data/effect_names.json,
        // built from David Torno's public AE match-name spreadsheet) —
        // loaded lazily once; display-only, compatibility never uses it
        private List<EffectNameEntry> _names;
        private string _namesError;
        private readonly ObservableCollection<EffectRowVm> _rows = new ObservableCollection<EffectRowVm>();
        private int _filterMode; // 0 all, 1 missing only, 2 compatible only
        private bool _sortDesc;

        // inspector state: selected effect (by stable effect index), selected
        // animated parameter (for the keyframes/graph tabs), active tab,
        // graph mode (0 = value like AE's value graph, 1 = speed graph),
        // selected keyframe (drives the graph's ring + handles and the
        // Keyframes tab's highlight + easing numbers)
        private int _inspEffectIndex = -1;
        private int _animParamIndex = -1;
        private int _tab;
        private int _graphMode;
        private int _selKf = -1;
        private bool _syncingCombo;

        // DragEnter/DragLeave fire on every child boundary crossing; a depth
        // counter is the only flicker-free way to know the drag truly left.
        private int _dragDepth;

        // folder / multi-file queue: when a folder (or a multi-file selection)
        // is loaded, the page keeps the file list and deep-reads one file at a
        // time — selection via the queue combo in the header. The folder
        // report (the baked-in batch inspect) runs the deep read across the
        // whole queue into the ScanFlyout table, exportable as CSV.
        private List<string> _queue;
        private int _queueIndex;
        private volatile bool _scanRunning;
        // scan generation: loading a new queue invalidates an in-flight
        // scan so its rows can never mix into the new folder's report
        private int _scanGen;
        private readonly ObservableCollection<ScanRowVm> _scanRows = new ObservableCollection<ScanRowVm>();

        // ---------- view modes: AE Effect Controls panel vs. split inspector ----------
        // 0 = Effect Controls (the AE-style panel, the default), 1 = split
        // compatibility list + tabbed inspector. Group open/closed state and
        // the per-effect status/vendor header lines survive every rebuild;
        // the dictionaries are keyed by stable effect index.
        // the Inspector is the default section: a freshly loaded preset
        // opens the split workspace (compatibility list + inspector), the
        // way AE opens its Inspector rather than Effect Controls
        private int _viewMode = 1;
        private readonly Dictionary<int, bool> _ecOpen = new Dictionary<int, bool>();
        // AE anatomy state: each named parameter group's disclosure state
        // (keyed "effectIndex|groupPath"); survives every rebuild
        private readonly Dictionary<string, bool> _ecGroupOpen = new Dictionary<string, bool>();
        // the Inspector's group disclosure state (keyed "effectIndex|path") —
        // the inspector's captions collapse now, same persistence rules
        private readonly Dictionary<string, bool> _inspGroupOpen = new Dictionary<string, bool>();
        // the live Effect Controls blocks — disclosure flips fold these IN
        // PLACE (INPC) instead of rebuilding the whole list, which reset
        // the scroll position on every toggle and could leave blocks
        // visually stuck until the next full rebuild
        private List<EcGroupVm> _ecGroups;
        private readonly Dictionary<int, string> _ecStatus = new Dictionary<int, string>();
        private readonly Dictionary<int, string> _ecVendor = new Dictionary<int, string>();

        // live-resize redraw coalescing: while the window is being dragged
        // by its border, SizeChanged fires per pixel and a full graph
        // rebuild (geometry + labels + 260 samples) per pixel is exactly
        // the kind of work that makes Win7 resize feel rough. The timer
        // merges the storm into ~1 redraw per 70 ms plus a final one.
        private DispatcherTimer _graphRedrawTimer;

        public ListerPage(PluginProfile profile)
        {
            InitializeComponent();
            _profile = profile;
            EffectList.ItemsSource = _rows;
            ScanList.ItemsSource = _scanRows;
            StatusBarVersion.Text = $"FFX Compatibility Tool {AppInfo.DisplayVersion}";
            UpdateRecentCard();
            GraphCanvas.SizeChanged += (s, e) => ScheduleGraphRedraw();
            KfTimeline.SizeChanged += (s, e) => DrawKfTimeline();
            SetTab(0);
            SetView(0); // the AE Effect Controls panel is the default workspace view
            // the graph and the keyframe strip bake brush colors per Refresh —
            // re-run on theme changes; the list rows and the EC panel theme
            // live through DynamicResource
            ThemeService.Changed += () => { Refresh(); BuildKeyframes(); DrawGraph(); };
        }

        public void OnShown() => UpdateRecentCard();

        public void OnProfileChanged() => Refresh();

        private void UpdateRecentCard()
        {
            var recent = HistoryStore.Load().FirstOrDefault();
            if (recent != null)
            {
                RecentText.Text = recent.FileName;
                RecentCard.Opacity = 1;
                RecentCard.Cursor = Cursors.Hand;
                RecentCard.ToolTip = "Open the recent-files picker";
            }
            else
            {
                RecentText.Text = "View last 5 analyzed presets";
                RecentCard.Opacity = 0.55;
                RecentCard.Cursor = Cursors.Hand;
                RecentCard.ToolTip = "No recent files yet";
            }
        }

        // ---------- recent-files flyout ----------
        private void Recent_Click(object sender, MouseButtonEventArgs e)
        {
            var entries = HistoryStore.Load();
            var rows = new List<RecentRow>();
            foreach (var entry in entries.Take(5))
            {
                bool exists = File.Exists(entry.Path);
                rows.Add(new RecentRow
                {
                    FileName = entry.FileName,
                    // HistoryStore.TimeAgo finally gets its call site: "12 mins ago"
                    Meta = exists
                        ? $"{entry.EffectCount} effect{(entry.EffectCount == 1 ? "" : "s")} · {HistoryStore.TimeAgo(entry.Timestamp)}"
                        : "File moved or deleted",
                    Path = entry.Path,
                    Exists = exists
                });
            }

            if (rows.Count > 0)
            {
                RecentList.ItemsSource = rows;
                RecentEmptyHint.Visibility = Visibility.Collapsed;
            }
            else
            {
                RecentList.ItemsSource = null;
                RecentEmptyHint.Visibility = Visibility.Visible;
            }

            // a previously picked item stays selected in its list box; reset so
            // re-opening shows no stale highlight and re-picking still fires
            RecentList.SelectedIndex = -1;
            RecentFlyout.IsOpen = true;
        }

        private void RecentList_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (RecentList.SelectedItem is RecentRow row && row.Exists && File.Exists(row.Path))
            {
                RecentFlyout.IsOpen = false;
                LoadFile(row.Path);
            }
        }

        // ---------- loading ----------
        public void OpenFile()
        {
            var dlg = new OpenFileDialog { Filter = "After Effects Presets (*.ffx)|*.ffx", Multiselect = true };
            if (dlg.ShowDialog() != true) return;
            if (dlg.FileNames.Length == 1) { LoadFile(dlg.FileNames[0]); return; }
            LoadQueue(dlg.FileNames.Where(File.Exists).ToList());
        }

        private void Open_Click(object sender, RoutedEventArgs e) => OpenFile();

        private void FolderBtn_Click(object sender, RoutedEventArgs e)
        {
            var dlg = new System.Windows.Forms.FolderBrowserDialog
            {
                Description = "Pick the folder that holds the .ffx presets.",
                ShowNewFolderButton = false
            };
            if (dlg.ShowDialog() != System.Windows.Forms.DialogResult.OK) return;
            LoadQueue(FolderScan.Collect(dlg.SelectedPath, true));
        }

        // ---------- folder queue ----------

        /// <summary>Folder / multi-file mode: the queue drives the workspace —
        /// the combo lists every file and picking one deep-reads it with the
        /// exact single-preset anatomy (LoadFile).</summary>
        private void LoadQueue(List<string> files)
        {
            if (files == null || files.Count == 0)
            {
                MessageBox.Show(Window.GetWindow(this),
                    "No .ffx presets were found there.", "Nothing to load",
                    MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }
            _queue = files;
            _queueIndex = 0;
            _scanGen++; // any in-flight report belongs to the OLD queue
            QueuePanel.Visibility = Visibility.Visible;
            QueueCombo.ItemsSource = files.Select(System.IO.Path.GetFileName).ToList();
            QueueCombo.SelectedIndex = 0; // fires QueueCombo_SelectionChanged → LoadFile
        }

        private void QueueCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            // the combo only ever receives items in code, after the page is
            // built — but guard anyway, per the round-32 parse-order lesson
            if (!IsInitialized || _queue == null) return;
            int i = QueueCombo.SelectedIndex;
            if (i < 0 || i >= _queue.Count) return;
            _queueIndex = i;
            LoadFile(_queue[i]);
        }

        // ---------- drag feedback ----------
        private void Page_DragEnter(object sender, DragEventArgs e)
        {
            if (!HasPayload(e.Data)) return;
            _dragDepth++;
            DragOverlay.Visibility = Visibility.Visible;
            e.Effects = DragDropEffects.Copy;
        }

        private void Page_DragLeave(object sender, DragEventArgs e)
        {
            if (_dragDepth > 0) _dragDepth--;
            if (_dragDepth == 0) DragOverlay.Visibility = Visibility.Collapsed;
        }

        private void Page_Drop(object sender, DragEventArgs e)
        {
            _dragDepth = 0;
            DragOverlay.Visibility = Visibility.Collapsed;

            if (e.Data.GetData(DataFormats.FileDrop) is string[] items && items.Length > 0)
            {
                // a folder drop loads the whole folder, subfolders included
                string folder = items.FirstOrDefault(Directory.Exists);
                if (folder != null) { LoadQueue(FolderScan.Collect(folder, true)); return; }

                var dropped = items.Where(f => f.EndsWith(".ffx", StringComparison.OrdinalIgnoreCase) && File.Exists(f))
                                   .Distinct(StringComparer.OrdinalIgnoreCase)
                                   .OrderBy(p => p, StringComparer.OrdinalIgnoreCase)
                                   .ToList();
                if (dropped.Count == 1) { LoadFile(dropped[0]); return; }
                if (dropped.Count > 1) { LoadQueue(dropped); return; }
            }
            MessageBox.Show(Window.GetWindow(this),
                "No .ffx preset was found in the dropped items.",
                "Unsupported file", MessageBoxButton.OK, MessageBoxImage.Information);
        }

        private static bool HasPayload(IDataObject data) =>
            data.GetDataPresent(DataFormats.FileDrop) &&
            data.GetData(DataFormats.FileDrop) is string[] files &&
            files.Any(f => Directory.Exists(f) ||
                           f.EndsWith(".ffx", StringComparison.OrdinalIgnoreCase));

        private void LoadFile(string path)
        {
            try
            {
                // a fresh file invalidates every index the inspector holds —
                // effect, animated-parameter and keyframe selections are all
                // positions in the PREVIOUS file's decoded data
                _inspEffectIndex = -1;
                _animParamIndex = -1;
                _selKf = -1;
                // ...and the panel's own state is per-file too: disclosure
                // states keyed by effect index belong to the OLD preset and
                // must not leak into the new one
                _ecOpen.Clear();
                _ecGroupOpen.Clear();
                _inspGroupOpen.Clear();
                _ecGroups = null; // the live blocks belong to the old file too
                _ecStatus.Clear();
                _ecVendor.Clear();
                // a fresh preset opens the Inspector section (the split
                // workspace), regardless of the view the old file was in
                _viewMode = 1;

                FileChipText.Text = _queue != null
                    ? (_queueIndex + 1) + " / " + _queue.Count + " — " + System.IO.Path.GetFileName(path)
                    : System.IO.Path.GetFileName(path);
                byte[] bytes = File.ReadAllBytes(path);
                _currentEffects = Pipeline.ListEffects(bytes);

                // deep inspection is additive — if a preset carries a
                // structure the inspector can't decode, the list above still
                // works and the panel SAYS what couldn't be read (never
                // silently)
                _inspectErrors = new List<string>();
                try { _details = PresetInspector.Inspect(bytes, _inspectErrors); }
                catch (Exception ipx)
                {
                    _details = new List<PresetEffectDetails>();
                    _inspectErrors.Add("the preset structure couldn't be read: " +
                                       ipx.GetType().Name + " — " + ipx.Message);
                }
                foreach (var w in _inspectErrors)
                    LogService.Append("inspect: " + System.IO.Path.GetFileName(path) + " — " + w);

                int ecParams = _details.Sum(x => x.Parameters.Count);
                int ecAnim = _details.Sum(x => x.AnimatedCount);
                EcSub.Text = $"{_currentEffects.Count(x => !x.IsSentinel)} effects · " +
                             $"{ecParams} parameter{(ecParams == 1 ? "" : "s")} · " +
                             $"{ecAnim} animated — read-only, like AE's panel";
                if (_inspectErrors.Count > 0)
                {
                    // one-line heads-up on the panel; the full list rides the
                    // tooltip and the log
                    EcSub.Text += $"  ⚠ {_inspectErrors.Count} decode " +
                                  $"warning{(_inspectErrors.Count == 1 ? "" : "s")}";
                    EcSub.ToolTip = string.Join("\n", _inspectErrors);
                }
                else
                {
                    EcSub.ToolTip = null;
                }

                HistoryStore.Push(path, _currentEffects.Count(e => !e.IsSentinel));
                Refresh();
                if (_rows.Count > 0) EffectList.SelectedIndex = 0; // open the inspector right away
            }
            catch (Exception ex)
            {
                // the full problem report instead of a bare message box:
                // it names the failing method, is copyable for a bug
                // report, and lands in both logs (crash.log + session log)
                App.Report($"reading '{System.IO.Path.GetFileName(path)}' into the Effect Controls panel", ex);
                FileChipText.Text = "No file loaded";
                _currentEffects = new List<Pipeline.EffectInfo>();
                _details = new List<PresetEffectDetails>();
                EcSub.Text = "Open a preset to see its effect controls";
                Refresh();
            }
        }

        // ---------- folder report ----------

        /// <summary>The baked-in batch inspect: deep-read every preset in the
        /// queue into the report flyout — one row per file, nothing written.</summary>
        private void Scan_Click(object sender, MouseButtonEventArgs e)
        {
            if (_queue == null || _scanRunning) return;
            _scanRunning = true;
            int gen = ++_scanGen;
            var files = new List<string>(_queue);
            _scanRows.Clear();
            ScanCsvLink.Visibility = Visibility.Collapsed;
            ScanSummary.Text = "Reading 0 / " + files.Count + "\u2026";
            ScanFlyout.IsOpen = true;

            int total = files.Count;
            var reporter = new Progress<(int done, string text)>(v => ScanSummary.Text = v.text);
            Task.Run(() =>
            {
                int done = 0;
                foreach (var path in files)
                {
                    var captured = ScanOne(path);
                    done++;
                    ((IProgress<(int, string)>)reporter).Report((done,
                        "Reading " + done + " / " + total + "\u2026 " + captured.FileName));
                    Dispatcher.BeginInvoke(new Action(() =>
                    { if (gen == _scanGen) _scanRows.Add(captured); }));
                }
                Dispatcher.BeginInvoke(new Action(() =>
                {
                    if (gen != _scanGen) return;
                    int bad = _scanRows.Count(r => r.Status == "FAILED");
                    int warn = _scanRows.Count(r => r.Status == "WARN");
                    int good = _scanRows.Count - bad - warn;
                    ScanSummary.Text = _scanRows.Count + " preset(s) — " + good + " ok · " +
                                       warn + " warning" + (warn == 1 ? "" : "s") + " · " +
                                       bad + " failed";
                    ScanCsvLink.Visibility = Visibility.Visible;
                    _scanRunning = false;
                    LogService.Append("folder report: " + _scanRows.Count + " file(s) — " +
                                      good + " ok, " + warn + " warn, " + bad + " failed");
                }));
            });
        }

        /// <summary>The read-only deep read the Effect Lister gives one
        /// preset, summarized as one report row.</summary>
        private ScanRowVm ScanOne(string path)
        {
            var row = new ScanRowVm
            {
                FileName = System.IO.Path.GetFileName(path),
                Status = "OK", Effects = "—", Params = "—",
                Animated = "—", Size = "—", Note = ""
            };
            try { row.Size = FolderScan.FmtSize(new FileInfo(path).Length); }
            catch { /* unreadable metadata — the size stays "—" */ }
            try
            {
                byte[] data = File.ReadAllBytes(path);
                var errors = new List<string>();
                var effects = PresetInspector.Inspect(data, errors);
                int par = 0, anim = 0;
                foreach (var e in effects) { par += e.Parameters.Count; anim += e.AnimatedCount; }
                row.Effects = effects.Count.ToString();
                row.Params = par.ToString();
                row.Animated = anim.ToString();
                if (errors.Count > 0)
                {
                    row.Status = "WARN";
                    row.Note = string.Join(" | ", errors.Take(2)) + (errors.Count > 2 ? " …" : "");
                }
                else if (effects.Count == 0)
                {
                    row.Status = "WARN";
                    row.Note = "No effects or property groups decoded";
                }
            }
            catch (Exception ex)
            {
                row.Status = "FAILED";
                row.Effects = "—"; row.Params = "—"; row.Animated = "—";
                row.Note = ex.Message;
            }
            return row;
        }

        private void ScanCsv_Click(object sender, MouseButtonEventArgs e)
        {
            if (_scanRows.Count == 0) return;
            var dlg = new SaveFileDialog { Filter = "CSV report (*.csv)|*.csv", FileName = "folder_report.csv" };
            if (dlg.ShowDialog() != true) return;
            try
            {
                var sb = new StringBuilder();
                sb.AppendLine("File,Status,Effects,Params,Animated,Size,Note");
                foreach (var r in _scanRows)
                    sb.AppendLine(string.Join(",",
                        Q(r.FileName), Q(r.Status), Q(r.Effects), Q(r.Params), Q(r.Animated), Q(r.Size), Q(r.Note)));
                // UTF-8 with BOM: Excel on Windows reads it correctly
                File.WriteAllText(dlg.FileName, sb.ToString(), new UTF8Encoding(true));
                ScanSummary.Text = "CSV written: " + dlg.FileName;
                LogService.Append("folder report: " + _scanRows.Count + " row(s) → " + dlg.FileName);
            }
            catch (Exception ex)
            {
                MessageBox.Show(Window.GetWindow(this),
                    "Could not write the CSV:\n" + ex.Message,
                    "Export failed", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        /// <summary>CSV field quoting: wrap in quotes and double any inner
        /// quote — commas inside file names/notes can never split a cell.</summary>
        private static string Q(string s) => "\"" + (s ?? "").Replace("\"", "\"\"") + "\"";

        // ---------- filter / sort / refresh ----------
        private void Filter_Click(object sender, RoutedEventArgs e)
        {
            _filterMode = (_filterMode + 1) % 3;
            // active filter gets the primary tint — resource KEY, tracks theme swaps
            FilterIcon.SetResourceReference(IconGlyph.ForegroundProperty,
                _filterMode != 0 ? "B.Primary" : "B.OnSurfaceVariant");
            Refresh();
        }

        private void Sort_Click(object sender, RoutedEventArgs e)
        {
            _sortDesc = !_sortDesc;
            SortIcon.SetResourceReference(IconGlyph.ForegroundProperty,
                _sortDesc ? "B.Primary" : "B.OnSurfaceVariant");
            Refresh();
        }

        public void Refresh()
        {
            var table = PluginLookup.LoadTable();
            if (PluginLookup.TableLoadError != _lastTableError)
            {
                // each NEW table failure is logged once — statuses degrade
                // to Unknown plugin, the list itself keeps working
                _lastTableError = PluginLookup.TableLoadError;
                if (_lastTableError != null)
                    LogService.Append("plugin table: " + _lastTableError +
                                      " \u2014 every effect shows as Unknown plugin until it's fixed");
            }
            if (_names == null)
            {
                _names = EffectNameLookup.Load();
                if (EffectNameLookup.LoadError != null && EffectNameLookup.LoadError != _namesError)
                {
                    _namesError = EffectNameLookup.LoadError;
                    LogService.Append("effect names table: " + _namesError +
                                      " \u2014 effect titles fall back to their match names");
                }
            }
            var real = _currentEffects.Where(e => !e.IsSentinel);

            // stable position of every effect within the file — rows are
            // sorted/filtered, but the inspector needs the original index
            var fileOrder = real.Select((eff, i) => new { eff, i })
                                .ToDictionary(x => x.eff, x => x.i);

            IEnumerable<Pipeline.EffectInfo> ordered = _sortDesc
                ? real.OrderByDescending(e => e.MatchName, StringComparer.OrdinalIgnoreCase)
                : real.OrderBy(e => e.MatchName, StringComparer.OrdinalIgnoreCase);

            // one recognized match per match name — the filter, the rows
            // and the Effect Controls headers must never disagree about a
            // vendor: system scan catalog first, reference tables second
            var matches = new Dictionary<string, PluginMatch>(StringComparer.Ordinal);
            foreach (var e in real)
            {
                string key = e.MatchName ?? "";
                if (!matches.ContainsKey(key))
                    matches[key] = PluginRecognition.Resolve(e.MatchName, table, _names);
            }

            if (_filterMode == 1)
                ordered = ordered.Where(e =>
                {
                    var m = matches[e.MatchName ?? ""];
                    return !m.Installed && _profile.Owns(m.Vendor) == false; // on disk can't be missing
                });
            else if (_filterMode == 2)
                ordered = ordered.Where(e =>
                {
                    var m = matches[e.MatchName ?? ""];
                    return m.Installed || _profile.Owns(m.Vendor) != false;
                });

            int shown = 0, missing = 0;
            _rows.Clear();
            foreach (var eff in ordered)
            {
                shown++;
                var match = matches[eff.MatchName ?? ""];
                var owned = _profile.Owns(match.Vendor);

                string status;
                if (match.Installed)
                {
                    // the system scan found this plugin on the user's disk —
                    // the strongest signal there is, checked before any table
                    status = "Installed";
                }
                else if (match.Vendor == null)
                {
                    status = "Unknown plugin";
                }
                else if (owned == false)
                {
                    status = "Likely to fail";
                    missing++;
                }
                else if (owned == true)
                {
                    status = "Compatible";
                }
                else
                {
                    status = "Native";
                }

                string disp = DisplayNameFor(eff.MatchName);
                int ei = fileOrder[eff];
                _rows.Add(new EffectRowVm
                {
                    Name = disp,
                    MatchTip = disp == eff.MatchName ? null : eff.MatchName,
                    VendorLabel = VendorLine(match),
                    Status = status,
                    EffectIndex = ei,
                    // the "N animated" diamond needs the decoded stream
                    // count; 0 for a slot the inspector couldn't decode
                    Animated = ei < _details.Count ? _details[ei].AnimatedCount : 0
                });

                // header data for the Effect Controls block, keyed by the
                // same stable effect index the rows carry
                _ecStatus[fileOrder[eff]] = status;
                _ecVendor[fileOrder[eff]] = VendorLine(match);
            }

            bool hasContent = _currentEffects.Any(e => !e.IsSentinel);
            EmptyState.Visibility = hasContent ? Visibility.Collapsed : Visibility.Visible;
            SetView(_viewMode); // shows/hides EcHost + SplitHost for the active view

            if (hasContent && shown > 0)
            {
                string filterText = _filterMode == 0 ? "All" : _filterMode == 1 ? "Missing only" : "Compatible only";
                string sortText = _sortDesc ? "Z→A" : "A→Z";
                StatusBarMode.Text = $"{shown} shown · {missing} likely to fail · {filterText} · {sortText}";
            }
            else
            {
                StatusBarMode.Text = hasContent ? "No effects match this filter" : "Ready";
            }

            // the list was rebuilt, so the ListBox selection is gone — restore
            // the inspector's effect if its row survived the filter/sort
            if (EffectList.SelectedIndex < 0 && _inspEffectIndex >= 0)
            {
                int idx = _rows.ToList().FindIndex(r => r.EffectIndex == _inspEffectIndex);
                if (idx >= 0) EffectList.SelectedIndex = idx;
            }

            if (_inspEffectIndex < 0) SetInspectorEmpty();

            // the left status slot is only populated by future features — keep
            // its separator hidden while empty so no orphan "·" ever shows
            StatusSep.Visibility = string.IsNullOrEmpty(StatusBarLeft.Text)
                ? Visibility.Collapsed : Visibility.Visible;

            RefreshEcRows();
        }

        // ---------- view modes: Effect Controls ↔ split inspector ----------
        private void ViewBtn_Click(object sender, RoutedEventArgs e)
        {
            SetView(sender == ViewInspectorBtn ? 1 : 0);
        }

        /// <summary>
        /// Switches the workspace between the AE Effect Controls panel
        /// (mode 0) and the split compatibility list + inspector
        /// (mode 1, the default). Hidden while no preset is loaded.
        /// </summary>
        private void SetView(int mode)
        {
            _viewMode = mode;
            if (ViewEcBtn == null) return; // XAML not loaded yet (design-time)
            bool has = _currentEffects.Any(x => !x.IsSentinel);
            ViewEcBtn.IsChecked = mode == 0;
            ViewInspectorBtn.IsChecked = mode == 1;
            ViewSwitcher.Visibility = has ? Visibility.Visible : Visibility.Collapsed;
            EcHost.Visibility = has && mode == 0 ? Visibility.Visible : Visibility.Collapsed;
            SplitHost.Visibility = has && mode == 1 ? Visibility.Visible : Visibility.Collapsed;
            if (mode == 0) RefreshEcRows();
            else
            {
                // the graph pane may be getting its first real size this
                // pass — repaint after layout instead of 70ms later
                Dispatcher.BeginInvoke(new Action(DrawGraph), DispatcherPriority.Loaded);
            }
        }

        /// <summary>
        /// Vendor line for the list row and the Effect Controls header —
        /// an honest "unrecognized" when no rule knows the match name.
        /// </summary>
        private static string VendorLine(PluginMatch m)
        {
            if (m == null) return "unrecognized — not in any reference table";
            if (m.Installed)
                return (m.Vendor ?? "your system") + " — installed: " + m.Suite;
            if (m.Vendor == null) return "unrecognized — not in any reference table";
            return m.Vendor + (string.IsNullOrEmpty(m.Suite) ? "" : " — " + m.Suite);
        }

        /// <summary>
        /// Human effect title for a match name: the reference table's display
        /// name when known (AE never shows match names), else the match name.
        /// </summary>
        private string DisplayNameFor(string matchName)
        {
            var e = EffectNameLookup.Resolve(matchName, _names);
            if (!string.IsNullOrEmpty(e?.name) && e.name != matchName) return e.name;
            return matchName;
        }

        /// <summary>Effect Controls block title: display name, ShortName, match name.</summary>
        private string EcDisplayName(PresetEffectDetails d)
        {
            var e = EffectNameLookup.Resolve(d.MatchName, _names);
            if (!string.IsNullOrEmpty(e?.name) && e.name != d.MatchName) return e.name;
            return string.IsNullOrEmpty(d.ShortName) ? d.MatchName : d.ShortName;
        }

        /// <summary>Vendor/status line under one Effect Controls block title.</summary>
        private string EcSubFor(int effectIndex, PresetEffectDetails d)
        {
            string vendor = _ecVendor.TryGetValue(effectIndex, out string v) ? v : "unknown plugin";
            string status = _ecStatus.TryGetValue(effectIndex, out string s) ? s : "";
            var e = EffectNameLookup.Resolve(d.MatchName, _names);
            string cat = string.IsNullOrEmpty(e?.category) ? "" : e.category + "  ·  ";
            return $"{cat}{vendor}  ·  {d.Parameters.Count} parameter{(d.Parameters.Count == 1 ? "" : "s")}" +
                   $"  ·  {d.AnimatedCount} animated  ·  {status}";
        }

        /// <summary>
        /// Rebuilds the Effect Controls groups from the decoded preset. The
        /// rows share the inspector's ParamRowVm/template, so the property
        /// lines read identically in both views. Cheap — runs only on load,
        /// theme change, or a toggle.
        /// </summary>
        private void RefreshEcRows()
        {
            if (EcList == null) return;
            var groups = new List<EcGroupVm>();
            for (int i = 0; i < _details.Count; i++)
            {
                var d = _details[i];
                if (d.Error != null)
                {
                    // the decoder kept this effect's slot but couldn't decode
                    // its parameter tree — show WHY, never an empty gap
                    LogService.Append($"effect controls: effect #{i + 1} ({d.MatchName}) " +
                                      $"couldn't be decoded \u2014 {d.Error}");
                    groups.Add(new EcGroupVm
                    {
                        Title = EcDisplayName(d),
                        Sub = "\u26a0 couldn't be decoded \u2014 " + d.Error,
                        Open = false,
                        Items = new List<object>(),
                        EffectIndex = i
                    });
                    continue;
                }
                if (d.Parameters.Count == 0)
                {
                    // a real effect whose every row was hidden (housekeeping
                    // markers, no parT tree...) still gets its header line —
                    // an honest "nothing decoded" beats an invisible effect
                    groups.Add(new EcGroupVm
                    {
                        Title = EcDisplayName(d),
                        Sub = EcSubFor(i, d),
                        Open = false,
                        Items = new List<object>(),
                        EffectIndex = i
                    });
                    continue;
                }
                try
                {
                    bool open = !_ecOpen.TryGetValue(i, out bool o) || o;
                    var root = new List<object>();
                    var created = new Dictionary<string, EcSubGroupVm>();
                    foreach (var p in d.Parameters)
                    {
                        // the Effect Controls' own row VM — kind-aware values,
                        // separate from the inspector's simpler ParamRowVm
                        var row = new EcParamVm(p);
                        AddEcRow(root, created, i, p.Group, row);
                    }
                    groups.Add(new EcGroupVm
                    {
                        Title = EcDisplayName(d),
                        Sub = EcSubFor(i, d),
                        Open = open,
                        Items = root,
                        Animated = d.AnimatedCount,
                        EffectIndex = i
                    });
                }
                catch (Exception ex)
                {
                    // one malformed effect must never take the panel down —
                    // AE never dies on a preset either: degrade to a closed
                    // block that still names the effect AND the failure
                    LogService.Append($"effect controls: effect #{i + 1} ({d.MatchName}) " +
                                      $"couldn't be displayed \u2014 {Describe(ex)}");
                    groups.Add(new EcGroupVm
                    {
                        Title = EcDisplayName(d),
                        Sub = "\u26a0 parameters couldn't be displayed \u2014 " + Describe(ex),
                        Open = false,
                        Items = new List<object>(),
                        EffectIndex = i
                    });
                }
            }
            try
            {
                _ecGroups = groups;
                EcList.ItemsSource = groups;
                // realize the row templates HERE, inside the guard: without
                // this the realization happens in the layout pass — past
                // every try/catch in the load path — where one bad row
                // crashed the whole window (the reported preset-load crash)
                EcList.UpdateLayout();
                if (groups.Count == 0)
                {
                    EcEmpty.Text = _inspectErrors.Count > 0
                        ? "Parameter data for this preset couldn't be decoded:\n" +
                          string.Join("\n", _inspectErrors) +
                          "\nThe compatibility list still works."
                        : "Parameter data for this preset couldn't be decoded - the compatibility list still works, and the inspector explains what could be read.";
                    EcEmpty.Visibility = Visibility.Visible;
                }
                else
                {
                    EcEmpty.Visibility = Visibility.Collapsed;
                }
            }
            catch (Exception ex)
            {
                string chain = Describe(ex);
                LogService.Append($"effect controls: the panel couldn't be rendered \u2014 {chain}");
                _ecGroups = null;
                EcList.ItemsSource = null;
                EcEmpty.Text = "This preset's parameter panel couldn't be rendered \u2014 " + chain +
                               " - the compatibility list still works. Details in About \u2192 Logs.";
                EcEmpty.Visibility = Visibility.Visible;
            }
        }

        /// <summary>
        /// Display name of a possibly-disambiguated group path: the Core
        /// decoder suffixes repeat group instances with \u0002&lt;k&gt;
        /// on the PATH only (so two same-named groups file and collapse
        /// separately) — this strips it for what the user reads.
        /// </summary>
        private static string StripDisambiguator(string name)
        {
            int u = name.IndexOf('\u0002');
            return u < 0 ? name : name.Substring(0, u);
        }

        /// <summary>
        /// Places one property row into the EC body tree: at the root, or
        /// inside its parameter group — intermediate group nodes are
        /// created on first encounter, so the preset's document order of
        /// groups and rows (AE's panel order) is preserved.
        /// </summary>
        private void AddEcRow(List<object> root, Dictionary<string, EcSubGroupVm> created,
                              int effectIndex, string path, object row)
        {
            if (string.IsNullOrEmpty(path)) { root.Add(row); return; }
            AddEcNode(root, created, effectIndex, path).Items.Add(row);
        }

        /// <summary>Gets or creates the group node for a group path.</summary>
        private EcSubGroupVm AddEcNode(List<object> root, Dictionary<string, EcSubGroupVm> created,
                                       int effectIndex, string path)
        {
            if (created.TryGetValue(path, out var g)) return g;
            string name = path, parent = null;
            int sep = path.LastIndexOf('\u0001');
            if (sep >= 0) { name = path.Substring(sep + 1); parent = path.Substring(0, sep); }
            name = StripDisambiguator(name);
            g = new EcSubGroupVm
            {
                Title = name,
                GroupKey = path,
                EffectIndex = effectIndex,
                // AE's Effect Controls starts named sub-settings collapsed —
                // only the user's explicit disclosure (persisted in
                // _ecGroupOpen) opens them
                Open = _ecGroupOpen.TryGetValue(effectIndex + "|" + path, out bool stored) && stored
            };
            created[path] = g;
            if (parent == null) root.Add(g);
            else AddEcNode(root, created, effectIndex, parent).Items.Add(g);
            return g;
        }

        // ---------- Effect Controls group toggles ----------
        private void FlipEcGroup(int effectIndex)
        {
            bool open = !_ecOpen.TryGetValue(effectIndex, out bool cur) || cur;
            _ecOpen[effectIndex] = !open;
            // fold the LIVE block in place: the state was always persisted
            // here, but the full RefreshEcRows() rebuild reset the panel's
            // scroll position and re-realized every template on every
            // toggle — the collapse read as "sometimes it won't collapse".
            // The template's ToggleButton already writes Open through its
            // TwoWay binding when the twirl itself was clicked; raising it
            // here covers header-row clicks too.
            var g = _ecGroups?.FirstOrDefault(x => x.EffectIndex == effectIndex);
            if (g != null) g.Open = !open;
            else RefreshEcRows(); // stale list (fresh file) — rebuild once
        }

        private void EcToggle_Click(object sender, RoutedEventArgs e)
        {
            if ((sender as FrameworkElement)?.DataContext is EcGroupVm g)
            {
                FlipEcGroup(g.EffectIndex);
                e.Handled = true;
            }
        }

        private void EcHeader_Click(object sender, MouseButtonEventArgs e)
        {
            if ((sender as FrameworkElement)?.DataContext is EcGroupVm g)
            {
                FlipEcGroup(g.EffectIndex);
                e.Handled = true;
            }
        }

        // ---------- About popover: what this effect is and where it lives ----------
        private void EcAbout_Click(object sender, MouseButtonEventArgs e)
        {
            if ((sender as FrameworkElement)?.DataContext is EcGroupVm g &&
                g.EffectIndex >= 0 && g.EffectIndex < _details.Count)
            {
                ShowEcAbout(_details[g.EffectIndex], g.EffectIndex);
                e.Handled = true;
            }
        }

        /// <summary>
        /// AE's About line, made useful: the effect's proper name, the AE
        /// menu group it lives under, vendor/suite, the raw match name and
        /// the compatibility status — everything a downgrade decision
        /// needs, in one small card at the cursor. Handled=true keeps the
        /// header's own toggle from firing on the same click.
        /// </summary>
        private void ShowEcAbout(PresetEffectDetails d, int effectIndex)
        {
            var nameEntry = EffectNameLookup.Resolve(d.MatchName, _names);
            var plug = PluginRecognition.Resolve(d.MatchName, PluginLookup.LoadTable(), _names);

            AboutTitle.Text = EcDisplayName(d);
            string cat = string.IsNullOrEmpty(nameEntry?.category) ? null : nameEntry.category;
            AboutGroup.Text = cat != null
                ? "Effects \u25b8 " + cat
                : "AE menu group unknown for this match name";
            string vendorLine = VendorLine(plug);
            if (!string.IsNullOrEmpty(nameEntry?.version))
                vendorLine += "  \u00b7  v" + nameEntry.version;
            AboutVendor.Text = vendorLine;
            AboutMatch.Text = "match name: " + d.MatchName;
            AboutOrigin.Text = plug.Installed
                ? "found on your system — plugin scan"
                : nameEntry == null
                    ? (plug.Inferred
                        ? "recognized by rule — CC* plug-ins ship with AE"
                        : "not in the reference dataset yet")
                    : (nameEntry.stock ? "stock AE table" : "3rd-party listing") +
                      (string.IsNullOrEmpty(nameEntry.aeVersion) ? "" : " \u00b7 " + nameEntry.aeVersion);
            AboutStatus.Text = _ecStatus.TryGetValue(effectIndex, out string st) && !string.IsNullOrEmpty(st)
                ? "compatibility: " + st
                : "compatibility: unknown";
            EcAboutPop.IsOpen = true;
        }

        // named parameter group disclosure rows
        private void EcSubToggle_Click(object sender, RoutedEventArgs e)
        {
            if ((sender as FrameworkElement)?.DataContext is EcSubGroupVm g)
            {
                FlipEcSub(g);
                e.Handled = true;
            }
        }

        private void EcSub_Click(object sender, MouseButtonEventArgs e)
        {
            if ((sender as FrameworkElement)?.DataContext is EcSubGroupVm g)
            {
                FlipEcSub(g);
                e.Handled = true;
            }
        }

        private void FlipEcSub(EcSubGroupVm g)
        {
            string key = g.EffectIndex + "|" + g.GroupKey;
            // the default here must mirror AddEcNode's (collapsed): the row
            // the user clicked renders from THAT default, so the flip has
            // to read the same base state or the first click does nothing
            bool open = _ecGroupOpen.TryGetValue(key, out bool cur) && cur;
            _ecGroupOpen[key] = !open;
            // in-place fold on the LIVE node — same reasoning as FlipEcGroup
            var live = FindEcSub(_ecGroups, g.EffectIndex, g.GroupKey);
            if (live != null) live.Open = !open;
            else RefreshEcRows();
        }

        /// <summary>Depth-first search for a live sub-group node by effect
        /// index + path, so a disclosure flip touches exactly that node.</summary>
        private static EcSubGroupVm FindEcSub(List<EcGroupVm> groups, int effectIndex, string path)
        {
            if (groups == null) return null;
            foreach (var grp in groups)
            {
                if (grp.EffectIndex != effectIndex) continue;
                var hit = FindEcSubIn(grp.Items, path);
                if (hit != null) return hit;
            }
            return null;
        }

        private static EcSubGroupVm FindEcSubIn(IEnumerable<object> items, string path)
        {
            foreach (var item in items)
            {
                if (item is EcSubGroupVm sub)
                {
                    if (sub.GroupKey == path) return sub;
                    var hit = FindEcSubIn(sub.Items, path);
                    if (hit != null) return hit;
                }
            }
            return null;
        }

        // ---------- inspector ----------
        private PresetEffectDetails CurrentDetails() =>
            _inspEffectIndex >= 0 && _inspEffectIndex < _details.Count ? _details[_inspEffectIndex] : null;

        private PresetParameter CurrentAnimParam()
        {
            var d = CurrentDetails();
            if (d == null || _animParamIndex < 0) return null;
            var anim = d.Parameters.Where(p => p.IsAnimated).ToList();
            return _animParamIndex < anim.Count ? anim[_animParamIndex] : null;
        }

        private void EffectList_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            UpdateInspector();
        }

        private void UpdateInspector()
        {
            if (!(EffectList.SelectedItem is EffectRowVm row))
            {
                _inspEffectIndex = -1;
                SetInspectorEmpty();
                return;
            }
            ShowEffect(row.EffectIndex, row.Name);
        }

        /// <summary>
        /// Populates the inspector for a stable effect index. Normally the
        /// (visible) list row drives this; Effect Controls clicks resolve
        /// their owning effect directly, even when that row is filtered
        /// out of the compatibility list.
        /// </summary>
        private void ShowEffect(int effectIndex, string fallbackName)
        {
            try
            {
                ShowEffectCore(effectIndex, fallbackName);
            }
            catch (Exception ex)
            {
                // the inspector degrades with the reason, like the Effect
                // Controls panel already does per block — a decode or
                // drawing surprise never takes the workspace down
                LogService.Append($"inspector: effect #{effectIndex + 1} couldn't be shown \u2014 {ex.GetType().Name}: {ex.Message}");
                _inspEffectIndex = effectIndex;
                InspTitle.Text = fallbackName;
                InspSub.Text = "\u26a0 couldn't be shown \u2014 " + ex.GetType().Name + ": " + ex.Message;
                InspEmpty.Visibility = Visibility.Collapsed;
                InspUnavailable.Visibility = Visibility.Visible;
                ParamList.ItemsSource = null;
                KfList.ItemsSource = null;
                SetComboSource(null);
                _animParamIndex = -1;
                _selKf = -1;
                StatusBarLeft.Text = "";
                StatusSep.Visibility = Visibility.Collapsed;
                DrawGraph();
            }
        }

        private void ShowEffectCore(int effectIndex, string fallbackName)
        {
            _inspEffectIndex = effectIndex;
            var d = CurrentDetails();

            if (d == null)
            {
                InspTitle.Text = fallbackName;
                InspSub.Text = "No parameter data available";
                InspEmpty.Visibility = Visibility.Collapsed;
                InspUnavailable.Visibility = Visibility.Visible;
                ParamList.ItemsSource = null;
                KfList.ItemsSource = null;
                SetComboSource(null);
                _selKf = -1;
                StatusBarLeft.Text = "";
                StatusSep.Visibility = Visibility.Collapsed;
                DrawGraph();
                return;
            }

            InspTitle.Text = string.IsNullOrEmpty(d.ShortName) ? fallbackName : d.ShortName;
            InspSub.Text = $"{d.MatchName}  ·  {d.Parameters.Count} parameter{(d.Parameters.Count == 1 ? "" : "s")}" +
                           $"  ·  {d.AnimatedCount} animated";
            InspEmpty.Visibility = Visibility.Collapsed;
            InspUnavailable.Visibility = Visibility.Collapsed;

            RefreshParamRows();

            var animParams = d.Parameters.Where(p => p.IsAnimated).ToList();
            SetComboSource(animParams.Select(p => p.Name).ToList());
            _animParamIndex = animParams.Count > 0 ? 0 : -1;
            _selKf = -1;
            _syncingCombo = true;
            KfParamSelect.SelectedIndex = _animParamIndex;
            GraphParamSelect.SelectedIndex = _animParamIndex;
            _syncingCombo = false;

            StatusBarLeft.Text = $"{d.Parameters.Count} parameters · {d.AnimatedCount} animated";
            StatusSep.Visibility = Visibility.Visible;

            BuildKeyframes();
            DrawGraph();
        }

        private void SetInspectorEmpty()
        {
            InspTitle.Text = "Inspector";
            InspSub.Text = "Select an effect to inspect it";
            InspEmpty.Visibility = Visibility.Visible;
            InspUnavailable.Visibility = Visibility.Collapsed;
            ParamList.ItemsSource = null;
            KfList.ItemsSource = null;
            SetComboSource(null);
            _animParamIndex = -1;
            _selKf = -1;
            StatusBarLeft.Text = "";
            StatusSep.Visibility = Visibility.Collapsed;
            BuildKeyframes();
            DrawGraph();
        }

        private void SetComboSource(List<string> names)
        {
            _syncingCombo = true;
            KfParamSelect.ItemsSource = names;
            GraphParamSelect.ItemsSource = names;
            KfParamSelect.SelectedIndex = -1;
            GraphParamSelect.SelectedIndex = -1;
            _syncingCombo = false;
        }

        private void AnimParam_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_syncingCombo) return;
            int idx = sender == KfParamSelect ? KfParamSelect.SelectedIndex : GraphParamSelect.SelectedIndex;
            if (idx < 0) return;
            SelectAnimatedParam(idx);
        }

        /// <summary>
        /// Selects an animated parameter (combo, param row and navigator all
        /// funnel here), resets the keyframe selection and refreshes both tabs.
        /// </summary>
        private void SelectAnimatedParam(int animIndex)
        {
            if (animIndex < 0) return;
            _syncingCombo = true;
            KfParamSelect.SelectedIndex = animIndex;
            GraphParamSelect.SelectedIndex = animIndex;
            _syncingCombo = false;
            _animParamIndex = animIndex;
            _selKf = -1;
            BuildKeyframes();
            DrawGraph();
        }

        /// <summary>
        /// Position of p among the effect's animated parameters. Effect
        /// Controls clicks arrive without a (visible) list selection, so the
        /// owning effect is resolved from the decoded details on demand —
        /// which also makes the hidden inspector's state correct.
        /// </summary>
        private int AnimatedIndexOf(PresetParameter p)
        {
            if (p == null) return -1;
            var d = CurrentDetails();
            if (d != null)
            {
                var anim = d.Parameters.Where(x => x.IsAnimated).ToList();
                int idx = anim.IndexOf(p);
                if (idx >= 0) return idx;
            }
            for (int e = 0; e < _details.Count; e++)
            {
                if (_details[e].Parameters.Contains(p))
                {
                    var det = _details[e];
                    ShowEffect(e, string.IsNullOrEmpty(det.ShortName) ? det.MatchName : det.ShortName);
                    var anim = det.Parameters.Where(x => x.IsAnimated).ToList();
                    return anim.IndexOf(p);
                }
            }
            return -1;
        }

        /// <summary>
        /// Flattens an exception chain into one line — "Type: message ←
        /// caused by Type: message". The inner exception usually names the
        /// real fault (a XamlParseException's cause hides in its
        /// InnerException), so panel messages and log lines must carry the
        /// whole chain, not just the wrapper.
        /// </summary>
        private static string Describe(Exception ex)
        {
            var parts = new List<string>();
            for (var e = ex; e != null; e = e.InnerException)
                parts.Add(e.GetType().Name + ": " + e.Message);
            return string.Join(" \u2190 caused by ", parts);
        }

        /// <summary>
        /// Rebuilds the Parameters tab as one calm read: plain property
        /// lines under quiet group captions that collapse on click now
        /// (state persisted per effect+path, folding in place). The
        /// Keyframes and Graph tabs carry the motion data. Cheap — the
        /// list is at most a few dozen rows.
        /// </summary>
        private void RefreshParamRows()
        {
            var d = CurrentDetails();
            if (d == null)
            {
                ParamList.ItemsSource = null;
                return;
            }
            var root = new List<object>();
            var created = new Dictionary<string, EcSubGroupVm>();
            foreach (var p in d.Parameters)
            {
                var row = new ParamRowVm(p) { RowOpacity = 1.0 };
                if (string.IsNullOrEmpty(p.Group)) root.Add(row);
                else AddInspNode(root, created, _inspEffectIndex, p.Group).Items.Add(row);
            }
            try
            {
                ParamList.ItemsSource = root;
                // realize the row templates HERE, inside the guard: a
                // template fault would otherwise surface in the layout
                // pass, past every catch in this path, as an app-level
                // crash dialog instead of a named inline reason
                ParamList.UpdateLayout();
                ParamEmpty.Text = "No decodable parameters in this effect block.";
                ParamEmpty.Visibility = root.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            }
            catch (Exception ex)
            {
                LogService.Append($"inspector: the parameter panel couldn't be rendered \u2014 {Describe(ex)}");
                ParamList.ItemsSource = null;
                ParamEmpty.Text = "Parameter data for this effect couldn't be rendered \u2014 " + Describe(ex);
                ParamEmpty.Visibility = Visibility.Visible;
            }
        }

        /// <summary>
        /// Inspector group caption for a group path, created on first
        /// encounter. Open by default (the inspector's calm read), but
        /// collapsible now — the state persists per effect+path in
        /// _inspGroupOpen, exactly like the Effect Controls panel's
        /// _ecGroupOpen.
        /// </summary>
        private EcSubGroupVm AddInspNode(List<object> root,
            Dictionary<string, EcSubGroupVm> created, int effectIndex, string path)
        {
            if (created.TryGetValue(path, out var g)) return g;
            string name = path, parent = null;
            int sep = path.LastIndexOf('\u0001');
            if (sep >= 0) { name = path.Substring(sep + 1); parent = path.Substring(0, sep); }
            name = StripDisambiguator(name);
            g = new EcSubGroupVm
            {
                Title = name,
                GroupKey = path,
                EffectIndex = effectIndex,
                Open = _inspGroupOpen.TryGetValue(effectIndex + "|" + path, out bool stored) ? stored : true
            };
            created[path] = g;
            if (parent == null) root.Add(g);
            else AddInspNode(root, created, effectIndex, parent).Items.Add(g);
            return g;
        }

        /// <summary>
        /// Collapses/expands one INSPECTOR parameter group — in place on
        /// the live node (no rebuild, no scroll jump), with the state
        /// persisted for the next rebuild.
        /// </summary>
        private void FlipInspSub(EcSubGroupVm g)
        {
            string key = g.EffectIndex + "|" + g.GroupKey;
            // mirror AddInspNode's default (open) so the first click works
            bool open = _inspGroupOpen.TryGetValue(key, out bool cur) ? cur : true;
            _inspGroupOpen[key] = !open;
            g.Open = !open; // INPC folds the body in place
        }

        private void InspSubToggle_Click(object sender, RoutedEventArgs e)
        {
            if ((sender as FrameworkElement)?.DataContext is EcSubGroupVm g)
            {
                FlipInspSub(g);
                e.Handled = true;
            }
        }

        private void InspSub_Click(object sender, MouseButtonEventArgs e)
        {
            if ((sender as FrameworkElement)?.DataContext is EcSubGroupVm g)
            {
                FlipInspSub(g);
                e.Handled = true;
            }
        }

        // ---------- AE parameter rows: click + keyframe navigator ----------
        // Both parameter UIs funnel here: the inspector's simple ParamRowVm
        // and the Effect Controls' kind-aware EcParamVm share the decoded
        // parameter behind the row.
        private static PresetParameter ParamRefOf(object dc) =>
            (dc as ParamRowVm)?.ParamRef ?? (dc as EcParamVm)?.ParamRef;

        private static bool IsAnimatedRow(object dc) =>
            (dc as ParamRowVm)?.IsAnimated ?? ((dc as EcParamVm)?.IsAnimated ?? false);

        private void ParamRow_Click(object sender, MouseButtonEventArgs e)
        {
            var dc = (sender as FrameworkElement)?.DataContext;
            if (IsAnimatedRow(dc))
                SelectAnimatedParam(AnimatedIndexOf(ParamRefOf(dc)));
        }

        private void KfPrev_Click(object sender, RoutedEventArgs e)
        {
            var dc = (sender as FrameworkElement)?.DataContext;
            if (IsAnimatedRow(dc))
            {
                SelectAnimatedParam(AnimatedIndexOf(ParamRefOf(dc)));
                SelectKeyframe(_selKf < 0 ? 0 : _selKf - 1);
            }
        }

        private void KfNext_Click(object sender, RoutedEventArgs e)
        {
            var dc = (sender as FrameworkElement)?.DataContext;
            if (IsAnimatedRow(dc))
            {
                SelectAnimatedParam(AnimatedIndexOf(ParamRefOf(dc)));
                SelectKeyframe(_selKf < 0 ? 0 : _selKf + 1);
            }
        }

        /// <summary>The diamond button: jump to the Keyframes tab for that stream.</summary>
        private void KfShow_Click(object sender, RoutedEventArgs e)
        {
            var dc = (sender as FrameworkElement)?.DataContext;
            if (IsAnimatedRow(dc))
            {
                SelectAnimatedParam(AnimatedIndexOf(ParamRefOf(dc)));
                if (_viewMode == 0) SetView(1); // the Keyframes tab lives in the inspector
                SetTab(1);
            }
        }

        private void KfRow_Click(object sender, MouseButtonEventArgs e)
        {
            if ((sender as FrameworkElement)?.DataContext is KfRowVm vm)
                SelectKeyframe(vm.KfIndex);
        }

        /// <summary>
        /// Selects a keyframe (clamped): highlights its row, prints its
        /// slope/influence numbers and draws the graph's selection ring.
        /// </summary>
        private void SelectKeyframe(int i)
        {
            var p = CurrentAnimParam();
            if (p == null || p.Keyframes.Count == 0)
            {
                _selKf = -1;
            }
            else
            {
                _selKf = Math.Max(0, Math.Min(p.Keyframes.Count - 1, i));
            }
            BuildKeyframes();
            if (_tab == 2) DrawGraph(); // ring + handles follow the pick
        }

        private void TabBtn_Click(object sender, RoutedEventArgs e)
        {
            if (sender == TabParamsBtn) SetTab(0);
            else if (sender == TabKeyframesBtn) SetTab(1);
            else SetTab(2);
        }

        private void GraphMode_Click(object sender, RoutedEventArgs e)
        {
            _graphMode = sender == GraphSpeedBtn ? 1 : 0;
            GraphValueBtn.IsChecked = _graphMode == 0;
            GraphSpeedBtn.IsChecked = _graphMode == 1;
            DrawGraph();
        }

        private void SetTab(int tab)
        {
            _tab = tab;
            if (TabParamsBtn == null) return; // XAML not loaded yet (design-time)
            TabParamsBtn.IsChecked = tab == 0;
            TabKeyframesBtn.IsChecked = tab == 1;
            TabGraphBtn.IsChecked = tab == 2;
            ParamPane.Visibility = tab == 0 ? Visibility.Visible : Visibility.Collapsed;
            KfPane.Visibility = tab == 1 ? Visibility.Visible : Visibility.Collapsed;
            GraphPane.Visibility = tab == 2 ? Visibility.Visible : Visibility.Collapsed;
            BuildKeyframes();
            DrawGraph();
            if (tab == 2)
            {
                // first paint after the pane was collapsed: ActualWidth is
                // still 0 in this layout pass — repaint once layout is done
                // instead of leaving a blank plot until the SizeChanged timer
                Dispatcher.BeginInvoke(new Action(DrawGraph), DispatcherPriority.Loaded);
            }
        }

        private void BuildKeyframes()
        {
            try
            {
                BuildKeyframesCore();
            }
            catch (Exception ex)
            {
                LogService.Append("keyframes: the list couldn't be built \u2014 " + ex.GetType().Name + ": " + ex.Message);
                KfList.ItemsSource = null;
                KfDetail.Visibility = Visibility.Collapsed;
                KfEmpty.Text = "The keyframe list couldn't be built \u2014 " + ex.GetType().Name + ": " + ex.Message + " (details in About \u2192 Logs)";
                KfEmpty.Visibility = Visibility.Visible;
                DrawKfTimeline();
            }
        }

        private void BuildKeyframesCore()
        {
            var p = CurrentAnimParam();
            if (p == null || p.Keyframes.Count == 0)
            {
                KfList.ItemsSource = null;
                KfEmpty.Text = KfEmptyDefault; // an earlier failure may have left a reason here
                KfEmpty.Visibility = Visibility.Visible;
                KfDetail.Visibility = Visibility.Collapsed;
                DrawKfTimeline();
                return;
            }
            KfEmpty.Visibility = Visibility.Collapsed;
            var kfs = p.Keyframes;
            KfList.ItemsSource = kfs.Select((k, i) =>
                new KfRowVm(i + 1, k, i > 0 ? kfs[i - 1] : null)
                {
                    Selected = i == _selKf
                }).ToList();

            // easing numbers for the selected keyframe — the AE Graph
            // Editor's numeric readout of speed and influence per side
            if (_selKf >= 0 && _selKf < kfs.Count)
            {
                var k = kfs[_selKf];
                KfDetail.Text = $"#{_selKf + 1} {k.InterpLabel} — in:  speed {k.InSlope.ToString("0.###")} /s · influence {Pct(k.InInfluence)}" +
                                $"   ·   out:  speed {k.OutSlope.ToString("0.###")} /s · influence {Pct(k.OutInfluence)}";
                KfDetail.Visibility = Visibility.Visible;

                // keep the picked row visible (the list is small and plain
                // StackPanel-hosted, so the container exists after layout)
                Dispatcher.BeginInvoke(new Action(() =>
                {
                    try
                    {
                        if (KfList.ItemContainerGenerator.ContainerFromIndex(_selKf) is FrameworkElement fe)
                            fe.BringIntoView();
                    }
                    catch { /* cosmetic */ }
                }), DispatcherPriority.Loaded);
            }
            else
            {
                KfDetail.Visibility = Visibility.Collapsed;
            }
            DrawKfTimeline();
        }

        // ---------- AE-style graph ----------
        private const double Fps = 30.0; // frame numbers + grid assume 30 fps

        /// <summary>The hint / empty texts the XAML ships with — restored
        /// whenever a pane falls back to its hint state, so a failure
        /// reason never sticks around after a successful redraw.</summary>
        private const string GraphHintDefault = "Pick an animated parameter to plot its curve \u2014 then click any keyframe to inspect its easing.";
        private const string KfEmptyDefault = "This effect has no animated parameters \u2014 its values are static. Static values are listed under Parameters.";

        /// <summary>AE-style timecode h:mm:ss:ff (hours only when needed).</summary>
        private static string Timecode(double sec)
        {
            int total = (int)Math.Round(sec * Fps);
            int f = total % 30, s = (total / 30) % 60, m = (total / 1800) % 60, h = total / 108000;
            return h > 0
                ? $"{h}:{m.ToString("00")}:{s.ToString("00")}:{f.ToString("00")}"
                : $"0:{m.ToString("00")}:{s.ToString("00")}:{f.ToString("00")}";
        }

        /// <summary>AE shows influences as percentages ("Influence: 33.3%").</summary>
        private static string Pct(double v) => (v * 100).ToString("0.#") + "%";

        /// <summary>
        /// The two editor skins, picked by the app theme - AE's own editor
        /// follows AE's UI brightness the same way. Both keep AE's editor
        /// grammar (grid rhythm, emphasized zero line, #FFEE00 picked key,
        /// #FC0000 current-time line, in-field readout). Light theme:
        /// the app's own light tokens. Dark theme: AE's anatomy re-derived
        /// from a neutral dark field (#3B3F41) instead of AE's sampled
        /// #656565 - the sampled brightness (L*≈41) glares as a mid-gray
        /// island on this app's near-black dark chrome (surface L*≈6,
        /// cards L*≈12), which read as broken dark mode; at L*≈25 the pane
        /// stays unmistakably the editor while belonging to the window.
        /// </summary>
        private sealed class GraphSkin
        {
            public Brush Bg, Grid, Zero, Curve, Label, Key, KeyStroke, KeySel, Cti;
            public Brush Minor;   // faint half-step gridline between the majors
            public Brush Edge;    // the pane's 1px outline (reads on light theme)
            public double BgRadius;
        }

        private static Brush HexBrush(string hex, double alpha = 1.0)
        {
            var brush = new SolidColorBrush((Color)ColorConverter.ConvertFromString(hex));
            if (alpha < 1.0) brush.Opacity = alpha;
            brush.Freeze();
            return brush;
        }

        /// <summary>A theme brush at reduced opacity, frozen - the minor
        /// gridlines derive from the palette without hard-coding grays.</summary>
        private static Brush OpacityBrush(Brush src, double opacity)
        {
            var b = src.Clone();
            b.Opacity = opacity;
            b.Freeze();
            return b;
        }

        private GraphSkin _skinDark, _curSkin;

        /// <summary>The dark editor: AE's anatomy on a field tuned to
        /// this app's dark chrome (see the class comment for why the
        /// sampled #656565 was retired).</summary>
        private GraphSkin DarkSkin()
        {
            if (_skinDark != null) return _skinDark;
            _skinDark = new GraphSkin
            {
                Bg = HexBrush("#3B3F41"),
                Grid = HexBrush("#343839"),
                Minor = HexBrush("#36393B"),
                Zero = HexBrush("#4E5354"),
                Edge = HexBrush("#2B2E2F"),
                Curve = HexBrush("#D6D9DA"),
                Label = HexBrush("#C4C4C4"),
                Key = HexBrush("#E6E6E6"),
                KeyStroke = HexBrush("#333739"),
                KeySel = HexBrush("#FFEE00"),
                Cti = HexBrush("#FC0000"),
                BgRadius = 4
            };
            return _skinDark;
        }

        /// <summary>The light-theme editor: the app's OWN tokens, read
        /// fresh at every draw - ThemeService swaps the merged color
        /// dictionary, so a cached brush would freeze with the startup
        /// palette (the phase-4 lesson). AE's selection yellow and CTI
        /// red stay fixed: they read on any brightness.</summary>
        private GraphSkin LightSkin()
        {
            return new GraphSkin
            {
                Bg = (Brush)FindResource("B.Surface"),
                Grid = (Brush)FindResource("B.OutlineVariant"),
                Minor = OpacityBrush((Brush)FindResource("B.OutlineVariant"), 0.55),
                Zero = (Brush)FindResource("B.Outline"),
                Edge = (Brush)FindResource("B.OutlineVariant"),
                Curve = (Brush)FindResource("B.Primary"),
                Label = (Brush)FindResource("B.OnSurfaceVariant"),
                Key = (Brush)FindResource("B.Primary"),
                KeyStroke = (Brush)FindResource("B.OnSurface"),
                KeySel = HexBrush("#FFEE00"),
                Cti = HexBrush("#FC0000"),
                BgRadius = 4
            };
        }

        /// <summary>The graph follows the app theme - light editor on a
        /// light app, AE's dark editor on a dark one (the round-25 ask;
        /// round-24's always-dark editor is retired). AE behaves the
        /// same way: its editor follows AE's UI brightness.</summary>
        private GraphSkin SkinNow()
        {
            return ThemeService.Mode == Md3Mode.Dark ? DarkSkin() : LightSkin();
        }

        /// <summary>Timeline-strip markers stay themed chrome - accent on
        /// the theme surface, not the AE editor pane.</summary>
        private GraphSkin TimelineSkin()
        {
            return new GraphSkin
            {
                Key = (Brush)FindResource("B.Primary"),
                KeyStroke = (Brush)FindResource("B.Surface"),
                Bg = (Brush)FindResource("B.Surface")
            };
        }

        private sealed class PlotState
        {
            public List<PresetCurve.Segment> Segs;
            public List<PresetCurve.Segment> Segs1; // dimension 1 of a 2D stream (null on 1D)
            public double T0, T1;      // seconds span
            public double W, H;        // canvas size
            public double L, R, T, Bot; // margins
            public int Mode;           // 0 value, 1 speed
            public double VMin, VMax;  // value mode y-range
            public double SpeedMin, SpeedMax; // speed mode y-range (SIGNED)
            public bool Is2D;          // 2D stream: value plots X+Y curves, speed plots magnitude
        }
        private PlotState _plot;
        private Line _cursorLine;
        private Ellipse _cursorDot;

        /// <summary>
        /// AE's Graph Editor, redrawn to the user's reference screenshots,
        /// wearing the APP THEME (the round-25 ask): dark app = AE's dark
        /// editor palette, light app = the app's own light editor tokens.
        /// Value mode draws the thin curve(s) - ONE PER DIMENSION on a 2D
        /// stream, exactly AE's X+Y pair - with square keys, the picked
        /// key painted editor yellow with its direction handles, DASHED
        /// carries beyond the keyed span and the readout above the plot.
        /// Speed mode plots AE's speed: signed dv/dt for 1D (one analytic
        /// arc per segment, TRUE VERTICAL jump lines at discontinuities)
        /// or the ONE combined magnitude curve AE draws for 2D, zero
        /// carries outside the keyed span, interpolation-shaped key icons
        /// (circle = bezier, square = linear, half = hold), no area fill.
        /// Pure WPF shapes - Win7-safe, no bitmap effects. Curve geometry
        /// is deliberately NOT pixel-snapped: rounding every coordinate
        /// quantized the beziers into visible kinks.
        /// </summary>
        private void DrawGraph()
        {
            if (GraphCanvas == null) return;
            try
            {
                DrawGraphCore();
            }
            catch (Exception ex)
            {
                // a drawing surprise degrades the pane with the reason
                // instead of riding the global crash dialog; the SizeChanged
                // redraws keep calling in, so the next pass repaints normal
                LogService.Append("graph: couldn't be drawn \u2014 " + ex.GetType().Name + ": " + ex.Message);
                GraphCanvas.Children.Clear();
                _cursorLine = null;
                _cursorDot = null;
                _plot = null;
                if (GraphReadout != null) GraphReadout.Visibility = Visibility.Collapsed;
                GraphHint.Text = "The graph couldn't be drawn \u2014 " + ex.GetType().Name + ": " + ex.Message;
                GraphHint.Visibility = Visibility.Visible;
            }
        }

        private void DrawGraphCore()
        {
            if (GraphCanvas == null) return;
            GraphCanvas.Children.Clear();
            _cursorLine = null;
            _cursorDot = null;
            if (GraphReadout != null) GraphReadout.Visibility = Visibility.Collapsed;
            _plot = null;

            var p = CurrentAnimParam();
            if (p == null || p.Keyframes.Count == 0)
            {
                GraphHint.Text = GraphHintDefault; // an earlier failure may have left a reason here
                GraphHint.Visibility = Visibility.Visible;
                return;
            }
            GraphHint.Visibility = Visibility.Collapsed;

            double w = GraphCanvas.ActualWidth, h = GraphCanvas.ActualHeight;
            if (w < 60 || h < 60) return; // not laid out yet - SizeChanged redraws

            var skin = SkinNow();
            _curSkin = skin;

            var segs = PresetCurve.BuildSegments(p.Keyframes);
            // a one-keyframe stream has no spans but still plots - the value
            // branch draws it as AE does: a constant. No hint fallback here.

            // 2D stream: dimension 1's own segments drive AE's second
            // value curve and the combined speed magnitude. A Y side
            // that failed to decode (< 2 points) degrades the plot to
            // the honest 1D read.
            bool twoDim = p.Keyframes[0].DimCount > 1;
            var segs1 = twoDim ? PresetCurve.BuildSegments(p.Keyframes, 1) : null;
            if (segs1 != null && segs1.Count == 0) segs1 = null;
            if (GraphLegend != null)
                GraphLegend.Text = twoDim
                    ? (_graphMode == 0
                        ? "value over time · X + Y curves · left ruler = value"
                        : "speed magnitude · X + Y combined · left ruler = units/sec")
                    : (_graphMode == 0
                        ? "value over time · left ruler = value"
                        : "signed speed Δvalue/Δt · left ruler = units/sec");

            // AE's editor margins: the value ruler lives on the LEFT (the
            // reference screenshots' "-500" / "50" / "0" column), the time
            // ruler along the bottom, and the current-value readout above
            // the plot's top-left corner ("0 px/sec")
            const double L = 46, R = 14, T = 22, Bot = 24;
            // span from the KEYFRAMES, not the segments - a one-keyframe
            // stream has zero segments yet still plots as a constant
            double t0 = PresetCurve.Seconds(p.Keyframes[0].Time);
            double t1 = PresetCurve.Seconds(p.Keyframes[p.Keyframes.Count - 1].Time);
            if (t1 - t0 < 1e-6) t1 = t0 + 0.5; // single-instant stream still gets an axis
            // AE's Graph Editor floats the keyframed span inside a wider
            // time field - the value HOLDS before the first and after the
            // last key (a preset lives on a longer layer). Pad the window
            // by a tenth of the span each side and draw those holds, so
            // the curve reads with approach/exit context and the eased
            // part stops touching both plot edges.
            double tw = t1 - t0;
            double w0 = t0 - tw * 0.1, w1 = t1 + tw * 0.1;
            Func<double, double> xOf = t => L + (t - w0) / (w1 - w0) * (w - L - R);

            var plot = new PlotState
            {
                Segs = segs, Segs1 = segs1, Is2D = twoDim, T0 = w0, T1 = w1,
                W = w, H = h, L = L, R = R, T = T, Bot = Bot, Mode = _graphMode
            };

            // AE's red current-time indicator: at the picked keyframe's
            // time when one is selected, else at the stream's first key -
            // where both reference screenshots park it
            double ctiT = t0;
            if (_selKf >= 0 && _selKf < p.Keyframes.Count)
                ctiT = PresetCurve.Seconds(p.Keyframes[_selKf].Time);
            double ctiX = Math.Min(Math.Max(xOf(ctiT), L), w - R);

            // AE's current-value readout above the plot: the property value
            // (value mode) or the speed (speed mode) AT the current-time
            // line, with AE's "/sec" unit on speed
            string readout;
            if (_graphMode == 0)
            {
                double vAt = PresetCurve.ValueAt(segs, ctiT);
                if (double.IsNaN(vAt))
                    vAt = p.Keyframes[_selKf >= 0 && _selKf < p.Keyframes.Count ? _selKf : 0].Value;
                readout = vAt.ToString("0.###") + " units";
                if (twoDim)
                {
                    // AE's readout for a point carries the PAIR
                    double v2At = PresetCurve.ValueAt(segs1, ctiT);
                    if (double.IsNaN(v2At))
                    {
                        var ck = p.Keyframes[_selKf >= 0 && _selKf < p.Keyframes.Count ? _selKf : 0];
                        if (!double.IsNaN(ck.Value2)) v2At = ck.Value2;
                    }
                    if (!double.IsNaN(v2At))
                        readout = vAt.ToString("0.###") + ", " + v2At.ToString("0.###") + " units";
                }
            }
            else
            {
                double sAt = twoDim
                    ? PresetCurve.SpeedMagnitudeAt(segs, segs1, ctiT)
                    : PresetCurve.SpeedAt(segs, ctiT);
                if (double.IsNaN(sAt)) sAt = 0;
                readout = sAt.ToString("0.###") + " units/sec";
            }
            // the pane itself: a quiet field for grid, curve and markers,
            // outlined so the light editor reads as a panel on the card
            // (WPF Shapes.Rectangle has no X/Y - a Canvas child is placed
            // with Canvas.SetLeft/SetTop, not object-initializer coordinates)
            var backdrop = new Rectangle
            {
                Width = Math.Max(0, w - L - R + 12),
                Height = Math.Max(0, h - T - Bot + 12),
                RadiusX = skin.BgRadius, RadiusY = skin.BgRadius,
                Fill = skin.Bg,
                Stroke = skin.Edge,
                StrokeThickness = 1
            };
            Canvas.SetLeft(backdrop, L - 6);
            Canvas.SetTop(backdrop, T - 6);
            GraphCanvas.Children.Add(backdrop);

            // AE's current-value readout sits top-left INSIDE the editor
            // field (the reference screenshots' "0 units/sec" corner) -
            // added after the backdrop so the pane never paints over it
            var readoutLbl = new TextBlock { Text = readout, FontSize = 10, Foreground = skin.Label };
            Canvas.SetLeft(readoutLbl, L + 8);
            Canvas.SetTop(readoutLbl, T + 6);
            GraphCanvas.Children.Add(readoutLbl);

            DrawTimeGrid(w0, w1, xOf, w, h, T, Bot, L, R, skin.Grid, skin.Label);

            // AE's red current-time line, above grid, beneath the curve
            GraphCanvas.Children.Add(new Line
            {
                X1 = Math.Round(ctiX) + 0.5, X2 = Math.Round(ctiX) + 0.5,
                Y1 = T, Y2 = h - Bot,
                Stroke = skin.Cti, StrokeThickness = 1.5,
                IsHitTestVisible = false
            });

            double plotH = h - T - Bot;
            if (_graphMode == 0)
            {
                // AE scales the plot to the CURVE: keyframe values plus the
                // path's own samples set the range BEFORE the y mapping is
                // built, so the scale can never depend on draw order
                double vMin = p.Keyframes.Min(k => k.Value);
                double vMax = p.Keyframes.Max(k => k.Value);
                PresetCurve.SampleValues(segs, 240, out _, out var sampled);
                foreach (double v in sampled)
                {
                    if (v < vMin) vMin = v;
                    if (v > vMax) vMax = v;
                }
                if (segs1 != null)
                {
                    // the Y curve shares the plot - its extremes set
                    // the scale too (AE fits BOTH dimensions)
                    foreach (var k in p.Keyframes)
                    {
                        if (double.IsNaN(k.Value2) || double.IsInfinity(k.Value2)) continue;
                        if (k.Value2 < vMin) vMin = k.Value2;
                        if (k.Value2 > vMax) vMax = k.Value2;
                    }
                    PresetCurve.SampleValues(segs1, 240, out _, out var sampled1);
                    foreach (double v in sampled1)
                    {
                        if (v < vMin) vMin = v;
                        if (v > vMax) vMax = v;
                    }
                }
                if (vMax - vMin < 1e-9) { vMin -= 1; vMax += 1; }
                else { double pad = (vMax - vMin) * 0.12; vMin -= pad; vMax += pad; }
                plot.VMin = vMin; plot.VMax = vMax;

                Func<double, double> yOf = v => T + (plot.VMax - v) / Math.Max(plot.VMax - plot.VMin, 1e-9) * plotH;
                DrawValueGrid(plot, skin, yOf);
                if (segs.Count == 0)
                    DrawSingleKeyframePlot(plot, p.Keyframes[0], skin, yOf);
                else
                    DrawValueCurve(plot, p, skin, yOf);
                DrawSelection(plot, yOf, skin);
            }
            else
            {
                DrawSpeedCurve(plot, skin, p);
                DrawSelection(plot, null, skin);
            }

            _plot = plot;
        }
        /// <summary>
        /// Coalesces redraw storms while the window is resized: each
        /// SizeChanged restarts a 70 ms one-shot; the graph rebuilds once
        /// the storm pauses, then once more when it ends.
        /// </summary>
        private void ScheduleGraphRedraw()
        {
            if (_graphRedrawTimer == null)
            {
                _graphRedrawTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(70) };
                _graphRedrawTimer.Tick += (s, e) =>
                {
                    _graphRedrawTimer.Stop();
                    DrawGraph();
                };
            }
            _graphRedrawTimer.Stop();
            _graphRedrawTimer.Start();
        }

        /// <summary>Vertical grid at whole-frame multiples of 30 fps.</summary>
        private void DrawTimeGrid(double t0, double t1, Func<double, double> xOf,
            double w, double h, double top, double bot, double L, double R,
            Brush gridBrush, Brush labelBrush)
        {
            // pick the coarsest whole-frame step that keeps labels ~78px apart
            double targetLines = Math.Max(2.0, (w - L - R) / 78.0);
            double frameSpan = (t1 - t0) * Fps;
            double needStep = frameSpan / targetLines;
            double[] steps = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200 };
            double step = steps.FirstOrDefault(s => s >= needStep);
            if (step == 0) step = steps[steps.Length - 1];

            double stepSec = step / Fps;
            int first = (int)Math.Ceiling(t0 * Fps / step - 1e-9);
            double lastLabelX = -1000;
            for (int f = first; ; f += (int)step)
            {
                double t = f / Fps;
                if (t > t1 + 1e-9) break;
                double x = Math.Round(xOf(t)) + 0.5;
                GraphCanvas.Children.Add(new Line
                {
                    X1 = x, X2 = x, Y1 = top, Y2 = h - bot,
                    Stroke = gridBrush, StrokeThickness = 1
                });
                if (x - lastLabelX >= 44)
                {
                    AddTimeLabel(x + 4, h - bot + 6, t.ToString("0.##") + "s", labelBrush);
                    lastLabelX = x;
                }
            }

            // AE's dense minor frame ticks between the labeled lines — no
            // labels, just the rhythm of frames under the cursor
            int minor = Math.Max(1, (int)(step / 5));
            double minorPx = (w - L - R) * (minor / (double)Fps) / Math.Max(t1 - t0, 1e-9);
            if (minorPx >= 10)
            {
                int fStart = (int)Math.Ceiling(t0 * Fps / minor - 1e-9);
                for (int f = fStart; ; f += minor)
                {
                    double t = f / Fps;
                    if (t > t1 + 1e-9) break;
                    double x = Math.Round(xOf(t)) + 0.5;
                    GraphCanvas.Children.Add(new Line
                    {
                        X1 = x, X2 = x, Y1 = h - bot - 5, Y2 = h - bot,
                        Stroke = gridBrush, StrokeThickness = 1
                    });
                }
            }
        }

        /// <summary>
        /// Horizontal value grid + AE's LEFT ruler - the screenshots' value
        /// column ("50" / "0") right-aligned into the left gutter. Labels
        /// format to their step (AxisNum): the old "0.##" collapsed every
        /// small-magnitude gridline to "0", which read as a broken axis on
        /// presets whose values live below 0.01.
        /// </summary>
        private void DrawValueGrid(PlotState plot, GraphSkin skin, Func<double, double> yOf)
        {
            double vStep = NiceStep((plot.VMax - plot.VMin) / 3.2);
            double lastLabelY = -1000;
            for (double v = Math.Ceiling(plot.VMin / vStep) * vStep; v <= plot.VMax + 1e-9; v += vStep)
            {
                bool zero = Math.Abs(v) < vStep * 1e-6;
                double y = Math.Round(yOf(v)) + 0.5;
                GraphCanvas.Children.Add(new Line
                {
                    X1 = plot.L, X2 = plot.W - plot.R, Y1 = y, Y2 = y,
                    Stroke = zero ? skin.Zero : skin.Grid,
                    StrokeThickness = zero ? 1.2 : 1
                });
                // AE's fainter half-step line between the majors - the
                // rhythm the eye reads grid rhythm from, at half weight
                double yMid = Math.Round(yOf(v + vStep / 2)) + 0.5;
                if (v + vStep / 2 <= plot.VMax && yMid > plot.T && yMid < plot.H - plot.Bot)
                    GraphCanvas.Children.Add(new Line
                    {
                        X1 = plot.L, X2 = plot.W - plot.R, Y1 = yMid, Y2 = yMid,
                        Stroke = skin.Minor, StrokeThickness = 1
                    });
                if (Math.Abs(y - lastLabelY) >= 13)
                {
                    var lbl = new TextBlock
                    {
                        Text = AxisNum(v, vStep),
                        FontSize = 10,
                        Foreground = skin.Label,
                        Width = plot.L - 8,
                        TextAlignment = TextAlignment.Right
                    };
                    Canvas.SetLeft(lbl, 2);
                    Canvas.SetTop(lbl, y - 6);
                    GraphCanvas.Children.Add(lbl);
                    lastLabelY = y;
                }
            }
        }

        /// <summary>
        /// A one-keyframe stream is animated but has no spans: AE shows a
        /// constant. Draw the flat value line plus its keyframe marker -
        /// the old code fell back to the "pick a parameter" hint here,
        /// which read as a broken graph.
        /// </summary>
        private void DrawSingleKeyframePlot(PlotState plot, PresetKeyframe kf,
            GraphSkin skin, Func<double, double> yOf)
        {
            double y = yOf(kf.Value);
            GraphCanvas.Children.Add(new Line
            {
                X1 = plot.L, Y1 = y, X2 = plot.W - plot.R, Y2 = y,
                Stroke = skin.Curve, StrokeThickness = 2, Opacity = 0.9
            });
            if (kf.DimCount > 1 && !double.IsNaN(kf.Value2))
                GraphCanvas.Children.Add(new Line
                {
                    X1 = plot.L, Y1 = yOf(kf.Value2), X2 = plot.W - plot.R, Y2 = yOf(kf.Value2),
                    Stroke = skin.Curve, StrokeThickness = 2, Opacity = 0.9
                });
            string dims = kf.DimCount > 1 ? "  \u00b7  2D stream" : "";
            string tip = $"{Timecode(PresetCurve.Seconds(kf.Time))}  \u00b7  constant value {kf.Value.ToString("0.###")}{dims}  \u00b7  click to select";
            GraphCanvas.Children.Add(MakeMarker(PresetCurve.InterpLinear,
                XOf(plot, PresetCurve.Seconds(kf.Time)), y, skin, tip, 0, false));
        }

        /// <summary>
        /// Axis number format that adapts to the step size: whole steps get
        /// "0.#", fractional steps get exactly as many decimals as the step
        /// needs, and extreme magnitudes fall back to significant digits.
        /// </summary>
        private static string AxisNum(double v, double step)
        {
            if (Math.Abs(v) < 1e-12) return "0";
            if (Math.Abs(v) >= 100000 || Math.Abs(v) < 0.0005) return v.ToString("G4");
            int dec = 6;
            for (int d = 0; d <= 6; d++)
            {
                double scaled = step * Math.Pow(10, d);
                if (Math.Abs(scaled - Math.Round(scaled)) < 1e-6) { dec = d; break; }
            }
            string s = v.ToString("F" + dec);
            if (dec > 0) s = s.TrimEnd('0').TrimEnd('.');
            return s;
        }

        /// <summary>The value curve for ONE dimension as a styled path -
        /// identical line grammar for X and Y (AE draws the pair alike).</summary>
        private Path ValueCurvePath(PlotState plot, List<PresetCurve.Segment> segs,
            Func<double, double> yOf, GraphSkin skin)
        {
            var geo = new StreamGeometry();
            using (var ctx = geo.Open())
            {
                ctx.BeginFigure(new Point(XOf(plot, segs[0].T0), yOf(segs[0].V0)), false, false);
                foreach (var s in segs)
                {
                    var end = new Point(XOf(plot, s.T1), yOf(s.V1));
                    if (s.Mode == PresetCurve.InterpHold)
                    {
                        ctx.LineTo(new Point(end.X, yOf(s.V0)), true, false);
                        ctx.LineTo(end, true, false);
                    }
                    else if (s.Mode == PresetCurve.InterpLinear)
                    {
                        ctx.LineTo(end, true, false);
                    }
                    else
                    {
                        var c1 = new Point(XOf(plot, s.C1T), yOf(s.C1V));
                        var c2 = new Point(XOf(plot, s.C2T), yOf(s.C2V));
                        ctx.BezierTo(c1, c2, end, true, false);
                    }
                }
            }
            geo.Freeze();
            // AE's editor draws a plain thin line - no halo in either
            // mode (the round-24 screenshots have no fringe)
            return new Path
            {
                Data = geo,
                Stroke = skin.Curve,
                StrokeThickness = 2,
                StrokeLineJoin = PenLineJoin.Round,
                StrokeStartLineCap = PenLineCap.Round,
                StrokeEndLineCap = PenLineCap.Round
            };
        }

        /// <summary>The DASHED value carry beyond a dimension's keyed
        /// span (AE's stub after the last key), for either dimension.</summary>
        private Path DashedCarryPath(PlotState plot, List<PresetCurve.Segment> segs,
            Func<double, double> yOf, GraphSkin skin)
        {
            var hold = new StreamGeometry();
            using (var hctx = hold.Open())
            {
                if (plot.T0 < segs[0].T0 - 1e-9)
                {
                    hctx.BeginFigure(new Point(XOf(plot, plot.T0), yOf(segs[0].V0)), false, false);
                    hctx.LineTo(new Point(XOf(plot, segs[0].T0), yOf(segs[0].V0)), true, false);
                }
                double lastT = segs[segs.Count - 1].T1;
                if (plot.T1 > lastT + 1e-9)
                {
                    hctx.BeginFigure(new Point(XOf(plot, lastT), yOf(segs[segs.Count - 1].V1)), false, false);
                    hctx.LineTo(new Point(XOf(plot, plot.T1), yOf(segs[segs.Count - 1].V1)), true, false);
                }
            }
            hold.Freeze();
            return new Path
            {
                Data = hold,
                Stroke = skin.Curve,
                StrokeThickness = 1.2,
                Opacity = 0.85,
                StrokeDashArray = new DoubleCollection { 2.5, 2.5 },
                StrokeStartLineCap = PenLineCap.Flat,
                StrokeEndLineCap = PenLineCap.Flat
            };
        }

        private void DrawValueCurve(PlotState plot, PresetParameter stream, GraphSkin skin, Func<double, double> yOf)
        {
            var segs = plot.Segs;

            // Direction lines belong to the PICKED key only - the AE
            // reference screenshot's unselected keys carry none
            // (round-23 drew every bezier key's lines; disproven by the
            // round-24 screenshots). DrawSelection owns them, so this
            // pass draws curve + keys, nothing else.

            // the curve itself - smooth coordinates, no pixel snapping
            GraphCanvas.Children.Add(ValueCurvePath(plot, segs, yOf, skin));
            // AE's value editor draws ONE CURVE PER DIMENSION for a 2D
            // property (round-25 research: "when you animate Position,
            // the value graph shows two separate lines, X and Y") - the
            // Y curve comes from its own tangent block and rides the
            // same editor with the same line style
            if (plot.Segs1 != null)
                GraphCanvas.Children.Add(ValueCurvePath(plot, plot.Segs1, yOf, skin));

            // the holds outside the keyframed span - AE's value carries
            // before the first and after the last key as a DASHED
            // extension (the reference screenshot's dashes after the last
            // key), so the eased span stays the solid visual anchor
            if (plot.T0 < segs[0].T0 - 1e-9 || plot.T1 > segs[segs.Count - 1].T1 + 1e-9)
                GraphCanvas.Children.Add(DashedCarryPath(plot, segs, yOf, skin));
            if (plot.Segs1 != null &&
                (plot.T0 < plot.Segs1[0].T0 - 1e-9 || plot.T1 > plot.Segs1[plot.Segs1.Count - 1].T1 + 1e-9))
                GraphCanvas.Children.Add(DashedCarryPath(plot, plot.Segs1, yOf, skin));

            // keyframe markers - AE's value editor draws squares regardless
            // of easing; clickable to select (ring + handles + numbers).
            // A multidimensional stream says so right in the tooltip: the
            // graph plots dimension 0 (X), never a silent half-truth.
            for (int i = 0; i < stream.Keyframes.Count; i++)
            {
                var k = stream.Keyframes[i];
                int shape = i + 1 < stream.Keyframes.Count ? k.InterpOut : k.InterpIn;
                double x = XOf(plot, PresetCurve.Seconds(k.Time));
                double y = yOf(k.Value);
                string dims = k.DimCount > 1
                    ? $"  \u00b7  X of {k.DimCount}D (Y = {k.Value2.ToString("0.###")})"
                    : "";
                string tip = $"#{i + 1}  {Timecode(PresetCurve.Seconds(k.Time))}" +
                             $"  \u00b7  value {k.Value.ToString("0.###")}{dims}  \u00b7  {k.InterpLabel}  \u00b7  click to select";
                int idx = i;
                var marker = MakeMarker(shape, x, y, skin, tip, idx, false);
                GraphCanvas.Children.Add(marker);
            }
        }

        private void DrawSpeedCurve(PlotState plot, GraphSkin skin, PresetParameter stream0)
        {
            var segs = plot.Segs;
            var stream = stream0 ?? CurrentAnimParam();
            // the magnitude read needs dimension 1 aligned segment-for-
            // segment with dimension 0 (same key count); anything else
            // degrades the speed pane to the honest 1D signed read
            bool sp2D = plot.Segs1 != null && plot.Segs1.Count == segs.Count;

            // AE's speed graph plots the SIGNED derivative - a growing
            // value rides above zero, a shrinking one below (the reference
            // screenshot dips under its -500 ruler line). The range always
            // includes the zero line, padded so the extremes breathe.
            // A 2D property plots the ONE magnitude curve AE draws for it
            // (round-25 research) - sqrt(vx²+vy²), always at or above zero.
            // Each segment contributes ONE exact arc - the analytic
            // dv/dt of its own cubic, evaluated straight from the control
            // points (the old 320-point SpeedAt sweep smeared every
            // keyframe discontinuity into a ~1-pixel slant and rode
            // sampling noise on top of the arcs). Where a keyframe's
            // incoming and outgoing speeds disagree, AE connects the two
            // levels with a TRUE VERTICAL jump line at the key's time;
            // zero carries fill the window outside the keyed span - the
            // reference screenshot's flat approach and the vertical rise
            // at its right edge are exactly these.
            double st0 = plot.T0, st1 = plot.T1;
            // analytic dv/dt straight from a segment's control points -
            // dim 0 for the 1D read; per-dimension for the 2D magnitude
            Func<List<PresetCurve.Segment>, int, double, double> segSpeed = (list, i, u) =>
            {
                var s = list[i];
                if (s.Mode == PresetCurve.InterpHold) return 0;
                if (s.Mode == PresetCurve.InterpLinear)
                    return (s.V1 - s.V0) / Math.Max(s.T1 - s.T0, 1e-9);
                double m = 1 - u;
                double dv = 3 * m * m * (s.C1V - s.V0) + 6 * m * u * (s.C2V - s.C1V) + 3 * u * u * (s.V1 - s.C2V);
                double dt = 3 * m * m * (s.C1T - s.T0) + 6 * m * u * (s.C2T - s.C1T) + 3 * u * u * (s.T1 - s.C2T);
                if (Math.Abs(dt) < 1e-12) return 0;
                double v = dv / dt;
                return double.IsNaN(v) || double.IsInfinity(v) ? 0 : v;
            };
            Func<int, double, double> segSpeedAt = (i, u) => segSpeed(segs, i, u);
            // the 2D magnitude at TIME t: each dimension solves its own
            // segment for t, then the rates combine as sqrt(vx²+vy²)
            Func<double, double> magAt = t =>
            {
                if (!sp2D) return 0;
                double vx = PresetCurve.SpeedAt(segs, t);
                double vy = PresetCurve.SpeedAt(plot.Segs1, t);
                if (double.IsNaN(vx) || double.IsNaN(vy)) return 0;
                return Math.Sqrt(vx * vx + vy * vy);
            };

            // RANGE: coarse arc samples (real extremes ride the arcs or
            // their keyframe endpoints); the zero carries keep 0 in view
            double spMin = 0, spMax = 0;
            if (sp2D)
            {
                // the magnitude is >= 0; sample each span's time evenly
                // (each dimension solves its own segment for that time)
                for (int i = 0; i < segs.Count; i++)
                {
                    if (segs[i].Mode == PresetCurve.InterpHold) continue;
                    for (int k = 0; k <= 24; k++)
                    {
                        double t = segs[i].T0 + (segs[i].T1 - segs[i].T0) * (k / 24.0);
                        double s = magAt(t);
                        if (s < spMin) spMin = s;
                        if (s > spMax) spMax = s;
                    }
                }
            }
            else
            {
                for (int i = 0; i < segs.Count; i++)
                {
                    if (segs[i].Mode == PresetCurve.InterpHold) continue; // flat zero
                    if (segs[i].Mode == PresetCurve.InterpLinear)
                    {
                        double c = (segs[i].V1 - segs[i].V0) / Math.Max(segs[i].T1 - segs[i].T0, 1e-9);
                        if (c < spMin) spMin = c;
                        if (c > spMax) spMax = c;
                        continue;
                    }
                    for (int k = 0; k <= 24; k++)
                    {
                        double s = segSpeedAt(i, k / 24.0);
                        if (s < spMin) spMin = s;
                        if (s > spMax) spMax = s;
                    }
                }
            }
            double spPad = Math.Max((spMax - spMin) * 0.08, 1e-9);
            spMin -= spPad; spMax += spPad;
            plot.SpeedMin = spMin; plot.SpeedMax = spMax;

            double plotH = plot.H - plot.T - plot.Bot;
            Func<double, double> yOf = v => plot.T + (plot.SpeedMax - v) / Math.Max(plot.SpeedMax - plot.SpeedMin, 1e-9) * plotH;

            // horizontal speed grid with AE's LEFT ruler - bare numbers
            // (the readout above the plot carries "units/sec"), and a
            // denser ladder than the value pane: AE's speed editor shows
            // six-ish lines (the reference's 0/-500/.../-3000 column)
            double vStep = NiceStep((plot.SpeedMax - plot.SpeedMin) / 6.0);
            double lastLabelY = -1000;
            for (double v = Math.Ceiling(plot.SpeedMin / vStep) * vStep; v <= plot.SpeedMax + 1e-9; v += vStep)
            {
                bool zero = Math.Abs(v) < vStep * 1e-6;
                double y = Math.Round(yOf(v)) + 0.5;
                GraphCanvas.Children.Add(new Line
                {
                    X1 = plot.L, X2 = plot.W - plot.R, Y1 = y, Y2 = y,
                    Stroke = zero ? skin.Zero : skin.Grid,
                    StrokeThickness = zero ? 1.2 : 1
                });
                double yMid = Math.Round(yOf(v + vStep / 2)) + 0.5;
                if (v + vStep / 2 <= plot.SpeedMax && yMid > plot.T && yMid < plot.H - plot.Bot)
                    GraphCanvas.Children.Add(new Line
                    {
                        X1 = plot.L, X2 = plot.W - plot.R, Y1 = yMid, Y2 = yMid,
                        Stroke = skin.Minor, StrokeThickness = 1
                    });
                if (Math.Abs(y - lastLabelY) >= 13)
                {
                    var lbl = new TextBlock
                    {
                        Text = AxisNum(v, vStep),
                        FontSize = 10,
                        Foreground = skin.Label,
                        Width = plot.L - 8,
                        TextAlignment = TextAlignment.Right
                    };
                    Canvas.SetLeft(lbl, 2);
                    Canvas.SetTop(lbl, y - 6);
                    GraphCanvas.Children.Add(lbl);
                    lastLabelY = y;
                }
            }

            // the curve: zero carry -> per-key jumps -> one exact arc per
            // segment -> zero carry. Neighbors whose one-sided speeds
            // agree (Easy Ease's zero-to-zero handoff) stay ONE stroke.
            const int ArcN = 48;
            // a jump under ~0.75px is just the curve continuing
            double jumpEps = 0.75 * (plot.SpeedMax - plot.SpeedMin) / Math.Max(plotH, 1e-9);

            var strokes = new List<StreamGeometry>();
            var jumpLines = new List<Line>();
            var pts = new List<Point>();
            double lastSp = 0;
            Action flushStroke = () =>
            {
                if (pts.Count >= 2)
                {
                    var g = new StreamGeometry();
                    using (var cx = g.Open())
                    {
                        cx.BeginFigure(pts[0], false, false);
                        for (int i = 1; i < pts.Count; i++) cx.LineTo(pts[i], true, false);
                    }
                    g.Freeze();
                    strokes.Add(g);
                }
                pts = new List<Point>();
            };
            void Append(double t, double sp) => pts.Add(new Point(XOf(plot, t), yOf(sp)));

            Append(st0, 0); // the carry before the keyed span
            for (int i = 0; i < segs.Count; i++)
            {
                var s = segs[i];
                double sIn, sOut;
                if (sp2D)
                {
                    // boundary magnitudes from each dimension's own edge
                    double vxa = segSpeed(segs, i, 0), vya = segSpeed(plot.Segs1, i, 0);
                    double vxb = segSpeed(segs, i, 1), vyb = segSpeed(plot.Segs1, i, 1);
                    sIn = Math.Sqrt(vxa * vxa + vya * vya);
                    sOut = Math.Sqrt(vxb * vxb + vyb * vyb);
                }
                else if (s.Mode == PresetCurve.InterpHold) { sIn = 0; sOut = 0; }
                else if (s.Mode == PresetCurve.InterpLinear)
                    sIn = sOut = (s.V1 - s.V0) / Math.Max(s.T1 - s.T0, 1e-9);
                else { sIn = segSpeedAt(i, 0); sOut = segSpeedAt(i, 1); }

                if (Math.Abs(sIn - lastSp) > jumpEps)
                {
                    Append(s.T0, lastSp);
                    flushStroke();
                    // AE's discontinuity: a vertical line at the key time
                    double jx = Math.Round(XOf(plot, s.T0)) + 0.5;
                    jumpLines.Add(new Line
                    {
                        X1 = jx, X2 = jx,
                        Y1 = yOf(lastSp), Y2 = yOf(sIn),
                        Stroke = skin.Curve, StrokeThickness = 2,
                        StrokeStartLineCap = PenLineCap.Round,
                        StrokeEndLineCap = PenLineCap.Round,
                        IsHitTestVisible = false
                    });
                    Append(s.T0, sIn); // the next stroke starts at the jump's foot
                }
                if (s.Mode == PresetCurve.InterpHold)
                {
                    Append(s.T1, 0);
                    lastSp = 0;
                }
                else if (sp2D)
                {
                    // the magnitude wanders even under a linear X when
                    // Y eases - sample the span's TIME evenly; the edge
                    // speeds come from the boundary values above
                    const int MagN = 36;
                    for (int k = 1; k <= MagN; k++)
                    {
                        double t = s.T0 + (s.T1 - s.T0) * (k / (double)MagN);
                        Append(t, k == MagN ? sOut : magAt(t));
                    }
                    lastSp = sOut;
                }
                else if (s.Mode == PresetCurve.InterpLinear)
                {
                    Append(s.T1, sOut);
                    lastSp = sOut;
                }
                else
                {
                    for (int k = 1; k <= ArcN; k++)
                    {
                        double u = k / (double)ArcN;
                        double m = 1 - u;
                        double t = m * m * m * s.T0 + 3 * m * m * u * s.C1T + 3 * m * u * u * s.C2T + u * u * u * s.T1;
                        Append(t, segSpeedAt(i, u));
                    }
                    lastSp = sOut;
                }
            }
            Append(st1, 0); // the carry after the keyed span
            flushStroke();

            foreach (var g in strokes)
                GraphCanvas.Children.Add(new Path
                {
                    Data = g,
                    Stroke = skin.Curve,
                    StrokeThickness = 2,
                    StrokeLineJoin = PenLineJoin.Round,
                    StrokeStartLineCap = PenLineCap.Round,
                    StrokeEndLineCap = PenLineCap.Round
                });
            foreach (var jl in jumpLines)
                GraphCanvas.Children.Add(jl);

            // keyframe markers on the speed curve with AE's icon grammar:
            // circle = bezier/easy-ease, hollow square = linear, half
            // square = hold; exact analytic speed at each key, clickable
            // like their value-graph twins
            if (stream == null) return;
            for (int i = 0; i < stream.Keyframes.Count; i++)
            {
                var k = stream.Keyframes[i];
                int shape = i + 1 < stream.Keyframes.Count ? k.InterpOut : k.InterpIn;
                double t = PresetCurve.Seconds(k.Time);
                double s = sp2D ? magAt(t) : PresetCurve.SpeedAt(segs, t);
                if (double.IsNaN(s)) s = 0; // one-keyframe stream: constant = zero speed
                string tip = $"#{i + 1}  {Timecode(t)}  \u00b7  speed {s.ToString("0.###")}/sec  \u00b7  {k.InterpLabel}  \u00b7  click to select";
                int idx = i;
                GraphCanvas.Children.Add(MakeMarker(shape,
                    XOf(plot, t), yOf(Math.Min(Math.Max(s, plot.SpeedMin), plot.SpeedMax)),
                    skin, tip, idx, true));
            }
        }

        /// <summary>
        /// AE-timeline strip for the Keyframes tab: a frame-grid baseline
        /// with the stream's keyframes as clickable markers and the selected
        /// one ringed — the "when do the keys land" view; the table below
        /// carries the exact numbers. Pure shapes, redrawn on size and
        /// theme changes.
        /// </summary>
        private void DrawKfTimeline()
        {
            if (KfTimeline == null) return;
            try
            {
                DrawKfTimelineCore();
            }
            catch (Exception ex)
            {
                // the strip degrades to empty; the table and the log
                // carry the reason
                LogService.Append("keyframe timeline: couldn't be drawn \u2014 " + ex.GetType().Name + ": " + ex.Message);
                KfTimeline.Children.Clear();
            }
        }

        private void DrawKfTimelineCore()
        {
            if (KfTimeline == null) return;
            KfTimeline.Children.Clear();
            var p = CurrentAnimParam();
            if (p == null || p.Keyframes.Count == 0) return;
            double w = KfTimeline.ActualWidth;
            if (w < 60) return; // not laid out yet — SizeChanged redraws

            var accent = (Brush)FindResource("B.Primary");
            var surface = (Brush)FindResource("B.Surface");
            var grid = (Brush)FindResource("B.OutlineVariant");
            var label = (Brush)FindResource("B.OnSurfaceVariant");

            const double y = 16, pad = 18;
            var kfs = p.Keyframes;
            double t0 = PresetCurve.Seconds(kfs[0].Time);
            double t1 = PresetCurve.Seconds(kfs[kfs.Count - 1].Time);
            if (t1 - t0 < 1e-6) t1 = t0 + 0.5;
            Func<double, double> xOf = t => pad + (t - t0) / (t1 - t0) * (w - 2 * pad);

            // baseline + round time ticks, coarsest step that keeps ~5 of them
            double midY = y + 0.5;
            KfTimeline.Children.Add(new Line
            {
                X1 = pad, Y1 = midY, X2 = w - pad, Y2 = midY,
                Stroke = grid, StrokeThickness = 1
            });
            double stepSec = NiceStep((t1 - t0) / 5.0);
            for (double t = Math.Ceiling(t0 / stepSec) * stepSec; t <= t1 + 1e-9; t += stepSec)
            {
                double x = Math.Round(xOf(t)) + 0.5;
                KfTimeline.Children.Add(new Line
                {
                    X1 = x, Y1 = y - 3, X2 = x, Y2 = y + 3,
                    Stroke = grid, StrokeThickness = 1
                });
            }

            // end timecodes (the table carries every exact per-key time)
            var first = new TextBlock { Text = Timecode(t0), FontSize = 9.5, Foreground = label };
            Canvas.SetLeft(first, pad - 2);
            Canvas.SetTop(first, y + 9);
            KfTimeline.Children.Add(first);
            double x1 = xOf(t1);
            if (kfs.Count > 1 && x1 > 128) // both timecodes need ~72px each
            {
                var last = new TextBlock { Text = Timecode(t1), FontSize = 9.5, Foreground = label, Opacity = 0.85 };
                Canvas.SetLeft(last, x1 - 56);
                Canvas.SetTop(last, y + 9);
                KfTimeline.Children.Add(last);
            }

            // keyframe markers — the same AE squares as the graph, clickable
            for (int i = 0; i < kfs.Count; i++)
            {
                var k = kfs[i];
                int shape = i + 1 < kfs.Count ? k.InterpOut : k.InterpIn;
                double t = PresetCurve.Seconds(k.Time);
                string tip = $"#{i + 1}  {Timecode(t)}  ·  value {k.Value.ToString("0.###")}  ·  click to select";
                KfTimeline.Children.Add(MakeMarker(shape, xOf(t), y, TimelineSkin(), tip, i, false));
            }

            // selection ring over its marker, in sync with the table + graph
            if (_selKf >= 0 && _selKf < kfs.Count)
            {
                double x = xOf(PresetCurve.Seconds(kfs[_selKf].Time));
                var ring = new Ellipse
                {
                    Width = 15, Height = 15, Stroke = accent, StrokeThickness = 1.6,
                    IsHitTestVisible = false
                };
                Canvas.SetLeft(ring, x - 7.5);
                Canvas.SetTop(ring, y - 7.5);
                KfTimeline.Children.Add(ring);
            }
        }

        /// <summary>
        /// The selected keyframe, AE Graph Editor style: the keyframe's
        /// own icon is painted the editor's selection yellow (AE shows no
        /// ring), plus its tangent handles in value mode (thin lines to
        /// the two control points with hollow dots) or its horizontal
        /// influence handles in speed mode, exactly when AE shows them.
        /// </summary>
        private void DrawSelection(PlotState plot, Func<double, double> yOf, GraphSkin skin)
        {
            var stream = CurrentAnimParam();
            if (stream == null || _selKf < 0 || _selKf >= stream.Keyframes.Count) return;

            var k = stream.Keyframes[_selKf];
            double t = PresetCurve.Seconds(k.Time);
            double kx = XOf(plot, t);

            if (yOf == null)
            {
                // speed mode: build the y mapping from the signed range the
                // speed curve drew
                double plotH = plot.H - plot.T - plot.Bot;
                yOf = v => plot.T + (plot.SpeedMax - v) / Math.Max(plot.SpeedMax - plot.SpeedMin, 1e-9) * plotH;
            }

            if (plot.Mode == 0)
            {
                double ky = yOf(k.Value);
                // outgoing handle: control point 1 of the segment that starts here
                if (_selKf < plot.Segs.Count && plot.Segs[_selKf].Mode == PresetCurve.InterpBezier)
                {
                    var s = plot.Segs[_selKf];
                    AddHandle(kx, ky, XOf(plot, s.C1T), yOf(s.C1V), skin.KeySel, skin.Bg);
                }
                // incoming handle: control point 2 of the segment that ends here
                if (_selKf > 0 && plot.Segs[_selKf - 1].Mode == PresetCurve.InterpBezier)
                {
                    var s = plot.Segs[_selKf - 1];
                    AddHandle(kx, ky, XOf(plot, s.C2T), yOf(s.C2V), skin.KeySel, skin.Bg);
                }
            }
            else
            {
                // AE draws the picked speed key's direction lines HORIZONTAL:
                // each spans its influence share of the neighboring segment,
                // at the key's own speed
                double ky = yOf(Math.Min(Math.Max(SpeedOfKey(plot.Segs, t, plot.Segs1), plot.SpeedMin), plot.SpeedMax));
                double pxPerSec = (plot.W - plot.L - plot.R) / Math.Max(plot.T1 - plot.T0, 1e-9);
                if (_selKf < plot.Segs.Count)
                {
                    var ns = plot.Segs[_selKf];
                    AddSpeedHandle(kx, ky, (ns.C1T - ns.T0) * pxPerSec, skin.KeySel, plot, true);
                }
                if (_selKf > 0)
                {
                    var ps = plot.Segs[_selKf - 1];
                    AddSpeedHandle(kx, ky, (ps.T1 - ps.C2T) * pxPerSec, skin.KeySel, plot, false);
                }
            }

            double y = plot.Mode == 0
                ? yOf(k.Value)
                : yOf(Math.Min(Math.Max(SpeedOfKey(plot.Segs, t, plot.Segs1), plot.SpeedMin), plot.SpeedMax));
            if (double.IsNaN(y)) return;
            // AE paints the picked key's own ICON yellow - a circle in
            // the speed editor (bezier keys), a square elsewhere; no ring
            int shape = _selKf + 1 < stream.Keyframes.Count ? k.InterpOut : k.InterpIn;
            UIElement cover;
            if (plot.Mode == 1 && shape == PresetCurve.InterpBezier)
                cover = new Ellipse
                {
                    Width = 9, Height = 9,
                    Fill = skin.KeySel, Stroke = skin.KeyStroke, StrokeThickness = 1,
                    IsHitTestVisible = false
                };
            else
                cover = new Rectangle
                {
                    Width = 9, Height = 9, RadiusX = 1.5, RadiusY = 1.5,
                    Fill = skin.KeySel, Stroke = skin.KeyStroke, StrokeThickness = 1,
                    IsHitTestVisible = false
                };
            Canvas.SetLeft(cover, kx - 4.5);
            Canvas.SetTop(cover, y - 4.5);
            GraphCanvas.Children.Add(cover);
        }

        /// <summary>Speed at t for the selection marker - the 2D
        /// magnitude when dimension 1 rides along, else the signed 1D
        /// speed; NaN-proofed for the one-keyframe stream.</summary>
        private static double SpeedOfKey(List<PresetCurve.Segment> segs, double t,
            List<PresetCurve.Segment> segs1 = null)
        {
            double s = segs1 != null && segs1.Count > 0
                ? PresetCurve.SpeedMagnitudeAt(segs, segs1, t)
                : PresetCurve.SpeedAt(segs, t);
            return double.IsNaN(s) ? 0 : s;
        }

        /// <summary>One of AE's horizontal speed handles: the influence
        /// share reaching left/right of the key, at the key's own speed,
        /// with a filled dot at its end.</summary>
        private void AddSpeedHandle(double kx, double ky, double pxLen, Brush brush, PlotState plot, bool outgoing)
        {
            if (pxLen < 2.5) return;
            double x2 = outgoing ? Math.Min(kx + pxLen, plot.W - plot.R)
                                 : Math.Max(kx - pxLen, plot.L);
            GraphCanvas.Children.Add(new Line
            {
                X1 = kx, Y1 = ky, X2 = x2, Y2 = ky,
                Stroke = brush, StrokeThickness = 1,
                IsHitTestVisible = false
            });
            var dot = new Ellipse
            {
                Width = 6, Height = 6,
                Fill = brush, StrokeThickness = 0,
                IsHitTestVisible = false
            };
            Canvas.SetLeft(dot, x2 - 3);
            Canvas.SetTop(dot, ky - 3);
            GraphCanvas.Children.Add(dot);
        }

        /// <summary>One tangent handle: a thin line plus a hollow handle dot.</summary>
        private void AddHandle(double x0, double y0, double x1, double y1, Brush lineBrush, Brush fill)
        {
            GraphCanvas.Children.Add(new Line
            {
                X1 = x0, Y1 = y0, X2 = x1, Y2 = y1,
                Stroke = lineBrush, StrokeThickness = 1
            });
            var dot = new Ellipse
            {
                Width = 7,
                Height = 7,
                Fill = fill,
                Stroke = lineBrush,
                StrokeThickness = 1.2,
                IsHitTestVisible = false
            };
            Canvas.SetLeft(dot, x1 - 3.5);
            Canvas.SetTop(dot, y1 - 3.5);
            GraphCanvas.Children.Add(dot);
        }

        /// <summary>
        /// AE Graph Editor keyframe icons. The value editor draws SQUARES
        /// regardless of easing (the reference screenshot's filled teal
        /// squares); the speed editor shapes them by interpolation -
        /// circle = bezier/easy-ease, hollow square = linear, left-half =
        /// hold. kfIndex &gt;= 0 makes the marker clickable for selection.
        /// </summary>
        private UIElement MakeMarker(int interp, double x, double y, GraphSkin skin, string tip, int kfIndex, bool shapedIcons)
        {
            const double S = 9, Half = 4.5;
            UIElement el;
            if (interp == PresetCurve.InterpHold)
            {
                var host = new Canvas { Width = S, Height = S };
                host.Children.Add(new Rectangle
                {
                    Width = Half, Height = S, RadiusX = 1.5, RadiusY = 1.5, Fill = skin.Key
                });
                host.Children.Add(new Rectangle
                {
                    Width = S, Height = S, RadiusX = 1.5, RadiusY = 1.5,
                    Fill = Brushes.Transparent, // full-rect hit target
                    Stroke = skin.Key, StrokeThickness = 1.2
                });
                el = host;
            }
            else if (shapedIcons && interp == PresetCurve.InterpBezier)
            {
                // AE's speed editor: the eased key is a circle
                el = new Ellipse
                {
                    Width = S, Height = S,
                    Fill = skin.Key, Stroke = skin.KeyStroke, StrokeThickness = 1.1
                };
            }
            else if (interp == PresetCurve.InterpLinear)
            {
                el = new Rectangle
                {
                    Width = S, Height = S, RadiusX = 1.5, RadiusY = 1.5,
                    // transparent fill keeps the whole square clickable
                    // while the marker still reads as hollow
                    Fill = Brushes.Transparent,
                    Stroke = skin.Key, StrokeThickness = 1.4
                };
            }
            else
            {
                el = new Rectangle
                {
                    Width = S, Height = S, RadiusX = 1.5, RadiusY = 1.5,
                    Fill = skin.Key, Stroke = skin.KeyStroke, StrokeThickness = 1.1
                };
            }

            Canvas.SetLeft(el, x - Half);
            Canvas.SetTop(el, y - Half);

            if (kfIndex >= 0)
            {
                var fe = (FrameworkElement)el;
                fe.Cursor = Cursors.Hand;
                fe.ToolTip = tip;
                fe.MouseLeftButtonUp += (s, e) =>
                {
                    SelectKeyframe(kfIndex);
                    e.Handled = true;
                };
            }
            return el;
        }

        /// <summary>
        /// Hover probe: dashed vertical cursor + floating readout with the
        /// exact time (and its frame number), the interpolated value (value
        /// mode) or numeric speed (speed mode) under the mouse.
        /// </summary>
        private void GraphCanvas_MouseMove(object sender, MouseEventArgs e)
        {
            if (_plot == null || GraphCanvas == null) return;
            var pt = e.GetPosition(GraphCanvas);
            double innerW = _plot.W - _plot.L - _plot.R;
            if (pt.X < _plot.L || pt.X > _plot.W - _plot.R || pt.Y > _plot.H - _plot.Bot)
            {
                ClearCursor();
                return;
            }
            double t = _plot.T0 + (pt.X - _plot.L) / innerW * (_plot.T1 - _plot.T0);
            int frame = (int)Math.Round(t * Fps);

            EnsureCursor();
            double x = Math.Round(pt.X) + 0.5;
            _cursorLine.Visibility = Visibility.Visible; // re-shown after ClearCursor
            _cursorLine.X1 = x;
            _cursorLine.X2 = x;
            _cursorLine.Y1 = _plot.T;
            _cursorLine.Y2 = _plot.H - _plot.Bot;

            string readout;
            string easing = SegmentEasingAt(t);
            double plotH = _plot.H - _plot.T - _plot.Bot;
            if (_plot.Mode == 0)
            {
                double v = PresetCurve.ValueAt(_plot.Segs, t);
                if (double.IsNaN(v))
                {
                    // one-keyframe stream: no spans, the constant holds
                    var pp = CurrentAnimParam();
                    if (pp != null && pp.Keyframes.Count > 0) v = pp.Keyframes[0].Value;
                }
                double y = _plot.T + (_plot.VMax - v) / Math.Max(_plot.VMax - _plot.VMin, 1e-9) * plotH;
                if (y < _plot.T) y = _plot.T;               // the dot rides the
                if (y > _plot.T + plotH) y = _plot.T + plotH; // plot, never the gutters
                Canvas.SetLeft(_cursorDot, x - 3.5);
                Canvas.SetTop(_cursorDot, Math.Round(y) - 3.5);
                _cursorDot.Visibility = Visibility.Visible;
                string vtxt = v.ToString("0.###") + " units";
                if (_plot.Segs1 != null)
                {
                    // a 2D probe reads the pair, X first
                    double v2 = PresetCurve.ValueAt(_plot.Segs1, t);
                    if (!double.IsNaN(v2)) vtxt = v.ToString("0.###") + ", " + v2.ToString("0.###") + " units";
                }
                readout = $"{t.ToString("0.##")} s · f{frame} · {vtxt}{easing}";
            }
            else
            {
                double s = _plot.Segs1 != null
                    ? PresetCurve.SpeedMagnitudeAt(_plot.Segs, _plot.Segs1, t)
                    : PresetCurve.SpeedAt(_plot.Segs, t);
                double y = _plot.T + (_plot.SpeedMax - s) / Math.Max(_plot.SpeedMax - _plot.SpeedMin, 1e-9) * plotH;
                if (y < _plot.T) y = _plot.T;                 // signed speed can sit
                if (y > _plot.T + plotH) y = _plot.T + plotH; // below the old 0-floor
                Canvas.SetLeft(_cursorDot, x - 3.5);
                Canvas.SetTop(_cursorDot, Math.Round(y) - 3.5);
                _cursorDot.Visibility = Visibility.Visible;
                readout = double.IsNaN(s)
                    ? $"{t.ToString("0.##")} s · f{frame}"
                    : $"{t.ToString("0.##")} s · f{frame} · {s.ToString("0.###")} units/sec{easing}";
            }

            GraphReadoutText.Text = readout;
            GraphReadout.Visibility = Visibility.Visible;
        }

        private void GraphCanvas_MouseLeave(object sender, MouseEventArgs e) => ClearCursor();

        /// <summary>The interpolation of the span under time t ("" outside).</summary>
        private string SegmentEasingAt(double t)
        {
            if (_plot == null) return "";
            foreach (var s in _plot.Segs)
            {
                if (t < s.T0 || t > s.T1) continue;
                return s.Mode == PresetCurve.InterpLinear ? " · linear"
                     : s.Mode == PresetCurve.InterpHold ? " · hold"
                     : " · bezier";
            }
            return "";
        }

        private void EnsureCursor()
        {
            if (_cursorLine != null) return;
            var skin = _curSkin;
            _cursorLine = new Line
            {
                Stroke = skin?.Label ?? (Brush)FindResource("B.OnSurfaceVariant"),
                StrokeThickness = 1,
                StrokeDashArray = new DoubleCollection { 2, 2 },
                IsHitTestVisible = false
            };
            _cursorDot = new Ellipse
            {
                Width = 7,
                Height = 7,
                Fill = skin?.Curve ?? (Brush)FindResource("B.Primary"),
                Stroke = skin?.Bg ?? (Brush)FindResource("B.Surface"),
                StrokeThickness = 1.2,
                IsHitTestVisible = false
            };
            GraphCanvas.Children.Add(_cursorLine);
            GraphCanvas.Children.Add(_cursorDot);
        }

        private void ClearCursor()
        {
            if (_cursorLine != null) _cursorLine.Visibility = Visibility.Collapsed;
            if (_cursorDot != null) _cursorDot.Visibility = Visibility.Collapsed;
            if (GraphReadout != null) GraphReadout.Visibility = Visibility.Collapsed;
        }

        private double XOf(PlotState plot, double t) =>
            plot.L + (t - plot.T0) / Math.Max(plot.T1 - plot.T0, 1e-9) * (plot.W - plot.L - plot.R);

        /// <summary>Round axis step from a raw division: 1/2/2.5/5 × 10^n.</summary>
        private static double NiceStep(double raw)
        {
            if (raw <= 0 || double.IsNaN(raw) || double.IsInfinity(raw)) return 1;
            double mag = Math.Pow(10, Math.Floor(Math.Log10(raw)));
            double norm = raw / mag;
            double step = norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10;
            return step * mag;
        }

        private void AddTimeLabel(double x, double y, string text, Brush labelBrush)
        {
            var lbl = new TextBlock { Text = text, FontSize = 10, Foreground = labelBrush };
            Canvas.SetLeft(lbl, x);
            Canvas.SetTop(lbl, y);
            GraphCanvas.Children.Add(lbl);
        }
    }

    /// <summary>
    /// One named parameter group inside an Effect Controls block — AE's
    /// collapsible sub-groups ("Compositing Options" and friends). A
    /// top-level class because the XAML template selector references it.
    /// </summary>
    public class EcSubGroupVm : System.ComponentModel.INotifyPropertyChanged
    {
        public string Title { get; set; }
        public string GroupKey { get; set; }
        public int EffectIndex { get; set; }
        // INPC: the disclosure toggle folds the node in place, in both
        // the Effect Controls tree and the inspector's captions
        bool _open;
        public bool Open
        {
            get { return _open; }
            set { _open = value; Fire(); }
        }
        public Visibility BodyVisible => Open ? Visibility.Visible : Visibility.Collapsed;
        public event System.ComponentModel.PropertyChangedEventHandler PropertyChanged;
        void Fire()
        {
            var h = PropertyChanged;
            if (h == null) return;
            h(this, new System.ComponentModel.PropertyChangedEventArgs("Open"));
            h(this, new System.ComponentModel.PropertyChangedEventArgs("BodyVisible"));
        }
        public List<object> Items { get; set; } = new List<object>();
    }

    /// <summary>
    /// Picks the Effect Controls body template: a group disclosure row for
    /// EcSubGroupVm nodes, the AE property line for everything else.
    /// </summary>
    public sealed class EcBodySelector : DataTemplateSelector
    {
        public DataTemplate GroupTemplate { get; set; }
        public string GroupTemplateKey { get; set; }
        public DataTemplate RowTemplate { get; set; }
        private DataTemplate _groupByKey;

        public override DataTemplate SelectTemplate(object item, DependencyObject container)
        {
            if (!(item is EcSubGroupVm)) return RowTemplate;
            if (GroupTemplate != null) return GroupTemplate;
            if (_groupByKey != null) return _groupByKey;
            // Recursive group templates cannot be wired with
            // {StaticResource Key} inside their own content: WPF expands
            // template content off-tree, where the template's own key is
            // unreachable ("Cannot find resource named ..."). Resolving
            // the same key from the live container is the supported
            // recursion path — the container is in the tree, so the page
            // resource lookup succeeds.
            if (!string.IsNullOrEmpty(GroupTemplateKey) && container is FrameworkElement fe)
                _groupByKey = fe.TryFindResource(GroupTemplateKey) as DataTemplate;
            return _groupByKey;
        }
    }
}
