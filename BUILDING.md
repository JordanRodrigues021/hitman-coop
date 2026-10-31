# Building

## Requirements

- Visual Studio 2022 (or its Build Tools) with the "Desktop development with C++" workload.
  ZHMModSDK's headers need C++23. VS 2019 can build and test `core/` but not the plugin.
- CMake 3.20+ (it comes with Visual Studio).
- PowerShell 5.1+.

## Steps

```powershell
# 1. Download ZHMModSDK v4.2.1 (developer package + release) and DirectX-Headers, check SHA-256
./tools/fetch-sdk.ps1

# 2. Configure and build the mod and the connection test against the developer package
cmake -S . -B _build -G "Visual Studio 17 2022" -A x64 -DZHMMODSDK_DEVPKG_DIR="$PWD/_deps/devpkg"
cmake --build _build --config RelWithDebInfo
cmake --install _build --config RelWithDebInfo --prefix _build/install

# 3. Check the DLL will load next to the shipped SDK (exports, imports, SDK ABI, no VC++ runtime)
./tools/check-dll.ps1

# 4. Make the player downloads in _dist/: the zip and the one-file HitmanCoop-Setup-<version>.exe
#    (the setup is compiled with the C# compiler that ships with Windows; no extra install), and
#    _dist/update-<version>/ for the mod's self-update. Write installer/WHATSNEW.txt first: it is
#    what players see in game after updating, and packaging refuses to run without this version in it.
./tools/package.ps1

# 5. Check what was packed is what was built, at this version, for the setup, the zip and the update
python -I tools/check-release.py <version>
```

A release has three files, so players see what to download: the setup, the zip, and
`_dist/update-<version>/update.txt` (label them; the checksums go in the release notes). All of
`_dist/update-<version>/` (`update.txt` and the game files it lists) also goes on the repo's
`updates` branch, one commit replacing the last, with the branch's own README saying it's not for
players. Since 0.13.2 the mod reads `update.txt` from that branch first (GitHub's raw file server)
and from the latest release only if that fails: on a player's connection Windows couldn't reach
the server GitHub sends release downloads to. 0.13.0 and 0.13.1 read only the release's.

```powershell
# in a fresh folder: the files of _dist/update-<version>/, plus that README
git init -b updates; git add -A; git commit -m "Update files for HITMAN Co-op <version>"
git push --force https://github.com/JordanRodrigues021/hitman-coop.git updates
```

Push the branch before publishing the release, and wait until
`https://raw.githubusercontent.com/JordanRodrigues021/hitman-coop/updates/HitmanCoop.dll` serves
the new file (its cache keeps the old one up to 5 minutes). Installed mods read `update.txt` when
the game starts, download the files that differ from the branch, and use them from
the next start (see `plugin/Src/Updater.h`). Every release must have `update.txt`, or players stop
getting updates. Set `update_url = <url of an update.txt>` under `[general]` in `HitmanCoop.ini` to
try an update from somewhere else; plain http is allowed only to 127.0.0.1.

To build in a default co-op server, add `-DCOOP_DEFAULT_RELAY=relay.example.com:47100` to the
configure step. Without it, players set the server under Settings in the CO-OP window, or use
direct IP connections.

## Testing in the real game

`plugin/Src/TestChannel.*` lets a script drive the mod. Add this to `Retail/mods/HitmanCoop.ini`:

```ini
[general]
test_control = C:\path\to\coop-cmd.txt
```

Write one command at a time to that file: `host`, `join <code or address>`, `leave`, or
`relay <host[:port]>`. The mod writes its state to `coop-cmd.txt.status` (JSON) twice a second,
lifecycle lines and a heartbeat to `coop-cmd.txt.trace`, and the first access violations with a
stack walk to `coop-cmd.txt.crash`. The SDK's own log is only written when the game exits cleanly.

The game ignores keyboard and mouse input injected through Windows (SendInput, PostMessage), so
scripts drive the menus through the mod: `press <action>` makes that input action read as pressed
for one frame. `KBMButtonA` is Enter, `KBMButtonB` is Back, `KBMButtonR1` is the next tab, and
`KBMButtonLeft/Right/Up/Down` move. `inputlog` traces the actions the current screen polls. The
route from the title screen into Paris is: A, (wait), B (closes the IOI account popup), R1, R1,
Right, A, A, A (spoiler alert), Down, Down, A.
`HitmanCoop-NetTest.exe bot <code or address> [command file]` plays the friend: it follows the
host's mission, circles the host's 47 in the host's disguise, and kills the NPC nearest the
host when it gets the `kill` command.

Notes on the developer package (v4.2.1; the game must be one it supports, 3.290.1 as of 4.2.0):

- The SDK headers include `<directx/d3d12.h>` from DirectX-Headers, which the package doesn't
  ship; `fetch-sdk.ps1` pins v1.614.0.
- fmt, bundled in the headers, needs `/utf-8`.
- `IPluginInterface` uses ImPlot, but the package's `ZHMModSDK` target doesn't link
  `implot.lib`, so the plugin links it itself.

Pushing the repo to GitHub runs the same steps in `.github/workflows/build.yml`, and the zip
shows up as a workflow artifact.

## Updating the SDK

Change `$SdkVersion` and both SHA-256 values in `tools/fetch-sdk.ps1`. Get the digests from
`gh api repos/OrfeasZ/ZHMModSDK/releases/tags/<tag> --jq '.assets[] | .name + " " + .digest'`.
Then rebuild and rerun the core tests.

## Testing in game

Run two game instances (two PCs, or two accounts), install the zip on both, host on one
and join from the other with `127.0.0.1` or the LAN address. The SDK log (`~` → console)
prints `[HitmanCoop]` lines for every session event.
