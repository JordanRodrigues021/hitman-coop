// HITMAN Co-op setup: Tailscale, the private network that connects two players' PCs directly.
// Created by Jordan Rodrigues.
//
// Downloads Tailscale's official Windows installer from tailscale.com, checks that Windows trusts
// its signature and that it is signed by Tailscale Inc. before running it, then signs the player in
// (Tailscale's own sign-in page in their browser) and reads their Tailscale address.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Runtime.InteropServices;
using System.Security.Cryptography.X509Certificates;
using System.Threading;

namespace HitmanCoopSetup
{
    class TailscalePeer
    {
        public string Name;
        public string Address;
        public string Os;
        public bool Online;
        public bool Shared; // a machine someone shared with this account
    }

    class TailscaleStatus
    {
        public string State = "NotInstalled"; // NotInstalled, ServiceDown, NeedsLogin, Stopped, Starting, Running, ...
        public string AuthUrl;
        public string Address;
        public string Name;
        public string Tailnet;
        public bool UsingExitNode;
        public List<TailscalePeer> Peers = new List<TailscalePeer>();

        public bool Running { get { return State == "Running"; } }
    }

    static class Tailscale
    {
        public const string DownloadUrl = "https://pkgs.tailscale.com/stable/tailscale-setup-latest.exe";
        public const string AdminUrl = "https://login.tailscale.com/admin/machines";
        public const string ShareHelpUrl = "https://tailscale.com/kb/1084/sharing";

        public static string Exe
        {
            get
            {
                foreach (string dir in new[] { Environment.GetEnvironmentVariable("ProgramW6432"), Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles) })
                {
                    if (string.IsNullOrEmpty(dir)) continue;
                    string p = Path.Combine(dir, "Tailscale", "tailscale.exe");
                    if (File.Exists(p)) return p;
                }
                return null;
            }
        }

        public static bool Installed { get { return Exe != null; } }

        static string Run(string args, int timeoutMs, out int exitCode)
        {
            exitCode = -1;
            var psi = new ProcessStartInfo(Exe, args)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            using (Process p = Process.Start(psi))
            {
                var output = new System.Text.StringBuilder();
                p.OutputDataReceived += delegate(object s, DataReceivedEventArgs e) { if (e.Data != null) lock (output) output.AppendLine(e.Data); };
                p.ErrorDataReceived += delegate(object s, DataReceivedEventArgs e) { if (e.Data != null) lock (output) output.AppendLine(e.Data); };
                p.BeginOutputReadLine();
                p.BeginErrorReadLine();
                if (!p.WaitForExit(timeoutMs))
                {
                    try { p.Kill(); } catch { }
                    return output.ToString();
                }
                p.WaitForExit();
                exitCode = p.ExitCode;
                lock (output) return output.ToString();
            }
        }

        public static TailscaleStatus Status()
        {
            var st = new TailscaleStatus();
            if (!Installed) return st;

            string text;
            int code;
            try { text = Run("status --json", 8000, out code); }
            catch { st.State = "ServiceDown"; return st; }

            int start = text.IndexOf('{');
            if (start < 0)
            {
                st.State = "ServiceDown";
                return st;
            }

            object json;
            try { json = Util.Json(text.Substring(start)); }
            catch { st.State = "ServiceDown"; return st; }

            st.State = Util.Get(json, "BackendState") as string ?? "Unknown";
            st.AuthUrl = Util.Get(json, "AuthURL") as string;
            object self = Util.Get(json, "Self");
            st.Name = Util.Get(self, "HostName") as string;
            st.Address = Util.Strings(Util.Get(self, "TailscaleIPs")).FirstOrDefault(a => a.Contains("."));
            st.Tailnet = Util.Get(Util.Get(json, "CurrentTailnet"), "Name") as string;
            st.UsingExitNode = Util.Get(json, "ExitNodeStatus") != null;

            var peers = Util.Get(json, "Peer") as Dictionary<string, object>;
            if (peers != null)
            {
                foreach (object peer in peers.Values)
                {
                    if (Util.Get(peer, "ExitNode") is bool && (bool)Util.Get(peer, "ExitNode")) st.UsingExitNode = true;
                    string os = Util.Get(peer, "OS") as string ?? "";
                    // Phones and tablets aren't co-op partners.
                    if (os == "android" || os == "iOS" || os == "tvOS") continue;
                    st.Peers.Add(new TailscalePeer
                    {
                        Name = Util.Get(peer, "HostName") as string ?? "?",
                        Address = Util.Strings(Util.Get(peer, "TailscaleIPs")).FirstOrDefault(a => a.Contains(".")),
                        Os = os,
                        Online = Util.Get(peer, "Online") is bool && (bool)Util.Get(peer, "Online"),
                        Shared = Util.Get(peer, "ShareeNode") is bool && (bool)Util.Get(peer, "ShareeNode"),
                    });
                }
            }

            st.Peers = st.Peers.Where(p => p.Address != null).OrderByDescending(p => p.Online).ThenBy(p => p.Name).ToList();
            return st;
        }

        // Downloads the official installer into p_Dir; progress 0..100.
        public static string Download(string dir, Action<int> progress)
        {
            ServicePointManager.SecurityProtocol = (SecurityProtocolType)3072 | (SecurityProtocolType)12288; // TLS 1.2 | 1.3
            string file = Path.Combine(dir, "tailscale-setup.exe");
            var request = (HttpWebRequest)WebRequest.Create(DownloadUrl);
            request.UserAgent = "HitmanCoopSetup/" + Program.Version;
            request.AllowAutoRedirect = true;
            request.Timeout = 30000;
            request.ReadWriteTimeout = 30000;

            using (var response = (HttpWebResponse)request.GetResponse())
            using (Stream s = response.GetResponseStream())
            using (FileStream f = File.Create(file))
            {
                long total = response.ContentLength, done = 0;
                var buffer = new byte[81920];
                int n;
                while ((n = s.Read(buffer, 0, buffer.Length)) > 0)
                {
                    f.Write(buffer, 0, n);
                    done += n;
                    if (total > 0) progress((int)(done * 100 / total));
                }
            }

            return file;
        }

        // Windows trusts the file's signature, and it is Tailscale's.
        public static bool IsGenuine(string file, out string why)
        {
            why = null;
            if (Trust.Verify(file) != 0)
            {
                why = "Windows doesn't trust its signature.";
                return false;
            }

            try
            {
                var cert = X509Certificate.CreateFromSignedFile(file);
                if (cert.Subject.IndexOf("Tailscale Inc", StringComparison.OrdinalIgnoreCase) < 0)
                {
                    why = "It isn't signed by Tailscale (" + cert.Subject + ").";
                    return false;
                }
            }
            catch
            {
                why = "It isn't signed.";
                return false;
            }

            return true;
        }

        // Runs Tailscale's installer (Windows asks the player for permission). True when installed.
        public static bool RunInstaller(string file, out string why)
        {
            why = null;
            try
            {
                var psi = new ProcessStartInfo(file, "/passive /norestart") { UseShellExecute = true, Verb = "runas" };
                using (Process p = Process.Start(psi))
                {
                    p.WaitForExit();
                    if (p.ExitCode == 1602 || p.ExitCode == 1223) { why = "The Tailscale install was cancelled."; return false; }
                    if (p.ExitCode != 0 && p.ExitCode != 3010 && p.ExitCode != 1641 && !Installed) { why = "Tailscale's installer stopped (code " + p.ExitCode + ")."; return false; }
                }
            }
            catch (System.ComponentModel.Win32Exception)
            {
                why = "Windows didn't get permission to install Tailscale.";
                return false;
            }

            for (int i = 0; i < 40 && !Installed; i++) Thread.Sleep(250);
            return Installed;
        }

        static Process s_Up;

        // Starts the sign-in: Tailscale's daemon makes a sign-in page (AuthURL in its status).
        public static void StartSignIn()
        {
            if (!Installed) return;
            try
            {
                if (s_Up != null && !s_Up.HasExited) return;
                s_Up = Process.Start(new ProcessStartInfo(Exe, "up") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true });
                s_Up.OutputDataReceived += delegate { };
                s_Up.ErrorDataReceived += delegate(object s, DataReceivedEventArgs e)
                {
                    // Settings made in Tailscale's own window: "up" wants them all repeated. "login" doesn't.
                    if (e.Data != null && e.Data.Contains("requires mentioning all"))
                    {
                        try { Process.Start(new ProcessStartInfo(Exe, "login") { UseShellExecute = false, CreateNoWindow = true }); }
                        catch { }
                    }
                };
                s_Up.BeginOutputReadLine();
                s_Up.BeginErrorReadLine();
            }
            catch { }
        }

        public static void StopSignIn()
        {
            try { if (s_Up != null && !s_Up.HasExited) s_Up.Kill(); }
            catch { }
            s_Up = null;
        }

        public static void Open(string url)
        {
            try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); }
            catch { }
        }
    }

    static class Trust
    {
        static readonly Guid s_Action = new Guid("00AAC56B-CD44-11d0-8CC2-00C04FC295EE"); // WINTRUST_ACTION_GENERIC_VERIFY_V2

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        class FileInfo
        {
            public uint cbStruct = (uint)Marshal.SizeOf(typeof(FileInfo));
            public string pcwszFilePath;
            public IntPtr hFile = IntPtr.Zero;
            public IntPtr pgKnownSubject = IntPtr.Zero;
        }

        [StructLayout(LayoutKind.Sequential)]
        class Data
        {
            public uint cbStruct = (uint)Marshal.SizeOf(typeof(Data));
            public IntPtr pPolicyCallbackData = IntPtr.Zero;
            public IntPtr pSIPClientData = IntPtr.Zero;
            public uint dwUIChoice = 2;          // WTD_UI_NONE
            public uint fdwRevocationChecks = 0; // WTD_REVOKE_NONE
            public uint dwUnionChoice = 1;       // WTD_CHOICE_FILE
            public IntPtr pFile;
            public uint dwStateAction = 0;
            public IntPtr hWVTStateData = IntPtr.Zero;
            public IntPtr pwszURLReference = IntPtr.Zero;
            public uint dwProvFlags = 0x10;      // WTD_REVOCATION_CHECK_NONE
            public uint dwUIContext = 0;
            public IntPtr pSignatureSettings = IntPtr.Zero;
        }

        [DllImport("wintrust.dll", ExactSpelling = true, CharSet = CharSet.Unicode)]
        static extern int WinVerifyTrust(IntPtr hwnd, [MarshalAs(UnmanagedType.LPStruct)] Guid action, Data data);

        public static int Verify(string file)
        {
            var info = new FileInfo { pcwszFilePath = file };
            IntPtr pInfo = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(FileInfo)));
            try
            {
                Marshal.StructureToPtr(info, pInfo, false);
                var data = new Data { pFile = pInfo };
                return WinVerifyTrust(new IntPtr(-1), s_Action, data);
            }
            finally
            {
                Marshal.DestroyStructure(pInfo, typeof(FileInfo));
                Marshal.FreeHGlobal(pInfo);
            }
        }
    }
}
