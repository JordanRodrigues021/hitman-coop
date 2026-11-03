# HITMAN Co-op

Two-player co-op for HITMAN World of Assassination. Made by Jordan Rodrigues.

[![Join the HITMAN Co-op Discord](https://img.shields.io/badge/Discord-Join%20the%20community-5865F2?logo=discord&logoColor=white)](https://discord.gg/rQeHdpbKUp)

> This is a fan mod. It's not made or supported by IO Interactive and has nothing to do with them.
> HITMAN is their trademark. You use it at your own risk, so please read the
> [disclaimer](DISCLAIMER.md) first.

You and a friend play the same mission at the same time. You can see each other walking around the
level and the NPCs are shared between both games. If you knock a guard out, he's knocked out for your
friend too. Same with kills, alerts and where people are walking. Bodies, doors and items are shared
as well, so if your friend dumps a body in a closet, your guards won't find it either.

There's also some stuff to help you plan things together: you can mark targets, there's a small
radar, quick signals, chat, and a ready check with a 3-2-1 countdown for when you both want to take
out a target at the same moment.

It's free, it's open source, and I keep updating it.

**Testers and developers:** the beta is on the [`beta` branch](https://github.com/JordanRodrigues021/hitman-coop/tree/beta)
(code) and its builds are the [pre-releases](https://github.com/JordanRodrigues021/hitman-coop/releases).
See [BETA.md](https://github.com/JordanRodrigues021/hitman-coop/blob/beta/BETA.md).

## Discord

There's a [HITMAN Co-op Discord](https://discord.gg/rQeHdpbKUp) now. You can find someone to play
with (by platform and region), hop in the two-player voice rooms, get help installing, report bugs,
suggest stuff and see what I'm working on. New releases get posted there first, and you can share
clips of your best double hits. Honestly it's the quickest way to get a bug fixed.

Founding Team applications are open until 30 November 2026. I'm looking for moderators, testers,
contributors and people to host events. Everyone who gets in during this first round keeps the
Founding Team role on the server for good and gets added to the [credits](#founding-team).
You can [apply here](https://forms.gle/vtcxvPjS4GTSKxbG9).

## A note on AI

I want to be upfront about this. I used an AI coding assistant (Claude) while making this mod and it
helped me a lot. I'm a single developer, I'm young, and I'm still in school. Building something this
big alone, single-handedly, just wasn't going to happen any other way, and I wanted to get it out
as soon as I could so people could actually play it.

The idea is mine. I designed how it works, paid for the AI plans and usage myself, and tested every
build in the game more times than I can count. A lot of the fixes came straight from bug reports
people sent in. HITMAN was never made for multiplayer, so getting two games to agree on every
guard, body and door meant breaking things over and over, reading logs and figuring out what the
game was actually doing.

I know some people don't want to use mods made with AI, and that's okay. Nobody has to use this.
If you do try it, I'd love to hear what you think, good or bad. Criticism on any part of it is
welcome. If something breaks, I'll fix it as fast as I can. So far there have been six updates in
the first week, almost all of them from player reports.

Other people have tried co-op for HITMAN before me (there's even an early multiplayer experiment
called Hitmen inside ZHMModSDK), so I'm not claiming to be the first. This is just my go at it.

## What's in it

The mission
- Your friend shows up in your game as their 47, moving exactly how they move, in whatever disguise
  they have on. If they're wearing one of 47's own suits you'll see them in normal clothes instead
  (putting the suit on the copy crashes the game), so look for their name tag.
- NPCs are shared: kills, knockouts, alert levels and where they walk. A guard your friend is
  holding stops reacting in your game, so you can both take someone down at once without the other
  takedown getting noticed.
- Bodies your friend drags off or hides in a closet get hidden in your game too.
- Doors and items are shared. If your friend picks something up it disappears for you, and what
  they're holding shows in their hand.
- Pause together, restart together, and start together (your mission waits until your friend has
  loaded in).
- Your friend's copy can't be seen by the AI or hurt. Their own game decides what happens to them.
- It updates itself. New versions download in the background and get used the next time you start
  the game, and you're shown what changed.

Working as a team
- Marks: aim and press the middle mouse button. Press it twice fast to mark danger, and again later
  to remove it. Marks stick to the person you marked.
- Signals over your head (OK, wait, help, look here).
- Ready check, 3-2-1 countdown, "where are you?", "come to me" and regroup (teleports you behind
  your friend).
- Chat and eight quick messages you can change.

Keeping track of your friend
- A HUD with ping, distance, how alert the NPCs near them are, their disguise, and whether they're
  in a menu, loading or away.
- A name tag over their head (it goes orange or red when they're in trouble) and an arrow at the
  edge of the screen when they're off it.
- Radar, compass, kill feed, Silent Assassin tracking for the team and a summary at the end.

Connecting
- Join codes through a small relay server, or straight by IP (LAN, Tailscale, port forwarding).
- It adapts to bad connections and reconnects by itself if you drop.
- It never uses IO Interactive's, Steam's or Epic's servers. It works with the game online or offline.

You can also rebind every hotkey, move and resize the HUD, switch to feet, turn on colour-blind
colours, and there's a streamer mode that hides join codes and IP addresses.

## What you need

- HITMAN World of Assassination on Steam or Epic, on Windows. The Game Pass / Microsoft Store
  version can't be modded.
- Both of you need the same version of HITMAN Co-op.
- You play at a location you both own. If your friend only has some of the locations, you can play
  those together. The mod never takes anyone somewhere they didn't start themselves.
- It's made for game version 3.290.1. If the game updates and something co-op needs is missing,
  co-op just stays off instead of breaking anything, until there's a fixed version.

## How to install

Both of you do this. It takes about a minute.

1. Close the game.
2. Go to the [Releases page](../../releases/latest) and download `HitmanCoop-Setup-<version>.exe`.
   That's the only file you need.
3. Run it and click INSTALL. It finds HITMAN on Steam or Epic on its own and installs the mod loader
   ([ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK)) and the mod. If it can't find the game, click
   Browse and pick your HITMAN 3 folder.
4. Start the game and press `~` (the key under Esc). CO-OP shows up at the top.

Windows SmartScreen will probably say "Windows protected your PC" because the setup isn't signed by
a company. Click More info and then Run anyway. The SHA-256 of the setup is on the Releases page if
you want to check it.

If you'd rather use a zip, download `HitmanCoop-<version>.zip`, extract it and run Install.bat.
Uninstall.bat takes it off again. To remove it with the setup, run it again and click UNINSTALL.
Your saves aren't touched either way.

## How to play together

1. Both of you start a mission at the same location.
2. The host presses `~`, opens CO-OP and clicks HOST. They get a join code (it's copied for them)
   or they can give their IP address.
3. The friend presses `~`, opens CO-OP, types the code under "Join a friend" and clicks JOIN. They
   get taken into the host's mission.
4. Once the HUD says you're in the mission together, you're good.

The CO-OP window has tabs for Play, Chat, Mission (stats, your co-op record, notification history),
Settings, Hotkeys and About (help and the "Copy diagnostics" button for bug reports).

## Playing over the internet

If you're not on the same Wi-Fi, I'd use Tailscale. It's free, it takes a few minutes and you don't
have to touch your router. It puts both PCs on a little private network so the friend can join the
host directly.

1. Both of you install [Tailscale](https://tailscale.com/download) and sign in.
2. The host invites the friend from the [Tailscale admin page](https://login.tailscale.com/admin/users)
   (Invite users), or shares their PC with them under Machines.
3. The host starts a mission, opens CO-OP and clicks HOST. Their Tailscale address (starts with
   `100.`) is shown in the Play tab. Send that to your friend.
4. The friend accepts the invite, starts a mission at the same location, types that `100.x.x.x`
   address under "Join a friend" and clicks JOIN.

For Tailscale, LAN or IP connections the host has to tick "Also allow direct IP connections through
Windows Firewall" in the setup (just run the setup again if you didn't). Join codes don't need it.

If it won't connect, check that Tailscale shows both PCs as connected, that the host ticked that
firewall box, and that you're both at the same location.

Other ways:
- Same network: the friend joins the host's local address, shown in the Play tab.
- Join codes: you both set the same co-op server under Settings > Connection and the host gets a
  six-letter code. There's no public server built in, but one of you can run one pretty easily (see
  below). It only passes packets between the two games.
- Port forwarding: the host forwards UDP port 47047 to their PC and the friend joins their public IP.

Streaming? Turn on streamer mode under Settings > Connection so your codes and addresses never show
on screen. There's also Test-Connection.exe in the zip if you want to check that two PCs can reach
each other before starting the game.

## Running your own co-op server

The relay is one Python 3 file with no dependencies, in [`server/`](server). Put it on anything
with a public IP (a cheap VPS is fine) and open UDP port 47100.

```sh
# Linux, as a service that starts on boot:
sudo sh server/install-linux.sh

# or with Docker:
docker build -t hitmancoop-relay server/
docker run -d --restart=always -p 47100:47100/udp hitmancoop-relay

# or just run it:
python3 -I server/relay.py --port 47100
```

Then you both put `your.server.address:47100` as the co-op server in Settings.

## Hotkeys

| What | Key |
| --- | --- |
| Mark what you're aiming at (twice fast for danger, again to remove) | Middle mouse |
| Quick messages 1-8 | Num 1 to Num 8 |
| Regroup | Num 0 |
| Where are you? | Num 9 |
| Ready check / say you're ready | Num . |
| 3-2-1 countdown | Num + |
| OK / wait / help | Num / , Num * , Num - |
| Come to me, look here, hide HUD or radar, screenshot mode | not set |

Num Lock has to be on. You can change any of them in the Hotkeys tab: click Change and press a key
(hold Ctrl, Shift or Alt for a combo). If a key is used twice it shows up red.

Everything else is in the Settings tab and gets saved to `Retail\mods\HitmanCoop.ini`.

## If something goes wrong

- No CO-OP button: press `~` (it's ^ on some keyboards). If still nothing, reinstall with the setup.
- "Different HITMAN Co-op versions": you both need the same release.
- Can't connect by IP: the host's firewall or router is blocking UDP 47047. Use a join code,
  Tailscale or forward the port.
- "Co-op can't run on this game version": the game got updated. Wait for (or grab) the matching
  release.
- Can't see your friend's 47: if they're in one of 47's suits they show up in normal clothes, so look
  for the name tag or the radar dot. If there's no name tag at all, restart the mission together. If
  it keeps happening, send me the diagnostics (they have a "Partner's 47" line that says why).
- "You are in a different mission" or "different difficulties": start the same mission on the same
  difficulty.

To report a bug, open an [issue](../../issues). In the CO-OP window go to About and click Copy
diagnostics (there are no IP addresses in it), and attach `Retail\mods\HitmanCoop.log` from both PCs.
If the game crashed, add `Retail\mods\HitmanCoop-crash.txt` too.

Still stuck? Ask in #install-help on the [Discord](https://discord.gg/rQeHdpbKUp) or post it in
#bug-reports. Someone there has probably run into it already.

## FAQ

**Can I get banned?** HITMAN doesn't have an anti-cheat like EasyAntiCheat or BattlEye. This runs
through ZHMModSDK, which is the normal HITMAN mod loader, and doesn't change any game files. It's
still a mod though, so read the [disclaimer](DISCLAIMER.md).

**Does it mess with the online stuff?** No. It never talks to IO Interactive's servers, co-op goes
over its own connection, and your game logs in like normal. When the game starts it checks this
GitHub page for a new version (you can turn that off under CO-OP > About). It works with the game
offline too, so if you're worried about your profile or leaderboards, just play co-op offline.

**Do I need Peacock?** No. I haven't tested it together with Peacock yet either, so if you try it,
let me know how it goes.

## What's new

0.13.2 fixed bodies going invisible by themselves and made updates download on more internet
connections. Everything older is in the [changelog](CHANGELOG.md).

## Building it yourself

See [BUILDING.md](BUILDING.md). Short version (Visual Studio 2022, CMake, PowerShell):

```powershell
./tools/fetch-sdk.ps1
cmake -S . -B _build -G "Visual Studio 17 2022" -A x64 -DZHMMODSDK_DEVPKG_DIR="$PWD/_deps/devpkg"
cmake --build _build --config RelWithDebInfo
cmake --install _build --config RelWithDebInfo --prefix _build/install
./tools/check-dll.ps1
./tools/package.ps1
```

The core part (protocol, networking, session and sync logic) doesn't need the game and builds and
tests on its own:

```powershell
cmake -S core -B core/build -G "Visual Studio 17 2022" -A x64
cmake --build core/build --config Release
core/build/Release/coop_tests.exe
python -I server/test_relay.py
```

## Founding team

These are the people with the Founding Team role on my Discord server. They helped me get this
mod to where it is now, and I really appreciate them.

- **Aerial Bot**: sent me a huge amount of bug reports and suggestions, was one of the first people
  to join the Discord, and put their hand up to help moderate it too. Thank you!

Want to be on this list? [Applications](https://forms.gle/vtcxvPjS4GTSKxbG9) for the founding round
are open until 15 November 2026.

## Support the developer

Donating is completely optional. The mod is free and it stays free, and nothing is locked behind
paying. Please only donate if you really like what I've made and you feel it's worth it to you.
If not, that's totally fine. Starring the repo, telling a friend or sending a bug report helps
just as much.

If you do want to:

**UPI (India)**

<p>
  <img src="docs/upi-qr.png" alt="UPI QR code" width="200"><br>
  Scan it with any UPI app or pay to <b><code>jordanrodrigues021@okhdfcbank</code></b>
</p>

**Steam gift card (works from any country)**

Add me on Steam with friend code `1300893237` (Friends > Add a Friend) or this
[invite link](https://s.team/p/gtmp-bvfh/JJRCJJDJ), then send a
[Digital Gift Card](https://store.steampowered.com/digitalgiftcards/) of any amount. Steam might
make you wait a few days after adding me before it lets you send one.

**Amazon gift card**

Send an amazon.in e-gift card to
[jordanrodrigues021@gmail.com](mailto:jordanrodrigues021@gmail.com). It has to be from amazon.in,
cards from other Amazon stores usually don't work in India.

If you want to help some other way, just email me or message me on Steam.

Thanks a lot for reading this far.

## Credits and licence

Made by Jordan Rodrigues, with a lot of help from the people above. Full credits are in
[CREDITS.md](CREDITS.md). It's built on [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK) by
OrfeasZ and contributors.

Licensed under the GPL-3.0, see [LICENSE](LICENSE).
