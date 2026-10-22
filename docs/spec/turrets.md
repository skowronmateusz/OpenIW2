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
  records the turret on the actor (+0x2114) or clears it, and returns 1 or 0 (`animscripts/utility.gsc::tryTurret`
  reads it).
- `stopuseturret()` (0x53f200 → 0x549a60): notifies the turret "turret_deactivate" if it was active, clears the
  in-use flag and owner, notifies "turretownerchange".
- `getturret()` (0x53f8e0): the turret if the actor is its owner.
- `setturretanim(anim)` (0x53f860): the gunner's turret animation.

## Turret think (0x51d250; implemented in `src/game/turretthink.cpp`)

Setup also sets flags 0x1003, team 3, and maxRange² from the `maxrange` key or the
weapon's `maxRange` (unlimited when ≤ 0). Weapon fields (table 0x5a8d74…0x5a8e1c):
leftArc 0x528, rightArc 0x52c, topArc 0x530, bottomArc 0x534, accuracy 0x538, aiSpread 0x53c, playerSpread 0x540,
minVertTurnSpeed 0x544, minHorTurnSpeed 0x548, maxVertTurnSpeed 0x54c, maxHorTurnSpeed 0x550, pitchConvergenceTime
0x554, yawConvergenceTime 0x558, suppressionTime 0x55c, maxRange 0x560; fireTime 0x204 (ms).

Every frame per turret: nothing while a player uses it; the fire countdown drops by 50 ms; then
- automatic without AI ("auto_nonai"): keep the engaged sentient for 4 s after engagement or while valid; check one
  more enemy sentient per frame (round robin; turret team 1 → allies, 2 → axis, others none) and switch when it is valid
  and closer. Valid (0x51c2c0): closer than maxRange², aimable inside the arcs from `tag_flash` (0x51ba20: the eye,
  else the origin + 2, else the eye at pitch 0), clear trace (mask 0x801). Else the script target, else idle.
- AI gunner: the gunner's enemy while known (visible or within the suppression time) and in range, else the script
  target, else idle (simplified, see TODO).
- AI flag without a gunner: state 0. Manual without a gunner: the script target in range, else idle.

Engaging a sentient (0x51bef0): a new target restarts the engagement clock. Until the yaw convergence time has passed
the turret first aims 63 units beside the target (perpendicular to the line of fire, on the side the target faces),
then closes in by offset · (33 + 30 · remaining fraction); afterwards it aims at the target. The state (0x51bd40) goes
idle → aiming (2) when within 5° of the wanted angles or 250 ms after engaging, and aiming → firing (1) after the yaw
convergence time; each change notifies "turretstatechange". `isfiringturret` is state ≠ 0; `getturrettarget` returns
the engaged target.

Turning (0x51b610 / 0x51b4e0): with a target, the rate that reaches the wanted angles by the end of the pitch / yaw
convergence time (else one frame), clamped to the weapon's min/max vert/hor turn speeds; without one 200°/s. The
turret angles (entity +0x70 pitch, +0x74 yaw, relative) are published as `$turretangles` for the renderer.

`shootturret` (0x51b250 → 0x51ada0): countdown = fireTime · 3; a bullet from `tag_flash`, towards the aim point with a
random offset of up to 8 units across the line of fire (0x5501e0) when it stays within 0.95 of the barrel direction,
else along the barrel; spread aiSpread (playerSpread for a player), range 8192. Scripts (`maps/_mg42.gsc`) call it
every 0.1 s while `isfiringturret()`.

## AI gunner turret state

`useturret` records the turret on the actor (+0x2114). The actor's exposed state (0x5496f0) then enters the turret
state (state 2 of the table at 0x56f720): start 0x549960 takes the free turret, think 0x54aba0 keeps it while the
actor is inside its goal + 92 units (0x502760; not checked with `setturretignoregoals`) and runs the turret weapon's
`script` animscript; 0x549a60 releases it. Implemented in `src/game/actorturret.cpp`.

## Not implemented (TODO)

- Full AI gunner rules of 0x51c630 / 0x51cc80 (remembered target +0x60, suppression and visibility from node
  visibility, the 65536 distance check, giving up the turret 0x54b020); here simplified as above.
- Turret bone rotation (tag positions ignore the turret angles), the near-miss spread 0x550740 before convergence,
  the 0x48b090 visibility value, the bullet spread distribution (0x531210), drop pitch flags 0x100…0x400, projectile
  turret weapons, and turret fire sounds/effects (0x4d8020).
- The enemy checks of 0x51d3c0 / 0x51d640: here a free turret can always be used.
- `restoredefaultdroppitch` (0x51cf10), player use of turrets (`useby`, `maketurretusable` effects).
