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
- `setturretanim(anim)` (0x53f860): the animation's node in the actor's tree (0x46e530) at actor +0x2118 and the flag
  +0x211a = 1; the turret pose step below drives that node.

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
`script` animscript (movemode 0, anim mode 1, 0x54ae56); 0x549a60 releases it. After the script start the actor must
still own the turret (0x54afd0; the mg42 animscripts call `stopuseturret` when the gunner is more than 90 units away),
else it gives the turret up (0x54b020) and becomes exposed. Then +0x220 = 0 and the pose step runs. Implemented in
`src/game/actorturret.cpp`.

### Gunner pose step (0x549ba0, from 0x54ae93; `World::turretPoseStep`)

Without it the setturretanim blend node has no weighted leaf and the gunner shows the reference pose (T-pose).
- Aborts (exposed state, return 1): the turret model has no `tag_weapon` ("WARNING: aborting turret behavior since
  'tag_weapon' does not exist", turret released when owned, +0x2114 cleared); no setturretanim ("... no turret animation
  specified", 0x54b020); no `tag_aim` when the gunner is outside the animated height range ("... 'tag_aim' does not
  exist"). A node or height child without children is the error "anim '%s' has no children".
- Input: `tag_weapon` in the turret model's space with its bone controls (0x4725f0 / 0x4d7460: quaternion + origin), its yaw
  (0x420270 frame, 0x421920 `atan2(f.y, f.x)`), h = the gunner's link offset height (tagInfo +0x3c: the actor is linked to
  the turret by the state start 0x549960) minus the tag's height, weapon `animHorRotateInc` (+0x564).
- Children in the exe's order, which is the reverse of the .atr file (docs/spec/animtree.md "Child order"): the node's
  children are gunner heights (ascending delta height), theirs turret yaw steps.
- Probe (descendants zeroed at once, 0x4789a0 with 0): child i to weight 1 (blend 1, rate 1); f = count·0.5 − yaw / inc
  clamped to [0, count − 1] (the centre is half a step off the middle grandchild, as in the exe), grandchild trunc(f) to
  1 − frac, the next to frac (when not 0), weights < 0.001 → 0; the child's delta at its current time by goal weights
  (0x4785d0) gives z; stop at the first child with z ≥ h.
- Final weights (zeroed again; blend time 1 / (|weight − goal| · 20), 0 when equal, the weight read after the zeroing):
  the probed child's two grandchildren; 0 < i < count: child i = t = (h − z(i−1)) / (z(i) − z(i−1)), child i−1 = 1 − t
  and its two grandchildren; else the probed child = 1 and, once the gunner has arrived (turret flag 0x20, offsets 0),
  the pitch bound +0xa8: angle = acos of the dot of (2D dist, tag_weapon.z − tag_aim.z) and (2D dist, h' − tag_aim.z)
  normalized (h' = link z − last probed z; 90 when the dot < 0, 0 when > 1), i = 0: pitch − angle with flags
  0x200 / ~0x400, i = count: pitch + angle with 0x600. Inside the range +0xa8 moves back by 0.1 per step and 0x200 is
  cleared once it passes topArc (0x400 set) or bottomArc; not arrived clears 0x200.
- Pose: the node's delta (0x4785d0) turned by the tag yaw (0x423570) + tag_weapon, yaw = delta yaw (0x423900) + tag yaw,
  into the turret entity frame (0x424ad0 / 0x422770 / 0x424b70). First step (flag 0x20 clear): flag set, offsets gunner −
  pose at turret info +0x90 (origin) / +0x9c (angles, 0x423fd0); every step they slide (0x53b9a0 / 0x53ba60, 0.05 s,
  540°/s). Entity angles = the result, facing blocks 0x504950 / 0x5049a0. Unlinked turret: +0x264 = 1, +0x250 = xy
  difference, physics 0x501d00; linked turret: origin = pose (0x54ab68). Velocities +0x230 / +0x250 zeroed, return 0.
- Turret turn (0x51b610, `World::turretTurn`): with flags 0x200 and 0x100 the pitch turns at 360°/s; after the step, with 0x200,
  a pitch beyond +0xa8 (above it with 0x400, below without) becomes the bone-control drop e+0x78 = pitch − (+0xa8) (0x51b7a3;
  tag_aim / tag_aim_animated turn by pitch − drop, tag_flash by drop), otherwise 0x100 is cleared. e+0x78 is zeroed at the start of
  every turret think (0x51d279).
- Obstruction check (0x54a8bc..0x54ab4d, `World::turretObstruction`, unlinked turret only, after the move): the move bumped an
  entity (+0x318 ≠ 0x7ff), the state was set ≥ 1 s ago (+0x20), topArc ≤ pitch ≤ bottomArc, rightArc ≤ yaw ≤ leftArc and the
  slide offsets are 0. A bumped non-sentient (gentity +0x104 = 0; the world 0x7fe included): yaw < 0 → rightArc + 1, else
  leftArc − 1 ("WARNING: capping rightarc/leftarc of turret at (%.2f, %.2f, %.2f) to %.2f"); an arc crossing 0 is set to 0 and
  the gunner detaches ("WARNING: AI %d at (...) with turret angles (%.2f, %.2f) detaching from turret due to obstruction").
  A bumped sentient detaches at once. Detach = 0x54b020 + exposed (return 1).
- Start (0x549960): refuses any turret in use (+0x112), keeping the recorded turret; clears keepclaimednode /
  keepclaimednodeingoal and releases a claimed node (0x5191a0).
- Savegame version 4 stores +0x2118 / +0x211a and the turret's +0x90 / +0x9c / +0xa8 / e+0x78.
- TODO(spec): 0x4e7150(+0x294, +0x298); the bumped-sentient team branch 0x54aafa..0x54ab45 (0x4e7010, 0x4f9f30, 0x4fa070); the
  actor-turret link itself (the port computes the link height from the current origins); 0x54e3f0 / 0x503e10 and the start
  argument that keeps flag 0x10 / +0x60 at 6; the debug string at actor +0x21a0.

## Fire view shake (player on a turret)

Two effects shake the first-person view while the player fires a mounted turret. Neither reads a view-kick field of the turret
weapon: the shipped turret weapon files (mg42_bipod_*, 30cal_*, mg42_tank_*) define no gunKick / viewKick keys, so the fire
impulse of the player weapon is zero for them.

1. View jitter (cgame 0x4aee10, run before the view offsets and the screen shake in the per-frame view setup). The weapon keys
   `horizViewJitter` (def +0x57c) and `vertViewJitter` (def +0x580) are floats in degrees (0.5 / 0.5 in every shipped turret
   weapon). While the player is on a turret (ps eflags 0x300), the view-lock word ps+0x564 is 2 and `cl_paused` is 0, every
   rendered frame adds `crandom() * vertViewJitter` to the view pitch and then `crandom() * horizViewJitter` to the yaw
   (crandom = 2 * rand / 32768 - 1). The draws are fresh every frame and are not accumulated or decayed, so the view buzzes by up
   to +-0.5 degrees for as long as the turret keeps firing. The view-lock word is set to 1 by the turret idle think (0x51b2b0 and
   0x51b140) and to 2 by the fire function 0x51b250 right after the shot (0x51ada0), so it is 2 exactly in frames where the
   think fired. Implemented as `World::playerTurretFireJitter` (the lock word is `playerViewLocked_`) and
   `turretViewJitter` (src/game/viewweapon.cpp); iw2view adds the result next to the recoil view kick.
   TODO(spec): the value 2 of ps+0x564 is only read as "fired this think"; other writers (0x51b150, 0x51b45a) set 1 and 0 for
   getting on and off.
2. Screen shake (cgame 0x4bf5e0 / 0x4bf700). The fire event 0xaf also adds a shake of scale 0.05, 100 ms, radius 100 at the
   turret origin (`ViewShake`). With the eye (tag_player) about 50 units from the origin the peak is about 0.02 degrees, so
   it is nearly invisible; the jitter above is the visible part. Constants checked in the 1.3 executable: 18 / 16 / 10 degree
   gains, 8 pi / 15 pi / 12 pi rates, 600 ms time base.

## Not implemented (TODO)

- Full AI gunner rules of 0x51c630 / 0x51cc80 (remembered target +0x60, suppression and visibility from node
  visibility, the 65536 distance check, giving up the turret 0x54b020); here simplified as above.
- Turret bone rotation (tag positions ignore the turret angles), the near-miss spread 0x550740 before convergence,
  the 0x48b090 visibility value, the bullet spread distribution (0x531210), drop pitch flags 0x100…0x400, projectile
  turret weapons, and turret fire sounds/effects (0x4d8020).
- The enemy checks of 0x51d3c0 / 0x51d640: here a free turret can always be used.
- `restoredefaultdroppitch` (0x51cf10), player use of turrets (`useby`, `maketurretusable` effects).

## Hint strings (sethintstring 0x4f0ec0)

- Valid on `trigger_use`, `trigger_use_touch` (hint index byte entity +3) and actors (+0x2124); anything else errors
  "The setHintString command only works on trigger_use entities and actors.". A plain empty-string first argument clears the
  hint (index 0xff / -1); every other call builds the text with 0x4eb9b0 (`src/game/hintstring.h`, name "Hint String").
- Buffer (max 1024 bytes, "<name> is too long. Max length is 1024"): a localized reference (`&"KEY"`) is validated by 0x4eb800
  (only for references longer than one character: alphanumerics and `_`), separated from the text before it by 0x14. A plain
  value is introduced by 0x15 when it is the first argument or follows a localized one; plain values in a row are concatenated.
  0x14 / 0x15 / 0x16 inside plain text is the error "bad escape character (%i) present in string" (checked before the first
  letter, 0x4eb860). Plain text with letters warns (error with dvar `loc_warningsAsErrors`; both default 0) and with dvar
  `loc_warnings` gets the prefix "UNLOCALIZED: ". Bytes outside 0x1f..0x7f are copied as `.`.
- The text goes into one of 32 configstrings (0x4cd + slot, 0x4f0da0): the first empty slot, or the slot already holding the
  same text; when all 32 differ the error "Too many different hintstring values. Max allowed is 32 different strings" is raised and
  the index stays -1. The table is shared with the weapons' useHintString / dropHintString that G_RegisterWeapon (0x522ba0)
  registers (def +0x574 / +0x578). TODO(spec): OpenIW2 does not register weapons, so those slots are not counted.
- Client reader 0x496e80 ("Hint String", from 0x4a9ac0): scans pieces delimited by 0x14 (localized follows) / 0x15 (plain follows)
  / 0x16, localizes the flagged ones (0x496dd0), enforces 1024 bytes and rewrites "&&<digit>" into 0x16 markers (a `&&0` is
  an error). `World::hintStringText` joins the pieces; TODO(spec): the &&N parameter substitution of 0x496e80 past 0x49704d
  and where plain pieces are localized are not decoded ("&&1" is replaced by the use key in the HUD as before).
- HINT_INHERIT (-1, only settable on trigger_use(_touch)): the cgame draw indexes the icon array at -1, which is the tracer
  material (0xf5b4a4), and shows the hint string when its index is >= 0. TODO(spec): scripts never use it (only HINT_NONE is
  used by shipped scripts), so it is not drawn here.
- Every hint name in the shipped scripts is accepted by the setcursorhint rule (case-insensitive HINT_NONE, HINT_ACTIVATE,
  HINT_HEALTH, HINT_FRIENDLY): only HINT_NONE occurs.
- A player on a vehicle: the crosshair (0x4bd000) reads the weapon byte of the entity at ps+0x568 (cg 0xf70e50; VEH_LinkPlayer stores
  the vehicle there, 0x511774); the vehicle setup writes it (0x50d8a3) as the weapon index of its turretWeapon.
- restoredefaultdroppitch (0x51cf10): tag poses are the dobj's (including the turret bone controls); the points are carried to the
  world by the full entity angles (e+0xe8) at the origin, not the yaw alone.
- Compass ping of a turret shot (0x4bb069): the user's centity byte +0x140 is "in the current snapshot" (0x4a4336).
