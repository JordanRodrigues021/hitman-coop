=====================================================
   HITMAN CO-OP 0.12.0
   * created by Jordan Rodrigues *
=====================================================

Two-player co-op for HITMAN World of Assassination. One player hosts, the other joins.
No IOI, Peacock, Steam or Epic servers are used: it works with the game online or offline.

INSTALL (both players, the same version)
  1. Close the game.
  2. Run HitmanCoop-Setup-0.12.0.exe and click INSTALL.
     (Windows may say "Windows protected your PC" because the file isn't signed:
     click "More info", then "Run anyway".)
     Prefer a zip? Extract HitmanCoop-0.12.0.zip and double-click Install.bat.
  The setup finds Steam, Epic and Heroic/Legendary installs by itself, or you pick
  the game folder (the one with Retail inside; picking Retail itself works too).
  It writes what it did to Retail\HitmanCoop-install.log.

PLAY
  1. Both start the game and press ~ (the key under Esc; ^ on German keyboards).
  2. Click CO-OP in the bar at the top.
  3. Host: click Host. The window shows this PC's addresses (Tailscale and local).
  4. Friend: type the host's address under "Join a friend" and click Join.
     Over the internet, both install Tailscale (tailscale.com) and use the host's
     Tailscale address (100.x.x.x). On the same network, the local address works.
  5. Host starts a mission. The friend starts any mission at the SAME LOCATION
     and is taken to the host's exact mission there.

WHAT IS SHARED
  - One world: every NPC, guard, bodyguard and target is run by one of the two games
    (the player nearer to it) and shown exactly the same, moving and animating, in
    the other. Kills, knock-outs and bodies being dragged too.
  - Pause: when one player opens the pause menu, both games pause.
  - Restart Mission: one player restarts, both restart (same mission only).
  - Your partner in the level, in their disguise, moving and animating like they do.
  - Items: one player takes an item, it's gone for the other; dropped, it reappears.
  - Doors: opened or closed by one player, for both.
  - NPC alert levels: guards alerted in one game are alerted in the other too. A
    partner's crime never makes your guards hunt or shoot you.
  - Mission complete / failed and target kills.
  Your partner's stand-in is invisible to the AI: it never reacts, speaks, joins a
  search, witnesses anything or sees through your disguise, and can't be subdued.

TOOLS FOR PLAYING TOGETHER (new in 0.9)
  - HUD (top right): your partner's name, ping (green / yellow / red), how far away
    they are, and whether they are loading, paused or in the menus.
  - Name tag: your partner's name and distance over their head; when they are off
    screen, an arrow at the edge of the screen points the way.
  - Marks: aim at someone (or at the floor nearby) and press the MIDDLE MOUSE BUTTON.
    A marker appears on both screens for 20 seconds and follows the person marked;
    off screen, an arrow points to it. Change the key under Settings (MOUSE4, MOUSE5,
    a letter, F1-F12, or OFF).
  - Chat: type in the CO-OP window, or click a quick message ("Wait for me.",
    "Target down." ...). Your partner sees it on their screen.
  - Partner alerts: you're told when your partner is spotted, trespassing, caught,
    in combat or hunted, when their disguise is blown, and when a body is found near
    them.
  - Regroup: a button in the CO-OP window puts your 47 right behind your partner's.
  - Reconnect: if the connection drops, the joining player reconnects by itself
    (up to 6 tries in about a minute) and is put back into the mission.
  - Loading screens never time you out: each game tells the other when it is loading.
  - Settings: pause together on/off, HUD, name tag, marks, notifications.

NEW IN 0.11
  - A new CO-OP window: tabs for Play, Chat, Mission, Settings, Hotkeys and About,
    a status line at the top, always fully on screen, with a text size setting.
  - Hotkeys tab: every action and its key; click Change and press any key (with
    Ctrl, Shift or Alt for a combination), Clear, Default, Reset all. Keys used
    twice are shown in red.
  - Teamwork: ready check, a 3-2-1 countdown that reaches GO on both screens at the
    same moment (for a double hit), "come to me", and signals (OK, wait, help, look
    here) shown over your head on your partner's screen.
  - Your own colour for your name tag, marks and radar dot.
  - Your partner shows as away when they tab out, and the HUD says when they
    haven't moved for a while.
  - Marks hit what you aim at (walls, doors, balconies) with the game's own
    collision; up to 3 marks each, and how long they last, under Settings.
  - Compass strip, radar range and size, and an optional target assist.
  - Your partner's map or notebook needn't pause your game (Settings).
  - Mission summary panel at the end, and your co-op record across sessions.
  - Write your own quick messages. Chat with times, an unread counter and Clear.
  - Feet or metres, colour-blind friendly colours, name tag size, notification
    length and which notifications show.
  - Streamer mode hides join codes and IP addresses; Copy invite; the join code
    goes to your clipboard by itself.
  - NPC sync detail (low / normal / high), the host can remove a partner,
    Reconnect now, and the partner's mod version shown.
  - Shooting your partner's stand-in by mistake tells you both (no harm done).

NEW IN 0.10
  - Your partner's status on the HUD: how alert the NPCs around them are (noticed,
    suspicious, hunted, being arrested, in combat), whether they are trespassing,
    their disguise, which menu they have open (the map, the notebook ...), and how
    far their loading has got. Their name tag turns orange or red with it, and says
    when they are a floor or more above or below you.
  - Radar under the HUD: your partner and both players' marks around you.
  - Danger marks: press the mark key twice quickly to warn your partner (a red,
    pulsing mark). Press it again on the same thing later to take your mark back.
    Targets are marked TARGET. A mark on someone who goes down clears by itself.
  - Kill feed: who your partner eliminated or knocked out (target, accident,
    headshot), and the challenges they complete.
  - Silent Assassin watch: the HUD tells you if the team is still on track, and why
    not (a body found, someone spotted, a non-target killed, caught on camera).
  - Mission panel and debrief: both players' kills, knock-outs, times spotted and
    more, side by side in the CO-OP window, and a summary when the mission ends.
  - Start together: when your mission begins while your partner's game is still
    loading it, yours waits paused for them (up to 90 s; a button starts without).
  - "Where are you?" (button or Num 9): both players' name tags pulse.
  - Number pad hotkeys (Num Lock on): Num 1-8 quick messages, Num 0 regroup.
  - Your partner's pause can't freeze your game for good: after 2 minutes yours
    goes on.
  - Connection quality: packet loss and jitter on the HUD and in Connection details,
    a ping graph, and a warning when your partner hasn't been heard from for a few
    seconds.
  - Sounds for your partner's messages, marks and warnings (can be turned off).
  - HUD position (any corner), size and background in Settings.
  - Notifications history, Paste button and recent join codes, and "Copy
    diagnostics" for bug reports.
  - On a slow PC the mod does less work per frame by itself.
  - The previous session's log and crash report are kept (HitmanCoop-previous.log).

NOT SHARED YET
  - Items in your partner's hands, and alarms.
  - Saves: loading a save restarts your partner's mission from the start (the save
    itself isn't sent). Both players' unlocks and progress stay their own.
  - What an NPC says is heard in the game that runs it (the other game shows the
    NPC talking without its voice).

COMPATIBILITY
  - Steam and Epic (also via Heroic/Legendary) versions: yes.
  - Microsoft Store / Game Pass version: no. Windows locks its files, so no mods can
    be installed into it. The setup tells you so.
  - Game versions: this build was tested on 3.280.0. After a game update the mod
    loader (ZHMModSDK) checks the game when it starts and turns mods off if they
    don't fit yet, so the game always still runs; install the HITMAN Co-op build made
    for the new version then. The co-op window says if anything it needs is missing.
  - Different HITMAN Co-op versions: the host refuses the join and both see which
    versions they have. Different game versions: you get a warning.
  - Different editions (for example full game + Part One only): you play together at
    locations you both own (with Part One: ICA Facility, Paris, Sapienza, Marrakesh,
    Bangkok, Colorado and Hokkaido). The mod never sends anyone to a location they
    didn't start themselves.
  - Another mod loader or other SDK mods already installed: the setup installs the
    loader version HITMAN Co-op needs, backs up yours, and puts it back on uninstall.
  - Steam Deck / Linux (Proton): set the game's launch options to
      WINEDLLOVERRIDES="dinput8=n,b" %command%
    so the mod loader is used.

TEST YOUR CONNECTION (no game needed)
  In the setup window, click "Test connection" (or run Test-Connection.exe).

UNINSTALL
  Run the setup again and click Uninstall (or double-click Uninstall.bat). The game
  folder is put back exactly as it was: the mod removed, a mod loader we added
  removed (kept if other mods need it), one we replaced restored.

HITMAN Co-op is (c) 2026 Jordan Rodrigues, free software under the GPL v3.
It uses ZHMModSDK by OrfeasZ. Licenses are in the licenses folder.
