# Weapons (`weapons/sp/<name>`) and the first-person view model

## Weapon files

A weapon file is one line of backslash-separated text: `WEAPONFILE\key\value\key\value…`. Keys are matched
case-insensitively (`World::weapon()` lower-cases them). Fields used by the engine so far:

| Field | Use |
|---|---|
| `damage`, `minDamage`, `minDamageRange`, `maxDamageRange` | bullet damage (only `damage` is applied yet) |
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
- OpenIW2: `PlayerWeapon::Event::EjectBrass` with its own timer (`brassTimer_`); TODO(spec): the exe's ps weaponDelay also
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
  Spread value (0x531c40): scale = aimSpread (ps+0x5b0) / 255; aim == 1.0: (view-height hip max - adsSpread +0x4bc) * scale +
  adsSpread, else (max - min) * scale + min, the range from 0x4dceb0 (`fireSpreadDegrees`). The same value drives the bullet and
  the projectile. 0x531170 draws two rand() (angle = r1 * 0.010986328125 deg, radius = r2 / 32768) as (cos * radius, sin *
  radius); the projectile direction is normalize(forward + tan(spread) * (x * left + y * up)) (AnglesToAxis 0x421da0), the bullet
  end is origin + forward * 8192 + tan(spread) * 8192 * (x * left + y * up) (0x531210). Muzzle (0x531b90): the eye plus the
  usercmd gun offset (client +0x864c..), zero at aim 0 and 1. OpenIW2 `World::playerFire` implements the spread, the offset
  parameter and returns the kick (forward * 64) that the caller subtracts from the velocity. TODO(spec): the gun offset and the
  gun angles between hip and full aim depend on the unported view-model sway, bob and kick (0x4b4a90, 0x4b4dc0, 0x4b51a0, 0x4b5d10).
- CG_Missile (0x4ab3af): `projTrailEffect` (+0x458, weapon info +0x170) is played once per missile (cent +0x143) bolted on
  its `tag_origin` (scr const 0x1c7f144); `projectileDLight` (+0x45c, info +0x14c) with `projectileRed/Green/Blue` adds a
  dynamic light (0x4ab404 → *0x617b1c). OpenIW2 queues the trail at the launch (rockets.cpp). TODO(spec): the dlight.
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
  tag exists. TODO(spec): the exact overlay-active condition of 0x4bc8a0 is approximated by the overlay alpha > 0.01.
