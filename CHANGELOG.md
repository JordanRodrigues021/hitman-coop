# Changelog

All notable changes to HITMAN Co-op, created by **Jordan Rodrigues**.
Both players always need the same version.

## 0.13.2 (2026-10-31)

If you have 0.13.0 or 0.13.1, it updates itself the next time you start the game. Plays with
0.13.0 and 0.13.1 (same network protocol).

**Fixed**
- **Bodies turned invisible by themselves** after a short while, though nobody had hidden them, and
  could still be dragged. A body that slid or settled on its own (each game's physics does that its
  own way) was taken for one your partner had moved, and hidden in their game. Now only a body a
  player actually drags, dumps or hides is shared.
- **Updates didn't download on some internet connections.** GitHub sends release downloads to a
  file server that, on some connections, has an address Windows can't reach, and Windows kept
  trying that one. The mod now gets its updates from the address GitHub serves the repository's
  files from, and tries the release only if that fails. If your 0.13.0 or 0.13.1 doesn't update by
  itself after a couple of game starts, install 0.13.2 by hand once.

## 0.13.1 (2026-10-31)

Fixes from the first reports on 0.13.0. If you have 0.13.0, it updates itself the next time you
start the game. Plays with 0.13.0 (same network protocol).

**Fixed**
- **Stripped bodies kept their clothes on your partner's screen.** The game leaves a body's outfit
  as it is when 47 takes it, and sets it again "nude", so the mod never saw a change. It now
  catches the game's own call and does the same in your partner's game.
- **A guard arresting you could freeze halfway, and the game crashed.** When you stand next to your
  partner, the host's game runs the NPCs around you both. A guard arresting, hunting or fighting
  you is now run by your own game, wherever you stand, until it calms down.
- **A body your partner put in a container could still be dragged in your game**, with its
  knock-out stars over it, and guards noticed it while it was dragged. It can't be dragged or
  grabbed any more, and the stars are gone.
- **Items your partner took from a table stayed there in your game** (like the ones at the start
  of Freeform Training). Those are drawn another way, which is now hidden too.

**Known limits**
- A body your partner drags still disappears in your game instead of moving (the game doesn't let
  a mod move a ragdoll body).
- A crash when leaving a mission was reported, without its crash file, and couldn't be reproduced
  here. If it happens to you, please send `Retail\mods\HitmanCoop-crash.txt`.
- Tested on game version 3.290.1 in Paris with the test partner; the arrest fix by the tests of
  the mod's core, as it needs two real players.

## 0.13.0 (2026-10-31)

Game version 3.290.1 moved things around inside the game, and a lot of the mod quietly stopped
working on it. This version is made for 3.290.1. **Both players need 0.13.0** (it can't play with
0.12.x).

**Fixed**
- **Items weren't shared at all**: picking up, dropping, throwing, and what your partner holds. The
  game update moved its list of items, so the mod saw none. It now finds the list where it is.
- **Outfit changes didn't show on your partner's screen**, and your partner never got your outfit.
  The mod loader HITMAN Co-op comes with, ZHMModSDK, is updated from 4.1.1 to 4.2.1, which knows
  where 3.290.1 keeps 47's outfit.
- **Taking a disguise off a body didn't show on your partner's screen.** The body now changes
  clothes on both screens.
- **Bodies your partner hid could still be found by guards in your game.** They are now also hidden
  from the AI, not only from sight.

**New**
- **HITMAN Co-op updates itself.** When the game starts, it checks GitHub for a new version and
  downloads only the files that changed, each checked by its SHA-256. The new version is used the
  next time you start the game, what's new is shown on screen, and the old files are deleted. Turn
  it off under CO-OP > About. (0.12.x can't update itself, so this one time, install 0.13.0 by hand.)

**Changed**
- Network protocol 13.

**Known limits**
- Tested on game version 3.290.1 in Paris with the test partner: items, outfits, stripped and hidden
  bodies, and the self-update end to end. Whether guards now ignore a hidden body, the item in your
  partner's hand while they really play, and dragged bodies couldn't be tried without a second
  player. A body your partner drags still disappears in your game instead of moving (the game
  doesn't let a mod move a ragdoll body).

## 0.12.2 (2026-10-31)

For players who were connected, with kills, marks and NPCs shared, but couldn't see each other's
47 at all. Plays with 0.12.0 and 0.12.1, but please update both PCs.

**Fixed**
- **Your partner's 47 didn't show at all, on either PC.** Before showing your partner, the mod
  needs to know their outfit. When it couldn't name the outfit 47 wears (likely with some unlocked
  or DLC suits), no outfit was ever sent, and your partner was never shown, on both PCs, without a
  word. Outfits are now also named by the outfit's own id, or the one it's based on, and if neither
  outfit is known after 3 seconds your partner is shown anyway in civilian clothes (with 47's
  head), with a message saying so.
- **The 0.12.1 download contained 0.12.0.** The mod inside it was the old one, so it said 0.12.0
  and none of the 0.12.1 changes were there. 0.12.2 contains them, and the release build now checks
  that the files it packs are the version being released.

**Changed**
- The log names an outfit that can't be named, and how one was found, to help with bug reports.

**Known limits**
- Tested on game version 3.290.1 in Paris and the ICA Facility with the test partner, including a
  partner whose outfit isn't known. The players' suits themselves couldn't be tried here: if you
  still can't see your partner, please send About > Copy diagnostics and `HitmanCoop.log` from
  both PCs.

## 0.12.1 (2026-10-30)

*The 0.12.1 download contained the 0.12.0 mod by mistake; these changes first shipped in 0.12.2.*

For a player who couldn't see their partner's 47 at all. Plays with 0.12.0 (same network protocol),
but updating both is best.

**Fixed**
- Your partner could stay invisible for good. Their stand-in is hidden for a moment while it is
  matched to their 47's skeleton; if that match never came, it never showed. Now it is shown
  anyway after 3 seconds.
- Your partner could fail to show without a word while the mod waited for something (their outfit,
  an outfit to dress the stand-in in, the game's NPC template). The log now says what it waits for
  after 10 seconds, and you're told on screen after 20.

**Changed**
- While your partner wears one of 47's own suits, you see them in civilian clothes with 47's head
  (a suit on their stand-in crashes the game). You're now told so once a mission, with a reminder
  to look for their name tag, so you don't take them for one of the crowd.
- About > Copy diagnostics has a new "Partner's 47" line: shown, hidden, or what it's waiting for,
  with distance and the outfit worn. The log names the outfit the stand-in was given.

**Known limits**
- Tested in Paris (game version 3.280.0) with the test partner. The 3-second fallback couldn't be
  triggered with the test setup. If you still can't see your partner, please send the diagnostics
  and `HitmanCoop.log` from both PCs.

## 0.12.0 (2026-10-29)

Fixes for the first reports from players. Thank you to everyone who tested 0.11.0 and wrote in!

**Fixed**
- **New York and Whittleton Creek main missions told both players they weren't in the same
  mission.** These missions (and Miami, Mumbai, Santa Fortuna, Isle of Sgail, Hawke's Bay and
  Himmelstein) add extra level content picked by each PC's number of CPU cores, so two different PCs
  never matched. That content is now left out of the comparison, and when you're taken to your
  partner's mission your game keeps the setting for your own CPU. The order of a mission's parts no
  longer matters either.
- **Dragged and hidden bodies had to be dealt with twice.** A body your partner drags away, or hides
  in a closet or container, is now hidden in your game too, so your guards don't find it. Dragging a
  body your partner knocked out is now noticed as well (it was skipped before).
- **Crash when reloading a mission.** Your partner's restart used to restart your game at once, even
  while your own restart or load was under way, or while your game was paused for them. It now waits
  until your mission is playing and lets go of the pause first, and is skipped if your mission is
  already reloading.
- **NPCs witnessed a takedown even when both players took down a pair at the same moment.** An NPC
  your partner has hold of now stops reacting in your game until they let go.
- **Weapons and items were invisible in your partner's hands.** What your partner holds is now given
  to their stand-in, in its right hand.

**Changed**
- Starting a mission on a different difficulty from your partner is now told as such ("picked
  different difficulties") instead of "a different mission".
- Location names in messages are right for Whittleton Creek, Isle of Sgail, Hawke's Bay, the sniper
  maps and the safehouse.
- The mod log lists each mission's parts as it loads, to help with bug reports.
- Network protocol 12 (0.11.0 and 0.12.0 can't play together).

**Known limits**
- A body your partner moves to a spot that isn't hidden disappears in your game, instead of showing
  where they left it: the game doesn't let a mod move a ragdoll body.
- Tested in Paris (game version 3.280.0) with the test partner. The HITMAN 2 locations, real drags
  into closets and the item in your partner's hand while they really play couldn't be tried with the
  test setup yet. Please report anything that looks wrong!

## 0.11.0 (2026-10-28)

First public release.

- A new CO-OP window with tabs (Play, Chat, Mission, Settings, Hotkeys, About) that always fits the screen.
- **Hotkeys tab**: see and rebind every action (with Ctrl, Shift or Alt), clear, reset, clashes shown in red.
- Teamwork: ready check, a 3-2-1 countdown that hits GO on both screens together, "come to me",
  signals over your head.
- Your own colour; away and idle indicators; friendly-fire notice.
- Marks hit walls, doors and balconies; up to 3 marks each; mark duration.
- Compass, radar range and size, optional target assist; your partner's map needn't pause you.
- Mission summary, co-op record, custom quick messages, chat with times and unread counter.
- Feet or metres, colour-blind colours, overlay size, notification filters, streamer mode, copy invite.
- NPC sync detail setting, remove a partner, reconnect now, mod version check.
- Fixes: settings saved safely from one thread, no IP address in diagnostics, accented names, long
  notifications wrap, faster per-frame status.

## Before 0.11.0

Development versions, not released publicly: shared NPCs, kills and knock-outs, the partner's
stand-in with their pose and disguise, doors and items, pause, restart and start together, join
codes through a relay server, the HUD, name tag, radar, marks, chat, reconnect, and the installer.
