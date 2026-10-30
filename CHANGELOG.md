# Changelog

All notable changes to HITMAN Co-op, created by **Jordan Rodrigues**.
Both players always need the same version.

## 0.12.1 (2026-10-30)

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
