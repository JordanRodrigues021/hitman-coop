// HITMAN Co-op setup: a one-file, windowed installer. Created by Jordan Rodrigues.
//
// Everything the player needs (the mod, the ZHMModSDK loader, installer.ps1, the connection test)
// is embedded as payload.zip. The window finds the game, then runs installer.ps1 hidden and shows
// its output, so the install logic stays in one tested place.
//
// Built by tools/package.ps1 with the C# compiler that ships with Windows (.NET Framework 4).

using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Principal;
using System.Windows.Forms;

[assembly: AssemblyTitle("HITMAN Co-op Setup")]
[assembly: AssemblyDescription("Two-player co-op for HITMAN World of Assassination")]
[assembly: AssemblyCompany("Jordan Rodrigues")]
[assembly: AssemblyProduct("HITMAN Co-op")]
[assembly: AssemblyCopyright("Copyright (C) 2026 Jordan Rodrigues. GPL-3.0-or-later.")]

namespace HitmanCoopSetup
{
    static class Program
    {
        public const string Version = "0.13.2";
        public const string Author = "Jordan Rodrigues";

        [STAThread]
        static void Main(string[] args)
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);

            string preselected = null;
            for (int i = 0; i + 1 < args.Length; i++)
                if (args[i] == "--game") preselected = args[i + 1];

            Application.Run(new SetupForm(preselected));
        }
    }

    static class Theme
    {
        public static readonly Color Back = Color.FromArgb(14, 14, 16);
        public static readonly Color Panel = Color.FromArgb(26, 26, 30);
        public static readonly Color Text = Color.FromArgb(232, 232, 236);
        public static readonly Color Dim = Color.FromArgb(150, 150, 160);
        public static readonly Color Red = Color.FromArgb(219, 20, 36);
        public static readonly Color RedDark = Color.FromArgb(140, 10, 22);
        public static readonly Color Gold = Color.FromArgb(255, 199, 64);
        public static readonly Color Good = Color.FromArgb(88, 214, 116);
    }

    // The title card: big red HITMAN CO-OP and a gold, shimmering "created by Jordan Rodrigues".
    class Banner : Control
    {
        readonly Timer m_Timer = new Timer();
        float m_Phase;

        public Banner()
        {
            DoubleBuffered = true;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer, true);
            m_Timer.Interval = 33;
            m_Timer.Tick += delegate { m_Phase = (m_Phase + 0.012f) % 1.6f; Invalidate(); };
            m_Timer.Start();
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing) m_Timer.Dispose();
            base.Dispose(disposing);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;

            // Drawn for a 700 px wide banner and scaled to the real size (high-DPI screens).
            float scale = Width / 700f;
            g.ScaleTransform(scale, scale);
            var area = new Rectangle(0, 0, 700, (int)Math.Ceiling(Height / scale));
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;

            using (var bg = new LinearGradientBrush(area, Color.FromArgb(30, 4, 8), Theme.Back, 0f))
                g.FillRectangle(bg, area);

            // Red accent bars, like a mission briefing.
            using (var red = new SolidBrush(Theme.Red))
            {
                g.FillRectangle(red, 0, 0, 700, 4);
                g.FillRectangle(red, 24, 26, 6, 64);
            }

            using (var title = new Font("Segoe UI Black", 40f, FontStyle.Bold, GraphicsUnit.Pixel))
            using (var white = new SolidBrush(Color.White))
            using (var redText = new SolidBrush(Theme.Red))
            {
                g.DrawString("HITMAN", title, white, 40, 18);
                SizeF w = g.MeasureString("HITMAN", title);
                g.DrawString("CO-OP", title, redText, 40 + w.Width - 14, 18);
            }

            // "created by Jordan Rodrigues" with a light sweep across the gold.
            string by = "★  created by " + Program.Author + "  ★";
            using (var font = new Font("Segoe UI Semibold", 20f, FontStyle.Bold, GraphicsUnit.Pixel))
            {
                SizeF size = g.MeasureString(by, font);
                var rect = new RectangleF(44, 72, size.Width, size.Height);
                float x = rect.Left + (m_Phase - 0.3f) * rect.Width;
                var sweep = new RectangleF(x - rect.Width * 0.25f, rect.Top, rect.Width * 0.5f + 1, rect.Height);

                using (var gold = new SolidBrush(Theme.Gold))
                    g.DrawString(by, font, gold, rect.Location);

                using (var shine = new LinearGradientBrush(sweep, Color.FromArgb(0, 255, 255, 255), Color.FromArgb(0, 255, 255, 255), 0f))
                {
                    var blend = new ColorBlend(3);
                    blend.Colors = new[] { Color.FromArgb(0, 255, 250, 220), Color.FromArgb(230, 255, 250, 220), Color.FromArgb(0, 255, 250, 220) };
                    blend.Positions = new[] { 0f, 0.5f, 1f };
                    shine.InterpolationColors = blend;
                    var clip = g.Clip;
                    g.SetClip(sweep);
                    g.DrawString(by, font, shine, rect.Location);
                    g.Clip = clip;
                }
            }

            using (var small = new Font("Segoe UI", 12f, FontStyle.Regular, GraphicsUnit.Pixel))
            using (var dim = new SolidBrush(Theme.Dim))
                g.DrawString("Two-player co-op for HITMAN World of Assassination   •   v" + Program.Version, small, dim, 46, 108);
        }
    }

    class FlatButton : Button
    {
        public FlatButton(string text, Color back, Color fore)
        {
            Text = text;
            FlatStyle = FlatStyle.Flat;
            FlatAppearance.BorderSize = 0;
            BackColor = back;
            ForeColor = fore;
            Font = new Font("Segoe UI Semibold", 10.5f, FontStyle.Bold);
            Cursor = Cursors.Hand;
            Height = 40;
        }
    }

    class SetupForm : Form
    {
        readonly ComboBox m_Games = new ComboBox();
        readonly CheckBox m_Firewall = new CheckBox();
        readonly TextBox m_Log = new TextBox();
        readonly Label m_Result = new Label();
        readonly FlatButton m_Install, m_Uninstall, m_Test, m_Browse;
        readonly string m_Preselected;
        string m_PayloadDir;
        bool m_Busy;

        public SetupForm(string preselected)
        {
            m_Preselected = preselected;
            SuspendLayout();
            AutoScaleDimensions = new SizeF(96f, 96f);
            AutoScaleMode = AutoScaleMode.Dpi;
            Text = "HITMAN Co-op Setup  —  created by " + Program.Author;
            ClientSize = new Size(700, 560);
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Theme.Back;
            ForeColor = Theme.Text;
            Font = new Font("Segoe UI", 10f);
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            var banner = new Banner { Bounds = new Rectangle(0, 0, 700, 136) };
            Controls.Add(banner);

            var gameLabel = new Label { Text = "Game folder", ForeColor = Theme.Dim, Bounds = new Rectangle(28, 152, 300, 22) };
            Controls.Add(gameLabel);

            m_Games.Bounds = new Rectangle(28, 176, 520, 30);
            m_Games.DropDownStyle = ComboBoxStyle.DropDown;
            m_Games.BackColor = Theme.Panel;
            m_Games.ForeColor = Theme.Text;
            m_Games.FlatStyle = FlatStyle.Flat;
            Controls.Add(m_Games);

            m_Browse = new FlatButton("Browse…", Theme.Panel, Theme.Text) { Bounds = new Rectangle(556, 174, 116, 32) };
            m_Browse.Click += delegate { Browse(); };
            Controls.Add(m_Browse);

            m_Firewall.Text = "Also allow direct IP connections through Windows Firewall (not needed for join codes)";
            m_Firewall.ForeColor = Theme.Dim;
            m_Firewall.Bounds = new Rectangle(28, 214, 650, 26);
            Controls.Add(m_Firewall);

            m_Install = new FlatButton("INSTALL", Theme.Red, Color.White) { Bounds = new Rectangle(28, 252, 220, 46) };
            m_Install.Font = new Font("Segoe UI Black", 13f, FontStyle.Bold);
            m_Install.FlatAppearance.MouseOverBackColor = Color.FromArgb(240, 40, 56);
            m_Install.Click += delegate { Run(false); };
            Controls.Add(m_Install);

            m_Uninstall = new FlatButton("Uninstall", Theme.Panel, Theme.Text) { Bounds = new Rectangle(258, 254, 140, 42) };
            m_Uninstall.Click += delegate { Run(true); };
            Controls.Add(m_Uninstall);

            m_Test = new FlatButton("Test connection", Theme.Panel, Theme.Gold) { Bounds = new Rectangle(408, 254, 170, 42) };
            m_Test.Click += delegate { TestConnection(); };
            Controls.Add(m_Test);

            m_Result.Bounds = new Rectangle(28, 306, 650, 48);
            m_Result.Font = new Font("Segoe UI Semibold", 10.5f, FontStyle.Bold);
            Controls.Add(m_Result);

            m_Log.Multiline = true;
            m_Log.ReadOnly = true;
            m_Log.ScrollBars = ScrollBars.Vertical;
            m_Log.BorderStyle = BorderStyle.None;
            m_Log.BackColor = Theme.Panel;
            m_Log.ForeColor = Color.FromArgb(200, 200, 208);
            m_Log.Font = new Font("Consolas", 9f);
            m_Log.Bounds = new Rectangle(28, 358, 644, 156);
            Controls.Add(m_Log);

            var footer = new Label
            {
                Text = "HITMAN Co-op v" + Program.Version + "  ·  © 2026 " + Program.Author +
                       "  ·  GPL-3.0  ·  uses ZHMModSDK by OrfeasZ",
                ForeColor = Theme.Dim,
                Font = new Font("Segoe UI", 8.5f),
                Bounds = new Rectangle(28, 524, 644, 22)
            };
            Controls.Add(footer);
            ResumeLayout(false);

            Shown += delegate { Prepare(); };
        }

        void Log(string line)
        {
            if (line == null) return;
            if (InvokeRequired) { BeginInvoke(new Action<string>(Log), line); return; }
            m_Log.AppendText(line + Environment.NewLine);
        }

        void SetResult(string text, Color color)
        {
            m_Result.Text = text;
            m_Result.ForeColor = color;
        }

        void SetBusy(bool busy)
        {
            m_Busy = busy;
            m_Install.Enabled = m_Uninstall.Enabled = m_Browse.Enabled = m_Games.Enabled = !busy;
            UseWaitCursor = busy;
        }

        // Unpacks the embedded payload and asks installer.ps1 which installs it can see.
        void Prepare()
        {
            SetBusy(true);
            SetResult("Looking for HITMAN…", Theme.Dim);

            var worker = new System.ComponentModel.BackgroundWorker();
            string[] found = new string[0];
            string error = null;

            worker.DoWork += delegate
            {
                try
                {
                    m_PayloadDir = Path.Combine(Path.GetTempPath(), "HitmanCoopSetup-" + Program.Version);
                    if (Directory.Exists(m_PayloadDir)) Directory.Delete(m_PayloadDir, true);
                    Directory.CreateDirectory(m_PayloadDir);

                    using (Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip"))
                    {
                        if (s == null) throw new InvalidOperationException("This setup file is damaged (no payload). Download it again.");
                        string zip = Path.Combine(m_PayloadDir, "payload.zip");
                        using (var f = File.Create(zip)) s.CopyTo(f);
                        ZipFile.ExtractToDirectory(zip, m_PayloadDir);
                        File.Delete(zip);
                    }

                    string output;
                    RunInstaller("-ListGames", out output, null);
                    found = output.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
                }
                catch (Exception ex) { error = ex.Message; }
            };

            worker.RunWorkerCompleted += delegate
            {
                SetBusy(false);

                if (error != null)
                {
                    SetResult(error, Theme.Red);
                    m_Install.Enabled = m_Uninstall.Enabled = false;
                    return;
                }

                foreach (string path in found) m_Games.Items.Add(path.Trim());

                if (!string.IsNullOrEmpty(m_Preselected)) m_Games.Text = m_Preselected;
                else if (m_Games.Items.Count > 0) m_Games.SelectedIndex = 0;

                if (m_Games.Items.Count > 0)
                    SetResult("Found HITMAN. Click INSTALL. (Close the game first.)", Theme.Text);
                else
                    SetResult("Couldn't find HITMAN automatically. Click Browse and pick the game folder (the one with Retail inside).", Theme.Gold);
            };

            worker.RunWorkerAsync();
        }

        void Browse()
        {
            using (var dialog = new FolderBrowserDialog())
            {
                dialog.Description = "Pick the HITMAN World of Assassination folder (it contains Retail\\HITMAN3.exe)";
                if (dialog.ShowDialog(this) == DialogResult.OK) m_Games.Text = dialog.SelectedPath;
            }
        }

        // Runs installer.ps1 hidden. Lines go to p_Line as they arrive; returns the exit code.
        int RunInstaller(string arguments, out string output, Action<string> onLine)
        {
            var psi = new ProcessStartInfo("powershell.exe",
                "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + Path.Combine(m_PayloadDir, "installer.ps1") + "\" " + arguments);
            psi.UseShellExecute = false;
            psi.CreateNoWindow = true;
            psi.RedirectStandardOutput = true;
            psi.RedirectStandardError = true;
            psi.WorkingDirectory = m_PayloadDir;

            var all = new System.Text.StringBuilder();

            using (var p = new Process { StartInfo = psi })
            {
                DataReceivedEventHandler handler = delegate (object sender, DataReceivedEventArgs e)
                {
                    if (e.Data == null) return;
                    lock (all) all.AppendLine(e.Data);
                    if (onLine != null) onLine(e.Data);
                };
                p.OutputDataReceived += handler;
                p.ErrorDataReceived += handler;
                p.Start();
                p.BeginOutputReadLine();
                p.BeginErrorReadLine();
                p.WaitForExit();
                output = all.ToString();
                return p.ExitCode;
            }
        }

        static bool CanWrite(string dir)
        {
            try
            {
                string probe = Path.Combine(dir, ".hitmancoop-write-test");
                File.WriteAllText(probe, "x");
                File.Delete(probe);
                return true;
            }
            catch { return false; }
        }

        static bool IsAdmin()
        {
            return new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator);
        }

        void Run(bool uninstall)
        {
            if (m_Busy) return;

            string game = m_Games.Text.Trim().Trim('"');

            // The Retail folder itself picked: its parent is the game folder.
            if (game.Length > 0 && string.Equals(Path.GetFileName(game.TrimEnd('\\')), "Retail", StringComparison.OrdinalIgnoreCase))
                game = Path.GetDirectoryName(game.TrimEnd('\\'));

            if (game.Length > 0 && File.Exists(Path.Combine(game, "Content", "Retail", "HITMAN3.exe")))
            {
                SetResult("This is the Microsoft Store / Game Pass version. Windows locks its files, so no mods can be installed into it. HITMAN Co-op works with Steam and Epic.", Theme.Red);
                return;
            }

            if (game.Length == 0 || !File.Exists(Path.Combine(game, "Retail", "HITMAN3.exe")))
            {
                SetResult("That folder doesn't contain Retail\\HITMAN3.exe. Pick the main game folder.", Theme.Red);
                return;
            }

            // Program Files installs may need administrator rights; ask once, then restart elevated.
            if (!CanWrite(Path.Combine(game, "Retail")) && !IsAdmin())
            {
                if (MessageBox.Show(this, "Windows needs administrator permission to change files in:\n\n" + game +
                        "\n\nRestart the setup as administrator?", "HITMAN Co-op Setup", MessageBoxButtons.YesNo,
                        MessageBoxIcon.Question) == DialogResult.Yes)
                {
                    try
                    {
                        Process.Start(new ProcessStartInfo(Application.ExecutablePath, "--game \"" + game + "\"") { Verb = "runas", UseShellExecute = true });
                        Close();
                    }
                    catch { SetResult("Administrator permission wasn't given, so nothing was changed.", Theme.Red); }
                }
                return;
            }

            SetBusy(true);
            m_Log.Clear();
            SetResult(uninstall ? "Removing…" : "Installing…", Theme.Dim);

            string args = "-Quiet -GamePath \"" + game + "\"" +
                (uninstall ? " -Uninstall -NoFirewall" : (m_Firewall.Checked ? " -Firewall" : " -NoFirewall"));

            var worker = new System.ComponentModel.BackgroundWorker();
            int code = -1;
            string output = "";

            worker.DoWork += delegate { code = RunInstaller(args, out output, Log); };
            worker.RunWorkerCompleted += delegate
            {
                SetBusy(false);

                if (code == 0 && uninstall)
                    SetResult("✔  HITMAN Co-op was removed.", Theme.Good);
                else if (code == 0)
                    SetResult("✔  Installed!  Start HITMAN, press ~ and click CO-OP to host or join.", Theme.Good);
                else
                {
                    string reason = "Something went wrong. See the details below.";
                    foreach (string line in output.Split('\n'))
                        if (line.StartsWith("ERROR:")) reason = line.Substring(6).Trim();
                    SetResult("✖  " + reason, Theme.Red);
                }
            };
            worker.RunWorkerAsync();
        }

        void TestConnection()
        {
            string exe = m_PayloadDir == null ? null : Path.Combine(m_PayloadDir, "Test-Connection.exe");

            if (exe == null || !File.Exists(exe))
            {
                SetResult("The connection test isn't available in this setup file.", Theme.Red);
                return;
            }

            Process.Start(new ProcessStartInfo(exe) { UseShellExecute = true, WorkingDirectory = m_PayloadDir });
        }
    }
}
