# Scripted animations and player links

`animscripted`, `startscriptedanim`, `stopanimscripted`, the AI animscripted state, script links in the parent's
tag frame and the player links (`playerlinktodelta`, `playerlinktoabsolute`), read with REA from `CoD2SP_s.exe`
(1.3). Implementation: `src/game/animscripted.cpp`, `src/game/playerlink.cpp`, link placement in
`src/game/movers.cpp`. Evidence: raw-carve and PE decompiles of the method entries and the objdump listing.

## Frames

Rotations are kept as three rows forward, left, up (AnglesToAxis 0x421da0 returns forward / right / up and the
callers negate right). AxisToAngles 0x424b70, `origin + v·rows` 0x423430, row product 0x422450 (row i of the result
= Σ local[i][j] · parent[j]), yaw frame 0x424b20, yaw of a delta quaternion (z, w) 0x423900:
`atan2(2wz / (z²+w²), 1 − 2z² / (z²+w²))` in degrees.

## animscripted (0x4f6240 → 0x4f5d50 with 1; startscriptedanim 0x53c520 → 0x4f5d50 with 0)

Arguments: notify name, origin, angles, animation [, mode [, root]]. More than 6: "too many parameters"; fewer than
4: "Incorrect number of parameters for animscripted command". The mode is undefined, `normal` (scr_const 0x1c7f0a6)
or `deathplant` (0x1c7f05e, string 0x58a520), else "Illegal mode %s for animScripted. Valid modes are normal and
deathplant". `startscriptedanim` needs an actor ("not an actor").

On an actor: a dead AI is an error ("tried to play a scripted animation on a dead AI…"). With `animscripted`
(0x4f5f2d): push state 7 (0x4feb40, result ignored), kill the animscript (0x53ac40), a running scripted animation is
set again at weight 1 / 0.2 s / rate 1 without notify, and `animscripts/scripted::init` (slot 0x1c7e084, string
0x587e38) is called with the six arguments; its `main` (slot 0x1c7e110) later calls `startscriptedanim`. Otherwise
(non-actors, and actors through `startscriptedanim`) the start below runs at once (0x53b190).

A linked entity (tagInfo at +0x1c8) keeps the parent frame of this moment in tagInfo +0x40 (0x4f60a3, 0x4d6ad0 then
the inverse 0x422c60), for animscripted on actors and for non-actors.

### Start (0x53b190)

Needs entity flag 0x2000 ("entity (classname: '%s') does not currently support animscripted"; here: a tree).
The scripted block (+0x1d0, 0x60 bytes from the pool 0x82e980) is created if missing: root (+0x4a), started
(+0x4c) = 0, animation (+0x48), deathplant (+0x4d).

- normal: origin as given, frame = AnglesToAxis(angles).
- deathplant: the origin is dropped by a trace from 36 above to 18 below (0x44bf00, mask 0x820011, passing the entity itself, box
  (-15,-15,0)..(15,15,72) read from 0x56f704 / 0x56f710; no start-solid and a hit: the hit height), the frame keeps yaw and roll
  (pitch 0). Implemented as `deathPlantOrigin` (animscripted.h, tests/test_deathplant.cpp). Only the drone death animations of
  _drones.gsc, _vehicledroneai.gsc and duhoc_assault_drones.gsc use the mode. Each frame (0x53bb84..0x53bc53) the same box goes from 18 above to 18 below the animated height plus the lift +0x50 (zero at the start, 0x53b389); a hit
  moves the position to the hit height and stores the difference in +0x50. `deathPlantFrame`. Open: the tilt, pitch += w * +0x54 and roll += w * +0x58 (0x53bc57..0x53bcb4), w = the animation's progress (0x477fe0) while +0x5c is negative, else +0x5c rising by 0.05 per frame to 1; its setup after 0x53b460 from 0x478740 at time 1 and 0x4fd890 is not read.

The root's children are cleared over 0.2 s (0x4789a0 = clear 0x478940 on every child). Open: for an actor with
+0x1b8 ≠ 0 and +0x1c4 ≠ 0 (the flag at 0x53b1ff..0x53b226; neither field is identified) the weights of the nodes at actor
+0x1d0 / +0x1d2 / +0x1d4 are first frozen: 0x478d10 on the +0x1d2 or +0x1d4 node when its weight is above 0 (0.2 s, rate 1), then
0x4795e0 on the +0x1d0 node with its own weight (0.2 s), and the root's children are cleared (0x4789a0, 0.2 s) only with a root
argument. Then the animation is set (0x4795e0, notify in
ecx) at weight 1, 0.2 s, rate 1, restarted unless it loops (leaf: xanim +2; blend node: entry +0x10 bit 1, open).
0x478220 (a 0.05 s time update up to the next notetrack, flag 1) is called right after the set by the setanim built-ins (0x4f5d45..0x4f74fd; skipped with the no-physics flag `+0x128 & 0x1000`) and here at 0x53b86d: `World::postSetAnimUpdate` -> `AnimState::updateTruncated(0.05, true)`. It steps from each node's old time and fires the notetracks of the first 50 ms at once; the frame's weight stage (stage 2, `updateWeights`) and time stage (stage 4, `updateTruncated`) are split as in the exe so the animation runs one 50 ms step ahead after a set (the post-set step moves the time, the next weight stage sets the old time to it) and no notetrack is sent twice (tests/test_animtree.cpp). 0x53b86d tests the flag like the setanim family (0x53b836..0x53b853: `+0x128 & 0x1000` set skips the call, then the model anim table entry 0x74a980 -> 0x74c200 must exist), so `postSetAnimUpdate` is right for both.

The pose at the animation's current time: delta d = 0x4785d0 (0x477b80 with tree byte +6 set → 0x475aa0: the
delta tracks sampled at the node's time, rotation (0, 0) → (0, 1)); position = origin + d.trans·frame, angles =
AxisToAngles(yawFrame(yaw(d.rot)) · frame). Offsets: origin offset (+0x30) = entity origin − position, angle
offsets (+0x3c) = AngleSubtract(entity angles, pose angles) (0x423f50).

Mode byte (0x4f6066..0x4f608c): `al = (mode == "deathplant")` (scr_const 0x1c7f05e) is the 0x53b190 flag stored at +0x4d, so an
undefined mode and `normal` both take the plain origin; only `deathplant` traces (36 above / 18 below, 0x591f5c / 0x591f44,
box (-15,-15,0)..(15,15,72) at 0x56f704 / 0x56f710, mask 0x820011) and plants every frame (0x53bb84 gate). The unlinked
per-frame write (0x53bd8f..0x53bdc5) stores the pose as the entity origin and angles with no z handling.

Floating scripted actors (moscow, `IW2_FLOAT_LOG`): `anim_loop` / `anim_single` (maps/_anim.gsc) pass node or script_origin
origins, and the map files place path nodes 16–48 units above the floor. The exe puts every path node on the ground once at
level start (0x515f80 → 0x515d70, docs/spec/ai.md "Node drop"), so idles without a delta translation (e.g.
`downtown_sniper_blocking_door_idle`, `moscow_guy*_coldidle`, the moscow `end_cool_node` rest idles) stand on the floor; the
openiw2 loader used to keep the raw z. The scripted path itself has no z handling (mode byte above; the normal branch 0x53b61d;
the unlinked write). script_origins are not dropped (spawn 0x5334d0: mover init 0x5332b0, contents 0, link): moscow's
`letlev_node` / `welcome_dialogue_node` (-2159 -734 86) is 37 above the floor because it is the reference of the sync animation
`mos_0_01_rs1_pickuprifle_t`, whose delta translation starts at z −37.3 (feet at 48.7, the floor is 49.2). For the block idle
the commissar plays there before the welcome dialogue starts (~1.5 s) no exe path that lowers him was found: the script_origin is
not dropped, the scripted path has no z handling, and only the sync animation's delta covers the height from the dialogue on.
Actors that stand still at their spawner height (cityhall's spawner (4386 −4304 64), 28.6 above the terrain; moscow's
stationary guards) are put on the floor by the actor spawn drop 0x4fc550 and the level-start spawner drop 0x5325f0
(docs/spec/ai.md "Actor spawn drop"); the actor physics alone never lowers them (0x5487c0 returns while the ground entity is
the initial 0x7fe and the move delta is zero).

### Every frame (0x53baf0)

Called by the actor state think and, for other entities, by 0x52ccb0. Without a tree (0x4d6090) or animation the
block is freed. Pose as above, then the origin offset shrinks: f = 1 − rsqrt(|o|²)·0.05·300 (0x591f4c, the fast
inverse square root with one Newton step); f > 0: o ·= f, position += o; else o = 0 (0x53b9a0). Each angle offset
moves towards 0 by 0.05 · entity +0x1f0 degrees (540, G_InitGentity 0x4d79b2; other writers of +0x1f0 not
checked); a rest outside ±step is added and the angle normalized to [0, 360) (0x53ba60, 0x4240a0).

Not linked: origin and angles become the pose. Linked: the pose is carried by the parent's motion since
animscripted (parent now · tagInfo +0x40, 0x4d6c70), written as the entity's origin and angles, and the link offset
is recomputed from it (0x4d6ca0 with 0) — the pose relative to the parent frame at animscripted.

First update: started = 1. Later: when the animation has played through (0x478020: no state, or not (old time ≤
time, time ≠ 1, cycle ≤ old cycle)) it is set again at weight 1 / 0.2 s / rate 1 without notify and the block's
animation becomes 0 (freed next update).

### stopanimscripted (0x4f61e0 → 0x4f60d0)

An actor whose predicted top state is 7 pops it. With a block: its animation (if any) is set again without notify
(the arguments of that 0x4795e0 call are open; the 0x53bdf6 ones are used) and the block is freed.

## AI state 7 (animscripted)

Start 0x54b060: true. Finish 0x54b070: frees the block. Think 0x54b0a0: debug name "animscripted" (+0x21a0),
bytes +0x740 / +0x741 cleared (meaning open), the claimed node released (0x5191a0), the path budget and points
cleared (0x541780, +0x6b0 / +0x6b2, +0x724), animscript `scripted` with movemode 0 and anim mode 4 (0x53ac90, slot
0x1c7e110), byte +0x220 = 1. When the animscript thread is gone (variable type 0x17) the state pops if it is the
predicted top; otherwise 0x53baf0 runs, and without a block afterwards the same pop. Else perception (0x4fe890),
0x504950 (look angles from pitch / yaw, open), velocities cleared, think result 0.

## Script links (linkto 0x4ed770)

An empty tag name is no tag. With at most two arguments: G_EntLinkTo (0x4d6540) then 0x4d6ca0 (the child's current
placement expressed in the parent frame); with offsets 0x4d66a0. The parent frame (0x4d6ad0) is the tag's world
frame, or the parent's angles at its origin without a tag. The linked child = offset frame · parent frame. Open: the
"failed to link" errors. The tag frame uses the parent's full angles +0xe8 (0x4d6aec -> AnglesToAxis 0x421da0).

## Player links

`playerlinktodelta(parent [, tag [, fraction]])` (0x4eda80): players only ("not a player entity"); client +0x8694 =
fraction (default 0), +0x8698 = 0, pm_flags &= ~0x10000000, then the entity link. `playerlinktoabsolute(parent [,
tag])` (0x4edc60): +0x8694 = 1, +0x8698 = 1, pm_flags |= 0x10000000. `unlink` (0x4ede30): 0x4d6720 and the client
fields cleared. ClientThink (0x4e7bc0): linked → pm_type 1 (5 when dead); pm_flags 0x8000 while +0x8630 or +0x8698.

ClientEndFrame (0x4e7f90 → 0x4d6ec0) while linked: F = parent frame (0x4d6ad0; the link offset is not used).
Without pm_flags 0x10000000: q = quat(F) (0x4262e0); the first time (+0x8680 = 0) the reference +0x8684 =
conj(q) and the delta is identity; later delta = q · reference (0x4235e0), reference = conj(q), and the delta is
moved towards identity by the fraction (xyz · f, (w − 1) · f + 1); the view angles (ps +0x56c) become the delta
applied to them (0x423680, 0x424b70). With 0x10000000: view angles = AxisToAngles(F). Eye = F.origin + F.up · view
height (ps +0xd4), origin = eye − view height in z, trajectory stationary, linked (0x44b2d0). Open: here the new
view angles go to the front end (their own channel, `takePlayerLinkView`; the original writes ps viewangles; its delta_angles handling
is not read). +0x8680 is cleared by a client frame without a link (0x4e7fd6 -> 0x4e8023), so a link replacing a live one keeps
the reference.

Any link on a client runs this: ClientEndFrame tests the entity's link pointer (+0x1c8 at 0x4e7fd6), not which call made it, so a plain
`linkto(parent)` on the player is placed the same way, with the view fraction left at 0 (the view stays, the position follows the parent's
frame; the offset given or implied by `linkto` is not used). Without a link the client's pm_type returns to 0 / 4 (0x4e8023..0x4e803c).
decoytown (decoytown.gsc:665-745, the opening ride): the player is linked with the delta mode, fraction 1, to the carrier's seat tag
(decoytown.gsc:692) after `dontinterpolate` (:691) and `setplayerangles` (:693); when the carrier has reached its end node the script waits
2 s, unlinks (:711), links the player to a script_origin with a plain `linkto` (:712), moves that origin along its targets in 0.5 s steps (:714-722)
and unlinks again (:730). The player thus leaves the carrier's collision hull on the origin's path. The port skipped the plain link for the player
(its entity pass runs no client), so the player stayed in the seat, inside the hull, and every move started solid (`test_decoyride`).

Client side: the view origin and view angles of a linked player are interpolated between the last two snapshots by the frame fraction
(0x4ab740, CG_InterpolatePlayerState 0x4b9940), unless the snapshots' toggle bits (eFlags bit 2, flipped by `dontinterpolate`, 0x4edf90) differ.
The port draws the origin from the previous and the current server frame by the fraction and spreads the link's view turn over the interval
(`LinkViewSmoother`, linkview.h); `dontinterpolate` on the player makes the front end take the new origin at once.
