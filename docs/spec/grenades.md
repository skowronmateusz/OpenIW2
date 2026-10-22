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
goes off at its `tag_weapon_right` (string 0x1c7f154). Radius damage (0x5289d0): entities with
takedamage in a box of radius·√2, distance to the origin (brush models: to their bounds) < radius, reachable
(0x5285b0, five traces), damage (1 − d/r)·(inner − outer) + outer, direction target − origin + (0, 0, 24), flags 9,
the handler's mod. Then the explosion event and the entity is freed; actors forget it (0x520380).

**Landing prediction** (0x527270, through 0x51f7c0): on a copy, 50 ms steps until the fuse with the same
bounce rules (0x527080); the rest point and its time, or the position at the fuse. Hitting the sky: no prediction (the
current origin).

## Throws

checkGrenadeThrowPos(armOffset, method, target) (0x53ead0, raw): needs grenadeammo ≥ 1; target → +0x20e8; 0x51f490:
fails while g (g_gravity, 0x1b15ee8) ≤ 0, while the actor already handles a grenade, or with a sentient of its team
within 1.1 × explosionRadius of the target (0x51f6e0); start = origin + forward·x − right·y + up·z (0x51eb00); the aim
point moves back from the target by the bounce off its surface (0x51ebe0: (target − start)·(1 − (par + (perp − par)·
0.9397)) + start); methods (strings 0x1c7f094…09c): min energy 0x51efb0 (t = √(2|d|/g), fails when (dz + |d|)·g >
810000), min time / max time 0x51ee30 (b = 810000 − dz·g, t = √(2(b ∓ √(b² − g²|d|²)))/g), infinite energy 0x51ef20;
v = d/t + (0, 0, g·t/2). The arc check 0x51e710 traces start → half rise → apex → halfway down
with boxes of the sag (g·t²/8 + 1), then to the target with a 2-unit box (clear, or ending within 4 units on the world
or on an enemy sentient). Success keeps start, velocity and the method (+0x20dc, +0x20f4, +0x20cc) and sets +0x20d0.

throwGrenade (0x53ee10, raw): dead → the pending throw is dropped. With a grenade or a pending throw: re-solved from
the hand when +0x20d4 is set; in the grenade state the held grenade is thrown back (released 0x520470, new trajectory,
the actor becomes its parent), otherwise a new grenade of grenadeweapon with a 5 s fuse, grenadeammo − 1.
MagicGrenadeManual(origin, velocity [, fuse s]) (0x4ee5e0) and MagicGrenade(origin, target [, fuse s]) (0x4ee3d0: min
energy, min time, max time, then 0x51f050) fire the actor's grenade weapon (default fuse 5000 ms; errors "MagicGrenade
only supports actors", "Actor [%s] doesn't have a grenade weapon set."). pickUpGrenade (0x53f0a0 → 0x5203c0): the
weapon's world model on `tag_weapon_right`, the actor turns to the throw velocity, facing 1, the grenade stops and is in
hand with the actor as parent.

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

TODO(spec): grenade hint nodes (info_grenade_hint, 0x51ece0 / 0x51f320), 0x51f050, 0x51fe90's last test, 0x4f9f30 /
0x4fa070, 0x520230 (re-planning when a grenade hits a character), water (0x526b40), the ground settle and rotation,
whizby (0x5297b0), the bounce sounds, the targets of 0x5285b0's five traces (Q3 CanDamage pattern assumed: the point
and four ±15 offsets), the player's cooked fuse, the danger planes (+0x2050).
