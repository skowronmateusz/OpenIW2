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
  Aiming is not allowed while reloading, nor while rechambering unless `rechamberWhileAds`. TODO(spec):
  `player_adsExitDelay`, `adsFire`, the 0x35c field.
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
