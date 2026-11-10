// HITMAN Co-op setup: the window. Created by Jordan Rodrigues.
//
// One window, a few clear steps: install (one button), how to connect (join code or Tailscale, set
// up here end to end), and done (launch the game). Everything happens in this program (see
// Engine.cs); nothing runs hidden. Command line, for testing and the "Apps" uninstall:
//   HitmanCoop-Setup.exe [--game <folder>] [--uninstall] [--silent [--log <file>] [--register]]
//
// Built by tools/package.ps1 with the C# compiler that ships with Windows (.NET Framework 4, C# 5).

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

[assembly: AssemblyTitle("HITMAN Co-op Setup")]
[assembly: AssemblyDescription("Installs HITMAN Co-op, two-player co-op for HITMAN World of Assassination")]
[assembly: AssemblyCompany("Jordan Rodrigues")]
[assembly: AssemblyProduct("HITMAN Co-op")]
[assembly: AssemblyCopyright("Copyright (C) 2026 Jordan Rodrigues. GPL-3.0-or-later.")]
[assembly: AssemblyTrademark("HITMAN is a trademark of IO Interactive A/S. HITMAN Co-op is a fan project, not made or endorsed by IOI.")]
[assembly: AssemblyVersion("0.13.95.0")]
[assembly: AssemblyFileVersion("0.13.95.0")]
[assembly: AssemblyInformationalVersion("0.13.95")]

namespace HitmanCoopSetup
{
    static class Program
    {
        public const string Version = "0.13.95";
        public const string Author = "Jordan Rodrigues";
        public const string Website = "https://github.com/JordanRodrigues021/hitman-coop";
        public const string Discord = "https://discord.gg/rQeHdpbKUp";

        public static string ExePath
        {
            get { return Assembly.GetExecutingAssembly().Location; }
        }

        public class Options
        {
            public string Game;
            public bool Uninstall, Silent, Register, KeepSettings = true, Auto;
            public string Log, FirewallExe, RenderTo;
        }

        [STAThread]
        static int Main(string[] args)
        {
            var o = new Options();
            for (int i = 0; i < args.Length; i++)
            {
                string a = args[i].ToLowerInvariant();
                string next = i + 1 < args.Length ? args[i + 1] : null;
                if (a == "--game" && next != null) { o.Game = next; i++; }
                else if (a == "--log" && next != null) { o.Log = next; i++; }
                else if (a == "--firewall-rule" && next != null) { o.FirewallExe = next; i++; }
                else if (a == "--uninstall") o.Uninstall = true;
                else if (a == "--silent" || a == "/s") o.Silent = true;
                else if (a == "--register") o.Register = true;
                else if (a == "--remove-settings") o.KeepSettings = false;
                else if (a == "--auto") o.Auto = true;
                else if (a == "--render-pages" && next != null) { o.RenderTo = next; i++; }
            }

            // The admin copy that only adds the firewall rule (see Firewall.AddRuleElevated).
            if (o.FirewallExe != null)
                return Firewall.AddRule(o.FirewallExe) ? 0 : 1;

            if (o.Silent)
                return RunSilent(o);

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new SetupForm(o));
            Tailscale.StopSignIn();
            Payload.CleanUp();
            return 0;
        }

        static int RunSilent(Options o)
        {
            var log = new List<string>();
            Action<string> say = delegate(string s) { log.Add(s); };
            try
            {
                Payload.Prepare();
                string dir = o.Game ?? AppsEntry.LastGame();
                if (dir == null)
                {
                    var found = Games.FindAll();
                    if (found.Count == 0) throw new SetupException("HITMAN wasn't found.", "Pass --game <folder>.");
                    dir = found[0].Path;
                }
                // Uninstalling (from Settings > Apps) a game folder that's gone (moved, the game removed):
                // nothing to put back, only the Apps entry to take off. It stayed there for good.
                if (o.Uninstall && !Games.IsGameDir(Games.Normalize(dir)))
                {
                    AppsEntry.Unregister();
                    say("The game folder " + dir + " is gone: nothing to remove; HITMAN Co-op is off the Apps list.");
                    return 0;
                }
                GameInstall game = Games.Inspect(dir);
                if (o.Uninstall)
                {
                    Installer.Uninstall(game, o.KeepSettings, say);
                    if (o.Register || string.Equals(AppsEntry.LastGame(), game.Path, StringComparison.OrdinalIgnoreCase)) AppsEntry.Unregister();
                }
                else
                {
                    Installer.Install(game, delegate { }, say);
                    if (o.Register) AppsEntry.Register(game);
                }
                return 0;
            }
            catch (SetupException e)
            {
                say("ERROR: " + e.Message + (e.Hint != null ? " " + e.Hint : ""));
                return 1;
            }
            catch (Exception e)
            {
                say("ERROR: " + e.Message);
                return 2;
            }
            finally
            {
                if (o.Log != null) { try { File.WriteAllLines(o.Log, log.ToArray()); } catch { } }
                Payload.CleanUp();
            }
        }
    }

    // ---- Look ---------------------------------------------------------------------------------

    static class Theme
    {
        public static readonly Color Bg = Color.FromArgb(13, 13, 16);
        public static readonly Color Surface = Color.FromArgb(23, 23, 28);
        public static readonly Color SurfaceHover = Color.FromArgb(30, 30, 36);
        public static readonly Color Border = Color.FromArgb(44, 44, 52);
        public static readonly Color Text = Color.FromArgb(242, 242, 245);
        public static readonly Color Dim = Color.FromArgb(150, 151, 162);
        public static readonly Color Faint = Color.FromArgb(96, 97, 108);
        public static readonly Color Red = Color.FromArgb(226, 26, 44);
        public static readonly Color RedHover = Color.FromArgb(244, 48, 64);
        public static readonly Color RedDeep = Color.FromArgb(120, 10, 22);
        public static readonly Color Green = Color.FromArgb(56, 210, 130);
        public static readonly Color Amber = Color.FromArgb(255, 180, 40);

        public static float Scale = 1f;

        public static int S(float v)
        {
            return (int)Math.Round(v * Scale);
        }

        static readonly Dictionary<string, bool> s_Have = new Dictionary<string, bool>();

        static bool Have(string family)
        {
            bool v;
            if (s_Have.TryGetValue(family, out v)) return v;
            using (var fonts = new InstalledFontCollection())
                v = fonts.Families.Any(f => f.Name.Equals(family, StringComparison.OrdinalIgnoreCase));
            s_Have[family] = v;
            return v;
        }

        // Pixel sizes, scaled. The HITMAN look: a condensed, heavy face for titles.
        public static Font Display(float px)
        {
            foreach (string f in new[] { "Bahnschrift SemiBold Condensed", "Bahnschrift SemiBold", "Bahnschrift" })
                if (Have(f)) return new Font(f, px * Scale, FontStyle.Regular, GraphicsUnit.Pixel);
            return new Font("Segoe UI", px * Scale, FontStyle.Bold, GraphicsUnit.Pixel);
        }

        public static Font Ui(float px, bool bold = false)
        {
            string f = bold ? (Have("Segoe UI Semibold") ? "Segoe UI Semibold" : "Segoe UI") : "Segoe UI";
            return new Font(f, px * Scale, bold && f == "Segoe UI" ? FontStyle.Bold : FontStyle.Regular, GraphicsUnit.Pixel);
        }

        public static Font Mono(float px)
        {
            return new Font(Have("Cascadia Mono") ? "Cascadia Mono" : "Consolas", px * Scale, FontStyle.Regular, GraphicsUnit.Pixel);
        }

        public static GraphicsPath Round(RectangleF r, float radius)
        {
            var p = new GraphicsPath();
            float d = Math.Min(radius * 2, Math.Min(r.Width, r.Height));
            if (d <= 0) { p.AddRectangle(r); return p; }
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        // The mark: two figures, side by side, on HITMAN red.
        public static void DrawLogo(Graphics g, RectangleF r)
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var path = Round(r, r.Width * 0.24f))
            using (var bg = new LinearGradientBrush(r, RedHover, RedDeep, 60f))
                g.FillPath(bg, path);

            float u = r.Width / 64f;
            using (var white = new SolidBrush(Color.White))
            using (var shade = new SolidBrush(Color.FromArgb(150, 255, 255, 255)))
            {
                // Behind: the partner. In front: you.
                g.FillEllipse(shade, r.X + 31 * u, r.Y + 13 * u, 14 * u, 14 * u);
                using (var body = Round(new RectangleF(r.X + 26 * u, r.Y + 29 * u, 24 * u, 26 * u), 10 * u)) g.FillPath(shade, body);
                g.FillEllipse(white, r.X + 17 * u, r.Y + 17 * u, 15 * u, 15 * u);
                using (var body = Round(new RectangleF(r.X + 11 * u, r.Y + 34 * u, 27 * u, 24 * u), 10 * u)) g.FillPath(white, body);
                // 47's tie, in the red.
                using (var tie = new SolidBrush(Red))
                    g.FillPolygon(tie, new[] { new PointF(r.X + 24.5f * u, r.Y + 35 * u), new PointF(r.X + 27 * u, r.Y + 39 * u), new PointF(r.X + 24.5f * u, r.Y + 50 * u), new PointF(r.X + 22 * u, r.Y + 39 * u) });
            }
        }
    }

    class Pill : Control
    {
        public enum Kind { Primary, Secondary, Ghost, Danger }
        public Kind Style = Kind.Primary;
        bool m_Hover, m_Down;

        public Pill(string text, Kind style)
        {
            Text = text;
            Style = style;
            Font = Theme.Ui(14, true);
            Cursor = Cursors.Hand;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            Size = new Size(Theme.S(160), Theme.S(42));
            TabStop = true;
        }

        protected override void OnMouseEnter(EventArgs e) { m_Hover = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { m_Hover = m_Down = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnMouseDown(MouseEventArgs e) { m_Down = true; Invalidate(); base.OnMouseDown(e); }
        protected override void OnMouseUp(MouseEventArgs e) { m_Down = false; Invalidate(); base.OnMouseUp(e); }
        protected override void OnEnabledChanged(EventArgs e) { Cursor = Enabled ? Cursors.Hand : Cursors.Default; Invalidate(); base.OnEnabledChanged(e); }
        protected override void OnTextChanged(EventArgs e) { Invalidate(); base.OnTextChanged(e); }

        protected override bool IsInputKey(Keys k) { return k == Keys.Enter || k == Keys.Space || base.IsInputKey(k); }
        protected override void OnKeyDown(KeyEventArgs e) { if (e.KeyCode == Keys.Enter || e.KeyCode == Keys.Space) OnClick(EventArgs.Empty); base.OnKeyDown(e); }
        protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
        protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
            var r = new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f);
            Color fill, text = Theme.Text, border = Color.Empty;

            switch (Style)
            {
                case Kind.Primary: fill = m_Down ? Theme.RedDeep : m_Hover ? Theme.RedHover : Theme.Red; text = Color.White; break;
                case Kind.Danger: fill = m_Down ? Theme.RedDeep : m_Hover ? Color.FromArgb(70, 226, 26, 44) : Color.FromArgb(40, 226, 26, 44); text = Theme.RedHover; border = Color.FromArgb(120, 226, 26, 44); break;
                case Kind.Secondary: fill = m_Down ? Theme.Bg : m_Hover ? Theme.SurfaceHover : Theme.Surface; border = Theme.Border; break;
                default: fill = m_Hover ? Theme.Surface : Color.Transparent; text = m_Hover ? Theme.Text : Theme.Dim; break;
            }

            if (!Enabled) { fill = Theme.Surface; text = Theme.Faint; border = Theme.Border; }

            using (var path = Theme.Round(r, r.Height / 2))
            {
                if (fill.A > 0) using (var b = new SolidBrush(fill)) g.FillPath(b, path);
                if (border != Color.Empty) using (var p = new Pen(border)) g.DrawPath(p, path);
                if (Focused && ShowFocusCues) using (var p = new Pen(Color.FromArgb(160, 255, 255, 255), 1.5f) { DashStyle = DashStyle.Dot }) g.DrawPath(p, path);
            }

            TextRenderer.DrawText(g, Text, Font, Rectangle.Round(r), text, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
        }
    }

    // A rounded panel; selectable cards show a red edge when chosen.
    class Card : Panel
    {
        public bool Selectable, Selected;
        public Color Edge = Color.Empty;
        bool m_Hover;

        public Card()
        {
            DoubleBuffered = true;
            SetStyle(ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Theme.Surface;
        }

        public void Hook(Control c)
        {
            c.MouseEnter += delegate { SetHover(true); };
            c.MouseLeave += delegate { SetHover(ClientRectangle.Contains(PointToClient(Cursor.Position))); };
            c.Click += delegate { OnClick(EventArgs.Empty); };
            if (Selectable) c.Cursor = Cursors.Hand;
        }

        void SetHover(bool on) { if (m_Hover != on) { m_Hover = on; Invalidate(); } }
        protected override void OnMouseEnter(EventArgs e) { SetHover(true); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { SetHover(ClientRectangle.Contains(PointToClient(Cursor.Position))); base.OnMouseLeave(e); }

        protected override void OnControlAdded(ControlEventArgs e)
        {
            base.OnControlAdded(e);
            Hook(e.Control);
        }

        protected override void OnPaintBackground(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var parent = new SolidBrush(Parent != null ? Parent.BackColor : Theme.Bg)) g.FillRectangle(parent, ClientRectangle);
            var r = new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f);
            Color fill = Selectable && m_Hover && !Selected ? Theme.SurfaceHover : Theme.Surface;
            Color edge = Selected ? Theme.Red : Edge != Color.Empty ? Edge : Theme.Border;
            using (var path = Theme.Round(r, Theme.S(12)))
            {
                using (var b = new SolidBrush(fill)) g.FillPath(b, path);
                using (var p = new Pen(edge, Selected ? 2f : 1f)) g.DrawPath(p, path);
            }
            if (Selectable)
            {
                // The choice dot, top right.
                var dot = new RectangleF(Width - Theme.S(30), Theme.S(16), Theme.S(16), Theme.S(16));
                using (var p = new Pen(Selected ? Theme.Red : Theme.Faint, 1.6f)) g.DrawEllipse(p, dot);
                if (Selected) using (var b = new SolidBrush(Theme.Red)) g.FillEllipse(b, RectangleF.Inflate(dot, -Theme.S(4), -Theme.S(4)));
            }
        }
    }

    class Bar : Control
    {
        float m_Shown, m_Target, m_Shimmer;
        public bool Busy;
        readonly System.Windows.Forms.Timer m_Timer = new System.Windows.Forms.Timer { Interval = 16 };

        public Bar()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            m_Timer.Tick += delegate
            {
                m_Shown += (m_Target - m_Shown) * 0.12f;
                m_Shimmer = (m_Shimmer + 0.012f) % 1.4f;
                Invalidate();
            };
            m_Timer.Start();
        }

        public float Value { get { return m_Target; } set { m_Target = Math.Max(0, Math.Min(100, value)); } }

        protected override void Dispose(bool disposing) { if (disposing) m_Timer.Dispose(); base.Dispose(disposing); }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var parent = new SolidBrush(Parent != null ? Parent.BackColor : Theme.Bg)) g.FillRectangle(parent, ClientRectangle);
            var track = new RectangleF(0, 0, Width - 1, Height - 1);
            using (var path = Theme.Round(track, Height / 2f)) using (var b = new SolidBrush(Theme.Border)) g.FillPath(b, path);

            float w = Busy ? track.Width * 0.3f : track.Width * m_Shown / 100f;
            float x = Busy ? (m_Shimmer - 0.3f) * track.Width : 0;
            if (w < 1) return;
            var fill = new RectangleF(Math.Max(0, x), 0, Math.Min(w, track.Width - Math.Max(0, x)), track.Height);
            if (fill.Width < 1) return;
            using (var path = Theme.Round(fill, Height / 2f))
            using (var b = new LinearGradientBrush(new RectangleF(0, 0, track.Width + 1, track.Height), Theme.RedDeep, Theme.RedHover, 0f))
                g.FillPath(b, path);
        }
    }

    // A checklist of steps: waiting, working (a spinner), done, failed or skipped.
    class Steps : Control
    {
        public enum State { Waiting, Working, Done, Failed, Skipped }

        class Row { public string Title, Detail; public State State; }

        readonly List<Row> m_Rows = new List<Row>();
        readonly System.Windows.Forms.Timer m_Timer = new System.Windows.Forms.Timer { Interval = 30 };
        float m_Spin;

        public Steps()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            m_Timer.Tick += delegate { m_Spin = (m_Spin + 12) % 360; if (m_Rows.Any(r => r.State == State.Working)) Invalidate(); };
            m_Timer.Start();
        }

        protected override void Dispose(bool disposing) { if (disposing) m_Timer.Dispose(); base.Dispose(disposing); }

        public void Add(string title, string detail) { m_Rows.Add(new Row { Title = title, Detail = detail }); Invalidate(); }

        public void Set(int i, State s, string detail = null)
        {
            if (i < 0 || i >= m_Rows.Count) return;
            m_Rows[i].State = s;
            if (detail != null) m_Rows[i].Detail = detail;
            Invalidate();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(Parent != null ? Parent.BackColor : Theme.Bg);
            float rowH = Theme.S(52), icon = Theme.S(22);

            using (Font title = Theme.Ui(15, true))
            using (Font detail = Theme.Ui(12.5f))
            {
                for (int i = 0; i < m_Rows.Count; i++)
                {
                    Row r = m_Rows[i];
                    float y = i * rowH;
                    var c = new RectangleF(0, y + Theme.S(4), icon, icon);

                    if (i < m_Rows.Count - 1)
                        using (var line = new Pen(r.State == State.Done ? Theme.Green : Theme.Border, 2f))
                            g.DrawLine(line, c.X + icon / 2, c.Bottom + Theme.S(3), c.X + icon / 2, y + rowH + Theme.S(1));

                    switch (r.State)
                    {
                        case State.Done:
                            using (var b = new SolidBrush(Theme.Green)) g.FillEllipse(b, c);
                            using (var p = new Pen(Theme.Bg, 2.4f) { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round })
                                g.DrawLines(p, new[] { new PointF(c.X + icon * 0.27f, c.Y + icon * 0.52f), new PointF(c.X + icon * 0.44f, c.Y + icon * 0.68f), new PointF(c.X + icon * 0.74f, c.Y + icon * 0.34f) });
                            break;
                        case State.Failed:
                            using (var b = new SolidBrush(Theme.Red)) g.FillEllipse(b, c);
                            using (var p = new Pen(Color.White, 2.2f) { StartCap = LineCap.Round, EndCap = LineCap.Round })
                            {
                                g.DrawLine(p, c.X + icon * 0.33f, c.Y + icon * 0.33f, c.X + icon * 0.67f, c.Y + icon * 0.67f);
                                g.DrawLine(p, c.X + icon * 0.67f, c.Y + icon * 0.33f, c.X + icon * 0.33f, c.Y + icon * 0.67f);
                            }
                            break;
                        case State.Working:
                            using (var p = new Pen(Theme.Border, 2.4f)) g.DrawEllipse(p, RectangleF.Inflate(c, -1, -1));
                            using (var p = new Pen(Theme.Red, 2.4f) { StartCap = LineCap.Round, EndCap = LineCap.Round }) g.DrawArc(p, RectangleF.Inflate(c, -1, -1), m_Spin, 100);
                            break;
                        case State.Skipped:
                            using (var p = new Pen(Theme.Faint, 2f)) g.DrawEllipse(p, RectangleF.Inflate(c, -1, -1));
                            using (var p = new Pen(Theme.Faint, 2f)) g.DrawLine(p, c.X + icon * 0.3f, c.Y + icon / 2, c.X + icon * 0.7f, c.Y + icon / 2);
                            break;
                        default:
                            using (var p = new Pen(Theme.Border, 2f)) g.DrawEllipse(p, RectangleF.Inflate(c, -1, -1));
                            break;
                    }

                    Color tc = r.State == State.Waiting ? Theme.Dim : Theme.Text;
                    TextRenderer.DrawText(g, r.Title, title, new Point((int)(icon + Theme.S(14)), (int)y), tc, TextFormatFlags.NoPadding);
                    if (!string.IsNullOrEmpty(r.Detail))
                        TextRenderer.DrawText(g, r.Detail, detail, new Rectangle((int)(icon + Theme.S(14)), (int)(y + Theme.S(22)), Width - (int)icon - Theme.S(14), Theme.S(28)),
                            r.State == State.Failed ? Theme.RedHover : Theme.Dim, TextFormatFlags.NoPadding | TextFormatFlags.EndEllipsis);
                }
            }
        }
    }

    // A big round badge: a check (done), a cross (error) or the logo.
    class Badge : Control
    {
        public enum Kind { Done, Error, Logo }
        public Kind Shape;
        float m_Grow;
        readonly System.Windows.Forms.Timer m_Timer = new System.Windows.Forms.Timer { Interval = 16 };

        public Badge(Kind kind)
        {
            Shape = kind;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            m_Grow = kind == Kind.Logo ? 1f : 0f;
            m_Timer.Tick += delegate { m_Grow = Math.Min(1f, m_Grow + 0.06f); Invalidate(); if (m_Grow >= 1f) m_Timer.Stop(); };
            if (m_Grow < 1f) m_Timer.Start();
        }

        protected override void Dispose(bool disposing) { if (disposing) m_Timer.Dispose(); base.Dispose(disposing); }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(Parent != null ? Parent.BackColor : Theme.Bg);
            float ease = 1f - (float)Math.Pow(1f - m_Grow, 3);
            float size = Math.Min(Width, Height) * (0.75f + 0.25f * ease);
            var r = new RectangleF((Width - size) / 2, (Height - size) / 2, size, size);

            if (Shape == Kind.Logo) { Theme.DrawLogo(g, r); return; }

            Color c = Shape == Kind.Done ? Theme.Green : Theme.Red;
            using (var glow = new SolidBrush(Color.FromArgb((int)(40 * ease), c))) g.FillEllipse(glow, r);
            var inner = RectangleF.Inflate(r, -size * 0.12f, -size * 0.12f);
            using (var b = new SolidBrush(c)) g.FillEllipse(b, inner);
            using (var p = new Pen(Theme.Bg, size * 0.07f) { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round })
            {
                float s = inner.Width, x = inner.X, y = inner.Y;
                if (Shape == Kind.Done)
                    g.DrawLines(p, new[] { new PointF(x + s * 0.28f, y + s * 0.52f), new PointF(x + s * 0.44f, y + s * 0.67f), new PointF(x + s * 0.73f, y + s * 0.36f) });
                else
                {
                    g.DrawLine(p, x + s * 0.34f, y + s * 0.34f, x + s * 0.66f, y + s * 0.66f);
                    g.DrawLine(p, x + s * 0.66f, y + s * 0.34f, x + s * 0.34f, y + s * 0.66f);
                }
            }
        }
    }

    // ---- The window -------------------------------------------------------------------------

    class SetupForm : Form
    {
        readonly Program.Options m_Options;
        readonly Panel m_Page = new Panel();
        readonly StringBuilder m_Log = new StringBuilder();
        readonly ToolTip m_Tip = new ToolTip();
        List<GameInstall> m_Found = new List<GameInstall>();
        GameInstall m_Game;
        bool m_UseTailscale, m_Firewall;
        volatile bool m_Closing;
        string m_AuthUrl;
        TailscaleStatus m_Ts;

        [DllImport("user32.dll")] static extern bool ReleaseCapture();
        [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, int msg, IntPtr w, IntPtr l);
        [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr h, int attr, ref int value, int size);

        public SetupForm(Program.Options options)
        {
            m_Options = options;
            using (Graphics g = CreateGraphics()) Theme.Scale = Math.Max(1f, g.DpiX / 96f);

            Text = "HITMAN Co-op Setup";
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Theme.Bg;
            ForeColor = Theme.Text;
            ClientSize = new Size(Theme.S(780), Theme.S(540));
            DoubleBuffered = true;
            KeyPreview = true;
            try { Icon = Icon.ExtractAssociatedIcon(Program.ExePath); } catch { }

            m_Page.SetBounds(Theme.S(1), Theme.S(48), ClientSize.Width - Theme.S(2), ClientSize.Height - Theme.S(49));
            m_Page.BackColor = Theme.Bg;
            Controls.Add(m_Page);
            BuildTitleBar();

            MouseDown += Drag;
            if (m_Options.RenderTo != null)
            {
                // Design check: every page drawn off screen into PNGs, nothing shown.
                StartPosition = FormStartPosition.Manual;
                Location = new Point(-30000, -30000);
                ShowInTaskbar = false;
                Shown += delegate { RenderPages(m_Options.RenderTo); Close(); };
            }
            else
                Load += delegate { Start(); };
            FormClosing += delegate { m_Closing = true; };
            KeyDown += delegate(object s, KeyEventArgs e) { if (e.KeyCode == Keys.Escape && !m_Busy) Close(); };
        }

        protected override CreateParams CreateParams
        {
            get
            {
                CreateParams cp = base.CreateParams;
                cp.ClassStyle |= 0x20000; // CS_DROPSHADOW
                cp.Style |= 0x20000;      // WS_MINIMIZEBOX: the taskbar button minimises it
                return cp;
            }
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            try
            {
                int round = 2; // Windows 11: rounded corners
                DwmSetWindowAttribute(Handle, 33, ref round, 4);
                int dark = 1;
                DwmSetWindowAttribute(Handle, 20, ref dark, 4);
            }
            catch { }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            using (var p = new Pen(Theme.Border)) e.Graphics.DrawRectangle(p, 0, 0, ClientSize.Width - 1, ClientSize.Height - 1);
            // A thin red line under the title bar, like a briefing file.
            using (var b = new LinearGradientBrush(new Rectangle(0, 0, ClientSize.Width, 2), Theme.Red, Color.FromArgb(0, Theme.Red), 0f))
                e.Graphics.FillRectangle(b, 1, Theme.S(47), ClientSize.Width - 2, Math.Max(1, Theme.S(1.5f)));
        }

        void Drag(object sender, MouseEventArgs e)
        {
            if (e.Button != MouseButtons.Left) return;
            ReleaseCapture();
            SendMessage(Handle, 0xA1, (IntPtr)2, IntPtr.Zero); // WM_NCLBUTTONDOWN, HTCAPTION
        }

        void BuildTitleBar()
        {
            var logo = new Badge(Badge.Kind.Logo) { Bounds = new Rectangle(Theme.S(16), Theme.S(12), Theme.S(24), Theme.S(24)), BackColor = Theme.Bg };
            logo.MouseDown += Drag;
            Controls.Add(logo);

            var title = new Label { AutoSize = true, Text = "HITMAN", Font = Theme.Display(17), ForeColor = Theme.Text, BackColor = Theme.Bg, Location = new Point(Theme.S(48), Theme.S(13)) };
            var coop = new Label { AutoSize = true, Text = "CO-OP", Font = Theme.Display(17), ForeColor = Theme.Red, BackColor = Theme.Bg };
            var setup = new Label { AutoSize = true, Text = "SETUP", Font = Theme.Ui(11, true), ForeColor = Theme.Faint, BackColor = Theme.Bg };
            Controls.Add(title);
            Controls.Add(coop);
            Controls.Add(setup);
            coop.Location = new Point(title.Right - Theme.S(4), Theme.S(13));
            setup.Location = new Point(coop.Right + Theme.S(6), Theme.S(18));
            foreach (Control c in new Control[] { title, coop, setup }) c.MouseDown += Drag;

            AddGlyph("\u2715", ClientSize.Width - Theme.S(46), delegate { if (!m_Busy || Confirm("Setup is still working. Close anyway?")) Close(); }, true);
            AddGlyph("\u2013", ClientSize.Width - Theme.S(88), delegate { WindowState = FormWindowState.Minimized; }, false);
        }

        void AddGlyph(string glyph, int x, Action click, bool danger)
        {
            var b = new Label { Text = glyph, Font = Theme.Ui(15), ForeColor = Theme.Dim, BackColor = Theme.Bg, TextAlign = ContentAlignment.MiddleCenter, Bounds = new Rectangle(x, Theme.S(6), Theme.S(40), Theme.S(36)), Cursor = Cursors.Hand };
            b.MouseEnter += delegate { b.BackColor = danger ? Theme.Red : Theme.Surface; b.ForeColor = Theme.Text; };
            b.MouseLeave += delegate { b.BackColor = Theme.Bg; b.ForeColor = Theme.Dim; };
            b.Click += delegate { click(); };
            Controls.Add(b);
        }

        bool Confirm(string text)
        {
            return MessageBox.Show(this, text, "HITMAN Co-op Setup", MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes;
        }

        bool m_Busy;

        void Say(string line)
        {
            lock (m_Log) m_Log.AppendLine(line);
        }

        // Work off the window's thread; then (on it) what comes next.
        void Work(Action work, Action<Exception> done)
        {
            m_Busy = true;
            var t = new Thread(delegate()
            {
                Exception error = null;
                try { work(); }
                catch (Exception e) { error = e; }
                if (m_Closing) return;
                try { BeginInvoke((Action)delegate { m_Busy = false; done(error); }); }
                catch { }
            });
            t.IsBackground = true;
            t.SetApartmentState(ApartmentState.STA);
            t.Start();
        }

        // On the window's thread; a page left meanwhile (its controls gone) is no error.
        void Ui(Action a)
        {
            if (m_Closing) return;
            Action safe = delegate { try { a(); } catch { } };
            try { if (InvokeRequired) BeginInvoke(safe); else safe(); }
            catch { }
        }

        // ---- Page helpers --------------------------------------------------------------------

        int m_PageId;
        volatile bool m_TsCancel;

        void Clear()
        {
            ++m_PageId;
            m_TsCancel = true;
            foreach (Control c in m_Page.Controls.Cast<Control>().ToArray()) { m_Page.Controls.Remove(c); c.Dispose(); }
        }

        Label Lbl(Control parent, string text, Font font, Color color, int x, int y, int width = 0, ContentAlignment align = ContentAlignment.TopLeft)
        {
            var l = new Label { Text = text, Font = font, ForeColor = color, BackColor = parent is Card ? Color.Transparent : parent.BackColor, Location = new Point(x, y), UseMnemonic = false, TextAlign = align };
            if (width > 0)
            {
                l.AutoSize = false;
                l.Width = width;
                l.Height = TextRenderer.MeasureText(text, font, new Size(width, 2000), TextFormatFlags.WordBreak).Height + Theme.S(4);
            }
            else l.AutoSize = true;
            parent.Controls.Add(l);
            return l;
        }

        Pill Btn(Control parent, string text, Pill.Kind kind, int x, int y, int w, int h, Action click)
        {
            var b = new Pill(text, kind) { Bounds = new Rectangle(x, y, w, h) };
            if (h < Theme.S(38)) b.Font = Theme.Ui(12.5f, true);
            b.Click += delegate { if (b.Enabled) click(); };
            parent.Controls.Add(b);
            return b;
        }

        int W { get { return m_Page.ClientSize.Width; } }
        int H { get { return m_Page.ClientSize.Height; } }

        void Heading(string title, string sub)
        {
            Lbl(m_Page, title.ToUpperInvariant(), Theme.Display(30), Theme.Text, Theme.S(48), Theme.S(28));
            if (sub != null) Lbl(m_Page, sub, Theme.Ui(14), Theme.Dim, Theme.S(50), Theme.S(70), W - Theme.S(100));
        }

        void Footer()
        {
            var f = Lbl(m_Page, "v" + Program.Version + "   \u00B7   free and open source (GPL-3.0)   \u00B7   by " + Program.Author, Theme.Ui(11.5f), Theme.Faint, Theme.S(24), H - Theme.S(30));
            var d = Lbl(m_Page, "Discord", Theme.Ui(11.5f, true), Theme.Dim, 0, H - Theme.S(30));
            d.Left = W - d.Width - Theme.S(24);
            d.Cursor = Cursors.Hand;
            d.Click += delegate { Tailscale.Open(Program.Discord); };
            var gh = Lbl(m_Page, "GitHub", Theme.Ui(11.5f, true), Theme.Dim, 0, H - Theme.S(30));
            gh.Left = d.Left - gh.Width - Theme.S(16);
            gh.Cursor = Cursors.Hand;
            gh.Click += delegate { Tailscale.Open(Program.Website + "/releases"); };
        }

        // ---- Start ------------------------------------------------------------------------------

        void Start()
        {
            Clear();
            var spin = new Steps { Bounds = new Rectangle(Theme.S(300), Theme.S(200), Theme.S(300), Theme.S(60)) };
            m_Page.Controls.Add(spin);
            spin.Add("Getting ready", "Unpacking HITMAN Co-op and looking for your game");
            spin.Set(0, Steps.State.Working);

            Work(delegate
            {
                Payload.Prepare();
                m_Found = Games.FindAll();
            }, delegate(Exception e)
            {
                if (e != null) { ShowError(e); return; }

                // The folder asked for, else the one HITMAN Co-op went into last time (an update or a
                // repair goes back there), else the first install found.
                string want = m_Options.Game ?? AppsEntry.LastGame();
                if (want != null && Games.IsGameDir(Games.Normalize(want)))
                {
                    m_Game = Games.Inspect(want);
                    if (!m_Found.Any(f => f.Path.Equals(m_Game.Path, StringComparison.OrdinalIgnoreCase))) m_Found.Insert(0, m_Game);
                }
                else
                    m_Game = m_Found.FirstOrDefault(f => f.InstalledModVersion != null) ?? m_Found.FirstOrDefault();

                if (m_Options.Uninstall && (want == null || !Games.IsGameDir(Games.Normalize(want))))
                {
                    AppsEntry.Unregister();
                    Clear();
                    m_Page.Controls.Add(new Badge(Badge.Kind.Done) { Bounds = new Rectangle((W - Theme.S(96)) / 2, Theme.S(50), Theme.S(96), Theme.S(96)) });
                    Lbl(m_Page, "NOTHING LEFT TO REMOVE", Theme.Display(34), Theme.Text, 0, Theme.S(160), W, ContentAlignment.TopCenter);
                    Lbl(m_Page, "The game folder HITMAN Co-op was in is gone, so there's nothing to put back. It's off the Apps list now.", Theme.Ui(15), Theme.Dim, Theme.S(60), Theme.S(208), W - Theme.S(120), ContentAlignment.TopCenter);
                    AcceptButtonOf(Btn(m_Page, "Close", Pill.Kind.Primary, (W - Theme.S(200)) / 2, Theme.S(290), Theme.S(200), Theme.S(48), Close));
                    Footer();
                }
                else if (m_Options.Uninstall && m_Game != null) { if (m_Options.Auto) RunUninstall(m_Options.KeepSettings); else ShowUninstall(); }
                else if (m_Options.Auto && m_Game != null) RunInstall();
                else ShowWelcome();
            });
        }

        // ---- Welcome ----------------------------------------------------------------------------

        void ShowWelcome()
        {
            Clear();
            m_Page.Controls.Add(new Badge(Badge.Kind.Logo) { Bounds = new Rectangle((W - Theme.S(76)) / 2, Theme.S(26), Theme.S(76), Theme.S(76)) });

            var t1 = Lbl(m_Page, "HITMAN", Theme.Display(44), Theme.Text, 0, Theme.S(108));
            var t2 = Lbl(m_Page, "CO-OP", Theme.Display(44), Theme.Red, 0, Theme.S(108));
            int total = t1.Width + t2.Width - Theme.S(8);
            t1.Left = (W - total) / 2;
            t2.Left = t1.Right - Theme.S(8);
            Lbl(m_Page, "Play HITMAN World of Assassination together, with a friend.", Theme.Ui(15), Theme.Dim, 0, Theme.S(164), W, ContentAlignment.TopCenter);

            var card = new Card { Bounds = new Rectangle(Theme.S(110), Theme.S(206), W - Theme.S(220), Theme.S(96)) };
            m_Page.Controls.Add(card);
            Pill install = null;

            if (m_Game != null)
            {
                Lbl(card, "INSTALLING INTO", Theme.Ui(11, true), Theme.Faint, Theme.S(20), Theme.S(14));
                Font pathFont = Theme.Ui(15, true);
                var path = Lbl(card, FitPath(m_Game.Path, pathFont, card.Width - Theme.S(170)), pathFont, Theme.Text, Theme.S(20), Theme.S(32));
                m_Tip.SetToolTip(path, m_Game.Path);
                string line = m_Game.Store + "   \u00B7   game " + m_Game.Version;
                if (m_Game.InstalledModVersion != null) line += "   \u00B7   HITMAN Co-op " + m_Game.InstalledModVersion + " is in it";
                Lbl(card, line, Theme.Ui(12.5f), Theme.Dim, Theme.S(20), Theme.S(62), card.Width - Theme.S(160));
            }
            else
            {
                bool store = Games.StoreBuilds().Count > 0;
                Lbl(card, store ? "Only the Microsoft Store / Game Pass version of HITMAN is here" : "We couldn't find HITMAN on this PC",
                    Theme.Ui(15, true), Theme.Text, Theme.S(20), Theme.S(18), card.Width - Theme.S(160));
                Lbl(card, store ? "Windows locks its files, so no mod can be installed into it. HITMAN Co-op works with the Steam and Epic versions."
                                 : "Choose your HITMAN World of Assassination folder (the one with the Retail folder in it).",
                    Theme.Ui(12.5f), Theme.Dim, Theme.S(20), Theme.S(44), card.Width - Theme.S(170));
            }

            Btn(card, m_Game == null ? "Browse..." : "Change", Pill.Kind.Secondary, card.Width - Theme.S(132), (card.Height - Theme.S(36)) / 2, Theme.S(112), Theme.S(36), ChangeGame);

            int y = Theme.S(318);
            if (m_Game != null && !Payload.HasBuild(m_Game.Build) || m_Game != null && !m_Game.Version.StartsWith(m_Game.Build + "."))
            {
                Lbl(m_Page, "\u26A0  There's no HITMAN Co-op build for game " + m_Game.Version + " yet. It installs, but co-op stays off until the game is updated.",
                    Theme.Ui(12.5f), Theme.Amber, Theme.S(110), y, W - Theme.S(220));
                y += Theme.S(26);
            }

            // A newer HITMAN Co-op already in: this setup would put an older one in its place.
            Version have, ours;
            bool older = m_Game != null && m_Game.InstalledModVersion != null && Version.TryParse(m_Game.InstalledModVersion, out have) &&
                Version.TryParse(Program.Version, out ours) && have > ours;
            if (older)
            {
                Lbl(m_Page, "\u26A0  HITMAN Co-op " + m_Game.InstalledModVersion + " is installed, newer than this setup (" + Program.Version + "). Installing puts the older one in its place: your partner needs the same version.",
                    Theme.Ui(12.5f), Theme.Amber, Theme.S(110), y, W - Theme.S(220));
                y += Theme.S(40);
            }
            if (Games.GameRunning())
            {
                Lbl(m_Page, "\u26A0  HITMAN is running: close it before you install.", Theme.Ui(12.5f), Theme.Amber, Theme.S(110), y, W - Theme.S(220));
                y += Theme.S(24);
            }

            string verb = m_Game == null || m_Game.InstalledModVersion == null ? "Install" : older ? "Install " + Program.Version :
                m_Game.InstalledModVersion == Program.Version ? "Repair" : "Update";
            install = Btn(m_Page, verb, Pill.Kind.Primary, (W - Theme.S(240)) / 2, Math.Max(y + Theme.S(8), Theme.S(330)), Theme.S(240), Theme.S(52), RunInstall);
            install.Font = Theme.Ui(17, true);
            install.Enabled = m_Game != null;
            AcceptButtonOf(install);

            var links = new List<KeyValuePair<string, Action>>();
            if (m_Game != null && m_Game.InstalledModVersion != null) links.Add(new KeyValuePair<string, Action>("Uninstall", ShowUninstall));
            string test = Path.Combine(Payload.Dir ?? "", "Test-Connection.exe");
            if (File.Exists(test))
                links.Add(new KeyValuePair<string, Action>("Test a connection", delegate { try { Process.Start(new ProcessStartInfo(test) { UseShellExecute = true, WorkingDirectory = Payload.Dir }); } catch { } }));
            links.Add(new KeyValuePair<string, Action>("Set up Tailscale only", delegate { m_UseTailscale = true; ShowTailscale(false); }));

            int lx = 0;
            var made = new List<Pill>();
            foreach (var l in links)
            {
                var p = Btn(m_Page, l.Key, Pill.Kind.Ghost, 0, install.Bottom + Theme.S(16), TextRenderer.MeasureText(l.Key, Theme.Ui(12.5f, true)).Width + Theme.S(28), Theme.S(32), l.Value);
                made.Add(p);
                lx += p.Width + Theme.S(6);
            }
            int x0 = (W - lx) / 2;
            foreach (var p in made) { p.Left = x0; x0 += p.Width + Theme.S(6); }

            Footer();
        }

        // "C:\Program Files (x86)\...\common\HITMAN 3": the start and the end, on one line.
        static string FitPath(string path, Font font, int width)
        {
            string fit = string.Copy(path);
            TextRenderer.MeasureText(fit, font, new Size(width, 100), TextFormatFlags.PathEllipsis | TextFormatFlags.ModifyString | TextFormatFlags.SingleLine);
            int nul = fit.IndexOf('\0');
            return nul >= 0 ? fit.Substring(0, nul) : fit;
        }

        void AcceptButtonOf(Pill p)
        {
            p.TabIndex = 0;
            BeginInvoke((Action)delegate { if (!p.IsDisposed && p.Enabled) p.Focus(); });
        }

        void ChangeGame()
        {
            var menu = new ContextMenuStrip { ShowImageMargin = false, Font = Theme.Ui(13), BackColor = Theme.Surface, ForeColor = Theme.Text, Renderer = new ToolStripProfessionalRenderer(new DarkMenuColors()) };
            foreach (GameInstall g in m_Found)
            {
                GameInstall pick = g;
                var item = new ToolStripMenuItem(g.Path + "   (" + g.Store + ", " + g.Version + ")") { ForeColor = Theme.Text, Checked = m_Game != null && g.Path == m_Game.Path };
                item.Click += delegate { m_Game = pick; ShowWelcome(); };
                menu.Items.Add(item);
            }
            if (m_Found.Count > 0) menu.Items.Add(new ToolStripSeparator());
            var browse = new ToolStripMenuItem("Choose another folder...") { ForeColor = Theme.Text };
            browse.Click += delegate { Browse(); };
            menu.Items.Add(browse);
            menu.Show(Cursor.Position);
        }

        class DarkMenuColors : ProfessionalColorTable
        {
            public override Color MenuItemSelected { get { return Theme.SurfaceHover; } }
            public override Color MenuItemBorder { get { return Theme.Border; } }
            public override Color MenuBorder { get { return Theme.Border; } }
            public override Color ToolStripDropDownBackground { get { return Theme.Surface; } }
            public override Color SeparatorDark { get { return Theme.Border; } }
            public override Color SeparatorLight { get { return Theme.Border; } }
        }

        void Browse()
        {
            using (var d = new FolderBrowserDialog { Description = "Choose your HITMAN World of Assassination folder (the one with the Retail folder in it).", ShowNewFolderButton = false })
            {
                if (d.ShowDialog(this) != DialogResult.OK) return;
                string dir = Games.Normalize(d.SelectedPath);
                if (Games.IsStoreBuild(dir))
                {
                    MessageBox.Show(this, "This is the Microsoft Store / Game Pass version of HITMAN. Windows locks its files, so no mods can be installed into it. HITMAN Co-op works with the Steam and Epic versions.", "HITMAN Co-op Setup", MessageBoxButtons.OK, MessageBoxIcon.Information);
                    return;
                }
                if (!Games.IsGameDir(dir))
                {
                    MessageBox.Show(this, "That folder doesn't have Retail\\HITMAN3.exe in it. Choose the HITMAN folder itself (for Steam: steamapps\\common\\HITMAN 3).", "HITMAN Co-op Setup", MessageBoxButtons.OK, MessageBoxIcon.Information);
                    return;
                }
                m_Game = Games.Inspect(dir);
                if (!m_Found.Any(f => f.Path.Equals(m_Game.Path, StringComparison.OrdinalIgnoreCase))) m_Found.Add(m_Game);
                ShowWelcome();
            }
        }

        // ---- Install ----------------------------------------------------------------------------

        void RunInstall()
        {
            if (m_Game == null) return;

            if (Games.GameRunning())
            {
                ShowError(new SetupException("HITMAN is running.", "Close the game, then click Try again."), RunInstall);
                return;
            }

            // A game folder only an administrator can write to: Windows asks, and the setup carries on as admin.
            if (!Util.CanWrite(m_Game.Retail) && !Util.IsAdmin())
            {
                try
                {
                    Process.Start(new ProcessStartInfo(Program.ExePath, "--auto --game " + Util.Quote(m_Game.Path)) { UseShellExecute = true, Verb = "runas" });
                    Close();
                }
                catch
                {
                    ShowError(new SetupException("Windows needs your permission to write to the game folder.", "Click Try again and choose Yes when Windows asks."), RunInstall);
                }
                return;
            }

            Clear();
            Heading("Installing", "HITMAN Co-op " + Program.Version + " into " + m_Game.Path);
            var bar = new Bar { Bounds = new Rectangle(Theme.S(50), Theme.S(150), W - Theme.S(100), Theme.S(8)) };
            m_Page.Controls.Add(bar);
            var step = Lbl(m_Page, "Starting", Theme.Ui(13), Theme.Dim, Theme.S(50), Theme.S(170), W - Theme.S(100));
            var pct = Lbl(m_Page, "0%", Theme.Display(64), Theme.Text, Theme.S(48), Theme.S(200));
            GameInstall game = m_Game;

            Work(delegate
            {
                Say("---- " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " installing HITMAN Co-op " + Program.Version + " (setup)");
                try
                {
                    Installer.Install(game, delegate(int p, string s) { Ui(delegate { bar.Value = p; step.Text = s; pct.Text = p + "%"; }); }, Say);
                }
                catch (UnauthorizedAccessException ex)
                {
                    Say(ex.Message);
                    throw new SetupException("Windows didn't let the setup write to the game folder.",
                        "If Windows Security's \"Controlled folder access\" is on, allow HITMAN Co-op Setup there (Ransomware protection > Allow an app), then try again.", true);
                }
                catch (IOException ex)
                {
                    Say(ex.Message);
                    bool full = (ex.HResult & 0xFFFF) == 0x70 || (ex.HResult & 0xFFFF) == 0x27;
                    throw new SetupException(full ? "The drive with the game is full." : "A file in the game folder is in use: " + ex.Message,
                        full ? "Free some space on it and try again." : "Close the game and anything else using its files (a mod manager, an antivirus scan), then try again.");
                }
                AppsEntry.Register(game);
                Thread.Sleep(350);
            }, delegate(Exception e)
            {
                WriteInstallLog(game);
                if (e != null) { ShowError(e, RunInstall); return; }
                m_Game = Games.Inspect(game.Path);
                ShowConnect();
            });
        }

        void WriteInstallLog(GameInstall game)
        {
            try { lock (m_Log) File.AppendAllText(Path.Combine(game.Retail, "HitmanCoop-install.log"), m_Log.ToString()); }
            catch { }
        }

        // ---- How to connect ---------------------------------------------------------------------

        void ShowConnect()
        {
            Clear();
            Heading("How will you play together?", "You can change this any time: both ways are in the CO-OP menu in the game.");

            bool tsHere = Tailscale.Installed;
            var join = ChoiceCard("Join code", "EASIEST",
                "The host gets a short code, the friend types it in. Nothing to set up, works from anywhere, through the HITMAN Co-op server.",
                Theme.S(50), Theme.S(124));
            var ts = ChoiceCard("Tailscale", "MOST RELIABLE",
                (tsHere ? "Tailscale is on this PC already. " : "") + "A private network between your two PCs: a direct connection with the least lag. Free. We set it up and sign you in.",
                W / 2 + Theme.S(8), Theme.S(124));

            m_UseTailscale = tsHere;
            Action refresh = delegate { join.Selected = !m_UseTailscale; ts.Selected = m_UseTailscale; join.Invalidate(); ts.Invalidate(); };
            join.Click += delegate { m_UseTailscale = false; refresh(); };
            ts.Click += delegate { m_UseTailscale = true; refresh(); };
            refresh();

            m_Firewall = true;
            var fw = new CheckBox
            {
                Text = "Let my partner connect straight to my PC when I host by IP or Tailscale (adds a Windows Firewall rule for HITMAN; Windows asks first)",
                Checked = true, ForeColor = Theme.Dim, BackColor = Theme.Bg, Font = Theme.Ui(12.5f), Location = new Point(Theme.S(52), Theme.S(320)), Size = new Size(W - Theme.S(104), Theme.S(40)), FlatStyle = FlatStyle.Flat
            };
            fw.CheckedChanged += delegate { m_Firewall = fw.Checked; };
            m_Page.Controls.Add(fw);

            var go = Btn(m_Page, "Continue", Pill.Kind.Primary, W - Theme.S(50) - Theme.S(180), H - Theme.S(90), Theme.S(180), Theme.S(46), delegate
            {
                Action next = delegate { if (m_UseTailscale) ShowTailscale(true); else ShowDone(); };
                if (m_Firewall) AddFirewallRule(next);
                else next();
            });
            AcceptButtonOf(go);
            Footer();
        }

        Card ChoiceCard(string title, string badge, string text, int x, int y)
        {
            var c = new Card { Selectable = true, Bounds = new Rectangle(x, y, W / 2 - Theme.S(58), Theme.S(180)), Cursor = Cursors.Hand };
            m_Page.Controls.Add(c);
            Lbl(c, title.ToUpperInvariant(), Theme.Display(24), Theme.Text, Theme.S(20), Theme.S(18));
            var b = Lbl(c, badge, Theme.Ui(10.5f, true), Theme.Red, Theme.S(22), Theme.S(54));
            Lbl(c, text, Theme.Ui(13), Theme.Dim, Theme.S(20), Theme.S(80), c.Width - Theme.S(40));
            return c;
        }

        void AddFirewallRule(Action then)
        {
            string exe = Path.Combine(m_Game.Retail, "HITMAN3.exe");
            Work(delegate { Say(Firewall.AddRuleElevated(exe) ? "Added the Windows Firewall rule for HITMAN (UDP in)." : "Skipped the firewall rule (Windows will ask the first time you host instead)."); },
                delegate { then(); });
        }

        // ---- Tailscale ----------------------------------------------------------------------------

        Steps m_TsSteps;
        Bar m_TsBar;
        Pill m_TsAction, m_TsNext;

        void ShowTailscale(bool fromInstall)
        {
            Clear();
            Heading("Set up Tailscale", "A private network for you and your partner. Your partner does this too, on their PC.");
            m_TsSteps = new Steps { Bounds = new Rectangle(Theme.S(52), Theme.S(124), W - Theme.S(104), Theme.S(52 * 4)) };
            m_TsSteps.Add("Download Tailscale", "The official installer, from tailscale.com");
            m_TsSteps.Add("Install it", "Windows asks for permission");
            m_TsSteps.Add("Sign in", "With Google, Microsoft, Apple or GitHub, in your browser");
            m_TsSteps.Add("Connected", "Your Tailscale address appears here");
            m_Page.Controls.Add(m_TsSteps);
            m_TsBar = new Bar { Bounds = new Rectangle(Theme.S(88), Theme.S(166), Theme.S(260), Theme.S(5)), Visible = false };
            m_Page.Controls.Add(m_TsBar);
            m_TsBar.BringToFront();

            m_TsAction = Btn(m_Page, "Open the sign-in page", Pill.Kind.Secondary, Theme.S(50), H - Theme.S(90), Theme.S(230), Theme.S(46), delegate { if (m_AuthUrl != null) Tailscale.Open(m_AuthUrl); });
            m_TsAction.Visible = false;
            m_TsNext = Btn(m_Page, "Skip", Pill.Kind.Ghost, W - Theme.S(50) - Theme.S(180), H - Theme.S(90), Theme.S(180), Theme.S(46), delegate { Tailscale.StopSignIn(); if (fromInstall) ShowDone(); else ShowWelcome(); });
            Footer();
            m_TsCancel = false;
            int page = m_PageId;

            string tmp = Path.Combine(Path.GetTempPath(), "HitmanCoopSetup-tailscale-" + Guid.NewGuid().ToString("N").Substring(0, 6));
            Work(delegate { TailscaleFlow(tmp); }, delegate(Exception e)
            {
                try { if (Directory.Exists(tmp)) Directory.Delete(tmp, true); } catch { }
                Tailscale.StopSignIn();
                if (page != m_PageId) return; // left the page meanwhile
                if (e != null)
                {
                    Say("Tailscale: " + e.Message);
                    m_TsNext.Text = fromInstall ? "Continue" : "Back";
                    m_TsAction.Visible = false;
                    Btn(m_Page, "Try again", Pill.Kind.Primary, Theme.S(50), H - Theme.S(90), Theme.S(180), Theme.S(46), delegate { ShowTailscale(fromInstall); });
                    var se = e as SetupException;
                    Lbl(m_Page, e.Message + (se != null && se.Hint != null ? " " + se.Hint : ""), Theme.Ui(13), Theme.RedHover, Theme.S(52), Theme.S(340), W - Theme.S(104));
                    return;
                }
                ShowTailscaleReady(fromInstall);
            });
        }

        void TailscaleFlow(string tmp)
        {
            if (!Tailscale.Installed)
            {
                Directory.CreateDirectory(tmp);
                Ui(delegate { m_TsSteps.Set(0, Steps.State.Working); m_TsBar.Visible = true; });
                string file;
                try { file = Tailscale.Download(tmp, delegate(int p) { Ui(delegate { m_TsBar.Value = p; }); }); }
                catch (Exception e)
                {
                    Ui(delegate { m_TsSteps.Set(0, Steps.State.Failed, "Couldn't download it: " + e.Message); m_TsBar.Visible = false; });
                    throw new SetupException("Tailscale couldn't be downloaded.", "Check your internet connection, or get it from tailscale.com/download and run this again.");
                }

                string why;
                if (!Tailscale.IsGenuine(file, out why))
                {
                    Ui(delegate { m_TsSteps.Set(0, Steps.State.Failed, "Not the genuine installer: " + why); m_TsBar.Visible = false; });
                    throw new SetupException("The downloaded Tailscale installer didn't pass the signature check, so it wasn't run.", "Get Tailscale from tailscale.com/download instead.");
                }
                Ui(delegate { m_TsSteps.Set(0, Steps.State.Done, "Signed by Tailscale Inc. and checked"); m_TsBar.Visible = false; m_TsSteps.Set(1, Steps.State.Working); });

                if (!Tailscale.RunInstaller(file, out why))
                {
                    Ui(delegate { m_TsSteps.Set(1, Steps.State.Failed, why); });
                    throw new SetupException(why ?? "Tailscale didn't install.", "Click Try again, and choose Yes when Windows asks.");
                }
                Ui(delegate { m_TsSteps.Set(1, Steps.State.Done, "Installed"); });
            }
            else
                Ui(delegate { m_TsSteps.Set(0, Steps.State.Skipped, "Already on this PC"); m_TsSteps.Set(1, Steps.State.Skipped, "Already installed"); });

            Ui(delegate { m_TsSteps.Set(2, Steps.State.Working, "Checking..."); });
            DateTime until = DateTime.Now.AddMinutes(10);
            bool asked = false, opened = false;
            while (!m_Closing && !m_TsCancel)
            {
                TailscaleStatus st = Tailscale.Status();
                m_Ts = st;
                if (st.Running) break;
                if (DateTime.Now > until) throw new SetupException("Signing in to Tailscale took too long.", "Click Try again to start over.");

                if (st.State == "ServiceDown" || st.State == "NoState" || st.State == "Starting")
                    Ui(delegate { m_TsSteps.Set(2, Steps.State.Working, "Waiting for Tailscale to start..."); });
                else
                {
                    if (!asked) { Tailscale.StartSignIn(); asked = true; }
                    if (!string.IsNullOrEmpty(st.AuthUrl))
                    {
                        m_AuthUrl = st.AuthUrl;
                        if (!opened) { Tailscale.Open(st.AuthUrl); opened = true; }
                        Ui(delegate { m_TsSteps.Set(2, Steps.State.Working, "Finish signing in in your browser (the page opened by itself)"); m_TsAction.Visible = true; });
                    }
                    else
                        Ui(delegate { m_TsSteps.Set(2, Steps.State.Working, st.State == "Stopped" ? "Turning Tailscale on..." : "Opening the sign-in page..."); });
                }
                Thread.Sleep(1500);
            }

            if (m_Closing || m_TsCancel) return;
            Ui(delegate { m_TsSteps.Set(2, Steps.State.Done, "Signed in" + (m_Ts.Tailnet != null ? " (" + m_Ts.Tailnet + ")" : "")); m_TsSteps.Set(3, Steps.State.Done, "Your address: " + m_Ts.Address); m_TsAction.Visible = false; });
            Thread.Sleep(600);
        }

        void ShowTailscaleReady(bool fromInstall)
        {
            Clear();
            Heading("Tailscale is ready", null);
            TailscaleStatus st = m_Ts ?? Tailscale.Status();

            var card = new Card { Bounds = new Rectangle(Theme.S(50), Theme.S(84), W - Theme.S(100), Theme.S(92)) };
            m_Page.Controls.Add(card);
            Lbl(card, "YOUR TAILSCALE ADDRESS", Theme.Ui(11, true), Theme.Faint, Theme.S(20), Theme.S(14));
            Lbl(card, st.Address ?? "-", Theme.Mono(30), Theme.Text, Theme.S(18), Theme.S(36));
            Btn(card, "Copy", Pill.Kind.Secondary, card.Width - Theme.S(130), Theme.S(28), Theme.S(110), Theme.S(36), delegate { try { Clipboard.SetText(st.Address ?? ""); } catch { } });

            int y = Theme.S(190);
            Lbl(m_Page, "When you host, your partner clicks JOIN in the CO-OP menu with this address. When they host, you join with theirs.",
                Theme.Ui(13), Theme.Dim, Theme.S(52), y, W - Theme.S(104));
            y += Theme.S(44);

            var online = st.Peers.Where(p => p.Online).Take(3).ToList();
            if (online.Count > 0)
            {
                Lbl(m_Page, "ON YOUR TAILSCALE NETWORK NOW", Theme.Ui(11, true), Theme.Faint, Theme.S(52), y);
                y += Theme.S(22);
                foreach (TailscalePeer p in online)
                {
                    Lbl(m_Page, p.Name + "   " + p.Address + (p.Shared ? "   (shared with you)" : ""), Theme.Ui(13, true), Theme.Text, Theme.S(52), y);
                    y += Theme.S(22);
                }
                y += Theme.S(6);
            }
            else
            {
                Lbl(m_Page, "Your partner's PC isn't on your Tailscale network yet. Once they have Tailscale, invite them: in Tailscale's admin page, open this PC's \u22EF menu > Share, and send them the link.",
                    Theme.Ui(13), Theme.Text, Theme.S(52), y, W - Theme.S(104));
                y += Theme.S(48);
            }

            if (st.UsingExitNode)
            {
                Lbl(m_Page, "\u26A0  An exit node is on: all your traffic, Steam's too, goes out through another place, and Steam may sign you out (\"signed in on another computer\"). Turn the exit node off while you play.",
                    Theme.Ui(12.5f), Theme.Amber, Theme.S(52), y, W - Theme.S(104));
            }

            Btn(m_Page, "Invite your partner", Pill.Kind.Secondary, Theme.S(50), H - Theme.S(90), Theme.S(210), Theme.S(46), delegate { Tailscale.Open(Tailscale.AdminUrl); });
            var next = Btn(m_Page, fromInstall ? "Continue" : "Done", Pill.Kind.Primary, W - Theme.S(50) - Theme.S(180), H - Theme.S(90), Theme.S(180), Theme.S(46), delegate { if (fromInstall) ShowDone(); else ShowWelcome(); });
            AcceptButtonOf(next);
            Footer();
        }

        // ---- Done ---------------------------------------------------------------------------------

        void ShowDone()
        {
            Clear();
            m_Page.Controls.Add(new Badge(Badge.Kind.Done) { Bounds = new Rectangle((W - Theme.S(96)) / 2, Theme.S(34), Theme.S(96), Theme.S(96)) });
            Lbl(m_Page, "YOU'RE ALL SET", Theme.Display(38), Theme.Text, 0, Theme.S(140), W, ContentAlignment.TopCenter);
            Lbl(m_Page, "Start HITMAN, press  ~  (the key under Esc) and click CO-OP to host or join.", Theme.Ui(15), Theme.Dim, 0, Theme.S(192), W, ContentAlignment.TopCenter);

            string how = m_UseTailscale && m_Ts != null && m_Ts.Address != null
                ? "Tailscale: the host clicks HOST; the partner joins with the host's Tailscale address (yours is " + m_Ts.Address + ")."
                : "Join code: the host clicks HOST and sends the code; the partner pastes it and clicks JOIN.";
            Lbl(m_Page, how, Theme.Ui(13), Theme.Faint, Theme.S(80), Theme.S(226), W - Theme.S(160), ContentAlignment.TopCenter);
            Lbl(m_Page, "Both of you need HITMAN Co-op " + Program.Version + ", and each your own Steam or Epic account.", Theme.Ui(13), Theme.Faint, Theme.S(80), Theme.S(266), W - Theme.S(160), ContentAlignment.TopCenter);

            int bw = Theme.S(200), gap = Theme.S(12);
            bool steam = m_Game != null && m_Game.Store == "Steam";
            int count = steam ? 3 : 2;
            int x = (W - (bw * count + gap * (count - 1))) / 2, y = Theme.S(330);
            if (steam)
            {
                var launch = Btn(m_Page, "Launch HITMAN", Pill.Kind.Primary, x, y, bw, Theme.S(48), delegate { Tailscale.Open("steam://rungameid/1659040"); Close(); });
                AcceptButtonOf(launch);
                x += bw + gap;
            }
            Btn(m_Page, "Join the Discord", Pill.Kind.Secondary, x, y, bw, Theme.S(48), delegate { Tailscale.Open(Program.Discord); });
            x += bw + gap;
            var close = Btn(m_Page, "Close", steam ? Pill.Kind.Secondary : Pill.Kind.Primary, x, y, bw, Theme.S(48), Close);
            if (!steam) AcceptButtonOf(close);
            Footer();
        }

        // ---- Uninstall --------------------------------------------------------------------------

        void ShowUninstall()
        {
            if (m_Game == null) { ShowWelcome(); return; }
            Clear();
            Heading("Remove HITMAN Co-op?", "From " + m_Game.Path + ". Your game goes back to how it was, with any mod loader or mods you had before.");
            var keep = new CheckBox { Text = "Keep my HITMAN Co-op settings (name, hotkeys, options) for next time", Checked = true, ForeColor = Theme.Dim, BackColor = Theme.Bg, Font = Theme.Ui(13), Location = new Point(Theme.S(52), Theme.S(150)), AutoSize = true, FlatStyle = FlatStyle.Flat };
            m_Page.Controls.Add(keep);
            Btn(m_Page, "Cancel", Pill.Kind.Secondary, W - Theme.S(50) - Theme.S(380), H - Theme.S(90), Theme.S(180), Theme.S(46), ShowWelcome);
            Btn(m_Page, "Uninstall", Pill.Kind.Danger, W - Theme.S(50) - Theme.S(180), H - Theme.S(90), Theme.S(180), Theme.S(46), delegate { RunUninstall(keep.Checked); });
            Footer();
        }

        void RunUninstall(bool keepSettings)
        {
            if (!Util.CanWrite(m_Game.Retail) && !Util.IsAdmin())
            {
                try
                {
                    Process.Start(new ProcessStartInfo(Program.ExePath, "--uninstall --auto --game " + Util.Quote(m_Game.Path) + (keepSettings ? "" : " --remove-settings")) { UseShellExecute = true, Verb = "runas" });
                    Close();
                }
                catch { ShowError(new SetupException("Windows needs your permission to change the game folder.", "Try again and choose Yes when Windows asks.")); }
                return;
            }

            Clear();
            Heading("Removing", m_Game.Path);
            var bar = new Bar { Bounds = new Rectangle(Theme.S(50), Theme.S(150), W - Theme.S(100), Theme.S(8)), Busy = true };
            m_Page.Controls.Add(bar);
            GameInstall game = m_Game;
            Work(delegate
            {
                Installer.Uninstall(game, keepSettings, Say);
                if (string.Equals(AppsEntry.LastGame(), game.Path, StringComparison.OrdinalIgnoreCase) || AppsEntry.LastGame() == null) AppsEntry.Unregister();
                Thread.Sleep(400);
            }, delegate(Exception e)
            {
                if (e != null) { ShowError(e, delegate { RunUninstall(keepSettings); }); return; }
                Clear();
                m_Page.Controls.Add(new Badge(Badge.Kind.Done) { Bounds = new Rectangle((W - Theme.S(96)) / 2, Theme.S(50), Theme.S(96), Theme.S(96)) });
                Lbl(m_Page, "HITMAN CO-OP WAS REMOVED", Theme.Display(34), Theme.Text, 0, Theme.S(160), W, ContentAlignment.TopCenter);
                Lbl(m_Page, "Your game is as it was before.", Theme.Ui(15), Theme.Dim, 0, Theme.S(208), W, ContentAlignment.TopCenter);
                AcceptButtonOf(Btn(m_Page, "Close", Pill.Kind.Primary, (W - Theme.S(200)) / 2, Theme.S(270), Theme.S(200), Theme.S(48), Close));
                Footer();
            });
        }

        void RenderPages(string dir)
        {
            Directory.CreateDirectory(dir);
            try { Payload.Prepare(); } catch { }
            m_Found = Games.FindAll();
            m_Game = m_Found.FirstOrDefault() ?? new GameInstall { Path = @"C:\Program Files (x86)\Steam\steamapps\common\HITMAN 3", Store = "Steam", Version = "3.290.1" };
            m_Ts = new TailscaleStatus { State = "Running", Address = "100.101.102.103", Name = "my-pc", Tailnet = "example.com" };
            m_Ts.Peers.Add(new TailscalePeer { Name = "partners-pc", Address = "100.88.77.66", Online = true, Shared = true });
            var pages = new List<KeyValuePair<string, Action>>
            {
                new KeyValuePair<string, Action>("1-welcome", ShowWelcome),
                new KeyValuePair<string, Action>("2-connect", ShowConnect),
                new KeyValuePair<string, Action>("3-tailscale-ready", delegate { ShowTailscaleReady(true); }),
                new KeyValuePair<string, Action>("4-done", ShowDone),
                new KeyValuePair<string, Action>("5-uninstall", ShowUninstall),
                new KeyValuePair<string, Action>("6-error", delegate { Say("Game: example"); ShowError(new SetupException("dinput8.dll was removed from the game folder right after it was copied.", "Your antivirus took it (it is part of the ZHMModSDK mod loader, a false alarm). Allow it in Windows Security > Protection history, then install again.", true), delegate { }); }),
                new KeyValuePair<string, Action>("7-nogame", delegate { m_Game = null; ShowWelcome(); }),
            };
            foreach (var p in pages)
            {
                p.Value();
                for (int i = 0; i < 30; i++) { Application.DoEvents(); Thread.Sleep(15); }
                using (var bmp = new Bitmap(Width, Height))
                {
                    DrawToBitmap(bmp, new Rectangle(0, 0, Width, Height));
                    bmp.Save(Path.Combine(dir, p.Key + ".png"), System.Drawing.Imaging.ImageFormat.Png);
                }
            }
            m_TsCancel = true;
        }

        // ---- Errors -----------------------------------------------------------------------------

        void ShowError(Exception e, Action retry = null)
        {
            Clear();
            var se = e as SetupException;
            Say("ERROR: " + e.Message + (se != null && se.Hint != null ? " " + se.Hint : "") + (se == null ? " (" + e.GetType().Name + ")" : ""));
            m_Page.Controls.Add(new Badge(Badge.Kind.Error) { Bounds = new Rectangle(Theme.S(48), Theme.S(36), Theme.S(64), Theme.S(64)) });
            Lbl(m_Page, se != null ? "SOMETHING NEEDS YOUR ATTENTION" : "SOMETHING WENT WRONG", Theme.Display(28), Theme.Text, Theme.S(128), Theme.S(44));
            Lbl(m_Page, e.Message, Theme.Ui(15, true), Theme.Text, Theme.S(130), Theme.S(84), W - Theme.S(180));
            int y = Theme.S(130);
            if (se != null && se.Hint != null) { y = Lbl(m_Page, se.Hint, Theme.Ui(13.5f), Theme.Dim, Theme.S(130), Theme.S(122), W - Theme.S(180)).Bottom + Theme.S(8); }

            string log;
            lock (m_Log) log = m_Log.ToString();
            var box = new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Text = log.Replace("\n", "\r\n").Replace("\r\r", "\r"), BackColor = Theme.Surface, ForeColor = Theme.Dim, BorderStyle = BorderStyle.None, Font = Theme.Mono(11.5f), Bounds = new Rectangle(Theme.S(130), Math.Max(y, Theme.S(170)), W - Theme.S(180), Theme.S(150)) };
            m_Page.Controls.Add(box);

            int x = W - Theme.S(50);
            x -= Theme.S(150);
            Btn(m_Page, "Close", Pill.Kind.Secondary, x, H - Theme.S(90), Theme.S(150), Theme.S(46), Close);
            if (retry != null)
            {
                x -= Theme.S(170);
                AcceptButtonOf(Btn(m_Page, "Try again", Pill.Kind.Primary, x, H - Theme.S(90), Theme.S(160), Theme.S(46), retry));
            }
            if (se != null && se.OfferSecurity)
                Btn(m_Page, "Open Windows Security", Pill.Kind.Secondary, Theme.S(50), H - Theme.S(90), Theme.S(230), Theme.S(46), delegate { Tailscale.Open("windowsdefender://threat/"); });
            Btn(m_Page, "Copy details", Pill.Kind.Ghost, Theme.S(130), box.Bottom + Theme.S(6), Theme.S(130), Theme.S(30), delegate { try { Clipboard.SetText(log.Length > 0 ? log : e.ToString()); } catch { } });
        }
    }
}
