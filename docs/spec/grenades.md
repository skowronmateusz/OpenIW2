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

TODO(spec): the qsort tie order of 0x51f320, 0x51fe90's last test, 0x4f9f30 /
0x4fa070, 0x520230 (re-planning when a grenade hits a character), water (0x526b40), the ground settle,
whizby (0x5297b0), the bounce sounds, the targets of 0x5285b0's five traces (Q3 CanDamage pattern assumed: the point
and four ±15 offsets), the danger planes (+0x2050).

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

## Item slide (G_RunItem 0x523040, 0x5231b2..0x52327f, decoded)
- Each frame an item evaluates its position 50 ms ahead (0x4fae80, now + 0x32) and traces the entity box from the current origin
  (owner as passEnt, mask 0x491 when clipmask is 0). If that move is shorter than 0.1 (0x420d90, 0x59202c) the end z drops by 1.
- No hit: origin = end. Hit: endpos = cur + (end - cur) * fraction. If the trace did not start solid, fraction < 0.01 and normal z
  < 0.5: end' = end + n * (1 - (end - cur) . n); a second trace from endpos to end' (0x4db7f0 -> 0x44bf00) overwrites the result and
  endpos = endpos + (end' - endpos) * fraction2 (0x47f390). Port: `itemSlideEnd` (src/game/itemslide.h, tests/test_grenadeangles.cpp).
- Then the position trajectory becomes type 3 (stop), duration 50 ms, base = old origin, delta = (endpos - origin) * 20; the
  currentOrigin is set to endpos at once. Settling (0x523315..0x5233a9) needs fraction < 0.01 and normal z > 0, else the item is freed;
  no-drop contents 0x80000000 at the origin free it too. Open (multi-function): the entity flag byte +0xa4 gating it, and the exe's
  per-frame gravity re-base (0x523086), which the port replaces with "settle on the first hit".

## Open findings (item settle, water), not ported
- Entity byte +0xa4 is the in-use flag (savegame and getent loops), so the "gate" in G_RunItem is always true for a live item.
- Item settle: the exe's frame loop settles only when the trace has fraction < 0.01 and normal z > 0, but the collision spec's trace
  leaves an epsilon of 0.125 before a surface, so with the 1-unit gravity step a fraction < 0.01 looks unreachable. The reading of the
  fraction operand (0x24(esp) after the slide lerp) or of 0x420d90 (<0.1 -> end z -= 1) is therefore unresolved; no headless item-drop
  harness exists, so runItems keeps "settle on the first hit".
- Missile water entry: G_RunMissile (0x526c6c..0x526dd4) traces twice (0x526840); when the second trace's surface type
  ((surfaceFlags & 0x1f00000) == 0x1400000, water) it calls 0x526b40, which creates a temp entity event 0xb8 (0x4d7f40) holding the
  quantised direction (bytes at +4/+3 from 0x420c30), the surface type (+1) and the entity number (+0xa). Client handling of event 0xb8
  is not decoded.
