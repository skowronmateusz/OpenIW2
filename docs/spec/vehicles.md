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

## Not implemented (TODO)

- Free vehicle physics without a path (0x510d30), `joltbody` (0x5136e0 → 0x50dcf0), player driving
  (`useby`, VEH_LinkPlayer), vehicle turrets (`setturrettarget*`, `fireturret`, `isturretready`).
- The speed fraction (speed / engineSndSpeed, vehicle +0x1fc / +0x1f8), engine sounds and texture scrolling
  (`g_vehicleTexScrollScale`).
- Animated wheel tags: the tag positions come from the entity's current animation pose (bind pose without one);
  the exact pose the original samples (0x4d7460) is not checked.
- Exact frame order of the vehicle think relative to script threads.
