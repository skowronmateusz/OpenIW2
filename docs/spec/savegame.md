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
  Implemented: `ui::saveSortKey/saveListBefore`.
- List builder 0x4c44a0 (PROVEN, stack offsets reduced to header offsets; `ui::saveHeaderInfo`, `saveDateText`, `saveDetailText`). Header offsets:
  `+0x00` version, `+0x20` map name (string), `+0x320` string (entry +8, unused by the menus), `+0x360` title/description (0x100 bytes; when its first byte
  is 0 the buffer is filled with `"\x15" + map name`), `+0x4a0` health (int), `+0x4a4` difficulty (0 EASY, 1 MEDIUM, 2 HARD, 3 FU, else UNKNOWN; jump table
  0x4c48e0 -> `GAME_DIFFICULTY_*`), `+0x4ac..+0x4cc` a `struct tm` (sec, min, hour, mday, mon, year-1900, wday, yday, isdst).
- Entry (64 bytes, array 0x10feda4): `+0x00` display name (`"%s/%s"` of directory and name for a sub directory, else the name), `+0x04` raw name,
  `+0x08` header +0x320, `+0x0c` header +0x20, `+0x10` detail text, `+0x14` date text, `+0x1c` the 9 tm dwords.
  Date text (version at +0 == 0x99, else `"(old save ver: %d)"`): `"%s %i, %i   %02i:%02i"` = localized `EXE_MONTH_ABV_*` of the month (table 0x5a4d64; June is
  `EXE_MONTH_ABV_JUN`), mday, year + 1900, hour, minute (via the localizer 0x4c6a20: a leading 0x15 byte means "not localized").
  Detail text, format 0x5811e4: title, `GAME_LEVELTIME` label, `"%2i:%s%i:%s%i"` (hour : "0" if min <= 9 : min : "0" if sec <= 9 : sec - code-exact, so the "level time"
  is the save time of day), difficulty label, `GAME_HEALTH` label, health.
- Feeder 0x10 (0x4c5f60 text, 0x4c6050 selection): column 0 = entry +4 (file name), column 1 = entry +0x14 (date); other columns empty. Selection (0x4c6050): with `developer`
  set prints "setting sel save to: %s (savename: %s)"; sets the dvar `ui_savegame` (registered 0x4c3280.., default "", flags 0x1000) to the entry's raw name, copies entry +0 into
  the UI state at 0x59b724 + 0x31e (UI_SAVEGAMENAME) and entry +0x10 into +0x41e (UI_SAVEGAMEINFO / `textsavegame`). openiw2 sets `ui_savegame` and keeps the detail text;
  drawing UI_SAVEGAMENAME / UI_SAVEGAME_SHOT / `textsavegame` is TODO(spec).
- Body: `SaveMemory_CreateCompressedSaveGame` (compressed snapshot of the game memory), `G_LoadGame` checks: size limits, version, "the bsp
  file has changed since the game was saved", "saved with different script files", "client mis-match". The engine state is a raw
  memory dump of the original game and cannot be reproduced by openiw2; TODO(spec) whether a different, openiw2-native save format is
  wanted. openiw2 therefore has no `.svg` writer; the save/load list reads existing `.svg` headers only.
