# Building

## Requirements

- Visual Studio 2022 (or its Build Tools) with the "Desktop development with C++" workload.
  ZHMModSDK's headers need C++23. VS 2019 can build and test `core/` but not the plugin.
- CMake 3.20+ (it comes with Visual Studio).
- PowerShell 5.1+, Python 3.10+ (tests, relay).

## Two builds, one download

HITMAN Co-op is built twice, once for each game version it supports, because each needs its own
ZHMModSDK (the SDK finds the game's code and data by version):

| Folder        | Game     | ZHMModSDK | Configure with                                   |
|---------------|----------|-----------|--------------------------------------------------|
| `_build`      | 3.290.x  | 4.2.1     | `-DZHMMODSDK_DEVPKG_DIR=_deps/devpkg`            |
| `_build-3280` | 3.280.x  | 4.1.1     | `-DCOOP_SDK=4.1.1 -DZHMMODSDK_DEVPKG_DIR=_deps/old-4.1.1/devpkg` |

The code sees `COOP_SDK_VERSION` (40201 or 40101) and `COOP_GAME_TARGET` ("3.290", "3.280");
`plugin/Src/SdkCompat.h` gives both SDKs the same names where they differ. Anything new must
build in both. A build started on the wrong game version keeps co-op off and says so (it never
crashes on fields that moved); `[general] any_game_version = true` overrides that for testing.
(`-DCOOP_SDK=4.0.0-rc.1`, game 3.240, is started but doesn't build yet: that SDK lacks a lot.)

The download holds both: `files/` and `files-3.280/`. `installer.ps1` (and the setup window,
which runs it) and `installer.sh` read the game's `HITMAN3.exe` version and install the build for
it. The self-updater reads the update file for the game version it runs on (`update.txt`, or
`update-3.280.txt` on 3.280), and reinstalls when the wrong build is in: a player who updates the
game gets switched to the right build by itself.

## Steps

```powershell
# 1. Download ZHMModSDK v4.2.1 and v4.1.1 (developer packages + releases) and DirectX-Headers, SHA-256 checked
./tools/fetch-sdk.ps1

# 2. Configure, build and install both builds
cmake -S . -B _build -G "Visual Studio 17 2022" -A x64 -DZHMMODSDK_DEVPKG_DIR="$PWD/_deps/devpkg"
cmake --build _build --config RelWithDebInfo
cmake --install _build --config RelWithDebInfo --prefix _build/install
cmake -S . -B _build-3280 -G "Visual Studio 17 2022" -A x64 -DCOOP_SDK=4.1.1 -DZHMMODSDK_DEVPKG_DIR="$PWD/_deps/old-4.1.1/devpkg"
cmake --build _build-3280 --config RelWithDebInfo
cmake --install _build-3280 --config RelWithDebInfo --prefix _build-3280/install

# 3. Check the DLL will load next to the shipped SDK (exports, imports, SDK ABI, no VC++ runtime)
./tools/check-dll.ps1

# 4. The player downloads, in _dist/: HitmanCoop-<v>.zip, HitmanCoop-Setup-<v>.exe and
#    HitmanCoop-<v>-linux.tar.gz (each with both builds), and _dist/update-<v>/ for the
#    self-updater. Write installer/WHATSNEW.txt first: players see it in game after updating, and
#    packaging refuses to run without this version in it. The version comes from CoopText.h.
./tools/package.ps1

# 5. The tests that need no game (see "Tests" below)
core/build22/RelWithDebInfo/coop_tests.exe
python -I tools/gametest/gametest.py installers netsim
```

Version strings: `core/include/coop/Session.h` (modVersion), `plugin/Src/CoopText.h`
(c_ModVersion), `installer/setup/Setup.cs`, `installer/setup/setup.manifest`, `installer/README.txt`.
The network protocol is `kProtocolVersion` in `core/include/coop/Protocol.h`: raise it whenever a
message changes, so mismatched versions refuse each other with a clear message.

## Releasing

A release has the setup, the zip, the Linux tarball and the two update manifests
(`update.txt`, `update-3.280.txt`), labelled, with the checksums in the notes. All of
`_dist/update-<version>/` (the manifests, the 3.290 game files, and `game-3.280/` with the 3.280
ones) also goes on the repo's `updates` branch, one commit replacing the last, with the branch's
own README saying it's not for players. The mod reads its manifest from that branch first
(GitHub's raw file server) and from the latest release only if that fails: on a player's
connection Windows couldn't reach the server GitHub sends release downloads to.

```powershell
# in a fresh folder: the files of _dist/update-<version>/ (game-3.280/ included), plus that README
git init -b updates; git add -A; git commit -m "Update files for HITMAN Co-op <version>"
git push --force https://github.com/JordanRodrigues021/hitman-coop.git updates
```

Push the branch before publishing the release, and wait until
`https://raw.githubusercontent.com/JordanRodrigues021/hitman-coop/updates/HitmanCoop.dll` serves
the new file (its cache keeps the old one up to 5 minutes). Installed mods read their manifest when
the game starts, download the files that differ, and use them from the next start (see
`plugin/Src/Updater.h`). Every release must have both manifests, or players stop getting updates.
Set `update_url = <url of a manifest>` under `[general]` in `HitmanCoop.ini` to try an update from
somewhere else; plain http is allowed only to 127.0.0.1.

To build in a default co-op server, add `-DCOOP_DEFAULT_RELAY=relay.example.com:47100` to the
configure step. Without it, players set the server under Settings in the CO-OP window, or use
direct IP connections.

## The log

`Retail\mods\HitmanCoop.log` (the last session's is `HitmanCoop-previous.log`) has everything the
mod and its session say, one line each:

```
2026-11-03 14:02:11.123   +3261.71  INFO  net     | connected to host 'Jordan' in 0.4 s: mod 0.13.94, game 3.290.1 (ours 3.290.1)
```

wall-clock time (to line up two players' logs), the game's clock, the level, a category and the
text. IP addresses are masked (`203.0.x.x`) unless `[log] show_addresses = true`. The level is
`[log] level` in `HitmanCoop.ini`, or CO-OP > Settings > Log: `info` for players, `debug`
("verbose") for bug hunting, `trace` for one bug (large).

- `start` lines first: version and build, game version and store, Windows, CPU, memory, the mod
  loader's version, the other mods in `mods.ini`, game patches applied, all settings.
- `stats` every 10 s while connected: round trip, loss, jitter, bandwidth, messages held a frame
  or dropped, NPCs listed / ours / puppets / the partner's without data, snapshots and poses sent
  and received per second, hand-overs, puppets made and released, how far puppets drift from where
  they were put, playback delays. The first line to read about lag or NPCs out of place.
- `check` every 10 s: the sync check (each game sends where it has every NPC; the other compares):
  how many are in the same place, 1-5 m apart, over 5 m, only in one game, and the worst with why.
- `perf` every 10 s: frame rate, what the mod took of each frame, puppets and how many of their
  AI updates were skipped (warns if puppets' AI isn't being skipped).
- `npcs` when it changes: how many NPCs can be shared, how many can't (actors sharing an id).
- `net`, `sync`, `npc`, `partner`, `item`, `body`, `update`... as things happen; `debug` adds why
  (each hand-over, puppets released for want of data).

CO-OP > About > Save bug report zips both logs, crash reports, the settings (masked) and the
diagnostics into `Retail\mods\HitmanCoop-reports\`.

## Tests

- `core/build22/RelWithDebInfo/coop_tests.exe` (or `core/build/Release` in CI): the session,
  connection, protocol and NPC sharing against a fake world (`core/tests/FakeWorld.h`), lossy
  simulated networks included. Fast; run after every change.
- `python -I tools/gametest/gametest.py <scenarios>`: results (report.md, report.json, logs,
  status timeline, screenshots) in `tools/gametest/results/<time>/`.
  - `installers`, no game: fake 3.290 and 3.280 game folders (another mod already in), both
    installers (the Linux one through WSL): the right build goes in, uninstall restores the folder.
  - `netsim`, no game: the connection test's host mode and the bot through a local relay adding
    60 ms each way, jitter and 3% loss: NPC hand-over, puppets following the bot's NPCs, the sync
    check, puppets released when their stream stops, the health line.
  - `smoke`, `npc`, `restart`, the real game (`--game "<Steam>/steamapps/common/HITMAN 3"`, Steam
    copies only, the game closed): the build is installed (the game's files backed up, put back
    after unless `--keep`), the game started, driven through the test channel into Freeform
    Training, the bot joining through a local relay; `npc` stands the bot away from 47 so the host
    hands it NPCs and checks they become puppets, follow, are skipped by the AI, are in place by
    the sync check, are released when the bot stops sending, and come back. `bodies` (after
    `npc`) knocks out an NPC and moves its body as the partner's word would (`bodymove here`),
    drags it as the partner's stream would (`bodymove drag`), and shows the pause screen.

### The test channel

`plugin/Src/TestChannel.*` lets a script drive the mod. In `Retail/mods/HitmanCoop.ini`:

```ini
[general]
test_control = C:\path\to\coop-cmd.txt
```

Write one command at a time to that file (the mod deletes it once taken): `host`,
`join <code or address>`, `leave`, `relay <host[:port]>`, `restart`, `press <action>`, and the
others in `HitmanCoop::RunTestChannel`. The mod writes its state to `coop-cmd.txt.status` (JSON)
twice a second (`npc.puppets`, `npc.check`, `npc.skippedThinks`, `log.level`...), every log line
and a heartbeat to `coop-cmd.txt.trace`, and the first access violations with a stack walk to
`coop-cmd.txt.crash`.

The game ignores keyboard and mouse input injected through Windows (SendInput, PostMessage), so
scripts drive the menus through the mod: `press <action>` makes that input action read as pressed
for one frame. `KBMButtonA` is Enter, `KBMButtonB` is Back, `KBMButtonR1` is the next tab, and
`KBMButtonLeft/Right/Up/Down` move. `inputlog` traces the actions the current screen polls.

`HitmanCoop-NetTest.exe bot <code or address> [command file] [--relay host:port]` plays the
friend: it follows the host's mission, circles the host's 47 in the host's disguise, and takes
commands (see `RunBot` in `core/nettest/NetTest.cpp`): `kill`, `ko`, `at x y z`, `stand`,
`orbit`, `npcwalk`, `npcstill`, `npcdrop`, `npcback`, `npcs`, `quit`... It prints the session's
own log lines (`log: ...`). `HitmanCoop-NetTest.exe host [--seconds N]` is a fake host.

## Notes on the developer packages

- The SDK headers include `<directx/d3d12.h>` from DirectX-Headers, which the packages don't
  ship; `fetch-sdk.ps1` pins v1.614.0.
- fmt, bundled in the headers, needs `/utf-8`. Windows' `minwindef.h` defines `near` and `far`:
  don't use them as names.
- `IPluginInterface` uses ImPlot, but the packages' `ZHMModSDK` target doesn't link `implot.lib`,
  so the plugin links it itself.

Pushing the repo to GitHub runs the same steps in `.github/workflows/build.yml` (both builds,
the core tests, the Linux installer's syntax), and the downloads show up as a workflow artifact.

## Updating the SDK

Change `$SdkVersion` and both SHA-256 values in `tools/fetch-sdk.ps1`. Get the digests from
`gh api repos/OrfeasZ/ZHMModSDK/releases/tags/<tag> --jq '.assets[] | .name + " " + .digest'`.
Then rebuild and rerun the core tests. When the game updates, the build for the version before
can stay as another `COOP_SDK` (as 4.1.1 did for 3.280).
