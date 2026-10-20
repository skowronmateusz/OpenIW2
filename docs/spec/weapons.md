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
it is not taken from the executable.

## View model placement

- The client registers `cg_gun_x`, `cg_gun_y`, `cg_gun_z`, `cg_gun_move_*`, `cg_gun_ofs_*` and `cg_gun_rot_*`
  with default 0 and `cg_fov` with default 80 (`FUN_004a10f0`).
- The view weapon is placed at the view origin plus the `cg_gun_x/y/z` offsets along the view axes, after bob
  and kick offsets (`FUN_004b6d40`, `FUN_004b61d0`, `FUN_004b6420`).
- The hands model (`viewmodel_hands_*`) has an all-zero bind pose; the weapon's xanim poses it completely with
  its root `tag_view` at the view origin. The gun model's root `j_gun` follows the hands' `tag_weapon`
  (inferred from the skeletons: no animation key moves `j_gun`).

With these rules the Kar98k, Thompson, M1 Garand, MP40 and similar weapons look as expected. **Open issue:** the
idle animations of the Enfield, Sten, Bren, Springfield and Mosin-Nagant put `tag_weapon` near (8, 11, 1) with a
90° yaw, so these guns point to the left. Something in the original placement or posing for these rigs is not
understood yet.
