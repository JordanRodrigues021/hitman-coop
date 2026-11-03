#!/usr/bin/env python3
"""HITMAN Co-op in-game tests: drives the real game through the mod's test channel, with the test
bot (HitmanCoop-NetTest.exe bot) as the partner and a local relay, and writes what it saw.

    python -I tools/gametest/gametest.py --game "C:/.../steamapps/common/HITMAN 3" [scenarios...]
        scenarios: smoke (default), npc, restart, netsim, installers; "all" runs every one
        --keep        leave the tested build installed (default: put back what was there)
        --version V   the build to test (default: the one in plugin/Src/CoopText.h), from
                      _dist/HitmanCoop-V (run tools/package.ps1 first)
        --no-shots    no screenshots

Each run gets tools/gametest/results/<time>/: report.md and report.json (every check, passed or
not, with what was seen), HitmanCoop.log, the bot's and the relay's output, the status timeline
(status.jsonl) and screenshots. Steam copies only (it starts the game the way Steam does), and the
game must be closed when it starts. The game's files are backed up first and put back after.

What the scenarios check:
  smoke    the mod loads (version, the build for this game, the log's header, patches applied),
           the title screen comes up, the game closes cleanly, no crash report.
  npc      into Freeform Training, host, the bot joins (through a local relay): in sync, the
           partner's stand-in shows; the bot stands away from 47 so the host hands it NPCs:
           they become puppets here, follow the bot's positions (walking and standing), the
           sync check finds them in place, their AI is skipped; the bot stops sending them:
           they get their own AI back; the bot comes back: they return to the host.
  restart  (after npc) the mission restarted with the bot in it: no crash, in sync again.
  installers  no game needed: fake 3.290 and 3.280 game folders (with another mod already in),
           the Windows installer and, through WSL, the Linux one: the right build goes in, the
           other mod stays, uninstall puts everything back.
  netsim   no game needed (any PC, CI): the bot's host mode and the bot through a local relay that
           adds 60 ms each way, jitter and 3% loss: NPC hand-over, puppets following the bot's
           walking NPCs, the sync check, puppets released when their stream stops, the health line.
"""

import argparse
import datetime
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
STEAM_APP_ID = "1659040"


class Failed(Exception):
    pass


def exe_version(path):
    """The version numbers in a PE file's VS_FIXEDFILEINFO ("3.290.1"), as the mod reads them."""
    with open(path, "rb") as f:
        data = f.read()
    at = data.find(b"\xbd\x04\xef\xfe")
    while at >= 0:
        if at + 16 <= len(data) and struct.unpack_from("<I", data, at + 4)[0] == 0x00010000:
            ms, ls = struct.unpack_from("<II", data, at + 8)
            return f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}"
        at = data.find(b"\xbd\x04\xef\xfe", at + 1)
    return "unknown"


class Report:
    def __init__(self, folder):
        self.folder = folder
        self.checks = []
        self.metrics = {}
        self.notes = []
        self.started = time.time()

    def check(self, name, ok, seen=""):
        self.checks.append({"name": name, "ok": bool(ok), "seen": str(seen)[:600], "at": round(time.time() - self.started, 1)})
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f": {str(seen)[:200]}" if seen != "" else ""), flush=True)
        return bool(ok)

    def note(self, text):
        self.notes.append(text)
        print(f"  - {text}", flush=True)

    def write(self):
        passed = sum(c["ok"] for c in self.checks)
        with open(os.path.join(self.folder, "report.json"), "w", encoding="utf-8") as f:
            json.dump({"passed": passed, "total": len(self.checks), "checks": self.checks, "metrics": self.metrics, "notes": self.notes}, f, indent=2)
        lines = [f"# HITMAN Co-op game test, {datetime.datetime.now():%Y-%m-%d %H:%M}", "", f"**{passed} of {len(self.checks)} checks passed.**", ""]
        lines += ["| | Check | Seen |", "|---|---|---|"]
        for c in self.checks:
            lines.append(f"| {'PASS' if c['ok'] else '**FAIL**'} | {c['name']} | {c['seen'].replace('|', '/')[:300]} |")
        if self.metrics:
            lines += ["", "## Measured", ""] + [f"- {k}: {v}" for k, v in self.metrics.items()]
        if self.notes:
            lines += ["", "## Notes", ""] + [f"- {n}" for n in self.notes]
        with open(os.path.join(self.folder, "report.md"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        return passed, len(self.checks)


class Game:
    """The game, the mod's test channel (a command file and the status JSON) and its log."""

    def __init__(self, folder, results, shots):
        self.folder = folder
        self.retail = os.path.join(folder, "Retail")
        self.mods = os.path.join(self.retail, "mods")
        self.results = results
        self.shots = shots
        self.cmd = os.path.join(results, "coop-cmd.txt")
        self.status_path = self.cmd + ".status"
        self.log_path = os.path.join(self.mods, "HitmanCoop.log")
        self.backup = os.path.join(results, "backup")
        self.backed_up = []
        self.process = None
        self.timeline = open(os.path.join(results, "status.jsonl"), "a", encoding="utf-8")
        self.last_status_line = None

    # --- installing the build under test, and putting things back -----------------------------

    def install(self, stage, log_level="debug"):
        version = exe_version(os.path.join(self.retail, "HITMAN3.exe"))
        payload = os.path.join(stage, "files-3.280" if version.startswith("3.280.") and os.path.isdir(os.path.join(stage, "files-3.280")) else "files")
        if not os.path.isfile(os.path.join(payload, "mods", "HitmanCoop.dll")):
            raise Failed(f"no build in {payload}: run tools/package.ps1 first")
        os.makedirs(self.backup, exist_ok=True)
        keep = [os.path.relpath(os.path.join(d, n), payload) for d, _, files in os.walk(payload) for n in files]
        keep += ["mods.ini", os.path.join("mods", "HitmanCoop.ini")]
        for rel in keep:
            src = os.path.join(self.retail, rel)
            if os.path.exists(src):
                dst = os.path.join(self.backup, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(src, dst)
            self.backed_up.append(rel)
        for d, _, files in os.walk(payload):
            for n in files:
                rel = os.path.relpath(os.path.join(d, n), payload)
                os.makedirs(os.path.dirname(os.path.join(self.retail, rel)), exist_ok=True)
                shutil.copy2(os.path.join(d, n), os.path.join(self.retail, rel))
        ini = os.path.join(self.retail, "mods.ini")
        text = open(ini, encoding="utf-8", errors="replace").read() if os.path.exists(ini) else ""
        if not re.search(r"^\s*\[HitmanCoop\]\s*$", text, re.I | re.M):
            with open(ini, "a", encoding="utf-8") as f:
                f.write(("" if text.endswith("\n") or not text else "\n") + "[HitmanCoop]\n")
        self.set_ini({("general", "test_control"): self.cmd, ("log", "level"): log_level})
        return version, os.path.basename(payload)

    def set_ini(self, values):
        """Sets keys in mods/HitmanCoop.ini, keeping everything else."""
        path = os.path.join(self.mods, "HitmanCoop.ini")
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines() if os.path.exists(path) else []
        for (section, key), value in values.items():
            start = next((i for i, l in enumerate(lines) if l.strip().lower() == f"[{section}]"), None)
            if start is None:
                lines += [f"[{section}]", f"{key} = {value}"]
                continue
            end = next((i for i in range(start + 1, len(lines)) if lines[i].strip().startswith("[")), len(lines))
            at = next((i for i in range(start + 1, end) if lines[i].split("=")[0].strip().lower() == key), None)
            if at is None:
                lines.insert(end, f"{key} = {value}")
            else:
                lines[at] = f"{key} = {value}"
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")

    def restore(self):
        for rel in self.backed_up:
            saved = os.path.join(self.backup, rel)
            live = os.path.join(self.retail, rel)
            if os.path.exists(saved):
                shutil.copy2(saved, live)
            elif os.path.exists(live):
                os.remove(live)  # we added it

    # --- the game process -------------------------------------------------------------------

    def running(self):
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq HITMAN3.exe", "/NH"], capture_output=True, text=True).stdout
        return "HITMAN3.exe" in out

    def launch(self):
        for name in (self.cmd, self.status_path, self.cmd + ".trace", self.cmd + ".crash"):
            if os.path.exists(name):
                os.remove(name)
        env = dict(os.environ, SteamAppId=STEAM_APP_ID, SteamGameId=STEAM_APP_ID)
        self.process = subprocess.Popen([os.path.join(self.retail, "HITMAN3.exe")], cwd=self.retail, env=env)
        self.launched_at = time.time()

    def close(self, timeout=60):
        """Asks the game to close (as the window's X does); forced only if it hangs."""
        if not self.running():
            return "not running"
        subprocess.run(["powershell", "-NoProfile", "-Command",
                        "Get-Process HITMAN3 -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }"], capture_output=True)
        end = time.time() + timeout
        while time.time() < end:
            if not self.running():
                return "closed"
            time.sleep(1)
        subprocess.run(["taskkill", "/F", "/IM", "HITMAN3.exe"], capture_output=True)
        time.sleep(3)
        return "killed (didn't close in time)"

    # --- the test channel -------------------------------------------------------------------

    def status(self):
        for _ in range(10):
            try:
                with open(self.status_path, encoding="utf-8") as f:
                    text = f.read()
                data = json.loads(text)
                if text != self.last_status_line:
                    self.last_status_line = text
                    self.timeline.write(json.dumps({"t": round(time.time() - self.launched_at, 2), "s": data}) + "\n")
                return data
            except (OSError, ValueError):
                time.sleep(0.1)
        return None

    def wait(self, what, test, timeout):
        """Waits until test(status) is true; returns that status. Raises Failed with the last one."""
        end = time.time() + timeout
        last = None
        while time.time() < end:
            if not self.running():
                raise Failed(f"the game closed while waiting for {what}")
            s = self.status()
            if s is not None:
                last = s
                try:
                    if test(s):
                        return s
                except (KeyError, TypeError, IndexError):
                    pass
            time.sleep(0.5)
        raise Failed(f"no {what} in {timeout} s (last status: state {last and last.get('state')}, scene {last and last.get('scene')})")

    def command(self, text, timeout=10):
        """Writes one command; returns once the mod has taken it (it deletes the file)."""
        end = time.time() + timeout
        while os.path.exists(self.cmd) and time.time() < end:
            time.sleep(0.1)
        with open(self.cmd, "w", encoding="utf-8") as f:
            f.write(text + "\n")
        while os.path.exists(self.cmd):
            if time.time() > end:
                raise Failed(f"the mod didn't take the command '{text}'")
            time.sleep(0.1)

    def press(self, action, then=1.5):
        self.command("press " + action)
        time.sleep(then)

    def log(self):
        try:
            with open(self.log_path, encoding="utf-8", errors="replace") as f:
                return f.read().splitlines()
        except OSError:
            return []

    def wait_log(self, pattern, timeout, since=0):
        end = time.time() + timeout
        rx = re.compile(pattern)
        while time.time() < end:
            for line in self.log()[since:]:
                if rx.search(line):
                    return line
            time.sleep(0.5)
        return None

    def shot(self, name):
        if not self.shots:
            return
        out = os.path.join(self.results, name + ".png")
        subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", os.path.join(HERE, "shot.ps1"), "-Out", out,
                        "-Process", "HITMAN3", "-MaxWidth", "1280"], capture_output=True)


class Partner:
    """A local relay and the test bot joining through it."""

    def __init__(self, results, nettest):
        self.results = results
        self.nettest = nettest
        self.cmd = os.path.join(results, "bot-cmd.txt")
        self.out_path = os.path.join(results, "bot.log")
        self.relay = None
        self.bot = None

    def start_relay(self, port=47100):
        self.relay_out = open(os.path.join(self.results, "relay.log"), "w")
        self.relay = subprocess.Popen([sys.executable, "-I", os.path.join(ROOT, "server", "relay.py"), "--bind", "127.0.0.1", "--port", str(port)],
                                      stdout=self.relay_out, stderr=subprocess.STDOUT)
        time.sleep(1.5)
        if self.relay.poll() is not None:
            raise Failed("the relay didn't start (see relay.log)")
        return f"127.0.0.1:{port}"

    def join(self, code, relay):
        self.bot_out = open(self.out_path, "w")
        self.bot = subprocess.Popen([self.nettest, "bot", code, self.cmd, "--relay", relay], stdout=self.bot_out, stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL)

    def send(self, text):
        end = time.time() + 10
        while os.path.exists(self.cmd) and time.time() < end:
            time.sleep(0.1)
        with open(self.cmd, "w", encoding="utf-8") as f:
            f.write(text + "\n")
        time.sleep(0.6)

    def output(self):
        try:
            with open(self.out_path, encoding="utf-8", errors="replace") as f:
                return f.read().splitlines()
        except OSError:
            return []

    def stop(self):
        for p in (self.bot, self.relay):
            if p and p.poll() is None:
                p.terminate()
                try:
                    p.wait(10)
                except subprocess.TimeoutExpired:
                    p.kill()


# --- reading the mod's log ---------------------------------------------------------------------

def last_line(lines, pattern):
    rx = re.compile(pattern)
    for line in reversed(lines):
        if rx.search(line):
            return line
    return None


def numbers(line, pattern):
    m = re.search(pattern, line or "")
    return [float(x) for x in m.groups()] if m else None


# --- the menus ---------------------------------------------------------------------------------

def to_title(g, r):
    s = g.wait("the title screen", lambda s: "frontend/mainmenu" in s.get("scene", "").lower(), 180)
    time.sleep(8)  # the title screen takes input a moment after it shows
    return s


def to_freeform(g, r):
    """From the title screen into ICA Facility, Freeform Training (menus as of game 3.290.1, 2026-11)."""
    g.press("KBMButtonA", 25)   # PRESS ENTER TO PLAY
    g.press("KBMButtonB", 3)    # a popup, if any
    g.press("KBMButtonR1"); g.press("KBMButtonR1", 3)  # Destinations: ICA Facility first
    g.press("KBMButtonA", 4)    # ICA Facility
    g.press("KBMButtonRight"); g.press("KBMButtonRight")  # Freeform Training
    g.press("KBMButtonA", 4)    # spoiler alert
    g.press("KBMButtonA", 5)    # confirm
    g.press("KBMButtonDown")    # Play
    g.press("KBMButtonA", 5)
    g.wait("Freeform Training loaded", lambda s: s.get("inScene") and s.get("location") == "thefacility", 180)
    time.sleep(40)              # the intro
    g.press("KBMButtonA", 4)    # the "Freeform Training" box
    g.shot("freeform")


# --- scenarios -----------------------------------------------------------------------------------

def scenario_smoke(g, r, ctx):
    g.launch()
    s = to_title(g, r)
    r.check("the mod reports this version", s.get("version") == ctx["version"], s.get("version"))
    r.check("it knows the game version", s.get("game") == ctx["game"], s.get("game"))
    log = g.log()
    head = "\n".join(log[:12])
    r.check("the log opens with the build line", f"HitmanCoop build for game {ctx['game'][:5]}" in head, (last_line(log[:12], r"\bstart\b") or "")[:200])
    r.check("the log names Windows and the hardware", re.search(r"Windows \d+\.\d+ build", head) is not None)
    r.check("the log names the mod loader", "mod loader ZHMModSDK.dll" in head, last_line(log[:12], "mod loader"))
    patches = numbers(last_line(log[:15], "game patches:"), r"game patches: (\d+) of (\d+)")
    r.check("every game patch applied", patches is not None and patches[0] == patches[1] and patches[1] > 0, patches)
    r.check("log lines carry date, game time, level and category", re.match(r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3} +[+-]\d+\.\d\d  (INFO|WARN|ERROR|DEBUG|TRACE) ", log[0] if log else "") is not None,
            log[0][:120] if log else "(no log)")
    r.check("the log is at debug level for testing", s.get("log", {}).get("level") == "DEBUG", s.get("log"))
    g.shot("title")


def scenario_npc(g, r, ctx):
    if not g.running():
        g.launch()
        to_title(g, r)
    to_freeform(g, r)
    p = ctx["partner"]
    relay = p.start_relay()
    g.command(f"relay {relay}")
    g.command("host")
    s = g.wait("a join code", lambda s: s.get("relay", {}).get("code"), 30)
    code = s["relay"]["code"]
    r.check("hosting gives a join code", bool(code), code)
    start = len(g.log())
    p.join(code, relay)
    s = g.wait("the bot in sync", lambda s: s.get("synced"), 60)
    r.check("the bot joins and syncs", s.get("synced"), s.get("state"))
    g.wait("the stand-in shown", lambda s: s.get("partnerSpawned"), 30)
    time.sleep(3)
    g.shot("together")
    r.check("the log says they are together", g.wait_log(r"sync +\| together in the mission", 10, start) is not None)

    # Standing together, the host runs every NPC. The AI-skip counter counts the stand-in's too:
    # its rate now (stand-in only) against the rate with puppets says whether theirs is skipped.
    s0, t0 = g.status()["npc"]["skippedThinks"], time.time()
    time.sleep(12)
    s = g.status()
    rate0 = (s["npc"]["skippedThinks"] - s0) / (time.time() - t0)
    r.check("standing together, every NPC is the host's", s["npc"]["puppets"] == 0 and s["npc"]["clientOwns"] == 0, s.get("shared"))
    stats = last_line(g.log(), r"stats +\| together")
    r.check("a health line every 10 s", stats is not None, (stats or "")[:300])
    check = s["npc"]["check"]
    r.check("the sync check ran and found the games agree", check["compared"] > 0 and check["outOfPlace"] == 0, check)

    # The bot stands where a calm NPC is ~25 m from 47: the host hands NPCs near it to the bot.
    spot = numbers(s.get("shared", ""), r"spot (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)")
    if not spot:
        r.check("found a spot with NPCs away from 47", False, s.get("shared"))
        return
    p.send("at {:.1f} {:.1f} {:.1f}".format(*spot))
    s = g.wait("NPCs handed to the bot", lambda s: s["npc"]["clientOwns"] > 0, 30)
    r.check("NPCs near the bot are handed to it", s["npc"]["clientOwns"] > 0, s.get("shared"))
    s = g.wait("puppets", lambda s: s["npc"]["puppets"] > 0, 15)
    r.check("they become puppets here once their positions arrive", s["npc"]["puppets"] > 0, s["npc"])
    skipped0, t1 = s["npc"]["skippedThinks"], time.time()

    p.send("npcwalk 1.2")
    time.sleep(22)
    s = g.status()
    g.shot("puppets-walking")
    rate1 = (s["npc"]["skippedThinks"] - skipped0) / (time.time() - t1)
    r.metrics["AI thinks skipped per second (stand-in only / with puppets)"] = (round(rate0, 1), round(rate1, 1))
    r.check("the puppets' AI is skipped (more skipped with puppets than for the stand-in alone)", rate1 > rate0 * 1.3 + 0.5,
            f"{rate0:.1f}/s before, {rate1:.1f}/s with {s['npc']['puppets']} puppets")
    log = g.log()
    stats = last_line(log, r"stats +\| together")
    drift = numbers(stats, r"puppet drift avg ([\d.]+) m, max ([\d.]+) m")
    r.metrics["puppet drift (walking)"] = drift
    r.check("puppets stay where they are put (drift under 0.3 m)", drift is not None and drift[0] < 0.3, (stats or "")[-250:])
    check = s["npc"]["check"]
    r.metrics["sync check (bot runs NPCs)"] = check
    # The bot walks its NPCs on made-up circles, through walls: the game pushes one back out, and
    # the mod gives that one its own AI ("doesn't stay where it is put"). Those are counted apart.
    stuck = {l.split("| ", 1)[-1].split(" ")[0] for l in g.log()[start:] if "doesn't stay where it is put" in l}
    worst_id = (check.get("worst") or " ").split(" ")[0]
    flagged = check["outOfPlace"] == 1 and worst_id in stuck
    if flagged:
        r.note(f"sync check: {worst_id} out of place, flagged by the mod as not staying put (the bot's path for it goes through the level)")
    r.check("the sync check finds them in place (but NPCs the mod flagged as not staying put)", check["compared"] > 0 and (check["outOfPlace"] == 0 or flagged), check)
    perf = last_line(log, r"perf +\|")
    r.metrics["perf"] = (perf or "")[40:]
    r.check("no 'AI isn't being skipped' warning", "ISN'T BEING SKIPPED" not in (perf or ""), (perf or "")[-200:])

    p.send("npcstill")
    time.sleep(12)
    r.metrics["sync check (standing)"] = g.status()["npc"]["check"]

    # The bot stops sending them: they run their own AI again after 5 s.
    before = g.status()["npc"]["puppets"]
    p.send("npcdrop")
    s = g.wait("puppets released", lambda s: s["npc"]["puppets"] < before, 15)
    r.check("NPCs the partner stops sending get their own AI back", s["npc"]["puppets"] < before, f"{before} -> {s['npc']['puppets']}")
    r.check("the log says why", g.wait_log(r"no positions from the partner for 5 s", 5, start) is not None)
    p.send("npcback")
    s = g.wait("puppets again", lambda s: s["npc"]["puppets"] > 0, 15)
    r.check("they become puppets again when the positions come back", s["npc"]["puppets"] > 0, s["npc"])

    # The bot comes back to 47: after the hold, every NPC is the host's again.
    p.send("orbit")
    s = g.wait("NPCs back with the host", lambda s: s["npc"]["clientOwns"] == 0 and s["npc"]["puppets"] == 0, 40)
    r.check("NPCs return to the host when the bot comes back", s["npc"]["puppets"] == 0, s.get("shared"))
    changes = [l for l in g.log()[start:] if "NPCs change hands" in l]
    r.metrics["hand-over messages"] = len(changes)
    r.check("no flipping back and forth (under 12 hand-over messages)", len(changes) < 12, len(changes))
    g.shot("back-together")
    r.note("bot: " + " / ".join(l for l in p.output() if "Shared NPCs" in l)[-400:])


def scenario_bodies_setup(g, r, ctx):
    """In a mission with the bot (as the npc scenario starts), unless already."""
    if g.status() and g.status().get("synced"):
        return
    if not g.running():
        g.launch()
        to_title(g, r)
    to_freeform(g, r)
    p = ctx["partner"]
    relay = p.start_relay()
    g.command(f"relay {relay}")
    g.command("host")
    code = g.wait("a join code", lambda s: s.get("relay", {}).get("code"), 30)["relay"]["code"]
    p.join(code, relay)
    r.check("the bot joins and syncs", g.wait("the bot in sync", lambda s: s.get("synced"), 60).get("synced"))
    time.sleep(5)


def scenario_bodies(g, r, ctx):
    """A body knocked out by 47: moved as the partner's word moves it (out of sight), pulled where
    it can be seen, and dragged as the partner's stream drags it; then the pause screen."""
    scenario_bodies_setup(g, r, ctx)
    start = len(g.log())
    # A body: the loneliest civilian knocked out, else killed, else the nearest NPC (one ignored
    # both the knock-out and the kill once, right after coming back from the bot: 2026-11-03).
    line = None
    for place, down in [("lonely 0.7", "knockout 3 1"), ("lonely 0.7", "takedown 2 1"), ("behind 0.7", "takedown 2 1")]:
        g.command(place)
        time.sleep(2)
        g.command(down)
        time.sleep(9)  # it falls and comes to rest
        g.command("body")
        line = g.wait_log(r"body +\| .* at \(-?[\d.]+, -?[\d.]+, -?[\d.]+\)", 5, start)
        if line:
            break
        r.note(f"no body after '{place}' + '{down}'")
    r.check("a body to move (knocked out, or killed)", line is not None, line)
    if not line:
        return
    g.shot("body-before")
    done_rx = r"body +\| .* (lies where the partner left it|couldn't be pulled)"

    def moved(what, command, shot_after=None, wait=12):
        s0 = len(g.log())
        g.command(command)
        if shot_after:
            time.sleep(shot_after)
            g.shot(what + "-moving")
        done = g.wait_log(done_rx, wait, s0)
        off = numbers(done, r"\(([\d.]+) m off, pulled ([\d.]+) m\)")
        r.metrics[what + ": m off, m pulled"] = off
        r.check(f"{what}: it really moved (over 1 m) and lies where it was put (under 0.6 m off)",
                done is not None and "lies where" in done and off and off[1] > 1.0 and off[0] < 0.6, done)
        g.shot(what + "-done")
        return s0

    moved("moved out of sight (the partner's word)", "bodymove here")
    moved("pulled in sight", "bodymove pull", shot_after=0.8)
    s0 = moved("dragged (the partner's stream)", "bodymove drag", shot_after=1.5, wait=15)
    r.check("dragged, it is pulled along", g.wait_log(r"pulled along with their drag", 2, s0) is not None)
    g.command("bodymove")
    r.note(g.wait_log(r"bodymove *\| \d+ being placed", 5, s0) or "(no bodymove summary)")

    # The partner's pause screen (its preview: the game isn't paused for it).
    g.command("pausescreen")
    time.sleep(1.2)
    g.shot("pause-screen")
    r.check("no crash through all of it", g.running() and not os.path.exists(g.cmd + ".crash"))

def scenario_bodyprobe(g, r, ctx):
    """Engine probe: each way of moving a body, on a knocked out NPC, where it really ends up."""
    scenario_bodies_setup(g, r, ctx)
    start = len(g.log())
    g.command("lonely 0.7"); time.sleep(2)
    g.command("knockout 3 1"); time.sleep(9)
    for m in (0, 1, 4, 2, 3):
        s0 = len(g.log())
        g.command(f"bodyprobe {m} 3")
        time.sleep(1.0)
        g.shot(f"probe-{m}-1s")
        time.sleep(2.5)
        g.shot(f"probe-{m}-end")
        for line in g.log()[s0:]:
            if "bodyprobe" in line:
                r.note(line.split("| ", 1)[-1])
        time.sleep(1.5)
    r.check("no crash through the probe", g.running() and not os.path.exists(g.cmd + ".crash"))


def scenario_restart(g, r, ctx):
    if not g.status() or not g.status().get("synced"):
        r.check("restart needs the npc scenario first", False)
        return
    start = len(g.log())
    g.command("restart")
    s = g.wait("the mission back after the restart", lambda s: s.get("inScene") and s.get("synced"), 150)
    r.check("restart: back in the mission and in sync", s.get("synced"), s.get("state"))
    r.check("no crash on restart", g.running() and not os.path.exists(g.cmd + ".crash"))
    r.check("the log has the restart", g.wait_log(r"together in the mission", 10, start) is not None)
    g.shot("after-restart")


def scenario_netsim(r, ctx):
    """No game: the bot's host mode (a fake mission, NPCs walking circles) and the bot, through a
    local relay that adds lag, jitter and loss. Checks NPC sharing end to end over a bad link."""
    results, nettest = ctx["results"], ctx["nettest"]
    relay_log = open(os.path.join(results, "relay.log"), "w")
    relay = subprocess.Popen([sys.executable, "-I", os.path.join(ROOT, "server", "relay.py"), "--bind", "127.0.0.1", "--port", "47199",
                              "--lag", "60", "--jitter", "25", "--loss", "3"], stdout=relay_log, stderr=subprocess.STDOUT)
    host_out = open(os.path.join(results, "host.log"), "w")
    procs = [relay]
    try:
        time.sleep(1.5)
        host = subprocess.Popen([nettest, "host", "--relay", "127.0.0.1:47199", "--seconds", "300"], stdout=host_out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        procs.append(host)

        def host_lines():
            with open(os.path.join(results, "host.log"), encoding="utf-8", errors="replace") as f:
                return f.read().splitlines()

        def wait_host(pattern, timeout, since=0):
            end = time.time() + timeout
            while time.time() < end:
                for line in host_lines()[since:]:
                    if re.search(pattern, line):
                        return line
                time.sleep(0.25)
            return None

        line = wait_host(r"JOIN CODE:\s+(\S+)", 20)
        r.check("netsim: the host gets a join code from the relay", line is not None, line)
        if not line:
            return
        code = re.search(r"JOIN CODE:\s+(\S+)", line).group(1)
        partner = Partner(results, nettest)
        partner.join(code, "127.0.0.1:47199")
        procs.append(partner.bot)
        r.check("netsim: the bot joins and they are together", wait_host(r"sync: together in the mission", 30) is not None)

        time.sleep(14)
        stats = last_line(host_lines(), r"INFO stats: together")
        r.check("netsim: a health line every 10 s, with the link figures", stats is not None and "link rtt" in stats, (stats or "")[:250])
        rtt = numbers(stats, r"link rtt (\d+) ms")
        r.metrics["netsim rtt (relay adds 2 x 60 ms + jitter)"] = rtt
        r.check("netsim: the measured round trip reflects the added lag", rtt is not None and rtt[0] >= 100, rtt)
        check = wait_host(r"sync check: \d+ NPCs", 15)  # the first comes 5 s in; a part may be lost
        r.check("netsim: the sync check finds every NPC in place", check is not None and " 0 over 5 m" in check, (check or "")[:250])

        # The bot stands among NPCs far from the host: the host hands them over.
        since = len(host_lines())
        partner.send("at 80 0 80")
        r.check("netsim: NPCs near the bot are handed to it", wait_host(r"to the client", 15, since) is not None)
        partner.send("npcwalk 1.2")
        time.sleep(24)
        lines = host_lines()
        stats = last_line(lines, r"INFO stats: together")
        puppets = numbers(stats, r"(\d+) puppets")
        r.check("netsim: they are puppets on the host", puppets is not None and puppets[0] > 0, (stats or "")[:300])
        check = last_line(lines, r"sync check: \d+ NPCs")
        r.metrics["netsim sync check (bot runs NPCs, walking, lossy link)"] = (check or "")[40:]
        r.check("netsim: walking puppets are where the bot has them", check is not None and " 0 over 5 m" in check, (check or "")[:300])
        held = numbers(stats, r"(\d+) held a frame, (\d+) dropped")
        r.metrics["netsim messages held a frame / dropped"] = held

        # The bot stops sending them: the host gives them their own AI back.
        since = len(host_lines())
        partner.send("npcdrop")
        r.check("netsim: NPCs the bot stops sending get their own AI back", wait_host(r"no positions from the partner for 5 s", 14, since) is not None)
        since = len(host_lines())
        partner.send("npcback")
        time.sleep(12)
        stats = last_line(host_lines()[since:], r"INFO stats: together")
        puppets = numbers(stats, r"(\d+) puppets")
        r.check("netsim: and become puppets again when they come back", puppets is not None and puppets[0] > 0, (stats or "")[:200])

        # The bot comes back: the host runs everything again once the hold is over.
        partner.send("orbit")
        time.sleep(16)
        stats = last_line(host_lines(), r"INFO stats: together")
        puppets = numbers(stats, r"(\d+) puppets")
        r.check("netsim: NPCs go back to the host when the bot comes back", puppets is not None and puppets[0] == 0, (stats or "")[:200])
        changes = [l for l in host_lines() if "change hands" in l]
        r.check("netsim: no flipping back and forth", len(changes) < 12, len(changes))

        since = len(host_lines())
        partner.send("quit")
        r.check("netsim: the host logs the partner leaving, with the link summary", wait_host(r"partner gone: .*rtt", 15, since) is not None)
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(10)
                except subprocess.TimeoutExpired:
                    p.kill()


def fake_game(folder, version, loader):
    """A game folder whose Retail/HITMAN3.exe says it is game `version`: a copy of a small PE file
    (an SDK DLL) with its version numbers changed."""
    retail = os.path.join(folder, "Retail")
    os.makedirs(retail, exist_ok=True)
    data = bytearray(open(loader, "rb").read())
    at = data.find(b"\xbd\x04\xef\xfe")
    while at >= 0 and struct.unpack_from("<I", data, at + 4)[0] != 0x00010000:
        at = data.find(b"\xbd\x04\xef\xfe", at + 1)
    major, minor, build = (int(x) for x in version.split("."))
    struct.pack_into("<IIII", data, at + 8, (major << 16) | minor, build << 16, (major << 16) | minor, build << 16)
    open(os.path.join(retail, "HITMAN3.exe"), "wb").write(bytes(data))
    return retail


def sha(path):
    import hashlib
    return hashlib.sha256(open(path, "rb").read()).hexdigest() if os.path.exists(path) else None


def scenario_installers(r, ctx):
    """No game: fake game folders (3.290.1 and 3.280.0, one with another mod and its loader), the
    Windows installer (and the Linux one through WSL, if there is one): the right build goes in,
    the other mod stays, uninstall puts the folder back as it was."""
    stage = ctx["stage"]
    root = os.path.join(ctx["results"], "installers")
    for version, folder in (("3.290.1", "files"), ("3.280.0", "files-3.280")):
        game = os.path.join(root, "win-" + version, "HITMAN 3")
        retail = fake_game(game, version, os.path.join(stage, "files", "ZHMModSDK.dll"))
        # Someone else's mod and mod loader there already.
        os.makedirs(os.path.join(retail, "mods"), exist_ok=True)
        open(os.path.join(retail, "ZHMModSDK.dll"), "wb").write(b"their loader")
        open(os.path.join(retail, "mods", "Other.dll"), "wb").write(b"their mod")
        open(os.path.join(retail, "mods.ini"), "w").write("[sdk]\n[Other]\n")
        before = {os.path.relpath(os.path.join(d, n), retail): sha(os.path.join(d, n)) for d, _, fs in os.walk(retail) for n in fs}
        out = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", os.path.join(stage, "installer.ps1"),
                              "-GamePath", game, "-Quiet", "-NoFirewall"], capture_output=True, text=True, timeout=120)
        ours = sha(os.path.join(stage, folder, "ZHMModSDK.dll"))
        r.check(f"installers: Windows, game {version}: the {folder} build goes in", out.returncode == 0 and sha(os.path.join(retail, "ZHMModSDK.dll")) == ours and
                sha(os.path.join(retail, "mods", "HitmanCoop.dll")) == sha(os.path.join(stage, folder, "mods", "HitmanCoop.dll")),
                (out.stdout + out.stderr).strip().splitlines()[-1:] if out.returncode else "")
        ini = open(os.path.join(retail, "mods.ini")).read()
        r.check(f"installers: Windows, game {version}: the other mod is kept on", "[Other]" in ini and "[HitmanCoop]" in ini, ini.replace("\n", " "))
        out = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", os.path.join(stage, "installer.ps1"),
                              "-GamePath", game, "-Uninstall", "-Quiet", "-NoFirewall"], capture_output=True, text=True, timeout=120)
        after = {os.path.relpath(os.path.join(d, n), retail): sha(os.path.join(d, n)) for d, _, fs in os.walk(retail) for n in fs if not n.startswith("HitmanCoop-install")}
        r.check(f"installers: Windows, game {version}: uninstall puts it back as it was", out.returncode == 0 and after == before,
                sorted(set(after.items()) ^ set(before.items()))[:4])

    wsl = shutil.which("wsl.exe") or shutil.which("wsl")
    if not wsl:
        r.note("installers: no WSL here, the Linux installer wasn't run")
        return

    def linux(path):
        p = os.path.abspath(path).replace("\\", "/")
        return "/mnt/" + p[0].lower() + p[2:]

    for version, folder in (("3.290.1", "files"), ("3.280.0", "files-3.280")):
        game = os.path.join(root, "linux-" + version, "HITMAN 3")
        retail = fake_game(game, version, os.path.join(stage, "files", "ZHMModSDK.dll"))
        out = subprocess.run([wsl, "bash", linux(os.path.join(stage, "installer.sh")), "--game", linux(game), "--quiet", "--no-firewall"],
                             capture_output=True, text=True, timeout=180)
        ours = sha(os.path.join(stage, folder, "ZHMModSDK.dll"))
        r.check(f"installers: Linux, game {version}: the {folder} build goes in", out.returncode == 0 and sha(os.path.join(retail, "ZHMModSDK.dll")) == ours,
                (out.stdout + out.stderr).strip().splitlines()[-2:])
        out = subprocess.run([wsl, "bash", linux(os.path.join(stage, "installer.sh")), "--game", linux(game), "--uninstall", "--quiet", "--no-firewall"],
                             capture_output=True, text=True, timeout=180)
        left = sorted(os.path.relpath(os.path.join(d, n), retail) for d, _, fs in os.walk(retail) for n in fs)
        r.check(f"installers: Linux, game {version}: uninstall leaves only the game", out.returncode == 0 and left == ["HITMAN3.exe"], left[:5])


GAME_SCENARIOS = {"smoke": scenario_smoke, "npc": scenario_npc, "bodies": scenario_bodies, "bodyprobe": scenario_bodyprobe, "restart": scenario_restart}
SCENARIOS = list(GAME_SCENARIOS) + ["netsim", "installers"]


def main():
    # (Piped, Windows gives stdout its ANSI code page: a log line with a BOM or a name with an accent stopped the run.)
    for s_Stream in (sys.stdout, sys.stderr):
        s_Stream.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", help="the game folder (the one with Retail in it), a Steam copy; not needed for netsim")
    ap.add_argument("--version")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--no-shots", action="store_true")
    ap.add_argument("scenarios", nargs="*", default=["smoke"])
    a = ap.parse_args()

    names = SCENARIOS if a.scenarios == ["all"] else a.scenarios
    for n in names:
        if n not in SCENARIOS:
            sys.exit(f"unknown scenario {n} (have: {', '.join(SCENARIOS)})")

    version = a.version or re.search(r'c_ModVersion = "([^"]+)"', open(os.path.join(ROOT, "plugin", "Src", "CoopText.h")).read()).group(1)
    stage = os.path.join(ROOT, "_dist", f"HitmanCoop-{version}")
    nettest = os.path.join(stage, "Test-Connection.exe")
    if not os.path.isdir(stage):
        sys.exit(f"no {stage}: run tools/package.ps1 first")

    game_names = [n for n in names if n in GAME_SCENARIOS]
    folder = None
    if game_names:
        if not a.game:
            sys.exit("--game is needed for " + ", ".join(game_names))
        folder = os.path.abspath(a.game)
        if not os.path.isfile(os.path.join(folder, "Retail", "HITMAN3.exe")):
            sys.exit(f"no Retail/HITMAN3.exe under {folder}")
        if "steamapps" not in folder.lower():
            sys.exit("Steam copies only: this starts the game the way Steam does")

    results = os.path.join(HERE, "results", datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    os.makedirs(results)
    r = Report(results)
    print(f"HITMAN Co-op {version} tests: {', '.join(names)}\nresults in {results}", flush=True)
    ctx = {"version": version, "results": results, "nettest": nettest, "stage": stage}

    if "installers" in names:
        print("== installers", flush=True)
        scenario_installers(r, ctx)

    if "netsim" in names:
        print("== netsim", flush=True)
        scenario_netsim(r, ctx)

    if game_names:
        g = Game(folder, results, not a.no_shots)
        if g.running():
            sys.exit("HITMAN is running: close it first")
        game, payload = g.install(stage)
        r.note(f"game {game}, installed {payload} from {stage}")
        ctx.update({"game": game, "partner": Partner(results, nettest)})
        try:
            for n in game_names:
                print(f"== {n}", flush=True)
                try:
                    GAME_SCENARIOS[n](g, r, ctx)
                except Failed as e:
                    r.check(f"{n}: got through", False, str(e))
                    g.shot(f"failed-{n}")
                    break
        finally:
            ctx["partner"].stop()
            closed = g.close()
            r.check("the game closes cleanly", closed == "closed", closed)
            r.check("no crash report", not os.path.exists(g.cmd + ".crash") and not os.path.exists(os.path.join(g.mods, "HitmanCoop-crash.txt")))
            for name in ("HitmanCoop.log", "HitmanCoop-crash.txt"):
                if os.path.exists(os.path.join(g.mods, name)):
                    shutil.copy2(os.path.join(g.mods, name), results)
            if os.path.exists(g.cmd + ".crash"):
                shutil.copy2(g.cmd + ".crash", results)
            if not a.keep:
                g.restore()

    passed, total = r.write()
    print(f"{passed} of {total} checks passed: {os.path.join(results, 'report.md')}", flush=True)
    sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
    main()
