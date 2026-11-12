// HITMAN Co-op setup: changes to the game's own files.
//
// The game keeps its engine settings in Retail\thumbs.dat, an encrypted text file ([application]
// and [Hitman5] sections of Name=Value lines). The format: a 16 byte magic, the CRC32 of the plain
// text, then the text in XTEA blocks of 8 bytes, zero padded. This reads it, changes single lines
// and writes it back in the same format (an unchanged file comes back byte for byte the same; see
// tools\gamecfg.py for the same in Python).
//
// What the setup changes, and why:
//   [application] OnlineDisableAutoReconnect=0   (the game ships with 1)
//     With 1, losing the connection to IOI's servers for a moment stops the mission with a
//     "connection lost" box until the player answers it; in co-op that freezes both players.
//     With 0 the game reconnects by itself.
//
// The file as it was is kept beside it (thumbs.dat.before-hitmancoop), each changed line's old
// value is written to the install manifest, and uninstalling puts the old values back. A game
// update replaces thumbs.dat; the mod itself then makes the change again (plugin GameTweaks.cpp).

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace HitmanCoopSetup
{
    static class GameFiles
    {
        static readonly byte[] Magic = { 0x22, 0x3D, 0x6F, 0x9A, 0xB3, 0xF8, 0xFE, 0xB6, 0x61, 0xD9, 0xCC, 0x1C, 0x62, 0xDE, 0x83, 0x41 };
        static readonly uint[] Key = { 0x30F95282, 0x1F48C419, 0x295F8548, 0x2A78366D };
        const uint Delta = 0x61C88647;

        public const string ThumbsName = "thumbs.dat";
        public const string BackupName = "thumbs.dat.before-hitmancoop";

        // Section, name, the value co-op wants.
        public static readonly string[][] Tweaks =
        {
            new[] { "application", "OnlineDisableAutoReconnect", "0" },
        };

        static uint[] s_CrcTable;

        public static uint Crc32(byte[] data, int length)
        {
            if (s_CrcTable == null)
            {
                var t = new uint[256];
                for (uint i = 0; i < 256; i++)
                {
                    uint c = i;
                    for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
                    t[i] = c;
                }
                s_CrcTable = t;
            }

            uint crc = 0xFFFFFFFF;
            for (int i = 0; i < length; i++) crc = s_CrcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return ~crc;
        }

        static bool HasMagic(byte[] data)
        {
            if (data == null || data.Length < 28 || (data.Length - 20) % 8 != 0) return false;
            for (int i = 0; i < Magic.Length; i++) if (data[i] != Magic[i]) return false;
            return true;
        }

        // The plain text, or null when the file isn't in the game's format or its checksum is off.
        public static byte[] Decrypt(byte[] data)
        {
            if (!HasMagic(data)) return null;

            uint crc = BitConverter.ToUInt32(data, 16);
            var plain = new byte[data.Length - 20];

            for (int i = 0; i < plain.Length; i += 8)
            {
                uint v0 = BitConverter.ToUInt32(data, 20 + i), v1 = BitConverter.ToUInt32(data, 24 + i);
                uint sum = 0xC6EF3720;
                unchecked
                {
                    for (int r = 0; r < 32; r++)
                    {
                        v1 -= (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + Key[(sum >> 11) & 3]);
                        sum += Delta;
                        v0 -= (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + Key[sum & 3]);
                    }
                }
                Buffer.BlockCopy(BitConverter.GetBytes(v0), 0, plain, i, 4);
                Buffer.BlockCopy(BitConverter.GetBytes(v1), 0, plain, i + 4, 4);
            }

            int length = plain.Length;
            while (length > 0 && plain[length - 1] == 0) length--;
            if (Crc32(plain, length) != crc) return null;

            var text = new byte[length];
            Buffer.BlockCopy(plain, 0, text, 0, length);
            return text;
        }

        public static byte[] Encrypt(byte[] text)
        {
            int padded = (text.Length + 7) / 8 * 8;
            var plain = new byte[padded];
            Buffer.BlockCopy(text, 0, plain, 0, text.Length);
            var data = new byte[20 + padded];
            Buffer.BlockCopy(Magic, 0, data, 0, 16);
            Buffer.BlockCopy(BitConverter.GetBytes(Crc32(text, text.Length)), 0, data, 16, 4);

            for (int i = 0; i < padded; i += 8)
            {
                uint v0 = BitConverter.ToUInt32(plain, i), v1 = BitConverter.ToUInt32(plain, i + 4);
                uint sum = 0;
                unchecked
                {
                    for (int r = 0; r < 32; r++)
                    {
                        v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + Key[sum & 3]);
                        sum -= Delta;
                        v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + Key[(sum >> 11) & 3]);
                    }
                }
                Buffer.BlockCopy(BitConverter.GetBytes(v0), 0, data, 20 + i, 4);
                Buffer.BlockCopy(BitConverter.GetBytes(v1), 0, data, 24 + i, 4);
            }

            return data;
        }

        // Bytes as they are (the file is ASCII with the odd byte above it): one char per byte.
        static readonly Encoding Raw = Encoding.GetEncoding(28591);

        // The value of Name in [section], or null when it isn't there. Comments (#) are not values.
        public static string Get(string text, string section, string name)
        {
            string current = "";
            foreach (string line in text.Split('\n'))
            {
                string t = line.Trim();
                if (t.StartsWith("[") && t.EndsWith("]")) { current = t.Substring(1, t.Length - 2); continue; }
                if (!current.Equals(section, StringComparison.OrdinalIgnoreCase) || t.StartsWith("#")) continue;
                int eq = t.IndexOf('=');
                if (eq > 0 && t.Substring(0, eq).Trim().Equals(name, StringComparison.OrdinalIgnoreCase)) return t.Substring(eq + 1).Trim();
            }
            return null;
        }

        // The text with Name=value in [section]: the line changed where it is, added under the
        // section's heading when it wasn't there, taken out when value is null. Line ends are kept.
        public static string Set(string text, string section, string name, string value)
        {
            var lines = text.Split('\n').ToList();
            string end = lines.Count > 1 && lines[0].EndsWith("\r") ? "\r" : "";
            string current = "";
            int heading = -1;

            for (int i = 0; i < lines.Count; i++)
            {
                string t = lines[i].Trim();
                if (t.StartsWith("[") && t.EndsWith("]"))
                {
                    current = t.Substring(1, t.Length - 2);
                    if (current.Equals(section, StringComparison.OrdinalIgnoreCase)) heading = i;
                    continue;
                }
                if (!current.Equals(section, StringComparison.OrdinalIgnoreCase) || t.StartsWith("#")) continue;
                int eq = t.IndexOf('=');
                if (eq <= 0 || !t.Substring(0, eq).Trim().Equals(name, StringComparison.OrdinalIgnoreCase)) continue;

                if (value == null) lines.RemoveAt(i);
                else lines[i] = name + "=" + value + (lines[i].EndsWith("\r") ? "\r" : "");
                return string.Join("\n", lines);
            }

            if (value == null || heading < 0) return text;
            lines.Insert(heading + 1, name + "=" + value + end);
            return string.Join("\n", lines);
        }

        // "tuned", "as the game ships it", "not readable", "not there": for the log and the pages.
        public static string Describe(string retail)
        {
            string path = Path.Combine(retail, ThumbsName);
            if (!File.Exists(path)) return "not there";
            byte[] text;
            try { text = Decrypt(File.ReadAllBytes(path)); } catch { return "not readable"; }
            if (text == null) return "not readable";
            string s = Raw.GetString(text);
            return Tweaks.All(t => Get(s, t[0], t[1]) == t[2]) ? "tuned" : "as the game ships it";
        }

        // Makes the changes. What each line was before goes into "was" (name -> old value, "" for
        // a line that wasn't there), unless it is known already from an earlier install. Returns
        // false when the file was left alone (not in the game's format: another tool's, or new).
        public static bool Apply(string retail, Dictionary<string, string> was, Action<string> log)
        {
            string path = Path.Combine(retail, ThumbsName);
            if (!File.Exists(path)) { log("Game settings file (thumbs.dat) not found: left as it is."); return false; }

            byte[] original = File.ReadAllBytes(path);
            byte[] plain = Decrypt(original);
            if (plain == null) { log("The game settings file (thumbs.dat) isn't in the format this setup knows: left as it is."); return false; }

            string text = Raw.GetString(plain);
            string changed = text;
            foreach (string[] t in Tweaks)
            {
                string now = Get(changed, t[0], t[1]);
                if (now == t[2]) continue;
                string id = t[0] + "/" + t[1];
                if (!was.ContainsKey(id)) was[id] = now ?? "";
                changed = Set(changed, t[0], t[1], t[2]);
            }

            if (changed == text) { log("Game settings (thumbs.dat) are tuned for co-op already."); return true; }

            byte[] data = Encrypt(Raw.GetBytes(changed));
            // What we are about to write must read back as what we meant, before anything is touched.
            byte[] check = Decrypt(data);
            if (check == null || Raw.GetString(check) != changed) { log("Game settings (thumbs.dat): the new file didn't check out, nothing changed."); return false; }

            // The game's own file, kept. A copy from before an older game version is replaced.
            File.WriteAllBytes(Path.Combine(retail, BackupName), original);
            string temp = path + ".hitmancoop-new";
            File.WriteAllBytes(temp, data);
            File.Copy(temp, path, true);
            File.Delete(temp);
            log("Game settings (thumbs.dat): the game now reconnects to IOI's servers by itself instead of stopping the mission with a \"connection lost\" box. The file as it was: " + BackupName + ".");
            return true;
        }

        // Puts back what Apply changed, line by line (the file may be a newer one than the copy we
        // kept, after a game update, so the copy isn't simply put back).
        public static void Restore(string retail, Dictionary<string, string> was, Action<string> log)
        {
            string path = Path.Combine(retail, ThumbsName);
            string backup = Path.Combine(retail, BackupName);
            if (was == null || was.Count == 0 || !File.Exists(path)) { TryDelete(backup); return; }

            try
            {
                byte[] plain = Decrypt(File.ReadAllBytes(path));
                if (plain == null) { log("Game settings (thumbs.dat) aren't in the format this setup knows any more: left as they are."); return; }

                string text = Raw.GetString(plain), changed = text;
                foreach (var kv in was)
                {
                    string[] id = kv.Key.Split('/');
                    if (id.Length != 2) continue;
                    string wanted = Tweaks.Where(t => t[0] == id[0] && t[1] == id[1]).Select(t => t[2]).FirstOrDefault();
                    // Only a line that still has our value: the player (or a game update) may have changed it since.
                    if (wanted == null || Get(changed, id[0], id[1]) != wanted) continue;
                    changed = Set(changed, id[0], id[1], kv.Value.Length == 0 ? null : kv.Value);
                }

                if (changed != text)
                {
                    byte[] data = Encrypt(Raw.GetBytes(changed));
                    byte[] check = Decrypt(data);
                    if (check != null && Raw.GetString(check) == changed)
                    {
                        File.WriteAllBytes(path, data);
                        log("Game settings (thumbs.dat) put back as they were.");
                    }
                }

                TryDelete(backup);
            }
            catch (Exception e)
            {
                log("Couldn't put the game settings (thumbs.dat) back: " + e.Message + ". The file as it was is " + BackupName + ".");
            }
        }

        static void TryDelete(string path)
        {
            try { if (File.Exists(path)) File.Delete(path); } catch { }
        }
    }
}
