# Weapons (`weapons/sp/<name>`) and the first-person view model

## Weapon files

A weapon file is one line of backslash-separated text: `WEAPONFILE\key\value\key\value…`. Keys are matched
case-insensitively (`World::weapon()` lower-cases them). Fields used by the engine so far:

| Field | Use |
|---|---|
| `damage`, `minDamage`, `minDamageRange`, `maxDamageRange` | bullet damage by distance (`World::fireBullet` → `bulletDamageAtDistance`) |
| `fireTime` | seconds between shots |
| `semiAuto`, `boltAction` | `semiAuto 0` fires while the trigger is held; `boltAction 1` rechambers after each shot |
| `rechamberTime`, `reloadTime`, `reloadEmptyTime`, `raiseTime` | state durations in seconds |
| `rechamberBoltTime` | (+0x20c, ms) when a boltAction casing is ejected, see "Bolt-action brass" below |
| `clipSize`, `startAmmo`, `maxAmmo` | ammunition |
| `gunModel`, `handModel` | view-model xmodels (without the `xmodel/` prefix) |
| `idleAnim`, `fireAnim`, `lastShotAnim`, `rechamberAnim`, `reloadAnim`, `reloadEmptyAnim`, `raiseAnim`, … | view-model xanims |
| `standMoveF`, `standMoveR`, `standMoveU` | standing view-model offset in view space (forward, right, up) |

The firing, rechamber and reload state machine in `src/game/playerweapon.cpp` is **inferred** from these fields;
it is not taken from the executable. Weapon changes, melee and offhand grenades follow PM_Weapon (docs/spec/controls.md).

### Damage range defaults (checked)

After a weapon file is parsed the executable normalises the two range fields (0x507630): a `maxDamageRange` that is
missing or `<= 0` becomes 999999.0 (float bits 0x497423f0), a `minDamageRange` missing or `<= 0` becomes 999999.25
(0x497423f2). `bulletDamageAtDistance` (0x531090) then returns the full `damage` for every distance below that, so a
weapon file that names only `damage` deals full damage at any range. The shipped `armoredcar_turret` (the toujane_ride
armored car gun, `damage 350` and no range keys) is such a file; without the default its bullets fell past
`maxDamageRange 0` and dealt `minDamage 0` to everything (tests/test_armoredcar.cpp). The player and AI weapons all
carry explicit range keys, so the default only shows in vehicle turret guns. Implemented in `World::weapon()`, which
fills the two keys after parsing.

## View model placement

- The client registers `cg_gun_x`, `cg_gun_y`, `cg_gun_z`, `cg_gun_move_*`, `cg_gun_ofs_*` and `cg_gun_rot_*`
  with default 0 and `cg_fov` with default 80 (`FUN_004a10f0`).
- The view weapon is placed at the view origin plus the `cg_gun_x/y/z` offsets along the view axes, after bob
  and kick offsets (`FUN_004b6d40`, `FUN_004b61d0`, `FUN_004b6420`).
- The hands model (`viewmodel_hands_*`) has an all-zero bind pose; the weapon's xanim poses it completely with
  its root `tag_view` at the view origin. The gun model's root `j_gun` follows the hands' `tag_weapon`
  (inferred from the skeletons: no animation key moves `j_gun`).

## View weapon offsets (CoD2SP_s.exe 1.3)

Implemented in `src/game/viewweapon.cpp` and `src/game/playerweapon.cpp`.

- `cg_gun_x/y/z`, `cg_gun_move_*`, `cg_gun_ofs_*`, `cg_gun_rot_*` all default to 0 (0x4a10f0) and are omitted.
- Position (0x4b5380): stance by the player's stance flags (prone / crouch / stand). Minimum speed =
  max(0, `{stand,ducked,prone}MoveMinSpeed`). When the horizontal speed exceeds it (and g_speed does, and the
  weapon is not in state 5), frac = min(1, (speed − min) / (g_speed − min)) and the target is frac ·
  `{stance}Move{F,R,U}`. At view height 40 the target adds `duckedOfs{F,R,U}`, at 11 `proneOfs{F,R,U}`. Each
  component approaches its target by (target − current) · rate · ms / 1000 with rate `posMoveRate`
  (`posProneMoveRate` at view height 11), at least 0.0001 · ms, without overshooting. The result is added fully
  when not aiming, × (1 − 2 · adsFrac) below half aim, and not at all above.
- Rotation (0x4b5730): the same with `{stance}Rot{P,Y,R}`, `{stance}RotMinSpeed`, `posRotRate` /
  `posProneRotRate`; the target is also × (1 − adsFrac).
- The offsets are forward, right, up in view space (0x4b4c50 negates the right component into the left axis).
- Aim fraction (0x4dd0d0): weapons without `aimDownSight` stay at 0; it rises by
  ms / adsTransInTime while aiming is allowed and falls by ms / adsTransOutTime otherwise, clamped to 0..1.
  Aiming is not allowed while reloading, nor while rechambering unless `rechamberWhileAds`.
  `player_adsExitDelay` (0x1c7efb8, ms): at aim 1.0 with aiming no longer allowed, ps+0xbc is set to time + delay and the aim is
  held until it expires, then the timer is cleared (implemented). `adsFire` (+0x348, with `aimDownSight`): the fire start 0x4dea00 sets weaponDelay (ps+0x38) to
  trunc(adsTransInTime ms * (1 - aim)) instead of `fireDelay`, and 0x4ded30 fires (event, ammo) only once the delay is 0; the
  aim function 0x4dd0d0 forces the aim to stay requested while the delay runs in the fire state (implemented: `deferredFire_`,
  panzerfaust / panzerschreck). Aim rates (0x5075b6): +0x588 = 1 / adsTransInTime ms (default 1/300 when not positive), +0x58c =
  1 / adsTransOutTime ms (default 1/500). Aim allowed (0x4dd0d0): `aimDownSight` 0 never; segmented reload (+0x35c) blocks
  states 5..8, state 9 only while weaponTime > adsReloadTransTime; otherwise state 5 under the same condition; state 4
  blocks when `rechamberWhileAds` is 0; else the pm_flags 0x40 request. (implemented)
- Field of view (0x4ae7d9): cg_fov (80, horizontal for 4:3) moves towards min(`adsZoomFov`, cg_fov):
  fully at aim 1; otherwise t = (adsFrac − (1 − frac)) / frac with frac = `adsZoomInFrac` while zooming in
  (flag set at aim 0, cleared at aim 1, 0x4b5ad0) or `adsZoomOutFrac`; when t > 0, fov −= (fov − zoom) · t.

### Aim on a turret (VAs in the patch 1.3 SP executable)

- Getting on a turret (0x51db50) does not touch the aim fraction (ps+0xb8), the aim flag or the weapon state; it
  only sets the turret bits in eFlags (0x100 / 0x200 / 0x300 by turret stance) and the use state.
- While those bits are set PM_Weapon returns before its weapon step (0x4dfc59: eFlags & 0x300), so the aim
  fraction stays where it was for the whole time on the turret.
- The cgame hides the scope instead: the crosshair/overlay function (0x4bd760) tests eFlags & 0x300 first
  (0x4bd76b) and on a turret draws only the turret reticle (0x4bcef0) and returns, so the overlay path
  (0x4bcdd0 → 0x4bca90) is never reached.
- The view fov (0x4ae7b0) is computed from cg_fov and the aim as usual, then replaced by the constant 55 when
  eFlags & 0x300 is set (0x4ae8a8..0x4ae8b4).
- After getting off the kept aim fraction applies again: if the aim is still held the zoom and the overlay are back
  at once; otherwise the aim runs down at the normal rate. (implemented: `playerViewFov`, `scopeOverlayDrawAlpha`)

### Aim when the weapon goes down (VAs in the patch 1.3 SP executable)

- A vehicle gun (the flak88 is a `script_vehicle`) is not a turret: no turret eFlags, so the fov is not replaced by 55
  (0x4ae8a8). The fov (0x4ae7b0) uses the player state's weapon and the aim fraction ps+0xb8 as on foot.
- In a vehicle the hand weapon is lowered to weapon 0 (0x4de880). The aim flag pm_flags 0x40 (0x4dd020) is cleared each
  step and set again only when 0x4dcf70 allows it: a linked player (pm_type 1) gets there with the local +0x30 zeroed by
  the linked branch (0x50c8a2), so the weapon tests decide: aimDownSight (+0x32c) and a weapon state other than 1, 2
  (raising, dropping), 10–16. So during the drop the aim runs out at the adsTransOutTime rate.
- With weapon 0 (no aimDownSight) the aim update (0x4dd13b) goes to 0x4dd12b: aim fraction and exit timer are 0.
  After getting out the weapon is raised first and the aim comes back only once it is ready.
  (implemented: `PlayerWeapon::update`)
- Other users of the overlay test 0x4bc8a0 (view flash 0x4b6131, damage indicators 0x4bdcb0 / 0x4be210) do not test
  the turret bits and still see the kept aim. TODO(spec): [stays: needs a run of the original] not checked in a window whether that is visible.
- Grenade danger icons (VAs in the patch 1.3 SP executable): the loop 0x4be210 calls the overlay test first (0x4be227) and returns when it is true and the bool dvar `cg_hudGrenadeIconInScope` (registered at 0x4a1622, flags 0x1001, default 0) is off. So by default the icons are hidden under a scope overlay and shown under plain aim; with the dvar on they are drawn there too.

Not done yet: bob (0x4b4a90), sway (0x4b4dc0), idle motion (0x4b51a0), gun kick (0x4b5d10), lean. While aiming,
OpenIW2 poses the view model with `adsUpAnim` at the aim fraction (inferred; the original's view-model anim
tree is not specified).

## View-model animation tree (0x4b2c90, 0x4b2550, 0x4b25e0)

The engine builds a flat "VIEWMODEL" tree with the weapon's 22 view animations in field order (idleAnim,
emptyIdleAnim, fireAnim, holdFireAnim, lastShotAnim, rechamberAnim, meleeAnim, reloadAnim, reloadEmptyAnim,
reloadStartAnim, reloadEndAnim, raiseAnim, dropAnim, altRaiseAnim, altDropAnim, quickRaiseAnim, quickDropAnim,
adsFireAnim, adsLastShotAnim, adsRechamberAnim, adsUpAnim, adsDownAnim; slots 1–22). The models are the hands
(`handModel`) and the gun (`gunModel`) attached at `tag_weapon`.

- Playing an action animation (slots 1–20) gives it weight 1 and the other action slots 0.
- The aim animations are never switched off: while zooming in `adsUpAnim` has weight 1 and its time is set to the
  aim fraction; otherwise `adsDownAnim` has weight 1 at time 1 − aim fraction (ADS anims must not loop).
- The pose is the blend of the action animation and the aim animation. OpenIW2 averages them with equal weights
  per bone (the XAnim blend itself is not implemented yet). With this the hip pose of every weapon (also the
  Enfield, Sten, Bren, Springfield and Mosin, whose idle and adsDown poses are turned ±90° in opposite directions)
  and the aimed sight picture match screenshots of the original.

Melee start pose (data property, not an engine fault): the view model switches to the melee animation at weight 1 with
no blend-in, so its first frame is shown at once. For the Kar98k, Mosin-Nagant and M1 Garand the first melee frame is
within about 2 degrees (mean per bone) of the idle pose; the SVT-40 melee animation (17 frames at 30 fps) is authored
starting at a different pose (mean 28.8 degrees, tag_weapon about 168 degrees, wrists 100-150 degrees) and ends on the
idle pose, so the weapon appears swung out on the first frame. Nothing in the engine changes this; covered by the
`melee_start` test.

## Hit-location damage multiplier (G_Damage 0x528100..0x528143)

For a sentient target the damage is `trunc(amount × table[location])` (`_ftol`); a result <= 0 leaves G_Damage before any
state change. The table is the attacker's weapon table (weapon def +0x5b0 + 4·location: the weapon-file keys `locNone`,
`locHelmet`, `locHead`, `locNeck`, `locTorsoUpper`, `locTorsoLower`, `locRightArmUpper`, `locLeftArmUpper`,
`locRightArmLower`, `locLeftArmLower`, `locRightHand`, `locLeftHand`, `locRightLegUpper`, `locLeftLegUpper`,
`locRightLegLower`, `locLeftLegLower`, `locRightFoot`, `locLeftFoot`, `locGun`; field table entry at file 0x1a8eac) when
the entity's weapon (inflictor byte +2, else the attacker's) is defined and its `weaponType` (+0x78) is 0 (`bullet`).
Otherwise the default table 0x1c91328 is used: 19 floats initialised to 1.0 (0x527a08) and loaded from
`info/ai_lochit_dmgtable` (`LOCDMGTABLE\name\value\...`, 0x527b73..0x527ba1; e.g. head 4, torso_upper 0.9, gun 0).
Location order: none, helmet, head, neck, torso_upper, torso_lower, right_arm_upper, left_arm_upper, right_arm_lower,
left_arm_lower, right_hand, left_hand, right_leg_upper, left_leg_upper, right_leg_lower, left_leg_lower, right_foot,
left_foot, gun (name table 0x5a9538). OpenIW2 uses the attacker's `$weapon` since damage() has no inflictor argument.
Deliberate deviation (single player only; multiplayer: docs/spec/mp-pmove.md): a weapon with `rifleBullet` and `weaponClass` rifle uses its head multiplier on the helmet location too, because the shipped `locHelmet` of 0.5 lets a helmet stop a full-power rifle round. Shipped scripts still pick the helmet or head death.
## Bolt-action brass (exe evidence)

Weapon field table entries (name 0x58bb8c / 0x58bb78 / 0x58b8c8, offset, type): `rechamberTime` +0x208, `rechamberBoltTime` +0x20c (type 7, ms), `boltAction` +0x328.
- EV_FIRE_WEAPON (cgame 0x4b7800) ejects the brass (CG_EjectWeaponBrass 0x4b7730) only when boltAction is 0 (0x4b79c0..0x4b79dc).
- A shot of a boltAction weapon leaves its ps+0x530 bit set (the casing still in the chamber). Entering the rechamber (0x4dd851):
  weaponTime = rechamberTime, weaponDelay = rechamberBoltTime when non-zero and below rechamberTime, else 1 ms (0x4dd864..0x4dd87b).
  When that delay expires in state 4 (0x4de720 returns 1) with the bit set, 0x4dd700 clears it and adds event 0xa2 (0x4dd799).
- A reload started with the bit set (the last round, no rechamber) takes weaponDelay = min(rechamberBoltTime, the add delay or
  weaponTime), at least 1 ms (0x4dd930..0x4dd970); at its end 0x4ddfa0 clears the bit, adds event 0xa2 (0x4de00a) and re-arms
  the delay for the ammo add minus rechamberBoltTime (0x4de0b6).
- Event 0xa2 (jump table 0x4bb74c, case 0x4bb113) calls 0x4b7730: shellEjectEffect bolted on tag_brass.
- Stock values: kar98k rechamberTime 1, rechamberBoltTime 0.65; enfield 1.0677 / 0.65 (fireTime 0.33): the casing comes out
  0.65 s into the bolt cycle. Only the pmove writes event 0xa2, so AI bolt-action rifles never eject brass (combat.cpp).
- OpenIW2: `PlayerWeapon::Event::EjectBrass` with its own timer (`brassTimer_`); TODO(spec): [stays: not read] the exe's ps weaponDelay also
  gates melee / other actions during that time, and the bit is kept per weapon across switches (implemented, brassBits_).

## Reload variant and projectile fire (panzerschreck, exe evidence)

- Reload round (0x4dd980): the empty variant (anim 0xc, `reloadEmptyTime` +0x21c, event 0x97) is chosen only when the clip is
  0 AND `weaponType` (+0x78) is 0 (bullet) (0x4dd997..0x4dd9d8); otherwise anim 0xb, `reloadTime` +0x218, event 0x96
  (0x4dd9e5..0x4dda14). The add delay (0x4dd8c0) uses the same pair. So an empty panzerschreck (projectile, no
  `reloadEmptyAnim`) plays `reloadAnim` for `reloadTime` 4 s, the round added at `reloadAddTime` 2.3 s. (OpenIW2 had picked
  the empty anim for every weapon type, i.e. the idle pose for the panzerschreck.)
- FireWeapon (game 0x531c40) dispatches on `weaponType`: 0 bullet 0x5312b0, 1 grenade 0x531990, 2 projectile 0x531a40, which
  turns the view forward by the spread (0x531170), launches G_FireRocket (0x527810) from the muzzle point of 0x531b90 (eye
  0x4e8b70 plus client +0x864c..0x8654) and subtracts forward · 64 (0x56f6e0) from the client velocity (0x531b1b).
  Spread value (0x531c40): scale = client +0x8670, the aim spread (ps+0x5b0) / 255 stored by ClientThink (0x4e7c7d..0x4e7c9f) before Pmove (0x4e7d06), so a shot's own `hipSpreadFireAdd` (0x4dece0, inside Pmove) only affects later commands; aim == 1.0: (view-height hip max - adsSpread +0x4bc) * scale +
  adsSpread, else (max - min) * scale + min, the range from 0x4dceb0 (`fireSpreadDegrees`). The same value drives the bullet and
  the projectile. 0x531170 draws two rand() (angle = r1 * 0.010986328125 deg, radius = r2 / 32768) as (cos * radius, sin *
  radius); the projectile direction is normalize(forward + tan(spread) * (x * right + y * up)) (AngleVectors 0x421da0: its second vector is the right vector, as the negation at 0x4b5fa1 shows), the bullet
  end is origin + forward * 8192 + tan(spread) * 8192 * (x * right + y * up) (0x531210). OpenIW2 `World::playerFire` implements the spread and the offset
  parameter and returns the kick (forward * 64) that the caller subtracts from the velocity.
- Shot direction and muzzle (0x531b90, implemented from this text as `computeGunAim` and `GunAimDelay`, src/game/viewweapon.h). With eFlags 0x20000 (`0x531bbc`) the shot keeps the view angles of the player state (ps+0xc4), so a vehicle shot has no view kick. Otherwise pitch and yaw come from the gun angles at client +0x8644/+0x8648, the roll from the player state, and the muzzle is the eye (0x4e8b70) plus the gun offset at client +0x864c..+0x8654. These are usercmd fields (+0x1c..+0x2c; the cgame stores 0xf79914..0xf79924 at 0x4bfc3f, the command builder copies them at 0x409d45) and ClientThink copies them to the client at 0x4e7e83, after the client events (0x4e7da8). A shot therefore uses the gun pose of the command before its own, and that command was built from the cgame frame drawn before it. Its own view kick (the fire event, 0x4b787d -> 0x4dfea0) cannot be in it: the event follows the command. OpenIW2 models this with one sample of delay per simulated frame (one frame per command): a shot uses the pose drawn two frames back. It is one sample in `GunAimDelay` because the sample is taken at the shot, after the previous frame was drawn. The order inside a real frame (when the server reads a new command relative to the draw) is not checked, and at most one more frame of lag could depend on it.
- The cgame computes that pose in the view weapon pass 0x4b6d40. The pass stops, leaving the previous values, with pm_type 2 or 3, with the developer test gun flag (0xf79864), with eFlags 0x300, and with eFlags 0x20000 outside weapon states 0x11..0x16. With no weapon it stores the view angles and a zero offset (0x4b6ede).
  - Gun angles, final step 0x4b60e0: a weapon without `aimDownSight` (+0x32c), aim fraction 0, or the scope overlay test 0x4bc8a0 being true all give the final view angles 0xf79190/94, read after the view offsets of 0x4ae290 (0x4af604) and the screen shake, so they include the residual view kick (0xf798c8, 0x4ae32c), the view bob and the damage kick. Hip fire is this case. Otherwise the angles are the gun's world angles from 0x4b6420, each reduced to 16 bits (truncate angle * 65536/360, mask 0xffff, back to degrees, range 0..360).
  - Gun angles relative to the view (0x4b6420): lean roll -2g, the rotation of 0x4b5730 plus aim fraction * `adsAimPitch` (+0x46c) in the pitch (0x4b5ad0), idle sway (0x4b51a0), gun bob (0x4b4d10), minus the view kick for a weapon without `aimDownSight`, the damage kick below, gun kick (0x4b5d10), then the sway lag subtracted by AngleDelta (0xf791a8/ac). Those angles become an axis, taken in the frame of the view angles and turned back to angles (0x4b5f80, using the view angles copied at 0x4af1bc).
  - Damage kick on the gun (0x4b64dc..0x4b660f), elapsed time t since the damage time 0xf79764, nothing while that is 0: f = (aim + 1) / 2, rise 100 * f ms, fall 400 * f ms, amplitude a = f, times (1 - 0.75 * aim) for an aimed weapon with an overlay reticle. During the rise k = a * x * (2 - |x|) with x = t / rise. Afterwards x = (t - rise) / fall, no kick once x reaches 1, else k = a * (1 - x * (2 - |x|)). The pitch gains 0.5 * k * 0xf7979c, the yaw loses k * 0xf797a0, the roll gains 0.5 * k * 0xf797a0.
  - Gun position (0x4b61d0): a vector forward / left / up of the lean offset plus the move offset (0xf79050), rotated by the raw gun bob angles 0xf79940 (0x4b4c50), then the sway side lag is subtracted from its left part and the sway up lag added to its up part. The view axes carry it to the world (0x4b5ea0, using the origin and angles copied by 0x4af140). The weapon landing dip adds a quarter of the eye's landing dip to the world z (0x4b6397..0x4b63d1, the dip's timing and size are those of 0xf790a0/a4).
  - Gun offset (0x4b6030): that vector from the view origin, times the aim fraction, for an `aimDownSight` weapon at aim above 0; zero otherwise. It is not zero at full aim while the gun sways or bobs, and it is not switched off by the scope overlay.
  Note that the view model drawn by OpenIW2 uses a simplified version of this pose; the shot does not.
- CG_Missile (0x4ab3af): `projTrailEffect` (+0x458, weapon info +0x170) is played once per missile (cent +0x143) bolted on
  its `tag_origin` (scr const 0x1c7f144); `projectileDLight` (+0x45c, info +0x14c) with `projectileRed/Green/Blue` adds a
  dynamic light (0x4ab404 → *0x617b1c). OpenIW2 queues the trail at the launch (rockets.cpp). The dlight colour fields (info +0x150..+0x158) have no writer in the cgame code range (0x4a0000..0x4d0000; CG_RegisterWeapon 0x4b2c90 zeroes the info and stores only the radius), so the light is black and nothing is drawn.
- A missile impact (0x526200, the part before the damage) with a parent that is not a grenade handler sends the AI line event 0x11 from the parent's origin to the impact point (0x535860, flags 0, default radius² 512²): enemy-team actors near the line learn the parent and mark it as their attacker (handler 0x535f20, the same as type 0x10 without the danger plane). Implemented in `World::rocketImpact`. The explosion event number is 0xc0 + flag for rockets (weapon +0x390 == 1), 0xc2 + flag for other custom types, 0xbf for grenades; the flag is 1 when the trigger_damage segment test 0x5295f0 hit or the trace's hit id (+0x1e) is not 0. The client handler 0x4ba8d0 sets 0xf79778 for 0xc1 / 0xc3 around the surface sound, impact effect and explosion effect, and CG_ImpactMark (0x4ad430) returns at once while it is set, so those effects leave no marks. See "Explosions without marks" below.
- Fire start (0x4dea00): a shot begun from a state other than firing (ps+0xb4 != 3) stores ps+0x40 = (aim fraction < 1 ? hipGunKickReducedKickBullets : adsGunKickReducedKickBullets) * fireTime + fireDelay (ms); PM_Weapon lowers it by the frame time down to 0 (0x4de72a). The cgame fire impulse (0x4dfea0) uses the ReducedKickPercent while ps+0x40 > 0. Implemented in `PlayerWeapon::reducedKick`.
- The view flash (`viewFlashEffect` panzerschreck_flash.efx) already played on the view model's tag_flash; in first person
  that tag lies outside the default field of view, so the visible launch effect is mostly the rocket and its trail.

## Fire sound for every weapon type and the view flash under a scope (exe evidence)

- EV_FIRE_WEAPON (cgame 0x4b7800) plays the weapon's fire sound for every weapon type; it is not part of the bullet path. The alias is
  the weapon info's fireSound (0xf48e60 + weapon * 0x1b8); the player's fireSoundPlayer (0xf48e64) replaces it only when it is set;
  on the last round of the clip lastShotSoundPlayer (0xf48e6c) or lastShotSound (0xf48e68) replaces it when set (0x4b7800, before
  0x4a35c0). Shipped panzerschreck and panzerfaust define only fireSound (the AI alias of the panzerfaust fire sample), so the player
  hears that alias, and the player's reload of the panzerschreck uses its own reloadSound. OpenIW2 had played the sound only from the
  bullet trace, so projectile weapons were silent; `World::playWeaponFireSound` now serves the bullet and the projectile branch of
  `World::playerFire`, with the fireSound fallback for the player.
- The same event sets the per-entity flash flag (cent +0x142, 0x4b7800). The view weapon pass 0x4b69b0 (only caller 0x4b6d40) clears
  the flag every frame it runs (0x4b6c13) but plays viewFlashEffect (0x4b6660) only when its third argument is non-zero. 0x4b6d40 passes
  zero while the scope overlay (0x4bc8a0) reports itself active, yet still poses the gun. So a shot fired under a scope overlay
  produces no flash at all, now or later. OpenIW2 skipped the whole pass under the overlay, which left the flag set and played the
  flash on leaving the scope (`consumeViewFlash`). Brass (event time, 0x4b7730) is not gated by the overlay; the gun is posed so its
  tag exists. TODO(spec): [stays: not read] the exact overlay-active condition of 0x4bc8a0 is approximated by the overlay alpha > 0.01.

## Bullet passes (0x531340)
One fired bullet can run several passes; each repeats the trace, the AI events, the trigger damage and the impact event, at
most 12 deep. Damage is the distance damage of the pass (measured from that pass's start) times the pass's factor, truncated.
- A hit on contents 0x10 goes on just behind the surface with no damage.
- An actor's bullet does not stop at the entity the actor is linked to (a vehicle it rides): it goes on from the hit point, ignoring that entity.
- An AI's bullet ignores sentients of its own team.
- A hit on the player counts only within 8 units of the player's vertical axis, measured across the shot in the horizontal
  plane. Farther out the bullet goes on from the hit point, ignoring the player. OpenIW2 applies this in single player only;
  TODO(spec): whether the MP server's Bullet_Fire_Extended (docs/spec/mp-pmove.md "Bullets") has the same test is not read.
- After a rifleBullet hit on an entity that still has a sentient, including an actor that just died, the bullet goes on from
  the hit point, ignoring that entity, with half the factor. In multiplayer the same pass-through through a hit client stops
  when scr_friendlyfire is 0 and the client is on the attacker's team (docs/spec/mp-pmove.md "Bullets").

### G_Damage order (0x528050)

Each step can end the call. Takes-damage byte (ent+0x111); the vehicle means-of-death filter (0x5122f0, ent+0x108); for a client
0x512390: riding (pm_flags 0x20000, not 0x80000, a vehicle at ent+0xf4) a vehicle whose type word is not 4 "artillery" means no damage.
For an actor only (ent+0x100; a client has none, so hit locations never scale damage to the player) the hit-location multiplier, leaving
on a result <= 0. A client in noclip (+0x8628) or ufo (+0x862c) leaves, so it gets no knockback. Knockback, then the god flag for any
entity. An amount below 1 then becomes 1 (0x5282c9): only the player and entities without an actor can still hold one; the zero test at
0x5282d3 cannot be true. For an actor 0x4ff360: when the amount reaches its health, allowdeath (+0xb0) is 0 and the state stack holds
animscripted (7) or negotiation (9) (0x4ff310), amount - health + 1 is held back, +0xb4 is set, "damage_armor" (0x1c7f05a) is notified
with the held part, and an amount of 0 left over ends the call (0x5283cc). The actor think (0x4ff215) kills such an actor when neither
state is on the stack any more: health + 1, no attacker, hit location 2, means of death 0.

Not modelled: the client window 0x527f00 (+0x86b5, keeps health at 1 for some means of death) and the skip of the "death" notify for a
client (0x528470); the port notifies it.

### Damage knockback (0x528187..0x5282ac)

G_Damage (0x528050) pushes a target with a client, after the hit-location multiplier and before the god check, so a god player is pushed too.
Without a direction the flag 8 is added, and flag 8 skips the push. The scale is 0.05, or 0.01 / 0.025 with pm_flags 1 (prone) / 2 (ducked); knockback is
amount × scale truncated, capped at 60, and 0 when the entity flags +0x128 have 0x20 (set when a client dies, 0x528458). Nothing happens with knockback 0 or eFlags & 0x300 (on a turret).
Velocity += normalized dir × knockback × `g_knockback` (0x1c13f60, default 1000) × 0.004. A target that is its own attacker with means of death 3..6
then has the velocity z (the whole component) times 0.25. With pm_time 0: pm_time = 2 × knockback clamped to 50..200 and pm_flags |= 0x400 (the same flag ClientSpawn sets).
OpenIW2: `World::damage` queues a `KnockbackHit`, the host applies it to its player state (`applyKnockback`, tests/test_knockback.cpp).

The damage flags: 2 goes to the vehicle filter 0x5122f0, 8 is the no-push flag above. Flag 0x20, passed by trigger_hurt with spawnflags 8 (0x529248), has no reader: G_Damage tests no
other bit and passes the flags to neither the notifies nor the pain / die callbacks.

### Explosions without marks

The missile impact (0x526494..0x526504) sends EV_ROCKET_EXPLODE_NOMARKS (0xc1) instead of 0xc0, and EV_CUSTOM_EXPLODE_NOMARKS (0xc3) instead of 0xc2, when the trace's partName (+0x1e) is set, which is any hit on a
bone box or on an xmodel collision surface (actors, vehicles, script models), or when 0x531b50 holds (hit and parent both have clients, differ, and the hit is alive; impossible with one client). A grenade type always sends 0xbf.
The fuse explosion (0x526779..0x526796) has no such variant. The client sets 0xf79778 only around the effect plays of those events (0x4bb45d, 0x4bb54c to 0x4bb5d8), so a Decal element spawned inside the play draws no mark;
an element with a delay of 1 ms or more is queued (0x4853f0) and spawns later with the flag clear. OpenIW2: `TraceResult::partHit`, `FxEvent::noMarks`, `FxSystem::noImmediateMarks`.

## Multiplayer view-animation rates (cgame weapon registration 0x4d3fbc..0x4d4322, MP client)

VAs in this section are in the patch 1.3 MP executable. When the MP client registers a weapon it stores one playback rate per
view-animation slot (the slot order of the "VIEWMODEL" tree above). All slots start at 1.0; then, for each slot below whose
weapon time (an integer in ms) is positive, rate = trunc(frames / frameRate · 1000) / time, i.e. the animation is stretched or
compressed to last exactly as long as the weapon state. The slot is played with this rate (0x4d3680 passes it to the goal-weight
call 0x48d780).

| slot | time field |
|---|---|
| holdFireAnim | holdFireTime |
| meleeAnim | meleeTime |
| reloadAnim | reloadTime |
| reloadEmptyAnim | reloadEmptyTime |
| reloadStartAnim | reloadStartTime |
| reloadEndAnim | **reloadTime** (0x4d4152 reads +0x218, not reloadEndTime +0x22c) |
| raiseAnim, dropAnim, altRaiseAnim, altDropAnim | raiseTime, dropTime, altRaiseTime, altDropTime |
| quickRaiseAnim, quickDropAnim | quickRaiseTime, quickDropTime |

idle, fire, lastShot, rechamber and the ADS slots keep 1.0. For most weapons the data matches the times (rate ≈ 1); the Trench
Gun does not: reload_start is 1.83 s for reloadStartTime 0.8 s, reload_loop 1.3 s for reloadTime 0.6 s, reload_end 0.9 s,
played over reloadTime 0.6 s. Without the rates the shotgun's view model was cut off in the middle of each reload phase.

Segmented reload of the Trench Gun (the shared state machine above): reload start 0.8 s (adds reloadStartAdd 1 shell at
min(reloadStartAddTime, reloadStartTime) = 0.8 s), then one loop round of 0.6 s per shell (added 1 ms before the round ends,
as reloadAddTime 0.8 is longer than the round), then reload end 0.8 s; a new attack press ends the loop after the running
round. Sounds: reloadStartSoundPlayer / reloadSoundPlayer (the load sound) per phase and the pump (reloadEndSoundPlayer) at the
end; after a shot the rechamber (pump) of 0.7333 s with rechamberSoundPlayer.

OpenIW2: `PlayerWeapon::mpAnimRates` (set for an MP world) and `animRate()`; iw2view samples the view animation at
animTime · animRate. TODO(spec): the exe gives rate 0 to a slot whose time is not positive (here 1), and whether the parts'
frame count is the file's frame count or one less (here the length (frames − 1) / frameRate).

## Multiplayer gun bob and movement offsets (patch 1.3 MP executable)

VAs in this section are in the patch 1.3 MP executable. The MP client has no `cg_bobWeapon*` dvars; its gun bob 0x4f60e0
uses the same numbers as the SP cgame bob (0x4b4a90, `game::gunBob`) as constants: lag 0.25 (the cycle phase
bobCycle / 255 · 2π + 2π + 0.25π + 2π), amplitude 0.16 per unit of speed, cap 10, the roll at the phase − 0.4712389 kept
only when negative, aiming scaled by 1 − (1 − adsBobFactor) · adsFrac (weapon +0x284) and, for a weapon with an
adsOverlayReticle (+0x278), by 1 − adsFrac. The stance amplitudes are the bg dvars registered by 0x5134e0 with the SP
defaults (bg_bobAmplitudeStanding 0.007, Ducked 0.0075, Prone 0.03, bg_bobMax 8); the stance is the view height target
(ps+0xf4: 11 prone, 40 crouched). The cycle is ps+8 and the speed the horizontal velocity, as in SP, so the port uses
the SP functions for both games.

The MP weapon files differ in the movement data the view weapon offsets read (`ViewWeaponMotion`): for example the
kar98k and thompson have standMoveMinSpeed / standRotMinSpeed 80 and duckedMoveMinSpeed 20 (0 in SP), standMoveU −1
(−2 in SP), standMoveR 0.6-1 (0 in SP) and posMoveRate 4 (6-8 in SP). So the gun moves less and only above 80 units/s
when standing; this is the weapon data, not a different code path. The network client's bob cycle comes from the
snapshot (docs/spec/net.md section 10).

## Multiplayer hands model (patch 1.3 MP executable)

VAs in this section are in the patch 1.3 MP executable. The script builtin `setviewmodel` (0x52ae80; the table entry
next to its name) looks the model up in the model index table (0x50e310) and stores the index in the client's session
data (+0x273c); `getviewmodel` (around 0x52afdc) returns the model configstring of that index. The client end-of-frame
path (0x500f80, at 0x501084) copies it into the player state's `viewModel` (ps+0xe4, an 8-bit netfield), so the client
gets the hands of the character the gametype gave the player. The MP character scripts set it per nationality and
map (`character/mp_*.gsc`, through `maps/mp/_utility.gsc:176` after `spawn`): cloth for the Americans and the
Afrika Korps, `viewmodel_hands_german` / `_german_winter`, British, British bare and Russian hands elsewhere, while
every MP weapon file names `viewmodel_hands_cloth` as handModel.

OpenIW2: an MP game draws the local player's hands from the model its script set (`game::viewHandsModel`, the
`$viewmodel` the builtin stores), the weapon's handModel when none is set; single player keeps the weapon's handModel.
TODO(spec): the exact MP cgame site that picks the hands model for the view weapon (the server side above is
verified), the SP `setviewmodel` (0x538bb0 in the SP executable, `_loadout.gsc`), and the network client, which has no
model configstrings yet and keeps the weapon's handModel.

