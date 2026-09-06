using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Windows;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Shell;
using FfxTool.Core;

namespace FfxTool.Gui
{
    /// <summary>Common surface MainWindow uses to drive the active page.</summary>
    public interface ISection
    {
        void OpenFile();
        void OnShown();
    }

    public partial class MainWindow : Window
    {
        // ---------- window edge + resize strategy ----------
        // The window is a plain rectangle with a 1px rim drawn just inside
        // the client area (MainWindow.xaml). Rounds 5-9 cut a rounded OS
        // region with SetWindowRgn and stroked an anti-aliased rim over it;
        // a GDI region is a 1-bit mask and WPF arcs are anti-aliased, so the
        // two curves never coincided and the user's Win7 machine kept showing
        // a crescent of backdrop "sticking out" past the edge. This build
        // removes the second curve entirely: no region, no clip, no radius -
        // the HWND and the rim are the same rectangle, so the artifact is
        // structurally impossible. Resizing is therefore the OS's own border
        // drag (the round-9 snapshot freeze frame read blurry and is gone);
        // only the black-flash guards below remain.

        // GetWindowLong/SetWindowLong need the *Ptr variants on x64; every
        // 64-bit Windows still exports the old names, so fall back to them.
        [DllImport("user32.dll", EntryPoint = "GetWindowLongPtr")]
        private static extern IntPtr GetWindowLongPtr64(IntPtr hWnd, int nIndex);
        [DllImport("user32.dll", EntryPoint = "GetWindowLong")]
        private static extern IntPtr GetWindowLong32(IntPtr hWnd, int nIndex);
        [DllImport("user32.dll", EntryPoint = "SetWindowLongPtr")]
        private static extern IntPtr SetWindowLongPtr64(IntPtr hWnd, int nIndex, IntPtr dwNewLong);
        [DllImport("user32.dll", EntryPoint = "SetWindowLong")]
        private static extern IntPtr SetWindowLong32(IntPtr hWnd, int nIndex, IntPtr dwNewLong);

        private static IntPtr GetWindowExStyle(IntPtr hwnd)
        {
            const int GWL_EXSTYLE = -20;
            return IntPtr.Size == 8 ? GetWindowLongPtr64(hwnd, GWL_EXSTYLE)
                                    : GetWindowLong32(hwnd, GWL_EXSTYLE);
        }

        private static IntPtr SetWindowExStyle(IntPtr hwnd, IntPtr style)
        {
            const int GWL_EXSTYLE = -20;
            return IntPtr.Size == 8 ? SetWindowLongPtr64(hwnd, GWL_EXSTYLE, style)
                                    : SetWindowLong32(hwnd, GWL_EXSTYLE, style);
        }

        private const int WM_ERASEBKGND = 0x0014;
        private const int WS_EX_COMPOSITED = 0x02000000;

        private HwndSource _source;
        private IntPtr _hwnd;
        private readonly PluginProfile _profile;
        private readonly ConvertPage _convert;
        private readonly ListerPage _lister;
        private readonly ProfilePage _profilePage;
        private readonly SettingsPage _settings;

        public MainWindow()
        {
            InitializeComponent();
            LoadWindowBounds();

            // taskbar / Alt-Tab icon (title bar is custom chrome, so this is
            // where the brand actually shows). Cosmetic — never block startup.
            try
            {
                Icon = BitmapFrame.Create(new Uri("pack://application:,,,/Assets/app.ico"));
            }
            catch { /* missing asset falls back to the exe icon */ }

            _profile = PluginProfile.Load();

            _lister = new ListerPage(_profile);
            _profilePage = new ProfilePage(_profile, OnProfileChanged);
            _convert = new ConvertPage(_profile);
            _settings = new SettingsPage(_profilePage);

            // Convert-first: it's the main thing this tool does. Both
            // Convert and the Effect Lister take a whole folder of presets
            // (the old Batch section is baked into them), Settings rides Ctrl+3.
            Rail.AddItem("Convert", "SwapHoriz", "Convert Preset · Ctrl+1");
            Rail.AddItem("Effect Lister", "List", "Effect Lister · Ctrl+2");
            Rail.AddItem("Settings", "Settings", "Settings · Ctrl+3");
            Rail.SelectionChanged += i => ShowSection(i);
            // the + is the upload button: jump to Convert and pick a file
            Rail.FabClicked += () =>
            {
                Rail.Select(0);
                _convert.OpenFile();
            };

            MinBtn.Click += (s, e) => WindowState = WindowState.Minimized;
            MaxBtn.Click += (s, e) => ToggleMaximize();
            CloseBtn.Click += (s, e) => Close();
            StateChanged += (s, e) => ApplyChromeState();
            // cache the HWND for the message hook as soon as it exists
            SourceInitialized += (s, e) =>
            {
                _hwnd = new WindowInteropHelper(this).Handle;
                _source = HwndSource.FromHwnd(_hwnd);
                _source?.AddHook(WndProc);
                EnableComposited();
            };
            // inactive windows dim their title, like a native caption does
            Activated += (s, e) => TitleBrand.Opacity = 1;
            Deactivated += (s, e) => TitleBrand.Opacity = 0.55;

            ShowSection(0);
            ApplyChromeState(); // first paint: rim + maximize paddings
        }

        private ISection ActiveSection()
        {
            switch (Rail.SelectedIndex)
            {
                case 0: return _convert;
                case 1: return _lister;
                case 2: return _settings;
                default: return null;
            }
        }

        private void ShowSection(int index)
        {
            switch (index)
            {
                case 0: PageHost.Content = _convert; break;
                case 1: PageHost.Content = _lister; break;
                case 2: PageHost.Content = _settings; break;
            }
            AnimateSectionIn();
            (PageHost.Content as ISection)?.OnShown();
        }

        /// <summary>
        /// Quiet fade + 12px rise on every section switch. Deliberately
        /// subtle (~1/5 s, decelerating) so it reads as material settling,
        /// not as an animation showcase.
        /// </summary>
        private void AnimateSectionIn()
        {
            if (PageHost.Content is UIElement page)
            {
                var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
                var slide = new TranslateTransform(0, 12);
                page.RenderTransform = slide;
                var dur = TimeSpan.FromMilliseconds(220);
                page.BeginAnimation(UIElement.OpacityProperty,
                    new DoubleAnimation(0, 1, dur) { EasingFunction = ease });
                slide.BeginAnimation(TranslateTransform.YProperty,
                    new DoubleAnimation(12, 0, dur) { EasingFunction = ease });
            }
        }

        private void OnProfileChanged()
        {
            _convert.OnProfileChanged();
            _lister.OnProfileChanged();
        }

        protected override void OnPreviewKeyDown(KeyEventArgs e)
        {
            base.OnPreviewKeyDown(e);
            if (Keyboard.Modifiers == ModifierKeys.Control && e.Key == Key.O)
            {
                ActiveSection()?.OpenFile();
                e.Handled = true;
            }
            else if (Keyboard.Modifiers == ModifierKeys.Control && e.Key >= Key.D1 && e.Key <= Key.D3)
            {
                int index = (int)e.Key - (int)Key.D1;
                Rail.SelectWithoutNotify(index);
                ShowSection(index);
                e.Handled = true;
            }
        }

        private void ToggleMaximize()
        {
            WindowState = WindowState == WindowState.Maximized
                ? WindowState.Normal
                : WindowState.Maximized; // bounds handled by ApplyChromeState
        }

        /// <summary>
        /// Keeps the window overlay correct in every state. Windows maximizes
        /// a borderless-chrome window a few pixels BEYOND the work area on
        /// each side (the invisible resize border hangs off-screen), so while
        /// maximized the content is padded back into the visible area —
        /// otherwise the outer 8px of the app silently disappear at the
        /// screen edges — and the rim collapses (a bordered rectangle
        /// floating inside the screen edge reads wrong). Restored windows
        /// show the 1px rim again.
        /// </summary>
        private void ApplyChromeState()
        {
            bool max = WindowState == WindowState.Maximized;
            WindowRoot.Padding = max ? new Thickness(8) : new Thickness(0);
            WindowRim.Visibility = max ? Visibility.Collapsed : Visibility.Visible;
            MaxIcon.IconName = max ? "Restore" : "Maximize";
            MaxBtn.ToolTip = max ? "Restore" : "Maximize";
        }

        /// <summary>
        /// Kills the "window goes black while resizing" artifact on Win7.
        /// Era-correct measures that survive the square-window rebuild:
        ///
        /// 1. WM_ERASEBKGND (WndProc) — answer "erased" without touching
        ///    anything. The old pixels stay on screen until WPF paints the
        ///    new frame instead of flashing an empty background between
        ///    frames; with no window region there are no orphan black
        ///    corners left to repaint at all.
        /// 2. WS_EX_COMPOSITED — bottom-up double-buffered painting of the
        ///    whole window subtree; silences child-repaint flicker on
        ///    DWM-off (Win7 Basic) machines where there is no compositor
        ///    to hide intermediate frames.
        ///
        /// Resizing itself is the OS's native border drag again — the
        /// round-9 snapshot freeze frame stretched a blurry bitmap over the
        /// window and is gone; the Lister's graph keeps redrawing through
        /// its own coalescing timer, which is the right price for live
        /// preview.
        /// </summary>
        private void EnableComposited()
        {
            try
            {
                if (_hwnd == IntPtr.Zero) return;
                IntPtr ex = GetWindowExStyle(_hwnd);
                if (((long)ex & WS_EX_COMPOSITED) == 0)
                    SetWindowExStyle(_hwnd, (IntPtr)((long)ex | WS_EX_COMPOSITED));
            }
            catch { /* cosmetic */ }
        }

        private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
        {
            if (msg == WM_ERASEBKGND)
            {
                handled = true;
                return new IntPtr(1); // "already erased" — keeps old pixels
            }

            return IntPtr.Zero;
        }

        // ---------- window bounds persistence (window.json) ----------
        [DataContract(Namespace = "")]
        private class StoredBounds
        {
            [DataMember] public double X;
            [DataMember] public double Y;
            [DataMember] public double Width;
            [DataMember] public double Height;
            [DataMember] public bool Maximized;
        }

        private string BoundsPath()
        {
            string dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "FFXCompatibilityTool");
            Directory.CreateDirectory(dir);
            return Path.Combine(dir, "window.json");
        }

        private void LoadWindowBounds()
        {
            try
            {
                if (!File.Exists(BoundsPath())) return;
                StoredBounds b;
                var serializer = new DataContractJsonSerializer(typeof(StoredBounds));
                using (var fs = File.OpenRead(BoundsPath()))
                    b = serializer.ReadObject(fs) as StoredBounds;
                if (b == null) return;

                if (b.Width >= MinWidth && b.Height >= MinHeight)
                {
                    Width = b.Width;
                    Height = b.Height;
                }
                WindowStartupLocation = WindowStartupLocation.Manual;
                Left = b.X;
                Top = b.Y;
                if (b.Maximized) WindowState = WindowState.Maximized;
                // make sure a saved off-screen position can't strand the window
                if (Left < -Width + 100 || Left > SystemParameters.VirtualScreenWidth - 100)
                    WindowStartupLocation = WindowStartupLocation.CenterScreen;
            }
            catch { /* defaults */ }
        }

        protected override void OnClosing(System.ComponentModel.CancelEventArgs e)
        {
            try
            {
                var b = new StoredBounds
                {
                    X = RestoreBounds.Left,
                    Y = RestoreBounds.Top,
                    Width = RestoreBounds.Width,
                    Height = RestoreBounds.Height,
                    Maximized = WindowState == WindowState.Maximized
                };
                var serializer = new DataContractJsonSerializer(typeof(StoredBounds));
                using (var fs = File.Create(BoundsPath()))
                    serializer.WriteObject(fs, b);
            }
            catch { /* best-effort */ }
            base.OnClosing(e);
        }
    }
}
