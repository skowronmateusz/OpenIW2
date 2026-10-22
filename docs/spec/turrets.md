# Turrets

Turret entities (`misc_turret`, `misc_mg42`, `spawnturret`) and the script methods on them and on their AI
gunners, read with REA from `CoD2SP_s.exe` (1.3). Implementation: `src/game/turrets.cpp`. Most methods have no
Ghidra procedure; they were read by raw-carving the executable at the method address.

## Path node types

The `type` field of path nodes is an engine field with a getter (node field table at 0x5a93b8: `type` → 0x515b60).
The node spawn table at 0x56fea8 maps
classnames to types 1–18 (`node_pathnode` 1, `node_cover_stand` 2, `node_cover_crouch` 3, `node_cover_crouch_window` 4,
`node_cover_prone` 5, `node_cover_right` 6, `node_cover_left` 7, `node_cover_wide_right` 8, `node_cover_wide_left` 9,
`node_concealment_stand` 10, `_crouch` 11, `_prone` 12, `node_reacquire` 13, `node_balcony` 14, `node_scripted` 15,
`node_negotiation_begin` 16, `node_negotiation_end` 17, `node_turret` 18); an unknown `node_*` class is an error
("%s doesn't have a spawn function", 0x52d400). Names (table 0x5a9368): "BAD NODE", "Path", "Cover Stand",
"Cover Crouch", "Cover Crouch Window", "Cover Prone", "Cover Right", "Cover Left", "Cover Wide Right",
"Cover Wide Left", "Conceal Stand", "Conceal Crouch", "Conceal Prone", "Reacquire", "Balcony", "Scripted", "Begin",
"End", "Turret". `maps/_load.gsc::auto_mg42Link` matches turrets to nodes of type "Turret".

## Turret setup (0x51dd80, spawn function 0x51e650, `spawnturret` 0x4ed0c0)

- `misc_turret` / `misc_mg42` need the `weaponinfo` key (error otherwise); `spawnturret(classname, origin,
  weaponinfo)` spawns the entity and sets it up the same way. At most 32 turrets (0x20 check).
- Arcs come from the map keys `rightarc`, `leftarc`, `toparc`, `bottomarc`, else from the weapon (`rightArc`, …).
  Right and top arcs are negated and clamped to ≤ 0, left and bottom clamped to ≥ 0.
- Convergence: `yawconvergencetime`, else `convergencetime`; `pitchconvergencetime`. Suppression time, AI and
  player spread from the weapon. The time units the original stores (it converts with ftol) are not checked.

## Turret methods

| Method | Behaviour (turret info flags at +8) |
|---|---|
| `setmode(m)` (0x4f01f0) | "auto_ai" flags 1\|2, "manual" clears 1 and 2, "manual_ai" 1 only, "auto_nonai" 2 only |
| `getturretowner()` (0x4f0300) | the user while the turret is in use (entity +0x112, owner +0xf4) |
| `isturretactive(t)` (0x4f47b0) | the in-use flag |
| `startfiring()` / `stopfiring()` (0x4f0000 / 0x4f00a0) | flag 4 |
| `isfiringturret()` (0x4eea60) | turret +4 ≠ 0 (set by the turret think; false here) |
| `settargetentity(e)` / `cleartargetentity()` (0x4f03c0 / 0x4f07c0) | script target (+0x10) |
| `getturrettarget()` (0x4f0b90) | target being engaged (+0x14 when flag 0x40; chosen by the think; nothing here) |
| `setaispread(f)` (0x4f04b0) | +0x84 |
| `setconvergencetime(t, ["yaw"\|"pitch"])` (0x4f05f0) | default yaw |
| `setsuppressiontime(t)` (0x4f0710) | +0x58 |
| `setturretteam(team)` (0x4f0870) | "axis" 1, "allies" 2, else error |
| `setturretignoregoals(b)` (0x4f0980) | flag 0x2000 |
| `maketurretusable()` / `maketurretunusable()` (0x4f0a40 / 0x4f0ae0) | flag 0x1000 |
| `setrightarc` / `setleftarc` / `settoparc` / `setbottomarc` (0x4f8440…0x4f85f0) | as at setup |
| `setdefaultdroppitch(p)` (0x4f8680) | +0x4c |

Errors: "entity type [%s] is not a turret" (0x5872f8) or "entity is not a turret" (0x5853ac).

## Gunner methods (actors)

- `canuseturret(t)` / `useturret(t)` (0x53f280 / 0x53f100 → 0x51d6d0): yes if the actor already owns the turret;
  no if the turret is in use by someone else; otherwise 0x51d3c0 (automatic turrets) or 0x51d640 (manual: only when
  an enemy is closer than a limit) decide from the actor's enemy (distance, turret range, sight). `useturret`
  records the turret on the actor (+0x2114) or clears it.
- `stopuseturret()` (0x53f200 → 0x549a60): notifies the turret "turret_deactivate" if it was active, clears the
  in-use flag and owner, notifies "turretownerchange".
- `getturret()` (0x53f8e0): the turret if the actor is its owner.
- `setturretanim(anim)` (0x53f860): the gunner's turret animation.

## Not implemented (TODO)

- The turret think (0x51c630 and helpers 0x51b3d0…0x51cc80): target selection for AI gunners and automatic turrets
  from the AI's sentient info, aiming with convergence, firing (`shootturret` → 0x51b250 / 0x51ada0), the
  "turretstatechange" notify and `getturrettarget` / `isfiringturret`.
- The enemy checks of 0x51d3c0 / 0x51d640: here a free turret can always be used.
- Taking ownership: here `useturret` makes the actor the owner at once (inferred; the original sets the in-use flag
  and owner elsewhere).
- `restoredefaultdroppitch` (0x51cf10), player use of turrets (`useby`, `maketurretusable` effects).
