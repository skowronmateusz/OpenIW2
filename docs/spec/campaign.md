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

- `savegame(name [, description [, image]])` and `savegamenocommit(...)`: error `Attempting to save while dead` when
  the player is dead. Empty name → `auto`. File: `autosave\<mapname>` or `autosave\<mapname>-<name>` (choice by
  FUN_0043f6d0; that `levelstart` selects the short form is inferred from the adjacent `levelstart` string, TODO
  verify). Description default empty, image default `$default`.
- FUN_0044a140: ignored while `g_reloading` (−1, script error `Attempting to save during a restart`);
  for no-commit saves only one per frame (−2, `Attempting to save multiple times in a single frame`); at most 3
  pending saves (−3). `savegamenocommit` returns the save id.
- `commitsave(id)`: errors `Attempting to commit an invalid save`, `Save Commit attempted on incorrect save
  currentsave:%i, commit:%i`, `Save Commit attempted with no valid committable save available`.
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
- The order of levels is script logic (`maps/_endmission.gsc`), which also relies on `game[]` surviving the change.

## Menus

`src/ui/menu.{h,cpp}` parses every shipped SP menu (`ui/menus.txt`, `ui/hud.txt`, `ui/ingame.txt`; test
`test_menu`). Not done: runtime (open stack, dvarTest, actions, `uiScript`), drawing, ownerdraws.

## Implementation status (OpenIW2)

`src/game/campaign.cpp`: objective built-ins and state messages as above; `changelevel`, `missionsuccess` and
`missionfailed` set a pending level change. `iw2view` rebuilds the world and scene for the next map when it is due
and copies `game[]` into the new world. Inferred and marked TODO: `objective_string`'s message, the failure
screen (OpenIW2 restarts the level after 3 s), how long objective messages stay on screen (4 s).
