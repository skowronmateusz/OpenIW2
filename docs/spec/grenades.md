# Grenades

Sources: `CoD2SP_s.exe` (1.3) with REA. Ghidra decompiles, raw-carved
method entries and the objdump listing.

## Weapon file keys (field table 0x5a8000…)

`damage` +0x1ec, `fuseTime` +0x248 (seconds → ms), `explosionRadius` +0x378, `explosionInnerDamage` +0x37c,
`explosionOuterDamage` +0x380, `projectileSpeed` +0x384, `projectileModel` +0x38c, `projExplosionType` +0x390,
`projExplosionEffect` +0x394, `projExplosionSound` +0x398, `projImpactExplode` +0x39c, `parallel<Surface>Bounce`
+0x3a0 + 4·surface, `perpendicular<Surface>Bounce` +0x3fc + 4·surface. Surfaces (trace surface flags >> 20 & 0x1f):
Default, Bark, Brick, Carpet, Cloth, Concrete, Dirt, Flesh, Foliage, Glass, Grass, Gravel, Ice, Metal, Mud, Paper,
Plaster, Rock, Sand, Snow, Water, Wood, Asphalt.

## The grenade entity (0x5275f0)

Classname "grenade", entity type 3 (missile), handler 14 of 0x5a5750 (`{?, type 3, mod 4 = MOD_GRENADE_SPLASH, think
0x526600}`), parent +0x134 = thrower, owner +0xf4, fuse +0x138 = now + fuse, flags 0x1000000 (bounces), clipmask
0x280e891, trajectory TR_GRAVITY (5) from the start with the velocity. BG_EvaluateTrajectory (0x4fae80) for TR_GRAVITY:
base + delta·t − (0, 0, 400·t²). AI fields: +0x150 the actor going for it, +0xa2 & 1 in an actor's hand, +0x154 the
predicted landing point (zero = none), +0x160 its time, +0x164 the last grenade_ping.

**Flight** (G_RunMissile 0x526c30): every 500 ms the AI event grenade_ping (8, radius² 262144 from
the event table 0x57031c, 0x526bd0); a resting grenade whose ground is gone falls; a flying one is traced (point,
0x280e891) from its origin to the trajectory position; sky (surface flag 0x10) removes it; anything else bounces
(0x526200 → 0x525f30): v' = v − 2(v·n)n, for bouncing grenades with v·n ≤ 0 scaled by
par + (perp − par)·(−v·n/|v|); normal z > 0.7 and |v'| < 20 stops it there; else it moves 0.1 along the normal (z ≤ 0)
and flies on from there. A bounce changing the velocity by more than 100 sends the bounce event (0xbd / 0xbe).

**Explosion** (think 0x526600): smoke grenades (offhandClass 2) still in the air wait 50 ms; one held in an actor's hand
goes off at its `tag_weapon_right` (string 0x1c7f154). Radius damage only when explosionInnerDamage (+0x37c) ≠ 0 (0x5267d3) (0x5289d0): entities with
takedamage in a box of radius·√2, distance to the origin (brush models: to their bounds) < radius, reachable
(0x5285b0, five traces), damage (1 − d/r)·(inner − outer) + outer, direction target − origin + (0, 0, 24), flags 9,
the handler's mod. Actors forget it (0x520380); the explosion event follows projExplosionType (+0x390, enum table 0x5a7e1c grenade 0 / rocket 1 / none 2; 0x526763): 0xbf, 0xc0, else 0xc2 (the client plays no surface sound or impact for 0xc2, only projExplosionEffect / projExplosionSound), and the entity is freed. The moscow potato (weapons/sp/potato: projExplosionType none, explosionInnerDamage 0, no effect or sound, fuseTime 25; moscow.gsc throws it with MagicGrenade and a 20 s fuse) therefore vanishes silently at the fuse.

**Landing prediction** (0x527270, through 0x51f7c0): on a copy, 50 ms steps until the fuse with the same
bounce rules (0x527080); the rest point and its time, or the position at the fuse. Hitting the sky: no prediction (the
current origin).

## Throws

checkGrenadeThrowPos(armOffset, method, target) (0x53ead0) and checkGrenadeThrow(armOffset, method) (0x53e720) both inline
the throw logic of 0x51f490. checkGrenadeThrowPos needs grenadeammo ≥ 1 and stores the target in +0x20e8 first.
checkGrenadeThrow returns nothing without an enemy sentient (sentient +0x1c) or without grenadeammo; with g ≤ 0 or a
grenade already handled it clears the pending throw (+0x20d0 = 0) and returns nothing; otherwise its target is the
enemy's origin plus the enemy's velocity (0x514ed0: an actor's +0x230, a player's ps velocity +0x20, i.e. one second of
lead), stored in +0x20e8 together with +0x20d4 = 1 (re-solve from the hand at throwGrenade) and the method (+0x20cc).

0x51f490 (also called by the throw back and by throwGrenade's re-solve): fails while g (g_gravity, 0x1b15ee8) ≤ 0,
while the actor already handles a grenade (unless the last argument says to go on anyway), or with a sentient of its
team within 1.1 × explosionRadius of the target (0x51f6e0); start = from + forward·x − right·y + up·z (0x51eb00, the
actor's yaw). Then a line trace between start and target (0x526840: no box, mask 0x811, pass entity 0x7ff = none; only
"fraction == 1" is read, so the direction does not matter for the decision) picks the aim point:
- clear, or blocked with an empty hint table, or blocked with no hint picked: the bounce-adjusted target (0x51ebe0:
  (target − start)·(1 − (par + (perp − par)·0.9397)) + start, the surface from a ±1 trace at the target);
- blocked and a hint picked by 0x51ece0 (below): the hint moved by flrand(hint − 15, hint + 15) on x, then y, then z
  (0x5a94b0 = 15.0; flrand 0x4d8480 = one draw of rand() at 0x5a56f4, (seed >> 17)·(hi − lo)/32768 + lo).
A zero aim distance fails. Methods (strings 0x1c7f09a "min energy", 0x1c7f09c "min time", 0x1c7f094 "max time",
0x1c7f096 "infinite energy"; other text: script error 0x58de30): min energy 0x51efb0 (t = √(2|d|/g), fails when
(dz + |d|)·g > 810000), min time / max time 0x51ee30 (flag 0 / 1; fails when |d|² < 1; b = 810000 − dz·g,
t = √(2(b ∓ √(b² − g²|d|²)))/g), infinite energy 0x51ef20; v = d/t + (0, 0, g·t/2). The arc check 0x51e710 traces start → half rise → apex → halfway down with boxes of the sag (g·t²/8 + 1), then to the target
with a 2-unit box (clear, or ending within 4 units on the world or on an enemy sentient). Success keeps start, velocity
and the method (+0x20dc, +0x20f4, +0x20cc) and sets +0x20d0; checkGrenadeThrow(Pos) return the velocity.

throwGrenade (0x53ee10, raw): dead → the pending throw is dropped. With a grenade or a pending throw: re-solved from
`tag_weapon_right` (no arm offset, anyway = 1, the kept target and method, through 0x51f490 including the hint choice)
when +0x20d4 is set, falling back to the kept start and velocity; in the grenade state the held grenade is thrown back
(released 0x520470, new trajectory, the actor becomes its parent), otherwise a new grenade of grenadeweapon with a 5 s
fuse, grenadeammo − 1. MagicGrenadeManual(origin, velocity [, fuse s]) (0x4ee5e0) fires the actor's grenade weapon.
MagicGrenade(origin, target [, fuse s]) (0x4ee3d0) does NOT go through 0x51f490: no gravity, held-grenade, friendly or
line-of-sight test and no hint pick; the bounce-adjusted aim 0x51ebe0 from the origin, then min energy, min time, max
time, then the hint lob 0x51f050 towards that aim; the grenade leaves from the origin (default fuse 5000 ms; failure
prints "MagicGrenade: None of the methods worked …" 0x587480; errors "MagicGrenade only supports actors", "Actor [%s]
doesn't have a grenade weapon set." — the weapon test comes after the solve). pickUpGrenade (0x53f0a0 → 0x5203c0): the
weapon's world model on `tag_weapon_right`, the actor turns to the throw velocity, facing 1, the grenade stops and is in
hand with the actor as parent.

## Grenade hint nodes

**Table.** `info_grenade_hint` (spawn 0x51e6a0) appends the entity origin (+0xdc) to a table of vec3 at 0x1c8f180,
count 0x1c6103c, limit 512 (ERR_DROP "MAX_GRENADE_HINTS (%i) exceeded", string 0x58de94), and then frees the entity
(0x4d7c30), so scripts never see hint entities. The only other writer of the table is the in-place sort of 0x51f320.
Where the count is reset between levels was not located (no direct store of 0 to 0x1c6103c in the executable; it is
presumably part of a cleared level block); the port keeps the table per World (one map load). Maps with hints (entity
string counts): trainyard 230, silotown_assault 160, breakout 144, newvillers 143, hill400_defend 109, cityhall 97,
toujane / toujane_ride 87, duhoc_assault 67, downtown_assault 61, crossroads 54, hill400_assault / downtown_sniper 45,
decoytown 34, bergstein 17, eldaba 16, demolition 7, decoytrenches 6, credits / duhoc_defend 5, beltot / rhine 1.

**Pick (0x51ece0, args start, target, out).** Arc = dvar ai_debugGrenadeHintArc (registered 0x4d9196: float, default
150, range 0..180, flags 0x1080; pointer 0x1c13f58); limit = cos((arc·0.5 + 180)·π/180) (= −cos(arc/2)). For each hint
in table order: skip unless target.z ≤ hint.z − 10; a = 2D unit(hint − target), dist = its length (0x421160 normalises
in place, leaves a zero vector alone); skip when dist > 192; b = 2D unit(hint − start); skip when a·b > limit; keep the
hint with the smallest dist (strictly smaller replaces, so ties keep the earlier entry). Writes the hint, returns true.

**MagicGrenade lob (0x51f050, args actor, start (edi), target (ebx), velocity out).** d = target − start,
D = |d.xy|. For each hint in table order, h = hint − start: skip when h·d < 0 (3D); L = |h.xy| with h.xy normalised; skip
unless L < D and L ≠ 0; the landing point P = start.xy + h.xy·D at z = target.z; skip when |target − P|² (2D) > 36864
(192²); A = L·dz − h.z·D, skip unless A < 0 (the hint is above the straight line); K = −0.5·g/A; horizontal speed
S = √((D − L)·K·L·D), vertical speed V = K·(h.z·D² − L²·dz)/S (the parabola through (L, h.z) and (D, dz) in the vertical
plane of the hint direction); skip when V² + S² > 810000; velocity = (h.x·S, h.y·S, V); the first one whose arc check
0x51e710(actor, start, velocity, P) passes is used (true), else false. (x87 `fsubrp %st,%st(1)` at 0x51f1e5 is the
bytes DE E9 = st(1) − st(0), consistent with the parabola algebra.)

**Throw-back fallback (0x51f320, eax = landing point, args actor, velocity out; called from 0x51fd7c).** The throw back
0x51fb20 tries min energy, min time, max time through 0x51f490 from the landing point with arm offset (0, 0, 80) at the
enemy (or the thrower) with +0x20d4 = 1; when all fail it clears +0x20d4 and calls 0x51f320: with hints, the table is
sorted in place by CRT qsort (0x552f40, 12-byte entries, comparator 0x51f280: squared 3D distance from the landing
point stored in 0x1c8f104, nearest first; the sorted order persists for 0x51ece0 / 0x51f050 later); then for each hint
the min time solver 0x51ee30 (flag 0) from the landing point itself (no arm offset, no bounce adjustment) to the hint;
the first success gives the velocity. On success the throw back keeps start = the landing point (+0x20dc, from the
local copy of grenade +0x154 — also in the normal three-method case, where the arm-offset start of 0x51f490 is not
kept), the velocity (+0x20f4), and only with +0x20d4 set the target (+0x20e8).

0x51f3b0 (enemy-lead variant calling 0x51f490 with +0x20d4 = 1) has no caller or pointer in the executable (dead).

## AI

grenade_ping (0x535d70 case 8/9 → 0x5200e0): actors with grenadeawareness ≠ 0 and no grenade (or one
without a landing) notice it with that probability; a grenade of the enemy team calls 0x4f9f30 / 0x4fa070(1); the
landing is predicted; it is forgotten when the path leads away from it (0x51fe90) or the actor's position is safe
(0x51f8b0: 0x5285b0 from the landing point within 1.1 × explosionRadius fails).

State 3 (start 0x5205b0, finish/suspend 0x520600/0x520650 drop a held grenade, resume 0x5206a0 = grenade and pending
throw, think 0x520c30), substates: 300 flee (0x5207a0: follow the path, facing 4; at the end combat when
safe, else cower), 0x12d take cover (0x520860: at a covering claimed node that is safe: cover attack), 0x12e cower
(0x5206c0: facing 1, `grenade_cower`), 0x12f combat, 0x130 cover attack (the node's cover script), 0x131 acquire
(0x520a50), 0x132 throw back (0x520930: `grenade_return_throw` until its thread ends, then exposed; still holding:
"ai classname '%s' at (%g %g %g) finished the grenade return script without throwing a grenade").
React 0x51f900, acquire 0x51fb20, cover node 0x54c110, flee search 0x545f30: see the comments in `src/game/actorgrenade.cpp`.

Give up (0x51fff0, eax = skipAcquire): with skipAcquire 0 and the grenade within 96 units of the actor (2D d² <= 9216,
0x59239c; the `jp` at 0x520041 skips the acquire when d² > 9216), the acquire 0x51fb20 is tried (success → false); holding the
grenade (+0x150 == the actor) at a safe position (0x51f8b0) → grenade +0x150 = 0, actor +0x20c4 = 0, push exposed (state 1)
(0x520083..0x5200bc) → true; otherwise react 0x51f900(actor, 1) → false. The acquire think's give-ups (moved more than 16 units
2D / 32 in z from the goal, 0x520b1e / 0x520b5b) pass skipAcquire 1; 0x520a90 passes 0 for another actor (the +0x72c branch of
the acquire think, not ported). The port's earlier "> 96 units → acquire again" was inverted and is fixed.

Grenade hits a character (0x520230, called from the flying bounce 0x526198 with eax = g_entities[trace entity], ebx = the
grenade): when the hit entity's contents (+0xc0) & 0x200c000, the grenade's landing (+0x154..+0x15c) is cleared and predicted
again (0x51f7c0); every actor (0x4fe2c0 / 0x4fe310) in the grenade state (+0x8c == 3) whose grenade (+0x20c4) it is runs the
give-up above with skipAcquire 0 (inlined, result ignored). Port: `grenadeHitCharacter` (live bounces only; the landing
prediction 0x527080 has its own bounce loop and does not call it).

trigger_damage on the flight path (0x5297b0; it is not a whizby): G_RunMissile calls it for handler type 3 (grenades; the
handler table 0x5a5750 + 0x2c·handler + 4: grenade handler 14 → 3, rocket handler 0xf → 5) every frame with ecx = the origin
before the move, then (attacker = the grenade entity, end = the new origin, damage = explosionInnerDamage +0x37c, mod 3
MOD_GRENADE) (0x526f90..0x526fbb), before the impact is handled. It lists entities with contents 0x400000 in the segment's
bounds (0x424560, 0x419ea0), keeps classname trigger_damage (0x1c7f0ea) with +0x128 & 0x4000 whose model the zero-box segment
touches (0x44c5b0), notifies "damage" (damage, attacker), runs the action 0x5293c0 and, without an accumulate value (+0x158),
resets the health to 32000: the missile twin of the bullet test 0x5295f0. Port: `triggerDamageSegment` from `runGrenade`.
TODO(spec): the port (like its bullet twin) filters by classname only, not by the +0x128 & 0x4000 gate.

Missile water entry (0x526d6b..0x526df7, all missiles): when |trDelta.z| > 30 (0x591e08) and the origin's point contents
(0x44c840) have no 0x20, the move trace uses clipmask | 0x20. A trace whose surface is water ((surfaceFlags & 0x1f00000) ==
0x1400000) calls 0x526b40 and the move is traced again with the clipmask alone. 0x526b40: dir = normalize(trDelta), z negated
when negative (0x591e48 = −1); temp entity event 0xb8 (0x4d7f40) at the missile's origin before the move: +4 = DirToByte(trace
normal), +3 = DirToByte(dir) (0x420c30), +1 = surface type, +0xa = the missile's number. The client's event switch 0x4ba8d0
(index event − 0x8b, jump table 0x4bb74c) sends 0xb8 to case 0x4bb260 → 0x4b81d0, the small-bullet impact (docs/spec/effects.md
"Bullet impacts"): bullet_small_water and the impact table's bullet_small normal / reflect effects for water. Port:
`missileTrace` (grenades and rockets); test: tests/test_itemdrop.cpp (rhine).

TODO(spec): the qsort tie order of 0x51f320, 0x51fe90's last test, 0x4f9f30 / 0x4fa070, the 1.5-unit grenade ground settle,
the bounce sounds, the targets of 0x5285b0's five traces (Q3 CanDamage pattern assumed: the point and four ±15 offsets), the
danger planes (+0x2050), the end-point z adjustments 0x526e94..0x526f84 and the hit-entity call 0x5268b0 (handler type 3,
hit entity +0x128 & 0x8000), the direction-byte quantisation of event 0xb8.

## Cooked fuse and bounce angular re-base (decoded)
- Spawn 0x5275f0: if the thrower has a client and client +0x3c (ps grenade fuse, ms) is non-zero, the entity fuse +0x138 =
  levelTime + that value and client +0x3c is set to 0 (0x527605..0x52762c); otherwise levelTime + the fuse argument (the weapon
  fuseTime, 0x531990 passes weapon +0x248). Port: `playerThrowGrenade(..., cookedFuseMs)` fed by `PlayerWeapon::takeFuseMs()`.
- 0x525cb0(trace, resting) re-bases the angular trajectory (+0x38..+0x58) at the hit time; port `grenadeRebaseAngles`
  (src/game/grenadeangles.h, tests/test_grenadeangles.cpp). out = angles at the hit time.
  - trace normal z <= 0.1 (wall): only a flying call changes anything: pitch spin +0x50 = AngleMod16(spin + (rand & 0x7f) - 63).
  - else: slope = pitch (0x421990) of the heading (cos yaw, sin yaw, 0), yaw = out[1], projected into the surface plane
    (0x4261a0); d = |AngleSubtract(out pitch, slope)| (0x423f50). The pitch spin is multiplied by rand/32768 * 0.3 + 0.85, and
    by -1 when d < 80. out pitch is normalised to (-180, 180] (0x424040). A flying bounce with d >= 45 keeps it (AngleMod16) or,
    for d < 80, adds d / 4; otherwise (d < 45 or resting) the pitch lies on the slope: slope if |pitch| <= 90, else slope + 180.
  - Flying caller (0x52615e): apos.base = out, apos.time = level time (not the hit time). Rest caller (0x5260d2): 0x4d8380 stops
    the position, 0x4d83c0 stops the angular trajectory (type, time, duration, delta = 0) with base = currentAngles = out.

## Item run (G_RunItem 0x523040, decoded; port `World::runItems`, test `tests/test_itemdrop.cpp`)
Per frame, on the entity (gentity stride 0x1fc, base pointer 0x1c60504; byte +0 eType, +0xc groundEntityNum, +0x14 pos.trType,
+0x18 trTime, +0x1c trDuration, +0x20 trBase, +0x2c trDelta, +0xdc currentOrigin, +0xf4 owner, +0x124 spawnflags, +0x12c clipmask,
+0x1c8 tagInfo):
1. Re-base (0x523049..0x5230b2): skipped when groundEntityNum != 0x7ff and the ground entity's trType is 0. Otherwise, when trType
   != 5 and (eType == 0xb or !(spawnflags & 1)): trType 5, trTime = levelTime, trBase = currentOrigin, trDelta = 0.
2. trType 0 or 6, or +0x1c8 != 0: only the think (0x5233b0).
3. end = BG_EvaluateTrajectory(levelTime + 50) (0x4fae80). 0x420d90 is the SQUARED distance (eax − ecx)²; if
   |end − origin|² < 0.1 (0x59202c) then end.z −= 1 (0x591e20).
4. G_Trace (0x44bf00: esi results, ebx start, edi end, stack mins, maxs, passEnt, mask) of the entity box from currentOrigin to
   end, passEnt = owner (+0xf4), mask = clipmask or 0x491. Trace layout: +0 fraction, +4 normal, +0x1c entity number (word),
   +0x23 startsolid.
5. fraction < 1: endpos = origin + (end − origin)·fraction; if !startsolid, fraction < 0.01 (0x591ea0) and normal z < 0.5
   (0x591e10): end' = end + n·(1 − (end − origin)·n) and a second trace endpos → end' (0x4db7f0 is only the stack-argument
   wrapper of 0x44bf00) overwrites the trace struct; endpos = lerp(endpos, end', fraction2) (0x47f390). Then trType 3, trTime =
   levelTime, duration 50, trBase = old origin, trDelta = (endpos − origin)·20 (0x591e18), currentOrigin = endpos. No hit:
   currentOrigin = end (trajectory unchanged).
6. Link (0x44b2d0), G_RunThink (0x4da090); a freed entity (in-use byte +0xa4) returns.
7. Settle only when the (possibly overwritten) trace fraction < 0.01: normal z <= 0 (0x591e00) → G_FreeEntity (0x4d7c30);
   no-drop contents (0x44c840 at currentOrigin, 0x80000000) → freed; else eType 0xb → 0x4fd9e0(ent, 0) (TODO(spec), not an item),
   other → 0x522ea0 (lay on the surface); G_SetOrigin(endpos) (0x4d8380: trType 0, time / duration / delta 0); groundEntityNum =
   trace entity; link.
Why settling works with the 0.125 trace epsilon: a hit leaves the item 0.125 above the surface with trType 3 and no ground; the next
frame re-bases it (zero velocity) and probes 1 unit down, which enters at once ((d1 − eps)/(d1 − d2) = 0), so the item settles one
frame after its first floor hit (headless: toujane straight drop, first hit frame 9, settled frame 10, z = the 4096-unit floor
trace). A wall hit (fraction >= 0.01) does not settle; the item then drops straight down from the wall with zero velocity.
LaunchItem (0x521e00): eType 2, weapon items (item type 1) box (−1, −1, −1)..(1, 1, 1), other items (−1, −1, 0)..(1, 1, 2);
TR_GRAVITY from the origin at the level time; no owner is set. FinishSpawningItem (0x5225e0) map items rest with trType 0 and
groundEntityNum = the drop trace's entity (0x52283f). Port: a ground entity counts as moving while its mover position channel is
active; passEnt is the item itself (owner unset); only weapon items run.

## Rockets (G_FireRocket 0x527810, G_RunMissile, decoded)
- +0x168 = destabilizeDistance (+0x5ac, int) / projectileSpeed (+0x384, int) · 1000 (0x52795d..0x527973); the shooter's +0x128 &
  0x20000 is copied (0x527979..0x52798d) and disables the destabilization (no writer of that flag found; the port leaves it clear).
  0x4d6110 (end of G_FireRocket and LaunchItem) builds the server dobj from the model index +0x114 and attachments +0x1d4: no
  gameplay beyond the model.
- G_RunMissile without a hit (0x527017..0x527061): |trDelta| != 0 → groundEntityNum = 0x7ff, and for weaponType (+0x78) 2
  (projectile) without +0x128 & 0x20000: 0x526960. Then the think.
- 0x526960 (ebx = missile): when trTime + ftol(+0x168) < levelTime: dir = normalize(trDelta) + tan(destabilizationAngleMax +0x5a8 ·
  π/180) · (c1, c2, c3), c = 2·rand15/32768 − 1 for three consecutive rand() (0x5a56f4); normalised again; trDelta = dir ·
  projectileSpeed, trBase = currentOrigin, trTime = levelTime; currentAngles = vectoangles(dir) (0x421b30), apos stationary
  with that base. +0x168 = destabilizationBaseTime (+0x5a0) · 1000 the first time (sets +0x128 |= 0x10000), afterwards
  +0x168 · destabilizationTimeReductionRatio (+0x5a4). Shipped: panzerfaust 1000 / 2800 → 357 ms, then 100, 80, 64 … ms,
  angle 10°; tank turrets angle 0 (only re-based). Port: `destabilizeRocket`, test in tests/test_itemdrop.cpp.
- projectileDLight (client, CG_Missile 0x4ab3fe..0x4ab43c): when weapon info +0x14c (float of projectileDLight +0x45c, 0x4b3cab)
  is not 0, *0x617b1c(lerp origin, radius, info +0x150, +0x154, +0x158). TODO(spec): the writer of info +0x150..+0x158 was not
  found (projectileRed / Green / Blue are weapon +0x460 / +0x464 / +0x468; panzerfaust / panzerschreck: 200, (0.75, 0.3, 1)).
