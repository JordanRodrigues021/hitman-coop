// HITMAN Co-op setup: the install logic (no window). Created by Jordan Rodrigues.
//
// Everything here is done by the setup itself, in C#: finding the game, copying the files with a
// backup of anything they replace, turning the mod on, the firewall rule (through the Windows
// Firewall API, not a command line), the "Apps" entry and the uninstall. Earlier setups ran a
// PowerShell script hidden with "-ExecutionPolicy Bypass", which antivirus products flag as the
// pattern of a dropper: players saw the setup blocked or deleted as a "trojan".
//
// Compiled by tools/package.ps1 with the C# 5 compiler that ships with Windows (.NET Framework 4):
// no string interpolation, no "?.", no expression-bodied members.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Web.Script.Serialization;
using Microsoft.Win32;

namespace HitmanCoopSetup
{
    // A failure the player can act on: Message says what happened, Hint what to do about it.
    class SetupException : Exception
    {
        public readonly string Hint;
        public readonly bool OfferSecurity; // a file vanished: most likely an antivirus took it

        public SetupException(string message, string hint = null, bool offerSecurity = false) : base(message)
        {
            Hint = hint;
            OfferSecurity = offerSecurity;
        }
    }

    class GameInstall
    {
        public string Path;
        public string Store = "Unknown store";
        public string Version = "unknown";
        public string InstalledModVersion; // null: HITMAN Co-op isn't in it

        public string Build
        {
            get { return Version.StartsWith("3.280.") ? "3.280" : "3.290"; }
        }

        public string Retail
        {
            get { return System.IO.Path.Combine(Path, "Retail"); }
        }

        public override string ToString()
        {
            return Path;
        }
    }

    static class Util
    {
        public static string Sha256File(string path)
        {
            using (var sha = SHA256.Create())
            using (var f = File.OpenRead(path))
                return BitConverter.ToString(sha.ComputeHash(f)).Replace("-", "").ToLowerInvariant();
        }

        public static bool IsAdmin()
        {
            try { return new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator); }
            catch { return false; }
        }

        public static bool CanWrite(string dir)
        {
            try
            {
                string probe = Path.Combine(dir, ".hitmancoop-write-test-" + Guid.NewGuid().ToString("N").Substring(0, 6));
                File.WriteAllText(probe, "x");
                File.Delete(probe);
                return true;
            }
            catch { return false; }
        }

        public static string Quote(string arg)
        {
            return "\"" + arg.Replace("\"", "") + "\"";
        }

        // A file copy that rides out a brief lock (an antivirus scanning the file it just saw).
        public static void CopyWithRetry(string from, string to)
        {
            for (int attempt = 0; ; attempt++)
            {
                try
                {
                    File.Copy(from, to, true);
                    return;
                }
                catch (IOException)
                {
                    if (attempt >= 4) throw;
                    Thread.Sleep(250 * (attempt + 1));
                }
                catch (UnauthorizedAccessException)
                {
                    if (attempt >= 2) throw;
                    // Read-only (copied from a read-only medium): clear it and go again.
                    try { if (File.Exists(to)) File.SetAttributes(to, FileAttributes.Normal); } catch { }
                    Thread.Sleep(200);
                }
            }
        }

        public static string RelativeTo(string root, string full)
        {
            string r = root.TrimEnd('\\') + "\\";
            return full.StartsWith(r, StringComparison.OrdinalIgnoreCase) ? full.Substring(r.Length) : full;
        }

        public static string FileVersion(string path)
        {
            try
            {
                if (!File.Exists(path)) return null;
                var info = FileVersionInfo.GetVersionInfo(path);
                if (info.FileMajorPart != 0 || info.FileMinorPart != 0)
                    return info.FileMajorPart + "." + info.FileMinorPart + "." + info.FileBuildPart;
                return string.IsNullOrEmpty(info.ProductVersion) ? null : info.ProductVersion.Trim();
            }
            catch { return null; }
        }

        public static Version FullFileVersion(string path)
        {
            try
            {
                if (!File.Exists(path)) return null;
                var info = FileVersionInfo.GetVersionInfo(path);
                return new Version(info.FileMajorPart, info.FileMinorPart, info.FileBuildPart, info.FilePrivatePart);
            }
            catch { return null; }
        }

        public static object Json(string text)
        {
            var js = new JavaScriptSerializer();
            js.MaxJsonLength = 64 * 1024 * 1024;
            return js.DeserializeObject(text);
        }

        public static string ToJson(object value)
        {
            return new JavaScriptSerializer().Serialize(value);
        }

        public static IEnumerable<string> Strings(object list)
        {
            var a = list as object[];
            if (a == null)
            {
                var s = list as string;
                if (s != null) yield return s;
                yield break;
            }
            foreach (object o in a)
                if (o is string) yield return (string)o;
        }

        public static object Get(object dict, string key)
        {
            var d = dict as Dictionary<string, object>;
            object v;
            return d != null && d.TryGetValue(key, out v) ? v : null;
        }
    }

    // The files to install: next to the setup (the zip download), or inside it (the one-file setup).
    static class Payload
    {
        public static string Dir;
        public static bool Embedded;
        static string s_Temp;

        public static bool IsPayloadDir(string dir)
        {
            return File.Exists(Path.Combine(dir, "files", "mods", "HitmanCoop.dll"));
        }

        public static void Prepare()
        {
            if (Dir != null) return;

            // Earlier setups unpacked into %TEMP%\HitmanCoopSetup-<version> and never cleaned up
            // (up to 24 MB each): anything of theirs over an hour old goes.
            try
            {
                foreach (string old in Directory.GetDirectories(Path.GetTempPath(), "HitmanCoopSetup-*"))
                    if (Directory.GetLastWriteTimeUtc(old) < DateTime.UtcNow.AddHours(-1))
                        try { Directory.Delete(old, true); } catch { }
            }
            catch { }

            string exeDir = AppDomain.CurrentDomain.BaseDirectory;

            if (IsPayloadDir(exeDir))
            {
                Dir = exeDir;
                Verify();
                return;
            }

            using (Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip"))
            {
                if (s == null)
                    throw new SetupException("The setup's files are missing.",
                        "If you downloaded the zip, extract all of it (right-click it, Extract All...) and run HitmanCoop-Setup.exe from the extracted folder.");

                s_Temp = Path.Combine(Path.GetTempPath(), "HitmanCoopSetup-" + Program.Version + "-" + Guid.NewGuid().ToString("N").Substring(0, 8));
                Directory.CreateDirectory(s_Temp);
                string zip = Path.Combine(s_Temp, "payload.zip");

                using (var f = File.Create(zip))
                    s.CopyTo(f);

                ZipFile.ExtractToDirectory(zip, s_Temp);
                File.Delete(zip);
                Dir = s_Temp;
                Embedded = true;
            }

            Verify();
        }

        // Each file against checksums.txt (made by the packaging): an antivirus that took a file out
        // of the extracted download, or a broken extraction, is caught here and named.
        static void Verify()
        {
            string list = Path.Combine(Dir, "checksums.txt");

            if (!File.Exists(list))
                return;

            foreach (string line in File.ReadAllLines(list))
            {
                int space = line.IndexOf(' ');
                if (space != 64) continue;
                string hash = line.Substring(0, 64);
                string rel = line.Substring(space + 1).Trim().TrimStart('*').Replace('/', '\\');
                string path = Path.Combine(Dir, rel);

                if (!File.Exists(path))
                    throw new SetupException("A file of the download is missing: " + rel + ".",
                        "Your antivirus may have removed it, or the download was only partly extracted. Allow HITMAN Co-op in Windows Security (Protection history), then download it again.", true);

                if (!string.Equals(Util.Sha256File(path), hash, StringComparison.OrdinalIgnoreCase))
                    throw new SetupException("A file of the download is damaged: " + rel + ".", "Download HITMAN Co-op again.");
            }
        }

        public static string FilesFor(string build)
        {
            string older = Path.Combine(Dir, "files-3.280");
            if (build == "3.280" && File.Exists(Path.Combine(older, "mods", "HitmanCoop.dll"))) return older;
            return Path.Combine(Dir, "files");
        }

        public static bool HasBuild(string build)
        {
            return build == "3.290" || File.Exists(Path.Combine(Dir, "files-3.280", "mods", "HitmanCoop.dll"));
        }

        public static void CleanUp()
        {
            try { if (s_Temp != null && Directory.Exists(s_Temp)) Directory.Delete(s_Temp, true); }
            catch { }
        }
    }

    static class Games
    {
        public const string ModName = "HitmanCoop";
        public const string ManifestName = "HitmanCoop-install.json";

        public static bool IsGameDir(string dir)
        {
            return !string.IsNullOrEmpty(dir) && File.Exists(Path.Combine(dir, "Retail", "HITMAN3.exe"));
        }

        public static bool IsStoreBuild(string dir)
        {
            return !string.IsNullOrEmpty(dir) && File.Exists(Path.Combine(dir, "Content", "Retail", "HITMAN3.exe"));
        }

        // "D:\Games\HITMAN 3\Retail" or the exe itself: the game folder above it.
        public static string Normalize(string dir)
        {
            if (string.IsNullOrEmpty(dir)) return dir;
            dir = dir.Trim().Trim('"');
            if (File.Exists(dir)) dir = Path.GetDirectoryName(dir);
            dir = dir.TrimEnd('\\', '/');
            if (string.Equals(Path.GetFileName(dir), "Retail", StringComparison.OrdinalIgnoreCase)) dir = Path.GetDirectoryName(dir);
            return dir;
        }

        public static GameInstall Inspect(string dir)
        {
            dir = Normalize(dir);
            var g = new GameInstall { Path = Path.GetFullPath(dir) };
            string exe = Path.Combine(g.Path, "Retail", "HITMAN3.exe");
            g.Version = Util.FileVersion(exe) ?? "unknown";

            if (g.Path.IndexOf("\\steamapps\\", StringComparison.OrdinalIgnoreCase) >= 0) g.Store = "Steam";
            else if (Directory.Exists(Path.Combine(g.Path, ".egstore"))) g.Store = "Epic Games";

            string manifest = Path.Combine(g.Retail, ManifestName);
            if (File.Exists(manifest) || File.Exists(Path.Combine(g.Retail, "mods", ModName + ".dll")))
            {
                g.InstalledModVersion = "installed";
                try
                {
                    object v = Util.Get(Util.Json(File.ReadAllText(manifest)), "version");
                    if (v is string) g.InstalledModVersion = (string)v;
                }
                catch { }
            }

            return g;
        }

        public static List<GameInstall> FindAll()
        {
            var paths = new List<string>();
            paths.AddRange(Steam());
            paths.AddRange(Epic());
            paths.AddRange(Legendary());

            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var found = new List<GameInstall>();

            foreach (string p in paths)
            {
                if (!IsGameDir(p)) continue;
                string key;
                try { key = Path.GetFullPath(p).TrimEnd('\\'); }
                catch { continue; }
                if (seen.Add(key)) found.Add(Inspect(key));
            }

            return found;
        }

        public static List<string> StoreBuilds()
        {
            var found = new List<string>();
            foreach (var drive in DriveInfo.GetDrives())
            {
                try
                {
                    if (drive.DriveType != DriveType.Fixed || !drive.IsReady) continue;
                    string root = Path.Combine(drive.RootDirectory.FullName, "XboxGames");
                    if (!Directory.Exists(root)) continue;
                    foreach (string d in Directory.GetDirectories(root, "HITMAN*"))
                        if (IsStoreBuild(d)) found.Add(d);
                }
                catch { }
            }
            return found;
        }

        static IEnumerable<string> Steam()
        {
            var roots = new List<string>();
            foreach (var key in new[] { new[] { "HKCU", @"Software\Valve\Steam", "SteamPath" }, new[] { "HKLM", @"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath" },
                                        new[] { "HKLM", @"SOFTWARE\Valve\Steam", "InstallPath" } })
            {
                try
                {
                    RegistryKey hive = key[0] == "HKCU" ? Registry.CurrentUser : Registry.LocalMachine;
                    using (RegistryKey k = hive.OpenSubKey(key[1]))
                    {
                        string v = k == null ? null : k.GetValue(key[2]) as string;
                        if (!string.IsNullOrEmpty(v)) roots.Add(v.Replace('/', '\\'));
                    }
                }
                catch { }
            }

            var result = new List<string>();
            foreach (string root in roots)
            {
                string vdf = Path.Combine(root, "steamapps", "libraryfolders.vdf");
                try
                {
                    if (File.Exists(vdf))
                        foreach (Match m in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s+\"([^\"]+)\""))
                            result.Add(Path.Combine(m.Groups[1].Value.Replace("\\\\", "\\"), "steamapps", "common", "HITMAN 3"));
                }
                catch { }
                result.Add(Path.Combine(root, "steamapps", "common", "HITMAN 3"));
            }
            return result;
        }

        static IEnumerable<string> Epic()
        {
            var result = new List<string>();
            string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "Epic", "EpicGamesLauncher", "Data", "Manifests");
            try
            {
                if (!Directory.Exists(dir)) return result;
                foreach (string item in Directory.GetFiles(dir, "*.item"))
                {
                    try
                    {
                        object json = Util.Json(File.ReadAllText(item));
                        string name = Util.Get(json, "DisplayName") as string;
                        string at = Util.Get(json, "InstallLocation") as string;
                        if (name != null && name.StartsWith("HITMAN", StringComparison.OrdinalIgnoreCase) && at != null) result.Add(at);
                    }
                    catch { }
                }
            }
            catch { }
            return result;
        }

        // Epic installs made with Legendary or Heroic.
        static IEnumerable<string> Legendary()
        {
            var result = new List<string>();
            string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            string roaming = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
            foreach (string file in new[] { Path.Combine(home, ".config", "legendary", "installed.json"), Path.Combine(roaming, "heroic", "legendaryConfig", "legendary", "installed.json") })
            {
                try
                {
                    if (!File.Exists(file)) continue;
                    var apps = Util.Json(File.ReadAllText(file)) as Dictionary<string, object>;
                    if (apps == null) continue;
                    foreach (var app in apps.Values)
                    {
                        string title = Util.Get(app, "title") as string;
                        string at = Util.Get(app, "install_path") as string;
                        if (title != null && title.StartsWith("HITMAN", StringComparison.OrdinalIgnoreCase) && at != null) result.Add(at);
                    }
                }
                catch { }
            }
            return result;
        }

        public static bool GameRunning()
        {
            try { return Process.GetProcessesByName("HITMAN3").Length > 0; }
            catch { return false; }
        }
    }

    // What an install did, kept in Retail\HitmanCoop-install.json so the uninstall can undo it.
    // The same file the PowerShell and Linux installers write (they read and keep these fields).
    class InstallManifest
    {
        public string Installed;
        public string Version;
        public string Build;
        public List<string> Added = new List<string>();
        public List<string> Backups = new List<string>();
        public Dictionary<string, string> Files = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        // [sdk] lines this setup added to mods.ini (taken out again on uninstall); "[sdk]" itself if
        // the section was ours too.
        public List<string> SdkLines = new List<string>();

        public static InstallManifest Load(string retail)
        {
            string path = Path.Combine(retail, Games.ManifestName);
            if (!File.Exists(path)) return null;

            try
            {
                object json = Util.Json(File.ReadAllText(path));
                var m = new InstallManifest();
                m.Installed = Util.Get(json, "installed") as string;
                m.Version = Util.Get(json, "version") as string;
                m.Build = Util.Get(json, "build") as string;
                m.Added.AddRange(Util.Strings(Util.Get(json, "added")));
                m.Backups.AddRange(Util.Strings(Util.Get(json, "backups")).Where(b => !string.IsNullOrEmpty(b)));
                m.SdkLines.AddRange(Util.Strings(Util.Get(json, "sdk")));
                var files = Util.Get(json, "files") as Dictionary<string, object>;
                if (files != null)
                    foreach (var kv in files)
                        if (kv.Value is string) m.Files[kv.Key] = (string)kv.Value;
                return m;
            }
            catch
            {
                return null;
            }
        }

        public void Save(string retail)
        {
            var d = new Dictionary<string, object>();
            d["installed"] = Installed;
            d["version"] = Version;
            d["build"] = Build;
            d["added"] = Added.Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
            d["backups"] = Backups.Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
            d["files"] = Files;
            d["sdk"] = SdkLines.Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
            File.WriteAllText(Path.Combine(retail, Games.ManifestName), Util.ToJson(d), new UTF8Encoding(false));
        }

        // A file in the game folder that an earlier install of ours put there (so not the player's).
        public bool Owns(string retail, string rel)
        {
            if (Files.ContainsKey(rel) || Added.Contains(rel, StringComparer.OrdinalIgnoreCase)) return true;
            // Older manifests (0.13.x) list only added files and backup folders: a file with a backup
            // was replaced by us then, so what's there now is ours.
            foreach (string b in Backups)
                if (File.Exists(Path.Combine(retail, b, rel))) return true;
            return false;
        }
    }

    static class Installer
    {
        public delegate void Progress(int percent, string step);

        public static void Install(GameInstall game, Progress progress, Action<string> log)
        {
            if (!Games.IsGameDir(game.Path)) throw new SetupException("There's no Retail\\HITMAN3.exe in " + game.Path + ".");
            if (Games.GameRunning()) throw new SetupException("HITMAN is running.", "Close the game, then try again.");

            string retail = game.Retail;
            string payload = Payload.FilesFor(game.Build);
            string build = payload.EndsWith("files-3.280", StringComparison.OrdinalIgnoreCase) ? "3.280" : "3.290";

            log("Game: HITMAN World of Assassination " + game.Version + " (" + game.Store + ") in " + game.Path);
            log("Installing the HITMAN Co-op " + Program.Version + " build for game " + build + ".x.");

            if (!game.Version.StartsWith(build + "."))
                log("Note: there's no HITMAN Co-op build for game " + game.Version + "; the " + build + " one is installed. Co-op stays off in the game (it says so) until the game is updated.");

            progress(5, "Checking your game");

            Version theirs = Util.FullFileVersion(Path.Combine(retail, "ZHMModSDK.dll"));
            Version ours = Util.FullFileVersion(Path.Combine(payload, "ZHMModSDK.dll"));
            if (theirs != null && ours != null && theirs != ours)
                log("Found mod loader (ZHMModSDK) " + theirs + "; HITMAN Co-op needs " + ours + ". The old one is backed up and comes back if you uninstall.");

            InstallManifest previous = InstallManifest.Load(retail);
            string stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss");
            string backupRel = "HitmanCoop-backup\\" + stamp;
            var manifest = new InstallManifest { Installed = stamp, Version = Program.Version, Build = build };
            if (previous != null)
            {
                manifest.Added.AddRange(previous.Added);
                manifest.Backups.AddRange(previous.Backups);
                manifest.SdkLines.AddRange(previous.SdkLines);
            }

            string[] files = Directory.GetFiles(payload, "*", SearchOption.AllDirectories);
            bool backedUp = false;

            for (int i = 0; i < files.Length; i++)
            {
                string rel = Util.RelativeTo(payload, files[i]);
                string dest = Path.Combine(retail, rel);
                progress(10 + 70 * i / Math.Max(1, files.Length), "Copying " + Path.GetFileName(rel));
                Directory.CreateDirectory(Path.GetDirectoryName(dest));
                string hash = Util.Sha256File(files[i]);
                manifest.Files[rel] = hash;

                if (File.Exists(dest))
                {
                    if (string.Equals(Util.Sha256File(dest), hash, StringComparison.OrdinalIgnoreCase))
                        continue;

                    // The player's own file (another mod loader, say): kept to put back on uninstall.
                    // One of ours from an earlier version is just replaced: backing that up made the
                    // uninstall after an update bring back the old version's loader.
                    if (previous == null || !previous.Owns(retail, rel))
                    {
                        string backup = Path.Combine(retail, backupRel, rel);
                        Directory.CreateDirectory(Path.GetDirectoryName(backup));
                        File.Copy(dest, backup, true);
                        backedUp = true;
                    }
                }
                else if (previous == null || !previous.Owns(retail, rel))
                {
                    manifest.Added.Add(rel);
                }

                Util.CopyWithRetry(files[i], dest);
            }

            if (backedUp) manifest.Backups.Add(backupRel);

            progress(85, "Turning the mod on");
            EnableInIni(Path.Combine(retail, "mods.ini"));
            SdkDefaults(Path.Combine(retail, "mods.ini"), manifest, log);

            progress(90, "Checking the files");
            // Everything really there: an antivirus can quietly take dinput8.dll (the mod loader).
            Thread.Sleep(400);
            foreach (string file in files)
            {
                string rel = Util.RelativeTo(payload, file);
                string dest = Path.Combine(retail, rel);
                if (!File.Exists(dest))
                    throw new SetupException(Path.GetFileName(rel) + " was removed from the game folder right after it was copied.",
                        "Your antivirus took it (it is part of the ZHMModSDK mod loader, a false alarm). Allow it in Windows Security > Protection history, then install again.", true);
                if (!string.Equals(Util.Sha256File(dest), manifest.Files[rel], StringComparison.OrdinalIgnoreCase))
                    throw new SetupException("Couldn't write " + rel + " into the game folder.", "Close anything that uses the game's files and try again.");
            }

            if (!IniHasMod(Path.Combine(retail, "mods.ini")))
                throw new SetupException("Couldn't turn the mod on in Retail\\mods.ini.");

            manifest.Save(retail);
            progress(100, "Installed");
            log("Installed HITMAN Co-op into " + game.Path);
        }

        public static void Uninstall(GameInstall game, bool keepSettings, Action<string> log)
        {
            if (!Games.IsGameDir(game.Path)) throw new SetupException("There's no Retail\\HITMAN3.exe in " + game.Path + ".");
            if (Games.GameRunning()) throw new SetupException("HITMAN is running.", "Close the game, then try again.");

            string retail = game.Retail;
            string ini = Path.Combine(retail, "mods.ini");
            DisableInIni(ini);
            TryDelete(Path.Combine(retail, "mods", Games.ModName + ".dll"));

            var others = new List<string>();
            if (File.Exists(ini))
                foreach (string line in File.ReadAllLines(ini))
                {
                    string t = line.Trim();
                    if (t.StartsWith("[") && t.EndsWith("]") && !t.Equals("[sdk]", StringComparison.OrdinalIgnoreCase)) others.Add(t);
                }

            InstallManifest m = InstallManifest.Load(retail);
            if (m != null)
                RemoveSdkLines(ini, m.SdkLines);

            if (m != null)
            {
                if (File.Exists(ini) && File.ReadAllText(ini).Trim().Length == 0) TryDelete(ini);

                bool hadLoader = m.Backups.Count > 0;
                if (others.Count > 0 && !hadLoader)
                    log("Other SDK mods are on, so the mod loader stays.");
                else
                    foreach (string rel in m.Added) TryDelete(Path.Combine(retail, rel));

                // Every backup back, oldest first, so the newest of the player's own files wins.
                foreach (string b in m.Backups.OrderBy(b => b, StringComparer.Ordinal))
                {
                    string src = Path.Combine(retail, b);
                    if (!Directory.Exists(src)) continue;
                    foreach (string file in Directory.GetFiles(src, "*", SearchOption.AllDirectories))
                    {
                        string dest = Path.Combine(retail, Util.RelativeTo(src, file));
                        Directory.CreateDirectory(Path.GetDirectoryName(dest));
                        File.Copy(file, dest, true);
                    }
                }

                TryDelete(Path.Combine(retail, Games.ManifestName));
                try { if (Directory.Exists(Path.Combine(retail, "HitmanCoop-backup"))) Directory.Delete(Path.Combine(retail, "HitmanCoop-backup"), true); }
                catch { }
            }

            // What the mod wrote while it ran: logs, crash notes, the updater's leftovers.
            string mods = Path.Combine(retail, "mods");
            foreach (string name in new[] { "HitmanCoop.log", "HitmanCoop-previous.log", "HitmanCoop-crash.txt", "HitmanCoop-crash-previous.txt", "HitmanCoop-whatsnew.txt" })
                TryDelete(Path.Combine(mods, name));
            if (!keepSettings) TryDelete(Path.Combine(mods, "HitmanCoop.ini"));
            try
            {
                if (Directory.Exists(retail))
                    foreach (string old in Directory.GetFiles(retail, "*.hcold", SearchOption.AllDirectories)) TryDelete(old);
            }
            catch { }

            TryDelete(Path.Combine(retail, "HitmanCoop-install.log"));
            try { if (Directory.Exists(mods) && !Directory.EnumerateFileSystemEntries(mods).Any()) Directory.Delete(mods); }
            catch { }

            log("Removed HITMAN Co-op from " + game.Path);
        }

        static void TryDelete(string path)
        {
            try { if (File.Exists(path)) File.Delete(path); }
            catch { }
        }

        static bool IniHasMod(string ini)
        {
            return File.Exists(ini) && File.ReadAllLines(ini).Any(l => l.Trim().Equals("[" + Games.ModName + "]", StringComparison.OrdinalIgnoreCase));
        }

        static void EnableInIni(string ini)
        {
            if (IniHasMod(ini)) return;
            var lines = File.Exists(ini) ? File.ReadAllLines(ini).ToList() : new List<string>();
            lines.Add("[" + Games.ModName + "]");
            File.WriteAllLines(ini, lines.ToArray(), Encoding.ASCII);
        }

        // The mod loader's own questions, answered before it asks: on a first start it opened a
        // "send crash reports?" box over the game's menu (the game sat paused under it), and its
        // "new SDK version" notice led players to update the loader, which breaks HITMAN Co-op (it is
        // built for this one). Only keys the player hasn't set; the uninstall takes them out again.
        static readonly string[] s_SdkDefaults = { "crash_reporting = false", "disable_update_check = true" };

        static void SdkDefaults(string ini, InstallManifest manifest, Action<string> log)
        {
            var lines = File.Exists(ini) ? File.ReadAllLines(ini).ToList() : new List<string>();
            int header = lines.FindIndex(l => l.Trim().Equals("[sdk]", StringComparison.OrdinalIgnoreCase));
            if (header < 0)
            {
                lines.Insert(0, "[sdk]");
                header = 0;
                manifest.SdkLines.Add("[sdk]");
            }

            int end = header + 1;
            while (end < lines.Count && !lines[end].Trim().StartsWith("[")) end++;

            foreach (string line in s_SdkDefaults)
            {
                string key = line.Substring(0, line.IndexOf('=')).Trim();
                bool set = false;
                for (int i = header + 1; i < end; i++)
                {
                    int eq = lines[i].IndexOf('=');
                    if (eq > 0 && lines[i].Substring(0, eq).Trim().Equals(key, StringComparison.OrdinalIgnoreCase)) set = true;
                }
                if (set) continue;
                lines.Insert(header + 1, line);
                end++;
                manifest.SdkLines.Add(line);
                log("Set " + line + " for the mod loader (in mods.ini).");
            }

            File.WriteAllLines(ini, lines.ToArray(), Encoding.ASCII);
        }

        static void RemoveSdkLines(string ini, List<string> ours)
        {
            if (!File.Exists(ini) || ours.Count == 0) return;
            var lines = File.ReadAllLines(ini).ToList();
            bool section = false;
            var kept = new List<string>();
            foreach (string line in lines)
            {
                string t = line.Trim();
                if (t.StartsWith("[")) section = t.Equals("[sdk]", StringComparison.OrdinalIgnoreCase);
                if (section && !t.StartsWith("[") && ours.Any(o => o.Equals(t, StringComparison.OrdinalIgnoreCase))) continue;
                kept.Add(line);
            }

            // Our own [sdk] header, now with nothing under it: gone too.
            if (ours.Any(o => o.Equals("[sdk]", StringComparison.OrdinalIgnoreCase)))
            {
                int h = kept.FindIndex(l => l.Trim().Equals("[sdk]", StringComparison.OrdinalIgnoreCase));
                if (h >= 0 && (h + 1 >= kept.Count || kept[h + 1].Trim().StartsWith("["))) kept.RemoveAt(h);
            }

            File.WriteAllLines(ini, kept.ToArray(), Encoding.ASCII);
        }

        static void DisableInIni(string ini)
        {
            if (!File.Exists(ini)) return;
            var kept = new List<string>();
            bool skipping = false;
            foreach (string line in File.ReadAllLines(ini))
            {
                string t = line.Trim();
                if (t.StartsWith("[")) skipping = t.Equals("[" + Games.ModName + "]", StringComparison.OrdinalIgnoreCase);
                if (!skipping) kept.Add(line);
            }
            File.WriteAllLines(ini, kept.ToArray(), Encoding.ASCII);
        }
    }

    // The Windows Firewall rule for hosting by IP address, through the firewall's own API (needs
    // admin: the setup asks Windows for it for just this step). Replaced, never added twice.
    static class Firewall
    {
        public const string RuleName = "HITMAN Co-op (UDP in)";

        public static bool AddRule(string exe)
        {
            try
            {
                Type policyType = Type.GetTypeFromProgID("HNetCfg.FwPolicy2");
                Type ruleType = Type.GetTypeFromProgID("HNetCfg.FWRule");
                if (policyType == null || ruleType == null) return false;

                object policy = Activator.CreateInstance(policyType);
                object rules = policyType.InvokeMember("Rules", BindingFlags.GetProperty, null, policy, null);

                for (int i = 0; i < 8; i++)
                {
                    try { rules.GetType().InvokeMember("Item", BindingFlags.InvokeMethod, null, rules, new object[] { RuleName }); }
                    catch { break; }
                    rules.GetType().InvokeMember("Remove", BindingFlags.InvokeMethod, null, rules, new object[] { RuleName });
                }

                object rule = Activator.CreateInstance(ruleType);
                Set(ruleType, rule, "Name", RuleName);
                Set(ruleType, rule, "Description", "Lets your co-op partner connect to your game when you host by IP address or Tailscale. Added by the HITMAN Co-op setup.");
                Set(ruleType, rule, "ApplicationName", exe);
                Set(ruleType, rule, "Protocol", 17);      // UDP
                Set(ruleType, rule, "Direction", 1);      // in
                Set(ruleType, rule, "Action", 1);         // allow
                Set(ruleType, rule, "Profiles", 0x7FFFFFFF);
                Set(ruleType, rule, "Enabled", true);
                rules.GetType().InvokeMember("Add", BindingFlags.InvokeMethod, null, rules, new object[] { rule });
                return true;
            }
            catch
            {
                return false;
            }
        }

        static void Set(Type t, object o, string name, object value)
        {
            t.InvokeMember(name, BindingFlags.SetProperty, null, o, new object[] { value });
        }

        // From the normal (not admin) setup: Windows asks the player, then a copy of the setup adds it.
        public static bool AddRuleElevated(string exe)
        {
            if (Util.IsAdmin()) return AddRule(exe);
            try
            {
                var psi = new ProcessStartInfo(Program.ExePath, "--firewall-rule " + Util.Quote(exe)) { UseShellExecute = true, Verb = "runas" };
                using (Process p = Process.Start(psi))
                {
                    p.WaitForExit(60000);
                    return p.HasExited && p.ExitCode == 0;
                }
            }
            catch
            {
                return false; // said no to Windows' question
            }
        }
    }

    // "HITMAN Co-op" under Settings > Apps, with Uninstall (and Modify: repair or update) there. For
    // this Windows user only (HKCU), pointing at a copy of this setup in %LOCALAPPDATA%.
    static class AppsEntry
    {
        const string Key = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\HitmanCoop";

        public static string Folder
        {
            get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "HitmanCoop"); }
        }

        public static void Register(GameInstall game)
        {
            try
            {
                Directory.CreateDirectory(Folder);
                string copy = Path.Combine(Folder, "HitmanCoop-Setup.exe");
                if (!string.Equals(Path.GetFullPath(Program.ExePath), Path.GetFullPath(copy), StringComparison.OrdinalIgnoreCase))
                    File.Copy(Program.ExePath, copy, true);

                long kb = 0;
                try { kb = Directory.GetFiles(Payload.FilesFor(game.Build), "*", SearchOption.AllDirectories).Sum(f => new FileInfo(f).Length) / 1024; }
                catch { }

                using (RegistryKey k = Registry.CurrentUser.CreateSubKey(Key))
                {
                    k.SetValue("DisplayName", "HITMAN Co-op");
                    k.SetValue("DisplayVersion", Program.Version);
                    k.SetValue("Publisher", Program.Author);
                    k.SetValue("DisplayIcon", copy + ",0");
                    k.SetValue("InstallLocation", game.Retail);
                    k.SetValue("UninstallString", Util.Quote(copy) + " --uninstall --game " + Util.Quote(game.Path));
                    k.SetValue("QuietUninstallString", Util.Quote(copy) + " --uninstall --silent --game " + Util.Quote(game.Path));
                    k.SetValue("URLInfoAbout", Program.Website);
                    k.SetValue("HelpLink", Program.Discord);
                    k.SetValue("NoModify", 1, RegistryValueKind.DWord);
                    k.SetValue("NoRepair", 1, RegistryValueKind.DWord);
                    k.SetValue("EstimatedSize", (int)Math.Min(int.MaxValue, kb), RegistryValueKind.DWord);
                    k.SetValue("InstallDate", DateTime.Now.ToString("yyyyMMdd"));
                }
            }
            catch
            {
                // Only a convenience: the mod works without it.
            }
        }

        public static void Unregister()
        {
            try { Registry.CurrentUser.DeleteSubKeyTree(Key, false); }
            catch { }
        }

        // The game folder the last install went into (for a repair or uninstall from Apps).
        public static string LastGame()
        {
            try
            {
                using (RegistryKey k = Registry.CurrentUser.OpenSubKey(Key))
                {
                    string retail = k == null ? null : k.GetValue("InstallLocation") as string;
                    return string.IsNullOrEmpty(retail) ? null : Path.GetDirectoryName(retail.TrimEnd('\\'));
                }
            }
            catch { return null; }
        }
    }
}
