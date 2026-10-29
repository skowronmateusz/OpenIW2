# Campaign flow: objectives, compass, saves, level changes, menus (work in progress)

Evidence from `CoD2SP_s.exe` (1.3) read with REA. Built-ins Ghidra did not create as functions in the PE analysis
were decompiled from raw x86 slices (`open_binary format=raw-binary entry_point=<va>`); in that output strings
appear as bare addresses, resolved with `read_bytes`.

## Objectives (`objective_*`, 0x4f12d0–0x4f2140)

- 16 objectives, index 0–15 (`index %i is an illegal objective index. Valid indexes are 0 to %i`). Each is stored as
  server configstring `13 + index` (bounds check against 0x7ff), an info string with keys `state`, `org%d`
  (positions 0–7, `"%i %i %i"`), `ring`, the text and the icon.
- States (FUN_004f1160, six script-string compares → 0..5) in the order of the error message
  `Valid states are "empty", "active", "invisible", "done", "current", "failed"`: empty 0, active 1, invisible 2,
  done 3, current 4, failed 5 (the order is read from the message; the string-constant identities are not yet read).
- `objective_add(index, state [, string [, position [, icon]]])`: at least two parameters. Existing fields are kept
  (the configstring is read and updated). String: a localized reference (`&"..."`, only alnum and `_`) or a plain
  string; max 1023 chars; `"`, `;` and `\` rejected; characters outside 0x20–0x7e rejected. Then the client gets
  `menu_show_notify "2"` and the state message below with the objective text (or `GAME_OBJECTIVESUPDATED` when no
  string was given).
- `objective_state(index, state)`: sets the state, message with `GAME_OBJECTIVESUPDATED`.
- State message (FUN_004f10b0): active/current → client command `obj_update "<text>"`; done →
  `obj_complete "GAME_OBJECTIVECOMPLETED"`; failed → `obj_update "GAME_OBJECTIVEFAILED"`; others nothing. A
  `current` message within 100 ms of the previous message is dropped.
- `objective_string` / `objective_string_nomessage` → FUN_004f18c0(1 / 0) (not decompiled yet).
- `objective_icon(index, icon)`: icon name max 63 chars, 0x20–0x7e.
- `objective_position(index, pos)` sets `org0`; `objective_additionalposition(index, n, pos)`,
  n 0–7, sets `org<n>`.
- `objective_current(i, …)`: every listed index becomes `current`(4), every unlisted index that was `current`
  becomes `active`(1); then `GAME_OBJECTIVESUPDATED` message. `objective_additionalcurrent(i, …)`:
  listed ones become `current`, others untouched.
- `objective_ring(index)`: toggles the `ring` key (`ring` = 1 when it was 0/missing).
- `objective_delete(index)`: clears the configstring.

## Compass (hud.menu "Compass", ownerdraws 84–89)

Cvars (registration FUN_004a10f0): `cg_hudCompassSize` 1.0, `cg_hudCompassMaxRange` 1500,
`cg_hudCompassMinRange` 0, `cg_hudCompassMinRadius` 0, `cg_hudCompassSpringyPointers` 0,
`cg_hudCompassSoundPingFadeTime` 2.0, `cg_hudObjectiveMinHeight` -4000, `cg_hudObjectiveMaxRange` 2048,
`cg_hudObjectiveMinAlpha` 1.0, `cg_hudObjectiveNumRings` 5, `cg_hudObjectiveRingTime` 10000 (ms),
`cg_hudObjectiveRingSize` 64, `cg_objectiveListWrapCountStandard` 600, `…Widescreen` 530.

Objective pointers (CG_PLAYER_COMPASS_POINTERS, FUN_004a9310; constants at 0x591e00:
0x591e10 = 0.5, 0x591e20 = 1.0, 0x591e84 = 16, 0x591f50 = π/180, 0x591f10 = 360/65536):
centre = (x + w·size·0.5, y + 0.5·size·h − (size − 1)·h). For each objective in state `current`, for each non-zero
position of its 8: d = 2D distance from the view origin; alpha = (minAlpha − 1)·(clamp(d, maxRange, objMaxRange) −
maxRange)/(objMaxRange − maxRange) + 1 (0 when the range is empty); radius = size·((1 − minRadius)·(clamp(d,
minRange, maxRange) − minRange)/(maxRange − minRange) + minRadius)·K (K = float at 0x591e90, still to be read
exactly); icon 16·size square at centre − (sin a, cos a)·radius, a = relative yaw in degrees (a 16-bit angle times
360/65536; how the view yaw is subtracted is TODO). Material: the objective icon if set,
else the default registered material (name TODO). Ring: when numRings > 0, ringTime > 0 and the ring start time is
set and not older than ringTime: period = ringTime/numRings, frac = phase within the period, alpha = 2·frac up to
0.5 then 1 − 2·(frac − 0.5), size = frac·ringSize·size (halved after the first ring). Alpha is capped by the HUD
fade value. TODO: compass face rotation (ownerdraw 84), back (85, FUN_004a90b0), actors/tanks (87/88).

## Saves (0x4f3a30–0x4f3db0, FUN_0044a140)

Superseded in detail by docs/spec/savegame.md sections 2-6 (implemented: src/game/savestate.cpp).

- `savegame(name [, description [, image]])` and `savegamenocommit(...)`: error `Attempting to save while dead` when
  the player is dead. Empty name → `auto`. File: `autosave\<mapname>` or `autosave\<mapname>-<name>` (choice by
  FUN_0043f6d0; that `levelstart` selects the short form is inferred from the adjacent `levelstart` string, TODO
  verify). Description default empty, image default `$default`.
- FUN_0044a140: ignored while `g_reloading` (−1, script error `Attempting to save during a restart`);
  for no-commit saves only one per frame (−2, `Attempting to save multiple times in a single frame`); at most 3
  pending saves (−3). `savegamenocommit` returns the save id.
- `commitsave(id)`: errors `Attempting to commit an invalid save`, `Save Commit attempted on incorrect save
  currentsave:%i, commit:%i`, `Save Commit attempted with no valid committable save available`. Byte 0x1b15ac1 (set by the engine's
  own commit of a `savegame()` write): while set, commitsave clears it and returns silently, before these checks.
- `issavesuccessful()`, `issaverecentlyloaded()` read engine flags.
- Console commands registered by FUN_00447370: `loadgame`, `loadgame_continue`, `nextlevel`,
  `difficultyEasy/Medium/Hard/Fu`, `savegame_lastcommit`, `map_restart`, `fast_restart`, `spmap`, `devmap`.
  The handlers are not read yet.
- Client commands (FUN_004a6ad0): `gamesavestart`, `gamesaved`, `gamesavefailed`,
  `opendeadscreen`, `closedeadscreen`, `obj_update`, `obj_complete`, `menu_show_notify`, `openvictoryscreen`.

## Level changes

- `missionsuccess(nextMap [, persistent])`: more than two parameters is an error; then the `changelevel` code and a
  flag. `changelevel(map [, persistent [, exitTime]])`: only with a live player;
  stores the map (63 chars) and an exit time in ms (`exitTime cannot be negative`). `missionfailed()`.
  `cinematic(name [, exitTime])`. `setsavedcvar` only for archived-flag dvars.
- The order of levels is script logic (`maps/_endmission.gsc`). `game[]` survives ONLY a persistent change: G_ShutdownGame
  (0x4d9ce0, 0x4d9d3c) calls 0x46e1c0 with "free" unless the change is persistent (0x1c60ff0), and 0x4689e0 then frees the
  game object (0x9bef30, pushed by the VM's `game` opcode 0x46980d, re-created by G_InitGame 0x4da481). So after
  `missionSuccess("demolition", false)` demolition starts with an empty `game[]`: `_loadout.gsc` sees no `gaveweapons` and
  gives the SVT40 / PPSh loadout. A persistent change also writes the client into a memory save (0x4d9dd1 -> 0x4e6c20) that
  carries the player's weapons into the next level ("weapons carry over but are REFILLED", `_loadout.gsc`).
- When the change falls due (0x4d9f90; due time 0x1c60f54 = now + exit + 1000 ms, set by 0x4d9da0): the movie-pending flag
  0x1c60fe4 (set by `missionsuccess` 0x4f4066 and `cinematic` 0x4f4220) is cleared and either the stored cinematic plays
  (`cinematic <name> 0`, its end loads the map) or, without a name, the server sends the client `openvictoryscreen`
  (0x4da008): the level is not left. Without that flag: `loadgame_continue` when the failed flag 0x1c60fe8 is set, else
  0x4d9f00 (`spmap <map>`, `spdevmap <map>` with sv_cheats 0x571290, `disconnect` without a stored map).
- 0x4d9da0 with the movie-pending flag sends `setchannelvol 1 <index> <exit+1000>` (0x582bf4) instead of `snd_fade`: index
  = configstring of the shock file "victoryscreen" (0x4d5860, base 0x45d, 16 entries), i.e. the channel volumes of
  shock/victoryscreen.shock go into sound volume slot 1.

## Victory screen and the next level

- Client `openvictoryscreen` (0x4a6d6a): channel pause flags 0x793d6f = all but menu and music (0x4429e0), volume slot 1
  cleared (0x443390(1)), then 0x4022b0("victoryscreen") -> UI_SetActiveMenu(15) (0x4c679e: no close-all, cursor (320, 448),
  key catcher 8) — only while the client is active (state 4) and no timedemo plays. The menu (ui/victoryscreen.menu, listed in
  ui/ingame.txt) pauses the game in onOpen (`cl_paused 1`), shows `$victoryBackdrop` (maps/<map>.csv) and the dvar
  `ui_victoryquote` (`maps/_quotes.gsc` setVictoryQuote, a random `@VICTORYQUOTE_*`), and offers by `ui_nextMission` (set
  by `_endmission.gsc`: 1 next level, 0 mission done, 2 campaign done) "Continue mission" / "Next mission" (menu command
  `nextlevel` 0x4c8e10 -> 0x4d9f00), "Mission select" and "Main menu".
- Sound pause: when cl_paused changes (0x442dc0 / 0x442df0 / 0x444690) the samples of channels whose flag is set pause
  (0x442a30) or resume (0x442bf0); the flags reset to "all but menu" (0x793d64) on sound init and client connect.
- Map load: 0x40ce90 loads maps/<map>.csv (0x4d0770: rows `key,material` -> material alias `$key`, lower case, 0x4d0700) and
  sets client state 5; the cgame load progress opens the load screen (0x4a300d / 0x4b9215 -> 0x4022b0("briefing") ->
  UI_SetActiveMenu(13) 0x4c6730: close all, open `briefing`, `$levelBriefing` + loadbar). When the load ends (0x449a2c):
  with `ui_autoContinue` (bool, default 0, flags 0x1000, registered 0x4c32c4) or a timedemo (0x4c5530) the game starts
  (0x4c54c0: cl_paused 0, console `fade 0 0 0 0 3`, close all), else cl_paused 1, close all, open `pregame` ("click to start
  the mission"; the click runs uiScript `playerstart` -> 0x4c54c0).

## Death screen

- Player death: `player_die` (0x527d80) sets the client respawn time to level time + 1700 ms (0x527e85). The client
  think (0x4e7eb7) calls the death screen 0x4e8e10 while pm_type ≥ 4 and level time > respawn time.
- `missionfailed()` (0x4f4080): when a player entity is in use (0x4d7850), calls 0x4e8e10 and sets 0x1c60fe8; a
  pending changelevel then runs `loadgame_continue` instead of leaving the level (0x4d9f90).
- 0x4e8e10, only while `g_reloading` is 0 (it sets it to 1): `loadgame_continue` is due `g_deathDelay` ms later
  (int dvar, default 3000, 0..INT_MAX) in real time (timeGetTime; checked each server frame, 0x4d9f90). Server
  commands to the client: `snd_fade 0 <d>`, `clear_blur`, `scr_blur <d> 32 1 2`, `time_slow <d> 1 0.1`, and
  `opendeadscreen <ui_deadquote>` when the dvar `ui_deadquote` exists.
- Client (0x4a6ad0): `snd_fade <vol> <ms>` (0x4431a0) moves the volume to `vol` at (vol − current)/ms per ms;
  `time_slow <ms> <from> <to>` (0x4bec20) lerps the frame-time scale 0x747edc from → to over real ms (0x4bee00; frame
  msec × scale × timescale, 0x42eb70); `scr_blur <ms> <radius> <realtime> <priority>` (0x4beb80) lerps the blur radius
  from the current one, `clear_blur` (0x4bebf0) zeroes it; `opendeadscreen` stores cg.time (0xf7966c),
  `closedeadscreen` clears it.
- The quote is ownerdraw `CG_DEADQUOTE` (0x4aa2a0) of hud.menu's `deadscreen` menu (item `quote`, textscale 0.4, rect
  80 200 480 20, centred): after `opendeadscreen`, alpha = elapsed cg time / `hud_deathQuoteFadeTime` (int, default
  1000, 0..100000), text = `ui_deadquote` without a leading `@`, localized. 0x4cc6a0 wraps at spaces to the rect
  width, centres each line, line step = text height + 5; glyph scale = textscale × 48 / font pixel height (1.3 gfx DLL
  refexport +0x128, 0x1000bfa0). The item's `textstyle` 3 gives each glyph the renderer's 1 px black, alpha-only
  shadow (see ui.md "Menu text styles"). Font of textfont 0 (0x4c38e0): s = textscale × screen height / 480; s ≤ `ui_smallFont`
  (0.25) → `$smallfont`, s ≥ `ui_extraBigFont` (0.55) → `$extrabigfont`, s < `ui_bigFont` (0.4) → `$font`, else
  `$bigfont` (aliases from the `assetGlobalDef` of ui/main.menu, parser 0x4d2fcb).
- `loadgame_continue` (0x447190) loads `sv_lastSaveGame` ("Unable to find save." otherwise).
- `radiusdamage(origin, range, max, min)` (0x4f3e70): radius damage 0x5289d0 from the world entity, inner = max,
  outer = min, MOD_EXPLOSIVE (14). `setplayerignoreradiusdamage(int)` (0x4f3ef0) stores 0x1c61058 (saved with the
  game); during `radiusdamage` it is copied to 0x1c61054, which makes 0x5289d0 skip entities with a client (0x528b8c).

## Menus

`src/ui/menu.{h,cpp}` parses every shipped SP menu (`ui/menus.txt`, `ui/hud.txt`, `ui/ingame.txt`; test
`test_menu`). Not done: runtime (open stack, dvarTest, actions, `uiScript`), drawing, ownerdraws.

## Implementation status (OpenIW2)

`src/game/campaign.cpp`: objective built-ins and state messages as above; `changelevel`, `missionsuccess` and
`missionfailed` set a pending level change; `World::takeDueLevelChange` applies 0x4d9f90 (victory screen or load).
`iw2view` shows the victory screen, loads the stored map on `nextlevel`, shows `briefing` for a frame before the
synchronous load and `pregame` after it, and copies `game[]` and the player's weapons only for a persistent change. Inferred and marked TODO: `objective_string`'s message, how long
objective messages stay on screen (4 s). Death screen as above, except: no save games (`loadgame_continue` restarts
the level), no blur pass (`scr_blur` ignored), no text shadow, the volume step per frame is inferred.
