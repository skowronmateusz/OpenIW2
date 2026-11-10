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
- `objective_string` / `objective_string_nomessage` → FUN_004f18c0(1 / 0): validates the formatted text (at most 1023 characters, no `"`, `;` or `\`, no
  control characters other than 0x14 / 0x15; each violation is a script error), stores it in the objective's configstring (index + 13) and, with the flag 1,
  sends the state message by the objective's current state: active and current → `obj_update "<text>"`, failed →
  `obj_update "GAME_OBJECTIVEFAILED"`, done / not-shown / others nothing (the done id is the one `FUN_004f10b0` answers with the complete
  message, but 0x4f1b18 skips the send because it equals the reference id).
- `objective_icon(index, icon)`: icon name max 63 chars, 0x20–0x7e.
- `objective_position(index, pos)` sets `org0`; `objective_additionalposition(index, n, pos)`,
  n 0–7, sets `org<n>`.
- `objective_current(i, …)`: every listed index becomes `current`(4), every unlisted index that was `current`
  becomes `active`(1); then `GAME_OBJECTIVESUPDATED` message. `objective_additionalcurrent(i, …)`:
  listed ones become `current`, others untouched.
- `objective_ring(index)`: toggles the `ring` key (`ring` = 1 when it was 0/missing).
- `objective_delete(index)`: clears the configstring.
- Positions that come from `getorigin` (0x4ee250): an entity with the brush-model flag (ent +0xa1, set when the model is a
  BSP submodel, 0x447808, and read by SV_LinkEntity 0x44b2d0) answers the centre of its linked world box (absmin +0xc4 and
  absmax +0xd0, each half-summed); any other entity answers its current origin (+0xdc). A trigger or `script_brushmodel` has no
  `origin` key, its origin stays zero and the box comes from the submodel bounds, so its `getorigin` is the bounds centre.
  The compass skips a zero position, so a zero here would show no star. Eldaba (maps/eldaba.gsc:3057-3059) places the
  "recover the documents" objective (index 3 when no `jumpto` is set) with `getorigin` of the origin-less `documents`
  trigger_use; with the origin key read instead the star was missing. Implemented in the `getorigin` built-in
  (`src/game/world.cpp`, box as in `brushLinkBounds`); test `getorigin_world`.

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
  carries the player's weapons into the next level (`_loadout.gsc`: the weapons are kept, with full ammunition).
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

## Level start timing

VAs in the patch 1.3 SP executable. The `map` command (0x449340) loads a level in one call:

1. The server spawn (0x4488b0) runs the frame start (level time 50), the entity spawn, the level's main thread and one full server frame
   (0x4da470); see gsc-vm.md. A sound the script starts here (`playsound` = a G_AddEvent on the entity, 0x4ef230) is only an entity event
   at this point: nothing is audible during the load.
2. The client then runs one cgame frame by hand (0x402ac0 -> 0x4a4ad0). Its entity event pass (0x4bb850, at most the newest four events per
   entity) starts the sounds of step 1 through the generic play routine 0x4423d0, which culls 3D sounds by distance from the listener
   (0x4424e6), so the listener is the first snapshot's view, not an unset origin.
3. The pregame decision follows (`ui_autoContinue` or `cl_paused 1` + `pregame`, 0x4c54c0), client state becomes 4, and the Com_Frame clock
   (0x74a92c) is set to the end of the load, so the load time (the engine prints it) is not frame time of the first frame.

Server frames beyond the spawn frame run only with client state 4 (0x44ab70), in 50 ms steps (0x44a9d0), and not while `cl_paused` is set
(its loop breaks). The frame time given to that accumulator is clamped to 1..200 ms while a server is running (5000 ms otherwise,
0x42eb70, after the scale), so a slow first frame runs at most four server frames. The sound update pauses the pausable channels when
`cl_paused` changes (0x442a30, sound.md), so the opening dialogue waits for the click on `pregame` at the position where it started.

openiw2 (iw2view): the world is built before the scene loads and the audio thread mixes in real time, so:
- `Mixer::setLoading` holds every voice from the world's creation until the end of the load; a started voice keeps its handle and length
  (the script's `playsound` notify depends on it). At the end of the load the mixer takes `cl_paused` first and then releases the voices,
  so a pregame keeps them at position 0.
- While loading, a script sound uses the player's spawn as the listener (the stand-in for step 2). Before this, Hill 400 (defend) lost its
  first line: the 3D sound of Randall's intro line was culled at the origin (handle 0, length 0), the script saw no sound and the next
  line started 0.1 s later; the rest of the opening ran ahead of the audio.
- The first frame after a load is no frame time (the pipeline and upload warm-up is part of the load): the frame clock restarts after it,
  and the simulation step per frame is clamped as above (`comFrameSeconds`, was 250 ms).
- TODO(spec): when a culled sound reports its length to a `playsound` notify (a far-away line with a notify name still returns length 0
  here); which cgame call places the listener before the entity event pass of step 2.

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
- Slow motion (death): the scale is not applied at death but when the client receives `time_slow`, i.e. with the death
  screen, 1700 ms of level time after the death (a dead body therefore moves at full speed until then). The curve is
  linear in real ms from 1 to 0.1 over `g_deathDelay` (0x4bee00); afterwards the scale stays at the end value 0.1, it never
  reaches 0 (the level reloads at the same moment, 0x4d9f90). The scale multiplies every Com_Frame msec (0x42eb70, min 1 ms)
  before the server frame and the client run (0x40cf60), so level time, player movement, actors and effects all slow
  down. Nothing else freezes the dead player: movement commands are cleared and the ground move loses 20 speed per step
  (0x508a10, 0x50c733). iw2view applies the scale to the player's movement and weapon/view steps too (`game/framescale.h`).
  Step length (decoded): the client makes one usercmd per client frame (0x409e50 -> 0x409da0; command time = client server time, the
  frame time of the cmd is the time delta capped at 200 ms), and Pmove (0x50cc10) runs one PmoveSingle (0x50c4a0, clamped to
  1..200 ms) per cmd, in chunks of at most 66 ms. Under time_slow the frame time and so every cmd time is scaled, so friction
  and gravity (both multiplied by the step's frametime) and the noclip / ufo move (0x508a90, 0x508cec: also per cmd frametime)
  all follow the scaled step, and the noclip camera of a running level is slowed by the same factor (tests/test_pmove.cpp).
  The dead ground move (0x508a10, called at 0x50ca6c only for pm_type 4, before PM_CheckLadderMove 0x50ca83) subtracts a fixed 20
  from the speed per PmoveSingle step, independent of the step length (one per usercmd up to 66 ms). That call precedes the ladder
  check, which leaves a dead player in the air move; the port ran the drop after it, where the ground state was already gone, so it
  never fired. It now runs at 0x50ca6c's position, once per 66 ms chunk (the viewer's 1/60 s sub-steps share one drop).
  The minimum of 1 ms per Com_Frame (0x42eb70) is the lower clamp of `comFrameSeconds` (src/game/framescale.h), which iw2view applies to its float frame time.
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
synchronous load and `pregame` after it, and copies `game[]` and the player's weapons only for a persistent change. Inferred and marked TODO: how long
objective messages stay on screen (4 s). Death screen as above, except: no save games (`loadgame_continue` restarts
the level), no blur pass (`scr_blur` ignored), no text shadow, the volume step per frame is inferred.

## HUD fixes: compass friendlies, low-health overlay, overlay draw order

Compass friendlies (ownerdraw 87). The flag the per-frame actor pass tests (global 0x1c61050) is set to 1 by level init (VA 0x4d9c83
in the patch 1.3 SP executable) and the `drawcompassfriendlies` built-in (0x4f4450) only overwrites it; no shipped SP script calls it, so
in the campaign it stays on. The pass (0x4db31f) marks an actor when the flag is set, its `drawoncompass` field (+0x2120) is non-zero and
its sentient team is 2 (allies); ownerdraw 87 (0x4c0f20, rules above) draws the marked actors. Scripts and map keys switch single actors
off with `drawoncompass` 0. Implemented: the World flag defaults to on.

Low-health overlay. SP has no code-driven overlay: the hud.menu "Low Health" definition (ownerdraw CG_PLAYER_LOW_HEALTH_OVERLAY) is
commented out in the shipped menu, and the `hud_healthOverlay_*` pulse dvars belong to an unused ownerdraw path (0x4a8d70 / 0x4a8f00,
draws nothing in SP). The red screen effect is a full-screen script hud element with the `overlay_low_health` material that the
skill script fades and pulses while health / maxhealth is at or below `level.healthOverlayCutoff` (Regular 0.28, Hardened and Veteran
0.35, Recruit 0.01). The engine's default `g_gameskill` is 1 (0x446b33); the former default of 0 (Recruit) made the cutoff 0.01 so the
overlay hardly ever appeared. Implemented: the default is 1; the difficulty menu still sets it. The damage-direction icons (0x4bdcb0)
are a separate path (see the player HUD notes); no further native blood overlay was found.

Overlay draw order. The 2D pass (0x4bfa70) draws: damage icons (0x4bdcb0), the crosshair step (0x4bd760), which draws the scope /
binocular overlay (0x4bcdd0) and returns the crosshair fade, then the hud elements of layer 0 (0x4c3030 with 0), the HUD menus, the
hud elements of layer 1, and later the subtitles and fades. The overlay therefore lies below every script hud element and subtitle.
Implemented: iw2view splices the overlay quads in below the hud-element quads (game/hudlayers.h). The hudelem layer
word (+0x7c, compared with the layer argument at 0x4c305a) is the script `foreground` flag: the hud-element field table pairs the name `foreground` with offset 0x7c (entry at 0x5700fc).

## loadgame_continue in openiw2 (implementation notes)

- Original (VAs in the patch 1.3 SP executable): the due-change handler 0x4d9f90 queues `loadgame_continue` when the failed flag 0x1c60fe8 is
  set; the command (0x447190) loads the save named by `sv_lastSaveGame`, i.e. the newest committed (non-internal) save, normally the
  last autosave or checkpoint; it does not restart the map ("Unable to find save." when none is named). openiw2 does the same with its own
  save container (docs/spec/savegame.md section 5).
- openiw2 addition: when the named save cannot be used (file missing, old openiw2 format, damaged), the front end restarts the level
  (the failed-mission restart) instead of staying in the old world with the death screen up. The original has no such case (its save
  always loads or the error is printed). After "Unable to find save." (0x447190: neither 0x4d55e0 nor 0x4e06e0 accepts the name, or the
  server is not running and 0x446e00 gives 0) the original raises an error of kind 1 through 0x42c7f0 (formatted by 0x4e07f0, the drop
  kind: it stores 1 in 0x74806c and unwinds the frame), so the running game is abandoned; it does not fall back to the map.
- `ui/menus.txt` lists `ui/rec_restart.menu`, which no archive contains; the original's menu loader meets the same missing file (the
  menu is not on the death or mission-failure path, which uses the `deadscreen` HUD menu and the `opendeadscreen` command).
