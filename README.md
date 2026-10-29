# HITMAN Co-op

**Two-player co-op for HITMAN World of Assassination.**
Created by **Jordan Rodrigues**.

> **Fan-made project.** HITMAN Co-op is not made, endorsed, supported or affiliated with
> IO Interactive. "HITMAN" and related names are trademarks of their owners. Use it at your own
> risk: read the [disclaimer](DISCLAIMER.md) before you install it.

Play a mission together with a friend: you see each other in the level, the NPCs are shared (who
dies, who is knocked out, who is alerted, where they walk), doors and items are shared, and you get
everything you need to work as a team: marks, chat, signals, a radar, a ready check and a 3-2-1
countdown for the perfect double hit.

> ❤️ **HITMAN Co-op is free, and made by one young developer.** If it gives you and a friend a great
> night, please consider [supporting me](#support-the-developer). Even a small tip makes my day.

---

## Contents

- [Features](#features)
- [Requirements](#requirements)
- [Install](#install)
- [Play together](#play-together)
- [Playing over the internet with Tailscale (recommended)](#playing-over-the-internet-with-tailscale-recommended)
- [Controls and hotkeys](#controls-and-hotkeys)
- [Settings](#settings)
- [Joining over the internet](#joining-over-the-internet)
- [Run your own co-op server](#run-your-own-co-op-server)
- [Troubleshooting](#troubleshooting)
- [FAQ](#faq)
- [What's new](#whats-new)
- [Build from source](#build-from-source)
- [❤️ Support the developer](#support-the-developer)
- [Credits and licence](#credits-and-licence)

---

## Features

**A shared mission**
- Your partner's 47 in your game, animated with their exact movements (their whole skeleton pose,
  30 times a second), wearing their disguise.
- Shared NPCs: kills, knock-outs (by any means), alert levels, and NPCs walking where they walk in
  your partner's game. An NPC your partner has hold of stops reacting in your game, so a guard you
  both take down at the same moment doesn't see the other takedown.
- Shared bodies: a body your partner drags away or hides in a closet or container is hidden in your
  game too, so your guards don't find it.
- Shared doors and items: what your partner picks up disappears for you; what they drop or throw
  lands where it lands. What your partner holds shows in their stand-in's hand.
- Pause together, restart together, and start together (your mission waits while your partner's
  game is still loading in).
- Your partner's stand-in is invisible to the AI and can't be hurt: their own game decides what
  happens to them.

**Teamwork**
- **Marks**: aim and press the middle mouse button. Twice quickly to warn of danger; again later to
  take it back. Marks follow the person marked, label targets, and hit walls and objects at any height.
- **Signals** (OK, wait, help, look here) over your head on your partner's screen.
- **Ready check**, a latency-compensated **3-2-1 countdown**, **"where are you?"**, **"come to me"** and
  **regroup** (puts you right behind your partner).
- **Chat** and eight quick messages, which you can rewrite.

**Always knowing where your partner stands**
- HUD: ping and packet loss, distance, how alert the NPCs around them are, trespassing, their
  disguise, which menu they have open, whether they're loading, away or idle.
- Name tag over their head in the colour they picked (orange or red when they're in trouble), with
  the height difference, and an arrow at the edge of the screen when they're off it.
- Radar and compass strip with your partner and both players' marks.
- Kill feed, completed challenges, and alerts when your partner is spotted, hunted or in combat.
- Silent Assassin watch for the team, a mission summary at the end, and a co-op record across sessions.

**Made for real connections**
- Join codes through a small relay server, or direct by IP (LAN, Tailscale, port forwarding).
- Smooth playback that adapts to your connection, less detail sent when the link struggles,
  automatic reconnect, and loading screens that never time you out.
- Works with the game online or offline. No IO Interactive, Steam or Epic servers are used.

**Comfort**
- Every hotkey can be rebound (with Ctrl, Shift or Alt) in the Hotkeys tab.
- HUD position, size and background; name tag size; metres or feet; colour-blind friendly colours;
  streamer mode (hides join codes and IP addresses); notification length and filters.

## Requirements

- HITMAN World of Assassination on **Steam** or **Epic** (Windows).
  The Microsoft Store / Game Pass version can't be modded.
- Both players need the **same version of HITMAN Co-op**.
- You play together at a location **you both own** (a friend with only some locations can play
  those with you). The mod never takes anyone to a location they didn't start themselves.
- Tested on game version 3.280.0. After a game update, the mod may need an update too: if the game
  build doesn't have what co-op needs, co-op stays off instead of breaking anything.

## Install

> ### **Both players do this. It takes about a minute.**
>
> 1. **Close the game.**
> 2. **Go to the [Releases page](../../releases/latest) and download `HitmanCoop-Setup-<version>.exe`.**
>    (You don't need the source code or anything else from this page.)
> 3. **Run it and click INSTALL.** It finds HITMAN on Steam or Epic by itself and installs the mod
>    loader ([ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK)) and HITMAN Co-op into the game.
>    If it can't find your game, click **Browse** and pick your HITMAN 3 folder.
> 4. **Start the game. Press `~` (the key under Esc) and you'll see CO-OP at the top.** That's it.

- **Windows SmartScreen** may say "Windows protected your PC", because the setup isn't signed by a
  company. Click **More info**, then **Run anyway**. The setup's SHA-256 checksum is published next
  to it on the Releases page.
- **Prefer a zip?** Download `HitmanCoop-<version>.zip`, extract it anywhere, and double-click
  **Install.bat** (Uninstall.bat removes it). It does exactly what the setup does.
- **To remove the mod**, run the setup again and click **UNINSTALL**.
- Your saves and progress aren't touched. **Both players need the same HITMAN Co-op version.**

## Play together

1. Both players start a mission at the same location.
2. The **host** presses **~** (the key under Esc), clicks **CO-OP** at the top, and clicks **HOST**.
   They get a join code (copied to the clipboard), or share their IP address.
3. The **friend** presses **~**, clicks **CO-OP**, types the code under **Join a friend** and clicks
   **JOIN**. They're taken into the host's mission.
4. When the HUD says you're in the mission together, you're playing co-op.

The CO-OP window has tabs: **Play** (hosting, joining, teamwork buttons), **Chat**, **Mission**
(both players' stats, your co-op record, notification history), **Settings**, **Hotkeys** and
**About** (help, and "Copy diagnostics" for bug reports).

## Controls and hotkeys

| Action | Default key |
| --- | --- |
| Mark what you aim at (twice quickly: danger; again: take back) | Middle mouse button |
| Quick messages 1-8 | Num 1 - Num 8 |
| Regroup (go to your partner) | Num 0 |
| Where are you? (both name tags pulse) | Num 9 |
| Ready check / answer "ready" | Num . |
| 3-2-1 countdown | Num + |
| Signals: OK / wait / help | Num / / Num * / Num - |
| Come to me, look here, show/hide HUD or radar, screenshot mode | not bound |

Number pad keys need **Num Lock** on. Change any of them in the **Hotkeys** tab: click **Change**
and press a key (hold Ctrl, Shift or Alt for a combination). Keys used twice are shown in red.

## Settings

Everything is in the **Settings** tab and saved in `Retail\mods\HitmanCoop.ini`:

- **Playing together**: follow the host's mission, pause together (and whether your partner's map
  pauses you), start together, reconnect by yourself, sounds.
- **HUD and overlay**: HUD on/off, position, size and background; name tag, marks, radar (range,
  size), compass, target assist (off by default), your colour, colour-blind colours, feet or metres.
- **Marks**: how many at once (1-3) and how long they last.
- **Notifications**: on/off, how long, and which kinds.
- **Quick messages**: write your own eight.
- **Connection**: co-op server, direct port, NPC sync detail, streamer mode, copying the join code.

## Playing over the internet with Tailscale (recommended)

> ### **Not on the same Wi-Fi? Use Tailscale. It's free, takes five minutes, and needs no router setup.**
>
> Tailscale puts both PCs on a small private network of their own, so the friend can join the host
> directly, as if you were in the same house.
>
> **Both players:**
> 1. **Download Tailscale from [tailscale.com/download](https://tailscale.com/download) and install it.**
> 2. **Sign in** (with Google, Microsoft, GitHub or an email address).
>
> **The host:**
> 3. **Invite your friend to your Tailscale network**: in the [Tailscale admin console](https://login.tailscale.com/admin/users)
>    click **Invite users** and send the invite (or **Share** your PC with them under **Machines**).
> 4. **Start a mission, press `~`, open CO-OP and click HOST.** Your Tailscale address (it starts
>    with `100.`) is shown in the **Play** tab. Send it to your friend.
>
> **The friend:**
> 5. **Accept the invite, start a mission at the same location, press `~`, open CO-OP, type the
>    host's `100.x.x.x` address under Join a friend, and click JOIN.**

> **The host should tick "Also allow direct IP connections through Windows Firewall" in the setup**
> (run the setup again to change it). Join codes don't need it; Tailscale, LAN and IP connections do.

If it doesn't connect: check that Tailscale shows both PCs as **connected**, that the host ticked the
firewall box above, and that both players are at the same location in the game.

### Other ways to connect

- **Same network (LAN)**: the friend joins the host's local address, shown in the **Play** tab.
- **Join code**: both players set the same **co-op server** under Settings, Connection, and the
  host gets a six-letter code. There is no public server built in: one of you, or a friend, can run
  one in a few minutes ([see below](#run-your-own-co-op-server)). It only passes packets between the
  two games.
- **Port forwarding**: the host forwards **UDP port 47047** on their router to their PC, and the
  friend joins the host's public IP address.

**Streaming?** Turn on **streamer mode** (Settings, Connection) so join codes and addresses never
show on screen.

**Test-Connection.exe** (in the zip) checks that two PCs can reach each other before you start the game.

## Run your own co-op server

The relay server is one Python 3 file with no dependencies, in [`server/`](server). It needs a
machine with a public IP address (any small VPS) and UDP port 47100 open.

```sh
# Linux, as a service that starts on boot:
sudo sh server/install-linux.sh

# or with Docker:
docker build -t hitmancoop-relay server/
docker run -d --restart=always -p 47100:47100/udp hitmancoop-relay

# or just run it:
python3 -I server/relay.py --port 47100
```

Then both players type `your.server.address:47100` as the co-op server in Settings.

## Troubleshooting

- **The CO-OP button doesn't show**: press **~** (the key under Esc; ^ on some keyboards).
  If nothing appears, reinstall with the setup.
- **"Different HITMAN Co-op versions"**: both players need the same release.
- **Can't connect by IP**: the host's firewall or router blocks UDP 47047. Use a join code,
  Tailscale, or forward the port.
- **"Co-op can't run on this game version"**: the game was updated; get the matching HITMAN Co-op release.
- **"You are in a different mission" / "different difficulties"**: both players start the same
  mission on the same difficulty. (Since 0.12.0, two PCs with different CPUs no longer count as
  different missions in New York, Whittleton Creek and other HITMAN 2 locations.)
- **Something went wrong**: open About and click **Copy diagnostics** (it contains no IP addresses),
  and attach `Retail\mods\HitmanCoop.log` to an issue. If the game crashed, attach
  `Retail\mods\HitmanCoop-crash.txt` too.

## FAQ

**Does it trigger anti-cheat or get me banned?** HITMAN doesn't use an anti-cheat system like
EasyAntiCheat or BattlEye. HITMAN Co-op runs through ZHMModSDK, the standard HITMAN mod loader, and
doesn't change or replace any game files. It's still a mod, so read the [disclaimer](DISCLAIMER.md).

**Does it break the always-online features?** No. The mod never talks to IO Interactive's servers:
co-op runs over its own connection (Tailscale, LAN or a join code), and your game logs in as usual.
It also works with the game in offline mode. If you're worried about your online profile or
leaderboards, play co-op offline.

**Do I need Peacock?** No. It works with the official servers or offline. It hasn't been tested
together with Peacock.

## What's new

**0.12.0**: fixes for the first players' reports: New York and Whittleton Creek main missions,
dragged and hidden bodies, crashes on restart, double takedowns being witnessed, and invisible items
in your partner's hands. See the [changelog](CHANGELOG.md) for every version.

## Build from source

See [BUILDING.md](BUILDING.md). In short (Visual Studio 2022, CMake, PowerShell):

```powershell
./tools/fetch-sdk.ps1
cmake -S . -B _build -G "Visual Studio 17 2022" -A x64 -DZHMMODSDK_DEVPKG_DIR="$PWD/_deps/devpkg"
cmake --build _build --config RelWithDebInfo
cmake --install _build --config RelWithDebInfo --prefix _build/install
./tools/check-dll.ps1
./tools/package.ps1
```

The engine-independent core (protocol, reliable UDP, session and sync logic) builds and tests on
its own:

```powershell
cmake -S core -B core/build -G "Visual Studio 17 2022" -A x64
cmake --build core/build --config Release
core/build/Release/coop_tests.exe
python -I server/test_relay.py
```

## Support the developer

Hi, I'm **Jordan**, a young developer from India. 👋

HITMAN Co-op is my baby. I built every part of it myself: the networking, the shared NPCs, the
overlay, the installer, the co-op server. Countless late nights went into making two copies of a
game that was never designed for multiplayer agree on every guard, every body and every door. I'm
genuinely proud of what it has become, and I give it to you for free: no ads, no paywalls, no
studio, no company. Just me.

The thing is, I do this alone. There's no team to fall back on and no money coming in for it.
Every time the game updates and something breaks, it's me sitting down to find out why and fix it.
Every new feature is me building and testing it by hand.

**If HITMAN Co-op gave you and a friend even one great evening of sneaking around together, please
think about sending a small tip.** It doesn't have to be much: the price of a coffee means more to
me than you'd imagine. It tells me that something I made with my own hands is out there making
people happy, and it's what keeps me going and keeps this mod alive.

### 🇮🇳 From India: UPI (instant, free)

<p>
  <img src="docs/upi-qr.png" alt="UPI QR code" width="200"><br>
  Scan with any UPI app (Google Pay, PhonePe, Paytm, BHIM...) or pay to:<br>
  <b><code>jordanrodrigues021@okhdfcbank</code></b>
</p>

### 🌍 From anywhere in the world: Steam gift cards

Steam converts gift cards from any country, so this works wherever you are.

1. **Add me on Steam**: friend code **`1300893237`**
   (Steam, then *Friends*, then *Add a Friend*), or use my
   [quick invite link](https://s.team/p/gtmp-bvfh/JJRCJJDJ).
2. **Send a Steam Digital Gift Card** of any amount: in the Steam store, open
   [Digital Gift Cards](https://store.steampowered.com/digitalgiftcards/), pick an amount and choose me
   as the recipient. Steam may ask you to be friends for a few days before it lets you send one.

### 🎁 Amazon gift cards

Send an **Amazon India (amazon.in) e-gift card** by email to
**[jordanrodrigues021@gmail.com](mailto:jordanrodrigues021@gmail.com)**: at checkout, enter that
address as the recipient and Amazon delivers it to me. Please use **amazon.in**; gift cards from
other Amazon stores usually can't be used in India. Add a note with your name and I'll thank you
personally.

### 💬 Any other way

Want to support me some other way? Email me at
**[jordanrodrigues021@gmail.com](mailto:jordanrodrigues021@gmail.com)** or message me on Steam and
we'll work it out. I'm open to it.

**Can't donate? That's completely okay.** ⭐ **Star this repository**, tell a friend, share a clip of
your co-op run, or report a bug. Every bit of it helps a small developer more than you know.

Thank you, truly. 🙏

## Credits and licence

- **HITMAN Co-op** is created by **Jordan Rodrigues**.
- Built on [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK) by OrfeasZ and contributors (GPL-3.0),
  which also provides Dear ImGui and the Material Design icons used in the overlay.
- [DirectX-Headers](https://github.com/microsoft/DirectX-Headers) by Microsoft (MIT).

HITMAN Co-op is free software under the **GNU General Public License v3.0**: see [LICENSE](LICENSE).
It comes with no warranty; see the [disclaimer](DISCLAIMER.md).
