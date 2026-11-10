# HITMAN Co-op 0.13.95: every change

[NEW] marks a new way of handling something (a mechanism the mod or its tools didn't have).

## Antivirus, SmartScreen and Chrome

1. [NEW] The setup does all of its work in C#. The old one ran PowerShell hidden with `-ExecutionPolicy Bypass`, the pattern of a "dropper" that antivirus products flag.
2. The setup has a full version resource (it said 0.0.0.0), with its publisher, description and copyright.
3. [NEW] `HitmanCoop.dll` has a Windows version resource, from one template in `core/res`.
4. `Test-Connection.exe` has a version resource too.
5. [NEW] Code signing in packaging: a .pfx certificate or Azure Trusted Signing, checked after each file is signed. `-RequireSigning` refuses to package unsigned.
6. The zip has no `Install.bat` or `Uninstall.bat` any more; it ships the native setup.
7. [NEW] Before installing, the setup checks every file against `checksums.txt`, and names any file an antivirus removed or a bad extraction lost.
8. After copying, the setup checks every file again. If one is gone, it explains it's a false alarm, says where to restore it, and has an "Open Windows Security" button.
9. Downloads come with `SHA256SUMS`.
10. README: what to do for SmartScreen, a Chrome or Edge block, or a Windows Security removal.
11. BUILDING: how to sign (Azure Trusted Signing, a certificate, SignPath Foundation), and reporting false positives to Microsoft.

## The setup

12. [NEW] A new design: one window in HITMAN style, one big Install button, the connection choice, a finish screen with "Launch HITMAN", and fixes for high-DPI screens and keyboard use.
13. [NEW] HITMAN Co-op appears in Settings > Apps, with Uninstall.
14. Updating over an older install backed up our own older files, so uninstalling afterwards put the old version's mod loader back. Fixed in the setup.
15. The same bug is fixed in `installer.ps1`.
16. The same bug is fixed in `installer.sh` (Linux and Steam Deck).
17. Uninstall restores every backup, oldest first, so the newest of the player's own files wins (it used to restore only the oldest).
18. Uninstall removes the mod's logs, crash notes, what's-new file and updater leftovers, and asks whether to keep settings.
19. [NEW] The firewall rule goes through the Windows Firewall API (no netsh command line), and is replaced instead of added again on each install.
20. Admin rights are requested only for the firewall rule, and only when the game folder needs them, and the setup resumes on its own.
21. Several HITMAN installs: a picker, instead of a console prompt. Browse checks the folder, and explains the Game Pass / Microsoft Store version.
22. [NEW] Silent mode for scripts and tests: `--silent`, `--game`, `--log`, `--uninstall`, `--register`.
23. [NEW] `--render-pages`: draws every page off screen into PNGs, to check the design without showing a window.
24. It remembers the last game folder, for updates and repairs.
25. Clear messages when Controlled Folder Access, a full disk or a locked file blocks the install.
26. Uninstalling from Apps after the game folder is gone removes the dead Apps entry. It used to stay for good.
27. It warns before replacing a newer installed version with an older one.
28. It says up front if HITMAN is running.
29. Earlier setups left their unpacked files (up to 24 MB each) in %TEMP%. Now it cleans up after itself, and removes those old leftovers.
30. Packaging checks that the setup, the manifest and the DLL's version resource all agree.
31. The C# sources are compiled as UTF-8 and kept ASCII, so no characters get garbled.

## Tailscale

32. [NEW] The setup downloads Tailscale's official installer and runs it only after Windows trusts its signature and it is signed by Tailscale Inc.
33. [NEW] It signs you in: the sign-in page opens by itself, the setup waits until Tailscale is connected, then shows your address with a Copy button.
34. [NEW] It shows the PCs online on your Tailscale network (marking shared ones), with an "Invite your partner" button.
35. [NEW] It warns about a Tailscale exit node, which can make Steam sign you out.
36. In game, the Tailscale address refreshes every 15 seconds. It used to be read once at game start, so starting Tailscale later never showed it.
37. In game, a Copy button next to your Tailscale address.
38. The Tailscale address comes from the network adapters. The name lookup used before could stall the game for seconds.
39. Joining a Tailscale address while Tailscale is off on this PC says so at once, instead of timing out after 10 seconds.
40. [NEW] The Linux installer ends with your Tailscale address, or how to set Tailscale up (Steam Deck included), plus the exit-node warning.
41. Diagnostics say whether Tailscale is connected.

## "Your account signed in on another computer"

42. The mod no longer holds back any of the game's own online events. It used to drop Kill, Pacify, BodyFound and similar events for NPCs killed only to match the partner's game. This is the first suspect since 0.13; the old way is still available behind an ini switch.
43. [NEW] An online watchdog logs every change in the game's connection to IOI's servers, by name, and counts drops.
44. During co-op, it says when the game's online session drops or signs in again, and that co-op's own connection is fine.
45. It notices when Steam signs this PC out: logged, and told to the player.
46. [NEW] It detects a copy borrowed through Steam Family Sharing, and warns.
47. [NEW] It detects both players on the same Steam account (they compare a salted hash, never the account id), and warns both.
48. The diagnostics "Online" line keeps a history of those changes, for bug reports.
49. README: a section on the error and its causes.

## Bodies

50. A body that can't be pulled to its spot no longer vanishes. It stays in view, and our AI ignores it.
51. [NEW] A stalled pull is tried again twice, 2.5 seconds apart.
52. Bodies are moved out of sight only when no player is within 25 m. In front of you they move in plain view.
53. Pushes go near the root (0.15 m up instead of 0.3 m), so bodies tumble and flail less.
54. Body speed is smoothed the same way at any frame rate.
55. [NEW] A dragged body follows where the partner's drag is heading, not where it was, so no lagging and yanking.
56. [NEW] Caught on a step, a railing or a chair, a body makes a small, speed-limited hop.
57. The time to settle grows with the distance (3–8 s), so long drags no longer count as failed.
58. A body being moved out of sight reappears when the partner picks it up.
59. A body the partner puts away stops any pull on it. The end of a pull could show it again outside the container.
60. Bodies that lie elsewhere or were put away share one "unseen to our AI" path.
61. Test commands to tune the pull from the game (`bodymove lever`, `bodymove bones`); the summary shows hops and retries.

## NPCs

62. The NPC sync-detail setting overwrote 0.13.94's moonwalk fix every frame, so games posed 10 NPCs within 35 m instead of 32 within 50 m. Normal is now 32 within 50 m, low 16 within 35 m, high 40 within 60 m.
63. [NEW] Hit claims: when our 47 shoots, throws something at or strikes an NPC the partner's game runs, our game takes it over at once. It was a puppet with its AI off and ignored knock-outs. Protocol 17, `MsgNpcClaim`.
64. [NEW] Our 47's hits are recognised through the game's own type information (`SHitInfo.m_rWeaponOwner`), so the mod's own hits don't count.
65. When the host's hit makes the claim, ownership changes at once, not at the next check.
66. [NEW] Once a second, the list of puppets whose AI is off is checked against each NPC's current runtime id. A reused id could leave another NPC frozen.
67. Puppets gone from the scene are dropped from that list.
68. The health line counts NPCs taken over by a hit.
69. Claims are cleared with the rest of the scene state.

## Connection

70. [NEW] Each reliable message backs off its resends exponentially (up to 1 s), instead of adding to the load on a bad link.
71. An address pasted with a tab, a space or a line break works (it was an "Invalid port").
72. An IPv6 address gets a clear message that co-op needs IPv4.
73. The version-mismatch refusal says where to get the matching version.

## Updates

74. [NEW] Signed updates: the mod installs an update only if its `update.txt` carries a valid ECDSA P-256 signature from the author's key.
75. Packaging signs both update manifests and refuses to go on if signing fails.
76. [NEW] `tools/update-key.ps1` creates the key once, signs and verifies.
77. Local test servers (`http://127.0.0.1`) still work unsigned; anything else must be signed.
78. The signing key's folder is never synced to GitHub.

## Bug reports, logs and stability

79. Bug reports include the setup's install log.
80. A copy button that showed its notice from the menu's thread now shows it from the game's thread.
81. The Tailscale address was read by the menu while the game thread wrote it. It's copied under the lock now.
82. The log of a Pacify event printed the JSON after the first 200 characters instead of the first 200.
83. Converting a short bone list to a pose no longer reads past its end.
84. Kill-feed and challenge text: characters from U+0800 up (curly apostrophes, Japanese names) were dropped from names. Lone surrogate halves become a replacement character.
85. Test commands `online` and `verifysig`.

## Tests

86. Protocol test: a claim round-trips; an id of 0 and a cut-short message are refused.
87. Session test: an NPC our 47 hits is ours to run at once, and goes back to the host later.
88. Socket tests: pasted addresses with whitespace, IPv6 refused clearly, the Tailscale address check.
89. Game-event test: 3-byte UTF-8 and surrogate halves.
90. Harness: the setup installs, updates without backing up our own files, and uninstalls back to the original folder, for both game versions.
91. Harness: in game, the online watchdog reports, and the mod accepts its signed `update.txt`.

## Found in the in-game test

92. The mod loader's first-start "Enable Crash Reporting?" box sat over the game's pause menu, so the game stayed paused. The setup now sets `crash_reporting = false` in mods.ini (only if you haven't set it), and the uninstall takes it out again.
93. The mod loader's "new SDK version" notice led players to update the loader, which breaks HITMAN Co-op. The setup sets `disable_update_check = true` (same rules).
94. A body's unstick hop could fire while the body was still getting going, and lift it onto a table. Hops now wait 1.5 s, need a second without progress while standing still, are gentler, and stop at 2.
95. "Near enough when time is up" also needs the body on the right floor (it accepted one lying on a table).
96. The body log lines say how far the body really moved.
97. Harness: answers the mod loader's prompt (the cause of 0.13.94's "knock-out did nothing" runs), measures how far bodies moved, and reads the 8-wide log category.

## Tried and taken back

- Timing the round trip only from direct acks: the chaos test showed NPCs drifting 2.73 m (limit 2.5 m), because playback is tuned on the old estimate. Reverted; not counted above.
