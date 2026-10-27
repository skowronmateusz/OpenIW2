# Vehicles

Script vehicles (`script_vehicle`, `spawnvehicle`) driving along `info_vehicle_node` paths, read with REA from
`CoD2SP_s.exe` (1.3). Implementation: `src/game/vehicles.cpp`.

Many vehicle methods (0x513070…0x5148e0) are reached only through the method table, so Ghidra's PE analysis has no
procedure there. They were read by carving the executable from the method address to the end of the file
(`exe[VA-0x400000:]`, the mapping REA `address_to_file_offset` reports for `.text`), opening it as raw x86 at that
address and reading the entry procedure's instructions and pseudocode.

## Units

- Speeds in the vehicle code are inches per second. Script speeds are miles per hour: `setspeed`, `resumespeed`,
  `setwaitspeed` and node `speed` multiply by 17.6 (float 0x5923e0 = `cdcc8c41`); `getspeedmph` multiplies by
  0.0568182 (0x5923f0 = `2eba683d`), `getspeed` returns inches per second.
- The vehicle think runs every server frame with dt = 0.05 s (0x591e54).


## Path nodes

`info_vehicle_node` and `info_vehicle_node_rotate` (spawn dispatcher 0x4f9770) add a 64-byte node (at most 1500, 0x5db
check). Node init 0x529e00: speed −1, lookahead −1, angles (π, π, π) meaning "not set", no next/previous node. The
`_rotate` class sets the rotate flag. Script-visible fields (table at 0x5a95f8, read-only): `targetname`, `target`,
`origin`, `angles`, `speed`, `lookahead`, `script_noteworthy`.

At spawn a non-negative `speed` is converted mph → inches per second (0x52b490).

Linking (0x52ada0), in node order:

1. `next` = first node whose `targetname` equals this `target`; `prev` = first other node whose `target` equals
   this `targetname`. A node linking to itself has none.
2. For nodes with a next: `dir` = unit vector to it, `length` = distance; normal (non-rotate) nodes take
   `angles` = vectoangles(dir).
3. Per node, in order (later nodes see earlier resolved values):
   - speed (0x529f70): own value if ≥ 0, else interpolated by path distance between the nearest earlier node and
     the nearest later node with speed ≥ 0 (only one found: its value; none: 0). Lookahead (0x52a100) the same with
     "> 0".
   - rotate nodes without their own angles interpolate them the same way between neighbours that have angles
     (0x52a290); none: (0, 0, 0).
   - angles are quantized to 16 bits into (−180, 180] (0x424040).
   - speed ≤ 0 or lookahead ≤ 0 ends the path here (next = −1). A last node with speed ≤ 0 gets 1, lookahead ≤ 0
     gets 1.

## Path position

The vehicle keeps a path position: current node, end flag, fraction along the node's segment, path speed and
lookahead at that point, rotate weight, origin, angles, lookahead point, and two node copies used by
`setswitchnode`. `attachpath(node)` (0x513070 → 0x52b200) sets it to the node (fraction 0, speed/lookahead from the
node, rotate weight 1 for a rotate node, origin and angles from the node) and places the entity there.

One path step (0x52b290):

1. If a switch node is set, the node named like it is temporarily replaced by the switched copy.
2. Lookahead point (0x52a840): walk `lookahead * speed + fraction * length` inches along the chain from the
   current node; stop at a node with no next (point = its origin) or inside a segment.
3. dir = point − origin. Zero length sets the end flag. Otherwise angles = vectoangles(dir) (quantized), origin +=
   unit dir × speed × 0.05.
4. Projection (0x52a8e0): from the current node, find the first segment with `dot(pos − node, dir) ≥ 0` and
   `dot(next − pos, dir) ≥ 0`; fraction = first / (first + second). A node without a next stops the walk with
   fraction 0. The end flag is "current node has no next". Speed and lookahead are interpolated between the node
   and its next by the fraction. The step reports "wait node passed" when the wait node is one of the nodes
   examined (including the current one).
5. Rotate weight (0x52a640) and angles (0x52a6c0): between two normal nodes nothing changes; last node: a rotate
   node's angles; otherwise the heading is blended with the rotate node's angles (rotate→rotate weight 1,
   rotate→normal 1 − fraction, normal→rotate fraction).
6. The switched node is restored.

`setswitchnode(from, to)` (0x5132a0 → 0x52b3c0) stores a copy of `from` whose next is `to` (with
direction and length recomputed) and an unmodified copy.

## Vehicle think (0x5110a0)

Called from the vehicle entity think 0x512b80 when the vehicle was started (`startpath` sets flag 2 at +0x174) and
the vehicle type is not 4; otherwise 0x510d30 runs free physics.

1. Speed: mode 0 (never set) uses the path speed. Mode 1 (`setspeed(speed, accel)`) and mode 2
   (`resumespeed(accel)`, target = path speed) move the speed towards the target by accel × 0.05 without
   overshooting; mode 2 returns to mode 0 when it reaches the target.
2. If the path speed is > 0: steps += speed / path speed. A copy one step ahead is computed; while steps > 1 the
   vehicle moves to it, steps −= 1 and a new step-ahead copy is computed (these steps report the wait node).
3. At the path end the speed is 0.
4. Entity origin = lerp(path origin, step-ahead origin, steps); angles = angle-lerp of the two, then each
   approaches the previous frame's angle by rate × 0.05 of the difference (pitch 6, yaw 4, roll 6; 0x420a20:
   only when the difference exceeds 0.001 and is not smaller than the step).
5. Velocity = (origin − previous origin) × 20.
6. Notifies on the vehicle: `reached_wait_node` when a step passed the wait node (wait node set), `reached_end_node`
   every frame at the path end, `reached_wait_speed` (wait speed ≥ 0) when the speed reached or crossed the wait
   speed this frame (also while it stays equal). Constant strings: 0x1c7f0b2/b4/b6 set in 0x5064a0.

Vehicle init (0x50da80): no path, wait node −1, wait speed −1, speed mode 0, speed 0.

## Methods

| Method | Behaviour |
|---|---|
| `attachpath(node)` | see above |
| `startpath()` | error "Can't start path on a vehicle that hasn't been attached" without a path |
| `setswitchnode(from, to)` | see above |
| `setwaitnode(node)` | wait node index |
| `setwaitspeed(mph)` | error when negative |
| `setspeed(mph, accel)` | mode 1; errors for negative speed/acceleration; further arguments are not read |
| `resumespeed(accel)` | mode 2 |
| `getspeed()` / `getspeedmph()` | current speed |
| `setvehicleteam(team)` | "axis" 1, "allies" 2, "none" 0, otherwise error |
| `freevehicle()` | classname becomes `script_vehicle_corpse`, notify `death` (0x513850) |
| `getvehicleowner()` | the driving player, nothing otherwise (0x513ab0) |
| `makevehicleusable()` / `makevehicleunusable()` | usable flag (0x513b20 / 0x513b80) |

## Vehicle files (0x50cf60)

`vehicles/<vehicletype>` starts with `VEHICLEFILE` followed by `\key\value` pairs; fields per the table at
0x5a91e8: `type` (enum "4 wheel" 0, "tank" 1, "plane" 2, "boat" 3,
"artillery" 4; strings at 0x5a91a8), `steerWheels`, `texureScroll`, `quadBarrel`, damage flags, floats
`texureScrollScale`, `maxSpeed`, `accel`, `rotRate`, `rotAccel`, `collisionDamage`, `collisionSpeed`,
`suspensionTravel`, `maxBodyPitch`, `maxBodyRoll`, turret fields, sound names and `engineSndSpeed`. `maxSpeed`,
`accel`, `collisionSpeed` and `engineSndSpeed` are multiplied by 17.6 at load. An unknown type warns and uses
`defaultvehicle` (0x50d1b0). Vehicle type 4 runs no think (0x512b80).

## Ground alignment (0x50ef70, axis helpers 0x421da0 / 0x424b70)

For types 0 (4 wheels) and 1 (6 wheels), after the path think (settling) and in `attachpath` (no settling):

1. Wheel tags `tag_wheel_front_left`, `_front_right`, `_back_left`, `_back_right`, `_middle_left`,
   `_middle_right` (constants 0x1c7f15a…164) are placed with the current angles at (origin x, origin y,
   previous frame's z). A missing tag is an error ("Script vehicle [%s] needs [%s]"); here alignment is skipped.
2. Each wheel traces a point from +64 to −256 (mask 0x211, 0x10211 for a player-driven vehicle). The hit height
   (or the trace end) is the wheel's ground; the surface type `(surfaceFlags >> 20) & 31` is kept for
   `getwheelsurface`. Settling: the wheel's vertical speed drops by 40 per frame and its height moves by speed ×
   0.05, never below the ground; without settling the wheel sits on the ground.
3. Plane: right − left wheel midpoints and front − back midpoints of the first four wheels give two unit vectors;
   their cross product is the normal; the plane passes through the front-left wheel, raised so that no wheel is
   more than `suspensionTravel` above it.
4. New forward/left axes from the normal and the old forward give pitch and roll; each approaches the previous
   frame's value at rate 6 and is clamped to ±60°. Yaw is unchanged. The origin's z is the plane height at the
   origin.

Matmata's jeep (`xmodel/vehicle_africa_jeep`) has its wheel tags at z = 0 (ground contact), so its origin ends on
the ground, about 40 units below the path nodes there.

`getwheelsurface(wheel)` (0x513920): wheel
names `front_left` … `middle_right`; errors "Vehicle type [%s] has no wheels" for types other than 0/1 and
"Vehicle has no middle wheels" for type 0; surface 0 → "none", 1–22 → the surface name table at 0x5a0344
(bark, brick, carpet, cloth, concrete, dirt, flesh, foliage, glass, grass, gravel, ice, metal,
mud, paper, plaster, rock, sand, snow, water, wood, asphalt), anything else → "default".

## Vehicle turret

Targets: `setturrettargetvec(point)` (0x513f40) and `setturrettargetent(ent, offset)` (0x514050)
store a target and error with "Can't set target on player's vehicle" or "Vehicle must have health to control the
turret" (health ≤ 0); `clearturrettarget()` (0x5141b0) clears it.

Per frame for every vehicle with a target and health ≥ 1 (0x5102e0):

1. Direction from `tag_barrel` (world position, 0x4d75b0) to the target point (entity origin + offset, inferred from
   the stored offset, or the vector), expressed in the vehicle's axes; its pitch and yaw are the wanted turret
   angles (relative to the body).
2. The turret pitch/yaw (entity +0x70/+0x74) turn towards them at `turretRotRate` degrees per second (0x420970 /
   0x4208f0: constant step, snap within one step), pitch clamped to [−turretVertSpanUp, turretVertSpanDown], yaw to
   [−turretHorizSpanRight, turretHorizSpanLeft].
3. When both differences (before turning) are under 1°: notify `turret_on_target`; for an entity target also
   `turret_on_vistarget` when a trace (mask 0x801) from the barrel to the target is clear. Constants 0x1c7f0f2 /
   0x1c7f0f4.

`fireturret()` (0x514260): needs health,
the vehicle file's `turretWeapon` ("No weapon specified for [%s]"), a bullet or projectile weapon ("Vehicles only
support bullet and projectile weapons") and `tag_barrel`. It fires from `tag_flash` (quadBarrel: two of
`tag_flash`, `tag_flash_11` / `tag_flash_2`, `tag_flash_22`, alternating each shot); bullets reach 8192 units. It
also jolts the body backwards (0x50dcf0) and stores the weapon's fire time.

Aim bend (0x5144e7): with a driving player, a single barrel and no pull-back (+0x19c = 0) the shot direction is
the flash direction moved towards the player's aim point (+0x1bc) by at most 10° (0x591e8c, −10 at 0x59213c) in
pitch and in yaw separately (AngleSubtract 0x423f50, roll 0).

Here bullets fly along the body's axes turned by the turret angles (inferred: the original uses the flash tag's own
axis, rotated by the client-side turret bone controllers).

### Turret projectiles and the fire event

A projectile turret weapon fires through 0x531a40 with no spread: a rocket from each flash tag along the same
direction (G_FireRocket 0x527810): classname `rocket`, ET_MISSILE, explode think in 30 s, flags 0x400, parent
(+0xf4 / +0x134), direct damage = the weapon's `damage` (+0x1ec), clip 0x280e891, linear trajectory at
`projectileSpeed` (+0x384) from 50 ms ago, the weapon's `projectileModel`. G_RunMissile (0x526c30) traces from the
origin to the trajectory point each frame (the parent passes); the sky (surface 0x10) frees it, else the impact
(0x526200): the hit entity takes the direct damage (MOD_PROJECTILE = 5, the missile classname's means of death at
0x5a5754 + 0xf · 0x2c; splash +4 = 6, MOD_PROJECTILE_SPLASH); with `projImpactExplode` the event by
`projExplosionType` (+0x390: 0 → 0xbf EV_GRENADE_EXPLODE, 1 → 0xc0 / 0xc1 EV_ROCKET_EXPLODE when a sentient was hit,
else 0xc2 / 0xc3) with the hit surface and normal, then radius damage (`explosionInnerDamage` +0x37c,
`explosionOuterDamage` +0x380, `explosionRadius` +0x378, the hit entity skipped). Client (0x4ba8d0): 0xc0 plays
`rocket_explode_<surface>` (0xf5b800, built from "rocket_explode" at 0x4a28f5) and impact effect type 10 (+0x398);
all explode events then play the weapon's `projExplosionEffect` and `projExplosionSound`.

The shot sends EV_FIRE_WEAPON (0x9e) or, for quad barrels, EV_FIRE_QUADBARREL_1 / _2 (0xb0 / 0xb1 by the barrel
pair). The client (0x4b7800, per flash tag) plays the weapon's `fireSound` and, for an entity that is not the local
client, its `worldFlashEffect` at the flash tag (0x4b6660). Open: projImpactExplode 0 (bounce), whizby, water, the
0xc1 / 0xc3 flag 0xf79778, the shell eject 0x4b7730, the recoil jolt 0x50dcf0.

## Entity think (0x512b80)

Every script vehicle, each frame: previous origin/angles = the entity's (0x50fc40); a dead owner (health < 1) is
let out (VEH_UnlinkPlayer); types other than 4 run the path think (started, +0x174 bit 2) or free physics; the
body is applied to the entity (0x50d3e0); touch (0x50ff00, TODO); +0x19c = 0; a player-driven vehicle (+0x174 bit
0) runs the player aim (0x5108e0); then the turret (0x5102e0). So artillery (type 4, flak88) does not move but
still aims and fires for its driver. Here the driver is kept at `tag_player` right after the think.

## Free physics and player driving (0x510d30)

Body layout from vehicle +0xb8: origin, previous origin +0xc, angles +0x18, previous angles +0x24, mins/maxs
+0x30/+0x3c (zero: the vehicle traces a point, 0x50d970), velocity +0x48, local velocity (forward, left, up) +0x54,
angular velocity +0x60. Clip mask 0x810211 (0x50d900), skipping the vehicle.

1. Command: zero without a driver. With one, pm_flags 0x40000 is set; unless pm_flags 0x80000 or ps+0xc 0x8000
   the command is the usercmd forward (+0x85dc) and right (+0x85dd) bytes and 127 while button 0x400 (jump:
   `+gostand` / `+moveup`, in a vehicle kbutton 0x5d2098 always counts, 0x409c56) is held; jump clears 0x40000.
2. Accelerations (0x50de40): forward target
   = (local forward ± accel × 0.05) × |forward| / 127, clamped to ±maxSpeed, acceleration (target − speed) × 20
   clamped to ±accel; sideways target 0; vertical target the speed clamped to ±maxSpeed. Yaw: right / left
   (right negative) like forward with rotAccel and rotRate; with jump held the target is (view yaw − previous yaw)
   × 20 clamped to ±rotRate, without the rotAccel limit. Pitch and roll speeds go to 0. The jolt override
   (+0x1dc/+0x1e4/+0x1e8, only set by `joltbody` 0x50dcf0) is not used.
3. Angular velocity += acceleration × 0.05; yaw = 16-bit-normalized(previous yaw + yaw speed × 0.05); pitch and roll
   0. Velocity += (forward, left, up of that yaw) · acceleration × 0.05.
4. If moving: ground trace 0.25 above to below (0x50e4f0, unstick 0x50e3a0 with the player's 26 offsets);
   walkable (normal z ≥ 0.7) → ground move (0x50ee50: clip keeping the speed, step slide without gravity);
   otherwise clip to a hit plane and step slide with gravity (0x50ebe0 / slide 0x50e630: 40 per frame, step 18).
   The clip rule (0x50e2f0) keeps a mostly horizontal velocity on walkable planes and gives it the plane's slope;
   other planes remove the normal part × 1.01 (into) / 0.99 (away).
5. Types 0/1: ground alignment with settling. For a player-driven vehicle off a path its height is not set from
   the wheel plane (0x50ef70 writes z only when +0x174 bit 2 is set or bit 0 is clear); alignment always sets the
   angular velocity to (angles − previous angles) × 20.
6. +0x1b0 = |local forward speed|. TODO: the engine sound blend +0x1f8/+0x1fc.

`rotRate` and `rotAccel` (vehicle info +0x70 / +0x74) are degrees per second and per second², not scaled.

### Player aim (0x5108e0)

Clears the turret target, then (driver present, no pm_flags 0x80000): fire timer +0x184 −= 50 ms while > 0,
otherwise attack (bit 1) held and no ps+0xc 0x8000 → notify **`turret_fire`** (scripts such as `_tank.gsc shoot()`
then call `fireturret`, which restarts the timer with the weapon's fireTime). Errors "Player vehicle has no
tag_flash" / "…tag_barrel". Target (+0x1bc) = eye (0x4e8b70, view height 60 in a vehicle) + view direction
(client +0xc4 pitch, +0xc8 yaw) × 10240, cut at a trace with mask 0x280e891 skipping the player. A blocked sight
line `tag_barrel` → `tag_flash` sets +0x19c (with it clear, `fireturret` bends a player vehicle's shot towards the
target within ±10°, 0x5144ce–0x5147ae, TODO).

Turret for a driven vehicle (0x5102e0): frame = vehicle angles, with the view yaw as yaw while pm_flags 0x40000 is
clear; wanted = vectoangles(target − `tag_barrel`) with the view yaw as yaw and roll 0; relative angles =
AxisToAngles(axis(wanted) · axis(frame)ᵀ); then the same turning, clamping and notifies as above.

### Use key (0x5343d0 → 0x5342b0 / 0x534350)

`+activate` (button 8) or `+usereload` (0x20) pressed this frame (client +0x863c): in a vehicle (pm_flags 0x20000,
owner set) that is usable (contents 0x200000) → 0x5341a0: notify `trigger` with the player, then the use function
(= get out). Otherwise the hint entity of the scan (0x534490)
is used after `g_useholdtime` (default 0): entities whose absbox touches eye ± (108, 108, 96) with contents
0x200000; absbox centre within 128 of the eye for vehicles (72 for others); score = distance + (1 − (dir · view
forward + 1) / 2) × 144, sorted; +10000 when the eye → centre trace (mask 0x11) is blocked, sorted again; the first
passing 0x512200 (player not in a vehicle and not linked, vehicle without owner, |+0x1b0| ≤ 1, health > 0,
usable). The vehicle's usable flag starts as spawnflags & 1 (0x50dcd5). The scan is gated by a global 0x9bef24
(treated as set). The vehicle absbox comes from its collmap brush (0x50dc40), turned by the angles; a vehicle without
one uses the xmodel bounds (inferred).

### Collmap (0x50dc40, finder 0x50d270, collmap spawn 0x512fd0)

After the level load (0x4d9c6f, every vehicle slot) and when a vehicle's model changes (0x511a6f, 0x511c5b) the
vehicle takes the brush model of the in-use `script_vehicle_collmap` whose targetname equals the vehicle's model
name (case-insensitive) and becomes solid with contents 0x800000 (0xa00000 with spawnflag 1); an empty brush model
warns "Cannot use empty vehicle collmap for [%s]". The collmap entities themselves have no contents (eType 0xe,
contents 0). The player (mask 0x281c011), bullets (0x280e831), rockets (0x280e891) and other vehicles (0x810211)
collide with it.

### Player in a vehicle

Pmove with pm_flags 0x20000 (and no 0x80000) stands with view height 60 (0x509d10); the weapon fire mask excludes
attack (0x4df9b0 uses 0x30014), so the player's own weapon does not fire. View (cgame 0x4aeb60): each frame D = Bprevᵀ · Bcur of `tag_body`'s
world axes (previous stored transposed, 0x422ab0), lerped from identity by `cg_viewVehicleInfluence` (default 1)
through quaternions; view axes · D (0x422450) → angles; pitch always taken, yaw only while pm_flags 0x40000. The
command's delta angles are adjusted so the input keeps that view.

## Player use (`useby`)

`useby(user)` (0x4ef020) notifies `"trigger"` with the user, then calls the entity type's use function (table
0x5a5740). For vehicles (0x512f00) a player user gets in, or out when already in a vehicle:

- VEH_LinkPlayer (0x511610): errors "Player is already using a vehicle", "Player already has an owner", "Vehicle
  already has an owner", "Trying to use vehicle without a bone [tag_player]"; the player is placed at `tag_player`
  with the vehicle's pitch and yaw (roll 0) and linked to that tag; vehicle +0xf4 and player +0xf4 / client +0x568
  record each other; the vehicle's +0x174 bit 0 (player-driven) is set; client flags `|= 0x60000`, `&= ~0x80000`.
- VEH_UnlinkPlayer (0x511780): error "Player is not using a vehicle"; the player is placed at `tag_detach`
  (warning "no [tag_detach] on vehicle" and the vehicle origin + body maxs z, which is 0, otherwise), takes the
  client's view pitch and yaw (roll 0), +0x174 bit 0 and pm_flags 0xe0000 are cleared, and both links are cleared.
- `getvehicleowner()` returns the linked player.

## Not implemented (TODO)

- `joltbody(point, intensity [, speedFraction 0…1, deceleration mph])` (0x5136e0; errors "Speed fraction must be
  between…", "Deceleration can't be negative"; deceleration × 17.6) → 0x50dcf0(dir = origin − point normalized):
  intensity clamped to [0, 1]; +0x1d4 / +0x1d8 = the direction against two of the body axes (0x421da0; which rows
  is not settled from the decompile), 2D-normalized (0x421160), × intensity × maxBodyPitch (info +0x78) /
  maxBodyRoll (+0x7c); +0x1dc = 1, +0x1e0 = 0, +0x1e4 = speedFraction × |speed| (+0x1b0), +0x1e8 = deceleration.
  Every think 0x510770: while +0x1dc > 0, es +0x64 / +0x6c = sin(+0x1e0°) · +0x1dc · +0x1d4 / +0x1d8, +0x1dc −=
  0.05, +0x1e0 += 36. Free physics (0x50de40) uses +0x1e4 / +0x1e8 instead of maxSpeed / accel while +0x1dc >
  0 (double 0x591e70 = 0). Open: the axis rows, and how the client draws es +0x64 / +0x6c (vehicle draw 0x4ac760, case 0xc
  of 0x4acf30). The fireturret recoil calls 0x50dcf0(−flash forward, 1, 0, 0). `isturretready`, touch (read, not implemented): 0x50ff00 (every think, for a solid vehicle +0xa1) takes the
  entities with contents 0x2006081 in the vehicle's absbox widened by its move and turn this frame (+0xb8 − +0xc4,
  AngleSubtract of +0xd0… − +0xdc…), skips itself and its owner, keeps players (eType 1), actors (9) and script
  movers (5) in exact contact (0x447830; a trigger-shaped entity via 0x4481c0), notifies "touch" both ways
  (scr_const 0x1c7f0e2) and runs both touch callbacks; then for a player or a living actor 0x50fe40: unless the
  entity is linked or neither move nor turn is ≥ 0.001², it is pushed (G_TryPushingEntity 0x52be60: moved and turned
  with the vehicle; when stuck (0x52bd20) a grid search of offsets up to +0xb4 / 0x5922e0; a pushed client's ps origin
  and delta yaw follow, an actor's look yaws); if it cannot be pushed and takes damage it gets 999999 damage with
  means of death 9 — except an actor in the animscripted (7) or negotiation (9) state (0x4ff310). The stuck test 0x52bd20 traces the entity box
  at the candidate with its clip mask (+0x12c: actors 0x2820011 from 0x4fc745, the player 0x281c011 from
  0x4e90ec; 0x811 without one; a missile passes its owner, others themselves). Search when maxs.x / 2 (double
  0x5922e0 = 2) exceeds 4 (0x5922d8): z = 0, ±4 …, then x, y = ±4, ±8 … (step 4, 0x591e64) below that. Implemented in
  `World::vehicleTouch` / `tryPushEntity` (contact = a zero-length box trace against 0x800000 starting in the
  vehicle; the pushed player goes to the front end through the setorigin / setplayerangles channel). Open: script
  movers and the touch callbacks, the entity list box, the ground entity reset, actor look yaws (0x504910). the
  ps+0xc 0x8000 state bit, the vehicle collmap absbox, other usable entities in the use scan.
- The recoil jolt, turret spin/stop sounds, and turret bone rotation in tag positions.
- Engine sounds (read, not implemented): the vehicle file's six sound names become alias indices at info +0x260…
  +0x26a after loading (0x5118b0, G_SoundAliasIndex 0x4d5860). Spawn (0x50dbb7): +0x1ec = 1 (engine on; script
  `startenginesound` 0x513e80 sets 1, `stopenginesound` 0x513ee0 sets 0), fractions +0x1f8 / +0x1fc = 0, and an
  ET_SOUND_BLEND helper (0x52f6f0 → G_Spawn, eType 6) for low/high idle (+0x1f0) and for low/high engine (+0x1f4)
  when both names of the pair are set. Fractions: path think (0x511513) f = clamp(+0x1b0 / engineSndSpeed, 0, 1),
  engine = f, idle = 1 − f; free physics (0x510fe6) DiffTrack (0x420a20, rate 4, 0.05 s) towards idle 1 / engine 0
  when the driver's forwardmove (client +0x85dc) is 0, else idle 0 / engine 1. Every think (0x510be0): dead (health
  < 1) or engine off → the helpers' aliases are 0; else helper es +0x8c / +0x8e = low / high alias, +0x94 = its
  fraction. Turret state +0x18c = 2 with turretSpinSnd: the vehicle's loop sound (es +6); = 1 with turretStopSnd:
  event 0xb3. cgame (0x4acf30 case 6 → 0x4ac8a0): with both aliases, the fraction interpolated between snapshots,
  play blend 0x442900 → 0x4423d0 at the helper's origin: culled beyond lerp(distMax), an already playing loop of the
  same pair only takes the volume lerp(volMin) (0x441f80), a new one a random volume in lerp(volMin…volMax) and
  pitch in lerp(pitchMin…pitchMax) (0x442310); both aliases must use the same file (0x442650). Open: where the
  helper entities get their origin (no vehicle code writes it; G_Spawn leaves it at 0), so playback is not wired.
- Texture scrolling (`g_vehicleTexScrollScale`).
- Animated wheel tags: the tag positions come from the entity's current animation pose (bind pose without one);
  the exact pose the original samples (0x4d7460) is not checked.
- Exact frame order of the vehicle think relative to script threads.

### Barrel pull-back (+0x19c / +0x198), partly decoded

- +0x198 (think 0x512a70..0x512b00): when the tag indices +0x218 and +0x214 are both >= 0, the distance between their origins
  (0x4d75b0 per tag, sqrt 0x565e40); stored every think. +0x214 is the tag of script constant 0x1c7f170, +0x218 the first of the
  tags named by the table at 0x5a91d4 (511ffe..512041; the names are not resolved here).
- +0x19c is cleared every think (0x512c89) and set to 1 in the player aim (0x510bc7) when the visibility test 0x44c1b0
  (zero box 0x56ce9c, mask 0x7ff, entity from +0xf4) returns 0.
- Fire (0x51474d): with +0x19c set the shot start is moved back along the shot direction: start = start - dir * (+0x198)
  (0x514757..0x5147a7: -(+0x198) * dir + origin).
- Open: the endpoints of the 0x44c1b0 test and the two tag names; therefore not implemented (shotDir/start unchanged).
- joltbody (above) and the fireturret recoil jolt are implemented (`World::vehicleJolt`, `vehicleJoltStep`); the heading
  "Not implemented" in this file is stale for them.
