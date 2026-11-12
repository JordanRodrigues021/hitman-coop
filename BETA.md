# HITMAN Co-op: the beta branch

This branch has the code of the current **beta**. `main` has the last stable release (0.13.2).
Betas go to the testers in the Discord's #beta-builds first, and come to `main` once they hold up.

## The beta builds

| Beta | Downloads | What's in it |
|---|---|---|
| **0.13.95** (current) | [v0.13.95-beta](https://github.com/JordanRodrigues021/hitman-coop/releases/tag/v0.13.95-beta) | A new setup that does everything itself (no PowerShell or batch files) and sets up Tailscale; Settings > Apps entry; signed updates; checks for the causes of "signed in on another computer" and no longer holds back the game's online events; bodies that never vanish and follow drags closely; NPC detail setting fixed (32 NPCs animated within 50 m); NPCs you hit are run by your game; about 100 fixes in all ([list](CHANGES-0.13.95.md)) |
| 0.13.94 | [v0.13.94-beta](https://github.com/JordanRodrigues021/hitman-coop/releases/tag/v0.13.94-beta) | Bodies the partner drags follow them and lie where they leave them; bodies and dropped items in the same place for both; every NPC near you animated (no more blinking or moonwalking); late network messages no longer thrown away; the pause screen |
| 0.13.93 | [v0.13.93-beta](https://github.com/JordanRodrigues021/hitman-coop/releases/tag/v0.13.93-beta) | One download for game 3.290 and 3.280; verbose logs and bug reports; Linux / Steam Deck installer; NPC fixes; the test harness |

Each beta is a GitHub **pre-release** (so installed copies don't update to it by themselves) with
the Windows setup, a Windows zip and a Linux / Steam Deck tarball. Each download holds both builds
(game 3.290.x and 3.280.x) and installs the one for the game it finds. Both players need the same
beta: they refuse each other's network protocol otherwise (0.13.95 is protocol 17).

The full list of changes, for players: [installer/WHATSNEW.txt](installer/WHATSNEW.txt).

## For developers

- Building, the two game versions, the log, the tests and the test channel: [BUILDING.md](BUILDING.md).
- `core/` (network, session, NPC sharing) builds and tests without the game:
  `core/build22/RelWithDebInfo/coop_tests.exe`, then `python -I tools/gametest/gametest.py netsim installers`.
- In the game (a Steam copy, the game closed): `python -I tools/gametest/gametest.py --game "<Steam>/steamapps/common/HITMAN 3" smoke npc bodies restart`.
  Results, logs and screenshots land in `tools/gametest/results/<time>/`.
- Signed updates (0.13.95 on): the mod only installs an `update.txt` with a valid `update.txt.sig`
  (BUILDING.md, "Signed updates"); `tools/package.ps1` signs with the maintainer's key.
- The setup: `installer/setup/*.cs` (C# 5, built by `tools/package.ps1` with the compiler in Windows).
  `HitmanCoop-Setup.exe --render-pages <folder>` draws every page into PNGs without a window;
  `--silent --game <folder> [--uninstall]` for scripts.
- Bug reports from players: CO-OP > About > Save bug report, on both PCs. The log
  (`Retail\mods\HitmanCoop.log`) has a health line (`stats`) every 10 s and a sync check (`check`).

Pull requests for beta work go against this branch.
