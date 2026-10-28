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

- Path: `save/<name>.svg` (reader format 0x5812ec; writer `save\%s.svg` 0x5837c8, built by 0x4e02b0 with the name rule below), screenshot
  `save/<name>.jpg` (0x5812c4). The dvar `sv_lastSaveGame` (0x5799d4, global 0x8254a4) names the last committed save; `loadgame_continue`
  loads it. Autosaves use the `autosave` prefix (0x5810fc), `ui_savegameListAutosave` toggles their listing.
- Name rule (0x4e02e0..0x4e0345): shorter than 64 characters; letters, digits, `_`, `-`; `/` and `\` handled as in `ui::saveFileName`.
  Errors: "filename '%s' is too long", "invalid character (%c)... Must use alphanumeric characters only".
- "internal" names (0x44a2f0, PROVEN): a name is internal when any of its path segments (start of the string, or right after a `/` or `\`)
  begins with `internal` (8 characters, case-insensitive compare 0x43f6d0) followed by `/`. Internal saves never become `sv_lastSaveGame`
  (0x4e055e, 0x4d56a9) and are used by the snd/vid restart (section 4).

### 2.1 File layout (PROVEN: writer 0x4e01f0, reader 0x4d5270)

    +0x000  header, 0x4d8 bytes (2.2)
    +0x4d8  body: zlib 1.1.4 deflate stream of the serialised game state (size = header +0x4d4)
    ...     only when the script debug flag 0x9bef12 is set: dword n, then n x (dword len, len bytes) of the
            script source buffers that are flagged (0x462500); release builds leave it 0

- The body is built in a memory file of capacity 0x280000 bytes (2.5 MB; 0x4e3f0c), state 3 while writing, 4 when complete. The compressor
  0x4d51c0 -> 0x48fc10 (zlib 1.1.4, "deflate 1.1.4" is embedded; 0x48fc10 = init with level 1 (pushed at 0x4d51e5), one deflate call with flush 4 (finish) that must return 1 (stream end), end) writes into 0x1895070 with an output
  bound of `n + 13 - (int)(n * -0.001)` (float 0x5920c8 = -0.001), logging "SaveMemory_CreateCompressedSaveGame: Compressed %d bytes (of %d
  bytes) into %d bytes". The loader inflates with 0x4956b0 ("SaveMemory_DecompressLoad: decompress failed.").
- Writing (0x4d56f0 -> 0x4e03b0 -> 0x4e01f0): the header is copied, +0x4d4 set to the compressed size, then header and body are written to
  `save\temp.svg` (0x5838b4) and the result moved to the final name (0x41cda0 path; failures send `gamesavefailed` and the
  `GAME_SAVE_INSUFFICIENT_FREE_DISK` error). Client commands: `gamesavestart` before, `gamesaved` after.
- Screenshot: the commit 0x4e0502..0x4e0552 tests `header +0x4a8` and then the ADDRESS `header + 0x320` (`lea 0x320(%ebx),%ecx; test %ecx,%ecx;
  jne`), which is never zero, so `screenshotJPEG savegame save/%s` (0x5837a8) is dead code in 1.3: no `.jpg` is written.
- Loading (0x4d5270): open (`G_LoadGame: savegame '%s' not found`), read 0x4d8 header bytes (`bad read`), version must be 0x99 (`wrong version
  (%i, should be %i)`), compressed size +0x4d4 <= 0x280000, read the body, re-registers `ui_campaign` and prints "Save game build number: %s"
  (+0x120) / "Save game changelist: %s" (+0x1a0). `header +0x04` must equal the bsp checksum argument ("the bsp file has changed since the
  game was saved"). The three script fingerprint dwords (+0x14 = [0x9bef44], +0x18 = [0x82d720], +0x1c = [0x9bef54] - [0x9bef50]) are compared
  with the running values; a mismatch sets the runtime byte `header +0x11`, which G_InitGame turns into the error "Save game saved with
  different script files" (0x4d9af2; with 0x4d9800 "Script function count mismatch"). Uncompressed size +0x4d0 <= 0x280000.

### 2.2 Header (0x4d8 bytes; built by 0x4d4f70, the save state keeps it at +0x1c)

| Offset | Size | Content | Evidence |
|---|---|---|---|
| +0x000 | dword | version 0x99 | 0x4d4fa3; loader 0x4d5270; list 0x4c44a0 accepts 0x75..0x99 |
| +0x004 | dword | bsp checksum (G_WriteGame argument) | 0x4d504b; loader "bsp file has changed" |
| +0x008 | dword | checksum of the uncompressed body, 0x43bc30(buffer, size). TODO(spec): the algorithm of 0x43bc30 | 0x4d504e |
| +0x00c | dword | save id (pending-save +0x180, section 4) | 0x4d4fe5 |
| +0x010 | byte | runtime only: set to 1 by the loader | 0x4d5270 local_4c8 |
| +0x011 | byte | runtime only: script files differ (see above) | 0x4d5270 |
| +0x014 | dword | script fingerprint [0x9bef44] | 0x4d5062 |
| +0x018 | dword | script fingerprint [0x82d720] | 0x4d5065 |
| +0x01c | dword | script fingerprint [0x9bef54] - [0x9bef50] | 0x4d5078 |
| +0x020 | 0x100 | `mapname` dvar | 0x4d4ff5 |
| +0x120 | 0x80 | `"%s %s"` of the build `pc_1.3_1_1` and the build date `Thu Mar 30 2006 07:35:42PM` | 0x4d500e |
| +0x1a0 | 0x80 | changelist: empty string in 1.3 | 0x4d503a |
| +0x220 | 0x100 | `ui_campaign` dvar (registered with default `american`, flags 0x1400) | 0x4d50c3 |
| +0x320 | 0x40 | screenshot/image name (script `savegame` 3rd argument, default `$default`) | 0x4d50d9, 0x44a0fc |
| +0x360 | 0x100 | description (script `savegame` 2nd argument); the list shows it, empty = the map name | 0x4d5115, 0x44a0e3 |
| +0x460 | 0x40 | save name (built file name, without `save\` / `.svg`) | 0x4d50f8 |
| +0x4a0 | dword | health percent: `100 * health / maxhealth` converted to int by 0x565994 (float-to-int helper), clamped to 1..100; 1 when either is 0. health = player entity +0x13c ([0x1b1603c]; `savegame` refuses while it is <= 0, "Attempting to save while dead"), max = its client +0x8624 | 0x4d5126..0x4d5173 |
| +0x4a4 | dword | `g_gameskill` (dvar global 0x1c13f7c, registered 0x4d868f) | 0x4d5182 |
| +0x4a8 | byte | 1 when the save type is 0 (a non-autosave), else 0 | 0x4d4f97 |
| +0x4a9 | byte | do-not-record flag: when 0 (always from G_WriteGame) and the name is not internal, the commit sets `sv_lastSaveGame` | 0x4d4f9d, 0x4e0554 |
| +0x4ac | 9 dwords | `struct tm` of the save time (0x427cd0) | 0x4d5188 |
| +0x4d0 | dword | uncompressed body size | 0x4d5192 |
| +0x4d4 | dword | compressed body size (written by 0x4e01f0) | 0x4e026e |

### 2.3 Body: section order of the original (PROVEN order; the inner layouts are raw dumps of the original's structures)

G_WriteGame (0x4e3ef0, "G_WriteGame '%s' '%s'") resets the memory file and writes:

1. 0x462440: two bytes, the script debug flags [0x9bef12], [0x9bef13].
2. 0x4e3070: dword count [0x1cbad9c], then for i = 1..count a byte length and the characters of the name that is the first field of entry i of
   the pointer table 0x1c64548 (the client model table indexed by cgame 0x4a76b0..0x4a99b0; TODO(spec): confirm it is the model table). Then
   0x4e2c60: a dword count (patched afterwards) and one 0x4e0ac0 string record for each non-zero slot 1..0x84 of 0x1c90988; the loader's
   0x4e4380 reads a count and names and resolves them as weapons ("Weapon index mismatch for '%s'") - shape-matched pairing, TODO(spec).
   Then 0x4e2b50: nine configstrings (0x4e2740, which names `SV_GetConfigstring`).
3. 0x4e31c0 (game state):
   - dword 0x12345678 placeholder, patched to the size of the level block; dword 0x12345678; level dwords [0x1c60534], [0x1c60530],
     [0x1c61070], words [0x1c610b0], [0x1c610b2], dwords [0x1c60fe0], [0x1c60fec], [0x1c60ff0], [0x1c60fe4], [0x1c60fe8], 0x40 bytes
     0x1c57d88, dword [0x1c61058] (the radius-damage ignore flag).
   - 0x4e2e00: the SAVED dvars as text (0x433200 with buffer 0x400: dword length, then the text).
   - 0x8000 bytes of 0x1cb2c00; dwords [0x1c61034], [0x1c61038], [0x1c61050]; the values of the dvars `g_gameskill` (0x1c13f7c),
     [0x1c13f88], [0x1c13f34].
   - 0x515570: 0x28 bytes of 0x1c8d990 (path code state); 0x402d60; patch of the first size.
   - 0x461810: script VM state (globals 0x9bef24..0x9bef58, the variable tables).
   - 0x4e2370, 0x4e25e0 (bad places 0x1c90e00).
   - dword [0x1c6050c]; dword 0x1fc (entity size); for each of the 0x800 entities (0x1b15f00, stride 0x1fc) in use (byte +0xa4):
     dword index, then 0x4e1a30 (field-table writer 0x4e1540); dword -1.
   - dword 0x86b8 (client size); the single client with state 2 (+0x85c0): dword index, 0x4e17c0; dword -1.
   - dword 0x21a4; 32 actors 0x4e1d20. dword 0x44; 33 sentients 0x4e1ef0. dword 0x244; 64 vehicles 0x4e2050. 0x512640 (path nodes
     0x1c7fce0 / 0x1c8d984). dword 0xb4; 32 records 0x4e2210 (base [0x1c60524]).
   - 64 records of 0x1c7e5b0 (stride 32): dword + 24 bytes. dword [0x1c628b4] then that many 12-byte records of 0x1c610b4.
   - 0x4e2ce0 (level 0x1c60f60), 0x4b41b0 and 0x4afd40 (client game state 0xf48f../0xf79...), 0x4019e0, 0x44a450 (server 0x825508),
     two configstrings (0x4e2740); patch of the second size; dword 0x12345678; 0x461b50 (script VM threads).
   - Anim trees (0x47bba0, "MAX_ANIM_TREE_SIZE exceeded"): the 32 pointers at 0x1c7e52c, the 64 pointers at 0x1c7e5ac (stride 32) and
     entity +0x1f4 of every used entity; 0x4b4050 (`CG_SaveViewModelAnimTrees`); patch of the last size.
4. The header (0x4d4f70) is filled after the body, so +0x008 / +0x4d0 describe the finished body.

TODO(spec): the field tables behind 0x4e1540 (entities, clients, actors, ...), the VM writers 0x461810 / 0x461b50 and the cgame blocks.
They are raw images of the original's memory structures, which openiw2 does not have; a byte-compatible body is therefore out of reach.

## 3. Save requests, autosaves and the snd/vid restart (PROVEN)

- `savegame(name [, description [, image]])` (0x4f3a30): refuses while the player is dead (entity +0x13c <= 0, script error "Attempting
  to save while dead"); empty name = `auto`; `levelstart` (case-insensitive) -> file `autosave\<mapname>`, else `autosave\<mapname>-<name>`
  (0x586214 / 0x586204, 64 characters); description default "", image default `$default`. It queues a request of type 1 (autosave) with
  flags 2 (commit) and clears the byte 0x1b15ac1.
- Queue (0x44a140, 3 entries of 0x18c bytes at 0x825538, count 0x8259dc): -1 and "savegame request ignored" while `g_reloading` is set
  (script error "Attempting to save during a restart."); a type-1 request when one was already queued this frame (byte 0x8259e0) -> -2
  ("Multiple Autosaves attempted in same frame", script error "Attempting to save multiple times in a single frame."); a full queue -> -3
  ("Pending Saves limit exceeded"). Entry (0x44a0c0): +0x000 name (63), +0x040 description (255), +0x140 image (63), +0x180 save id
  (type 1: ++[0x1b15ac4], else 0), +0x184 type, +0x188 flags. Returns the save id.
- Processing (0x44a230, once per server frame): nothing while `g_reloading` is set (the queue stays); otherwise each entry is written
  (0x4e4050 -> G_WriteGame; flags & 2 commits, & 6 writes the file) and then 0x44a050 checks its name; the queue and the per-frame
  autosave byte are cleared.
- snd/vid restart while a level runs (`vid_restart` 0x40cd10, `snd_restart` 0x40ce30): when the dvar at 0x747f04 is set (a level is
  running) the command queues a save named `internal/vid_restart` / `internal/snd_restart` (type 0, flags 4, empty description and image)
  instead of restarting. After that save is written 0x44a050 prints "Game saved for vid_restart" / "Game saved for snd_restart" and adds
  `disconnect;vid_restart;loadgame internal/vid_restart` / `disconnect;snd_restart;loadgame internal/snd_restart` to the command buffer:
  the level is saved, the subsystem restarts with no level loaded, and the save is loaded back. Without a level, `snd_restart` restarts
  the sound system (0x444c20, 0x444900) and then runs the `vid_restart` body (`jmp 0x40cd10`).

## 4. Player profile load and the profile list (PROVEN)

- `loadPlayerProfile` uiScript (0x4c4f10): when `ui_playerProfileSelected` is not empty, 0x4274a0 with that name: nothing when it equals
  `com_playerProfile` (case-insensitive) or the profile does not exist (0x4273c0); otherwise the name is written to `players/active.txt`,
  the console command `disconnect` runs, the dvars are reset (0x4333d0(-1, 1)), the profile's `config.cfg` is executed (0x427460), 0x42de10
  runs, and when any registered dvar's value at +8 differs from its value at +0xc (walk of the dvar list 0x7805c4 in 0x4331c0, compare by type
  0x430d70; our reading: current vs latched) the command `snd_restart` is queued. No `vid_restart` is issued (the only `vid_restart;` of the
  UI scripts belongs to `RunMod`, 0x4c5b77).
- `addPlayerProfiles` (0x4c4b80): count 0x11071b0 = 0, ascending flag 0x11072b4 = 1, lists the directories of `players` (0x41dfc0) and
  appends EVERY one to the 64-slot array 0x11071b4: the add loop has no cap (more than 64 directories would overrun the array). The cap
  lives in `createPlayerProfile` (0x4c4d20): count == 0x40 opens `profile_create_too_many_popmenu` (0x4c4d73). The names are sorted with
  the comparator 0x4c4ab0 into the index array 0x11072b8 and `ui_playerProfileCount` is set. (Correction: an earlier note called
  0x4c4c1c..0x4c4cec the savegame list sort; it is this profile list.)

## 5. openiw2 savegame container (implementation choice)

The original body cannot be reproduced (section 2.3), so openiw2 writes its own body behind the original header:

- Header: the 0x4d8-byte layout of 2.2 with version 0x99, so the save list (FEEDER_SAVEGAMES) shows our saves exactly like original ones.
  openiw2 fills +0x004 with the CRC-32 of the bsp file, +0x008 with the CRC-32 of the uncompressed body (not the 0x43bc30 algorithm),
  +0x00c save id, +0x020 map, +0x120 `openiw2`, +0x220 `ui_campaign`, +0x320 image, +0x360 description, +0x460 name, +0x4a0 health percent
  as above, +0x4a4 `g_gameskill`, +0x4a8 type == 0, +0x4ac local time, +0x4d0 / +0x4d4 sizes. The fingerprint dwords +0x14..+0x1c are 0.
- Body: zlib deflate of an openiw2 stream that starts with the magic `OIW2SAVE` and a format version (src/game/savestate.h); an original
  body does not start with it after inflating, and the loader refuses such a file with "not an openiw2 save".
- Contents (src/game/savestate.cpp): the script VM (objects, arrays, threads with frames, locals, stacks, references, wait/endon state,
  the woken-thread queue, time and id counters; functions by file + name) and the World state listed there. Per-entity runtime that is
  rebuilt rather than restored (animation playback, AI state machine, paths, vehicles, turrets, missiles, perception) is listed as
  TODO(spec) in the header of savestate.cpp.
