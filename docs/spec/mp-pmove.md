# Multiplayer movement, falling and bullets: differences from SP

The movement and weapon code of the multiplayer game is the same code as in SP (docs/spec/pmove.md, weapons.md); this
document lists only what differs and what was checked to be the same. Addresses are VAs in the 1.3 Linux dedicated server
unless marked "MP exe" (VAs in the patch 1.3 MP executable) or "SP" (VAs in the patch 1.3 SP executable).

## Dvar defaults

Registration calls read for name, default, min and max. Movement dvars at 0x80dc60c (jump), 0x80de900..0x80df0c0 (bg_*,
player_*), 0x8108680 (g_speed, g_gravity), 0x80dcf80 (mantle).

| dvar | SP | MP | MP registration |
|---|---|---|---|
| bg_fallDamageMinHeight | 200 | **256** (1..max float) | 0x80dea7d (MP exe 0x51368e) |
| bg_fallDamageMaxHeight | 350 | **480** | 0x80deab9 (MP exe 0x5136b1) |
| jump_slowdownEnable | false | **true** | 0x80dc695 (MP exe 0x52fb62) |
| inertiaAngle | −1000 (min = max = −1000) | **0** (−1..1) | 0x80deb5a |
| g_speed, g_gravity | 190, 800 | 190, 800 | 0x81086a1, 0x81086d5 |
| jump_height, jump_stepSize, jump_ladderPushVel, jump_spreadAdd | 39, 18, 128, 64 | same | 0x80dc648.. |
| friction, stopspeed, inertiaMax | 5.5, 100, 50 | same | 0x80deb96, 0x80debd2, 0x80deaf5 |
| player_backSpeedScale, player_strafeSpeedScale | 0.7, 0.8 | same | 0x80df08d, 0x80df051 |
| bg_bob*, bg_swingSpeed, bg_aimSpreadMoveSpeedThreshold, player_breath_*, bg_foliagesnd_* | | same | |
| mantle_enable, mantle_check_range/radius/angle, mantle_view_yawcap | 1, 20, 0.1, 60, 60 | same | 0x80dcfb3.. |
| player_view_pitch_up/down, bg_prone_yawcap, bg_ladder_yawcap | 85, 85, 85, 100 | same | 0x80de8a6.. |

There is no sprint and no sprint dvar. The pmove constant pool (0x8154f6c..0x81550fc) holds the same stance and aim factors
as SP (0.4, 3, 6, 0.15, 0.65, the lean and view-height values). So stances, lean, prone, mantle and ladders behave as in
SP; the differences below come only from the dvars.

## Falling (CrashLand 0x80e30c4)

Same formula as SP (0x508fe0): the fall height from the speed at the start of the step, damage 0..100 between the two
heights (above the min, 100 from the max on), none on surface flag 1 or for a dead player (pm_type above 5 in MP, which
has six living / spectator types), view dip, pm_time 500 + 35 · damage with the 0.5 → 0.2 speed factor, ×0.67 for a lethal
or slick landing, the same event numbers (0x74 + surface for a damaging landing). Differences:
- the heights are the MP defaults 256 / 480: a 300-unit drop gives 19 % in MP, 66 % in SP;
- a min height not below the max height is an error ("bg_fallDamageMaxHeight must be greater than
  bg_fallDamageMinHeight") and gives no damage;
- every landing starts the player animation script's land event (0x80d96ee with event 5, see docs/spec/mp-players.md);
  not part of this document.

The cgame rebuilds the dip height from the damage with the same dvars, so the MP view dip uses 256 / 480 too.

Fall damage (ClientEvents 0x80f52da): for each new player-state event of a player entity (entity type 1) in 0x74..0x8a,
the factor is 1.1 for a parameter above 99, else parameter · 0.01, stored as a float; zero gives nothing. The damage is
the client's max health times the factor, stored as a float and truncated, applied as G_Damage without inflictor or
attacker, means of death MOD_FALLING. In MP that goes to `CodeCallback_PlayerDamage` (docs/spec/mp.md), so the gametype
applies it. Every client gets it: the local player, bots and network clients.

## Jump slowdown (jump_slowdownEnable)

The SP code has the same paths, inert with the SP default false. With it on:
- Landing timer (0x80dc818, called by the walk move while pm_flags 0x80000 is set): pm_time above 1800 drops the flag and
  the jump origin (0x80dc718) and scales the velocity by 0.65; pm_time 0 starts the timer at 1200 with ×0.5 when the
  player landed at least 18 units above the jump's origin, else at 1800 with ×0.65; otherwise ×1. Without the dvar the
  factor is 1.
- Slowdown factor (0x80dc8ca / 0x80dc9ee): 1 without the dvar; 2.5 for pm_time above 1699; else 1 + pm_time · 1.5 / 1700.
- Friction (0x80e156e): in the walking branch (not on slick ground, not pm_flags 0x400) control = max(speed, stopspeed)
  is multiplied by 0.3 with pm_flags 0x200, else, with pm_flags 0x80000, by the slowdown factor (0x80dc91c: a timer above
  1800 drops the flag instead and gives 1).
- Jump start (0x80dca40): 2 · gravity · jump_height is divided by the slowdown factor when pm_flags 0x80000 is still set
  and pm_time is at most 1800, before the flag and pm_time are reset. A jump right after a landing reaches 39 / 2.5 units.

## Inertia (0x80e17fe, from Accelerate 0x80e188e)

After accelspeed = min(missing speed, max(wishspeed, stopspeed) · frametime · accel), the MP default inertiaAngle 0 makes
the inertia check live: not for pm_type 2; not when accelspeed is at most inertiaMax; not when the squared length of the
smoothed horizontal velocity (ps+0x2c, 2 floats) is below 0.0001; otherwise the new horizontal velocity v + wishdir ·
accelspeed and the smoothed one are normalised (2D) and when their dot product is below inertiaAngle accelspeed becomes
inertiaMax (50). inertiaDebug only prints. At the end of PmoveSingle (0x80e93e8..0x80e944e) the smoothed velocity moves
towards the horizontal velocity: old += (v − old) · min(frametime, 1). Turning the run around (the new velocity against the
smoothed one) therefore accelerates at most 50 units/s per step.

## Weapons

The MP game reads `weapons/mp/*`; the field set of the MP and SP weapon files is identical, and damage, minDamage and the
damage ranges, the hit-location multipliers (`loc*`), hip / ADS spread, recoil, fire and reload times, fuse times and
explosion radius / damage all come from these files through the same code. Notable data: all eight MP grenades have
`cookOffHold` 0 (no cooking: the fuse starts at the throw), smoke grenades are `projExplosionType` none with a
`projExplosionEffect` and fuseTime 1.

### Radius damage (0x81022a0)

The MP path differs from the single-player one: it does not ask a yes/no reach question but scales the damage by how much of
the target the explosion sees. The candidates come from an area query of a box of half size radius·√2 around the origin. An
entity is skipped when it is the ignore entity, cannot take damage, is at or beyond the radius (to the origin; a brush model
to its bounds), or is a client while the script's `setplayerignoreradiusdamage` is in effect. The amount is
outer + (inner − outer)·(1 − distance / radius). The visibility (0x8101ee6) makes five traces from the explosion (mask
0x802891, the target as pass entity). For a client the points are the middle between its origin and eye, and that middle
offset ±15 units sideways (perpendicular to the horizontal line to the explosion) and ± half of the eye height over the
middle. For any other entity they are the middle of its bounds and that middle offset (±15, ±15) horizontally. The factor is
0 with no clear point, 1 with four or five, else clear / 3. With a factor above 0 the entity takes the truncated product.
With none, a trace to the bounds centre (mask 0x811, no pass entity) that is blocked, and a centre closer than 0.2·radius,
give a truncated tenth of the amount; so an explosion just behind thin cover still hurts a little. Every hit has damage flags
1 (radius), the explosion origin as its point, hit location none and the direction target − explosion + (0, 0, 24).
OpenIW2: a client's bounds centre is taken as the eye / origin middle; brush models measure the skip test to their box
and take the box middle as the centre (`World::brushAbsBounds`, as in the single-player code; test
`mpradiusbrush_world`).

Friendly fire is gametype script: `scr_friendlyfire` modes 0..3 are read by the gametypes (`tdm.gsc:278-309`); the engine
reads the dvar only for bullets (below) and the server info.

## Bullets (Bullet_Fire_Extended 0x811fe90)

- More than 12 recursions abort the shot (0x811fea1, as SP).
- rifleBullet (weapon +0x31c) gives MOD_RIFLE_BULLET and damage flags **0x20** (SP 0x40); armorPiercing (+0x320) adds 2.
  The gametypes name 0x20 `iDFLAGS_PASSTHRU` (`_callbacksetup.gsc:87`).
- The bullet trace mask is 0x2802831.
- The trace's location priority table is chosen by rifleBullet as in SP (0x811fef9; the two tables at 0x8187c0c / 0x8187c1f hold
  the same values as the SP ones), so a rifle bullet prefers the head over a limb in the same way.
- There is no player-axis test: after the trace, a hit on an entity that takes damage and is not the attacker goes straight to
  the damage routine (0x812026a..0x812033e). The SP rule "only within 8 units of the player's axis" (0x53186f) is SP only.
- Damage = the distance damage (0x811fb7c, distance from this segment's start to the hit) times the shot's float
  multiplier, truncated.
- A hit on contents 0x10 continues from just behind the hit point (0.25 / cosθ for cosθ ≥ 0.125, else 0) without damage, as
  SP (0x8120153..0x8120260).
- Pass-through (0x812034f..0x81203c6): after a hit client (entity with a client, taking damage, not the attacker) was
  damaged by a rifle bullet (flag 0x20), the shot goes on from the hit point to the same end, ignoring that client, with the
  multiplier × 0.5 — unless `scr_friendlyfire` is 0 and the two are on the same team (0x811c0d0: both clients, sessionteam
  equal and not 0 = "none"). In SP the same-team pass-through only forwards the multiplier (always 1). So a rifle round
  hurts a second player behind the first with half the damage; pistol and SMG rounds stop.

### Location scaling of a client's damage

The entry for a client (0x8101b40) scales the amount before the script callback (0x8101bc9..0x8101c02): amount = truncated
(amount x multiplier), and an amount of 0 or less ends the hit without a callback. The multiplier (0x8101ae2) takes the weapon
number of the inflictor (entity +0xc8), else of the attacker. With a weapon whose weaponType (+0x78) is 0 (bullet) it reads the
weapon's own table (+0x5b0, indexed by the location); with no weapon or another type, the table of `info/mp_lochit_dmgtable`
(0x8628ee0). No rule replaces the helmet value: the helmet-piercing rule of docs/spec/weapons.md is a deliberate single-player
deviation and does not apply to multiplayer.

## Spread weapons (the Trench Gun; 0x8120484, 0x81203d4)

The weapon fire dispatcher (0x8120484) sends a bullet weapon whose weaponClass (+0x7c) is 3 ("spread", only `shotgun_mp`
ships one) to 0x81203d4 instead of the single bullet:

- It fires shotCount (+0x1dc; the shipped file: 8) pellets in a loop. Each pellet draws its own end point with the same
  function as the plain bullet (0x811fac8: a random angle over 360 degrees and a random radius, scaled by tan(spread angle)),
  with the shot's spread angle, but at the distance minDamageRange (+0x59c; 800) instead of 8192 (0x81204d7 passes 8192.0 for
  the plain bullet). So pellets cannot hit anything farther away than minDamageRange.
- Every pellet goes through Bullet_Fire_Extended (0x811fe90) with the multiplier 1.0, so each one deals the full distance
  damage: damage (+0x1ec; 50) under maxDamageRange (+0x598; 384), minDamage (+0x590; 5) from minDamageRange, the linear blend
  in between (0x811fb7c).
- The spread angle is the normal one (hip spread 6 degrees, adsSpread 5.5 for the shipped file).
- The fire event and sound belong to the shot (the player state's weapon event), not to the pellets: one sound per shot.

OpenIW2: one fire path, `World::fireWeaponShot` (src/game/combat.cpp), serves the local player and network clients (through
`clientFire`) and the bots (src/game/mpbots.cpp, which give it their aim direction and weapon). It fires the pellets for any
weapon with weaponClass "spread" (no SP weapon has it, so SP is unchanged); only the first pellet plays the fire sound. A
bot's pellets spread by the weapon's `hipSpreadMin` (`adsSpread` when aimed); bots are our own design, docs/spec/mp-bots.md.
TODO(spec): the pellet impact effects of the cgame are those of a single bullet.

TODO(spec): the sessionteam string of value 0 is taken as "none" from the order of the setter (0x80f7db6) and the error
text listing allies, axis, none and spectator.

## Players against each other

VAs in the 1.3 Linux dedicated server unless noted.

- A client's entity has the player contents 0x2000000 (set at 0x80f56eb); the move's trace mask holds that bit
  (PlayerClipMask 0x281c011), and the move skips only the moving client's own entity (pass entity = its number). So
  every other living client, bot or human, is solid to the move.
- ClientThink (from 0x80f5d2e) runs one command: the move, the box of the stance it ended in copied into the entity
  (three vector copies 0x80f64e8..0x80f6526), the client events (0x80f653b), then the entity is linked (0x80f6546). The link happens per command, not
  once per server frame: when two clients' commands are run in the same frame, the second one's move already traces
  against the first one at its new place. The single-player ClientThink does the same (link at 0x4e7dae, VAs in the
  patch 1.3 SP executable).
- The box: 15 to each side, from the origin up to 70 standing, 50 crouched, 30 prone. The player entity state sends
  it packed in `solid` (x/y half size, down, up + 32 in three bytes, docs/spec/net.md section 11).
- Client prediction: the other players of the snapshot are solid to the prediction with the box from their `solid`.
  `TODO(spec)`: which origin the original's solid list uses (the newest snapshot or the drawn, interpolated one);
  openiw2 uses the drawn one.

openiw2 (`World::mpLinkClient`, `World::playerClipHeight`): the server moves a network client and moves its clip with
the new origin and stance box at once; the listen host's own move (`setPlayerOrigin`) does the same. Entity clips are
otherwise rebuilt once per world frame, which let two clients walking into each other in the same frame each trace
against the other's old place; they then overlapped and walked through. A network client's view world places the
other players' clips each time it places them from the snapshots, with the sent box.

## OpenIW2

`multiplayerMoveParams()` (src/game/pmove.cpp) sets the MP dvar defaults; the local player of an MP map (iw2view), bots
(src/game/mpbots.cpp) and network movement (src/netgame/netmove.cpp) use it. `MoveParams` fields `jumpSlowdown`,
`fallDamageMinHeight/MaxHeight`, `inertiaMax/inertiaAngle`; `PlayerState::oldVelocity`; `jumpSlowdownScale`;
`World::clientFallEvent` (fall damage of any client, bots included); `World::fireBullet` pass-through and
`bulletDamageFlags`. SP keeps its defaults, so SP movement is unchanged. Tests: tests/test_mppmove.cpp. Not done: the dvars
are fixed defaults, not read from cvars (as in SP); the dedicated server does not yet hand network clients' landing events to
`clientFallEvent`; the port's friction tests the ground flag, not the exe's walking / not slick / not pm_flags 0x400 branch.
