# Savegames and campaign progress (clean-room notes)

Sources: `CoD2SP_s.exe` disassembly (VAs below), the shipped `maps/_endmission.gsc` and `ui/mission_select/*.menu` (read at runtime from
the iwds, not copied here). Status key: PROVEN = read from code/data, TODO(spec) = not decoded.

## 1. Campaign progress = archived dvars (PROVEN, implemented)

| Dvar | Type | Default | Registered | Meaning |
|---|---|---|---|---|
| `mis_01`..`mis_10` | int, 0..20, flags 0x1001 (archive) | 0 | 0x4d02b4..0x4d0540 | per mission (index = mission offset + 1): 0 locked, n = n levels unlocked, `lowestSkill + 9` (10..13) once all levels are completed |
| `mis_none` | int | 0 | same block | always 0; menu level slots beyond a mission's level count test it |
| `mis_difficulty` | string, 50 chars | `"0"` x 50 (0x581aa0) | ~0x4d05a3..0x4d05d1 | char `5*offset + level` = skill + 1 of the best completed run (0 = not done) |
| `ui_nextMission` | saved cvar | | set by `_endmission.gsc` | 1 = next level follows, 0 = mission done, 2 = campaign finished (victory screen) |

`setmissioncvar(name, value)` (0x4f40b0): `name` must be `mis_01`..`mis_10` or `mis_difficulty` (table 0x5a974c, else script error
"Invalid mission dvar name"); the dvar must exist ("valid name, but was not found"); an int dvar (type 5) takes the int argument,
a string dvar (type 7) the string argument, other types raise "valid name, but invalid type". `setsavedcvar` (0x4ec6b0) sets a plain cvar.

Dvar names compare case-insensitively (lookup 0x431260 -> compare 0x43f6d0); `_endmission.gsc` reads `g_gameSkill` for `g_gameskill`.

Level-end flow (`maps/_endmission.gsc` `nextmission`, run by the level script at the end): `levelEndSave` (TODO(spec): the autosave
at the level end), `setLevelCompleted` (mis_difficulty), `updateMissionDvar` (mis_NN), `unlockMissions` (a mission with all prerequisite
missions completed gets `mis_NN = 1`), then `missionsuccess` / `cinematic` -> level change. The mission-select menu shows a mission when
`mis_NN` is non-zero (`MISSION_NN_SHOW_DVAR`) and a level slot via `dvarTest mis_NN showDvar { ... }`.

Persistence: archived dvars are written to `players/<profile>/config.cfg` as `seta name "value"`. openiw2: script writes in the world
are copied into the profile cvars by iw2view every frame (mis_01..mis_10, mis_difficulty) and the file is rewritten. Verified end to end:
moscow `nextmission` at skill 1 gives `mis_01=2`, `mis_difficulty[0]='2'`; demolition `nextmission` then gives `mis_01=11`, `mis_02=1`,
`mis_03=1` (Egypt offset 1 and Not One Step Backwards offset 2 unlocked) and the relaunched mission select shows them enabled. A new game runs `set mis_01 1` (main menu) before loading the first level.

## 2. Savegame files

- Path: `save/<name>.svg` (format 0x5812ec; writer `save\%s.svg` 0x5837c8, temp file `save\temp.svg` 0x5838b4), screenshot `save/<name>.jpg`
  (0x5812c4), `savegame_lastcommit` / `sv_lastSaveGame` (0x5799d4) name the last save; `loadgame_continue` loads it.
  Autosaves use the `autosave` prefix (0x5810fc), `ui_savegameListAutosave` toggles their listing.
- Name rule (0x4e02e0..0x4e0345): shorter than 64 characters; letters, digits, `_`, `-`; `/` and `\` handled as in `ui::saveFileName`.
  Errors: "filename '%s' is too long", "invalid character (%c)... Must use alphanumeric characters only".
- Header (0x4d8 bytes at the file start): dword 0 = version. The loader check 0x4e06e0 (`sv_lastSaveGame` file, used to decide if "continue"
  is possible) requires a full 0x4d8 read and version == 0x99. The list builder (0x4c44a0) accepts 0x75..0x99; older ones are listed as
  "(old save ver: %d)". Implemented: `ui::saveHeaderLoadable/Listable`.
- List (FEEDER_SAVEGAMES, uiScripts `LoadSaveGames` 0x580fb4, `SavegameSort` 0x580f38, `DelSavegame` 0x580f48 "Deleted savegame: %s.svg",
  `Savegame`, `Loadgame` "loadgame %s"): up to 0x100 entries of 64 bytes, kept sorted newest first by the header dwords +0x4c0, +0x4c8,
  +0x4b4, +0x4b0, +0x4ac (these look like a `struct tm`: year, yday, hour, min, sec; the 9 dwords +0x4ac..+0x4cc are copied into the entry).
  Implemented: `ui::saveSortKey/saveListBefore`. The entry text (column 1 date `%s %i, %i   %02i:%02i` with `EXE_MONTH_ABV_*`, column 2 a
  block `%s\n%s: %s\n%s\n%s: %i` with `GAME_LEVELTIME` `%2i:%s%i:%s%i`, `GAME_HEALTH` and a `GAME_DIFFICULTY_EASY/MEDIUM/HARD/FU/UNKNOWN`
  chosen by a header dword 0..3) was read but the exact header offsets of the level name, time, health and difficulty are TODO(spec).
- Body: `SaveMemory_CreateCompressedSaveGame` (compressed snapshot of the game memory), `G_LoadGame` checks: size limits, version, "the bsp
  file has changed since the game was saved", "saved with different script files", "client mis-match". The engine state is a raw
  memory dump of the original game and cannot be reproduced by openiw2; TODO(spec) whether a different, openiw2-native save format is
  wanted. openiw2 therefore has no `.svg` writer: the save/load menu list is not implemented (no listbox item type in `ui/`).
