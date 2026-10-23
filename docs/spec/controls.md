# Player controls

Sources: CoD2SP_s.exe 1.3 (objdump listing and REA Ghidra decompiles),
`default.cfg` from the game's iw_00.iwd. Implemented in `src/game/clientinput.{h,cpp}`,
`src/game/playerweapon.{h,cpp}` and `src/game/playeractions.cpp`.

## Bindings

The client execs `default.cfg` (strings "default.cfg", "default_localize.cfg"; the Polish `default_localize.cfg` differs
only in the console key). Its binds are the defaults: W/S/A/D `+forward` `+back` `+moveleft` `+moveright`, Q/E
`+leanleft` `+leanright`, SHIFT `+melee_breath`, MOUSE1 `+attack`, MOUSE2 `toggleads`, 1 and 2 `weapnext`, MOUSE3
and G `+frag`, 4 `+smoke`, B `+binoculars`, F `+activate`, R `+reload`, SPACE `+gostand`, CTRL `goprone`, C
`gocrouch`. OpenIW2 reads the file from the game data at start; nothing is hard-coded.

## Client commands (registration 0x409fe4…0x40a3e4, handlers 0x408b10…0x4093e0)

- `+x` / `-x` press and release a key button (IN_KeyDown 0x408b50 / IN_KeyUp 0x408bd0): up to two keys hold it, the
  first press sets *active* and *was pressed*.
- `+melee_breath` presses `+melee` and `+holdbreath`; `+breath_binoculars` presses `+holdbreath` and `+binoculars`.
- Stance (client value 0 stand, 1 crouch, 2 prone): `gocrouch` → 1, `goprone` → 2, `togglecrouch` 1 ↔ 0,
  `toggleprone` 2 ↔ 0, `lowerstance` +1, `raisestance` −1, all ignored while `+prone` or `+movedown` is held.
  `+gostand` presses its own button; standing it also presses jump, otherwise it sets stand. `+moveup` steps up
  one stance (prone → crouch → stand) and jumps only when standing.
- `toggleads` flips a flag, `leaveads` clears it; the flag is cleared at map load (0x40e821).
- `weapnext` / `weapprev` (cgame 0x4b6fd0 / 0x4b7030 → 0x4b7410): select the weapon in the other of the two weapon
  slots (primary, primaryb) when it has one. TODO(spec): the fallback over weapons outside the slots (0x4b72f0),
  `cg_weaponCycleDelay`.

## usercmd buttons (CL_CmdButtons 0x409af0, stance 0x409500)

Held or pressed since the last command: attack 0x1, melee 0x4, activate 0x8, reload 0x10, usereload 0x20, leanleft
0x40, leanright 0x80, jump 0x400, binoculars 0x4000, holdbreath 0x8000, frag 0x10000, smoke 0x20000. In a vehicle
(pm_type 2/3, or pm_flags 0x20000 without 0x80000) the `+gostand` button also counts as jump. Stance: `+prone` held
→ 0x100 | 0x2000, else `+movedown` held → 0x200 | 0x2000, else client stance 1 → 0x200 (crouch), 2 → 0x100
(prone). Aim 0x1000 = `+speed` held XOR the `toggleads` flag, or always while the weapon state is 0x13 / 0x14.

## Weapon states (PM_Weapon 0x4dfc30)

Player-state fields: weapon +0xb0, state +0xb4, weaponTime +0x34, weaponDelay +0x38, grenade fuse +0x3c, offhand
+0xac, view animation +0x5ac (bit 0x200 toggles on every change). State changes run when the delay just ran out or
both timers are 0. View animation numbers paired with their timers: 2 fire, 4 rechamber, 8 melee, 9 drop,
10 raise, 15 altDrop, 16 altRaise, 17 quickDrop, 18 quickRaise, 19 holdFire.

- **Weapon change** (0x4de880 → 0x4ddb40): not in states 12–22; with the timer running only a rechamber or reload
  (states 4–9) may be interrupted, firing (3) and melee (10, 11) or a running delay may not. A command weapon
  different from the player's starts the drop: state 2 with `altDropTime` (view 15) when it is the current
  weapon's `altWeapon`, else `dropTime` (view 9); without a current weapon at once. At the end (state 2) the command
  weapon becomes the player's: state 1 with `altRaiseTime` (view 16) for the alt weapon, else `raiseTime` (view 10);
  state 1 ends in 0. TODO(spec): the aim flag check (ps+0xc 0x8000) that skips the change.
- **Melee** (0x4def20): not in 12–22; needs `meleeDamage`, button 4 newly pressed (latch ps+0xc 0x2000 until
  released), no delay running (or a reload), and aim fraction ≤ 0 or no `adsOverlayReticle` (with a scope the key
  holds the breath instead, 0x4dd350). Not in states 1, 2, 10, 11. View 8, event 0xa3; `meleeDelay` 0 → the blow at
  once, else state 10 with weaponTime `meleeTime` and delay `meleeDelay`. The blow (0x4dee30): weaponTime ≥
  `meleeTime − meleeDelay`, event 0xa4, state 11; state 11 ends in 0.
- **Offhand** (0x4df470): not with pm_flags 0x20300 or in states 12–15 or 17–22. +frag (class 1) / +smoke (class 2)
  held → the first weapon of that `offhandClass` with clip + stock ≠ 0 (0x4dc6d0); none → events 0x94 and 0xc9 /
  0xca. With a weapon (and not in state 16): state 12, `quickDropTime` of the weapon, view 17, then state 13; else
  state 13 at once: `holdFireTime` of the grenade, view 19, flag 0x10 (the view model is the offhand's), event 0xa5.
  13 → 14: grenade fuse = `fuseTime`. 14 waits while the frag/smoke button is held in this and the previous command;
  then 15: `fireTime`, view 2, event 0xa6 (the throw), one round off the grenade's clip (event 0x94 when the last).
  15 → 16: the weapon's `quickRaiseTime`, view 18 (no weapon: delay 1); 16 → 0 (0x4df0a0). Cooking (0x4df5a0): only
  with `cookOffHold` the fuse counts down while flag 0x10 is set; at 0 event 200 (the grenade goes off in the hand:
  0x527d80 with 100000 damage, MOD_SUICIDE).

Firing, rechambering and reloading are still the inferred machine of `docs/spec/weapons.md`.

## Player events in the game (event switch 0x4e769f)

- 0x9e / 0x9f / 0xa0 / 0xaf–0xb1: FireWeapon (0x531c40).
- 0xa4 melee (0x531da0 → 0x530f80 → 0x530de0): from the eye (0x4e8b70: origin + view height, lean offset) along the
  view axes (0x531b90: the vehicle view angles in a vehicle), locational traces (mask 0x280e831, priority map
  0x5a9584, skipping the player) to eye + forward · `player_meleeRange` (64) + right · `player_meleeWidth` (10) · a
  + up · `player_meleeHeight` (10) · b for (a, b) of table 0x58f7c4: (0, 0), (1, 1), (1, −1), (−1, 1), (−1, −1);
  only the centre when width or height ≤ 0. The first trace that ends before its end on a non-sky surface hits; an
  entity that takes damage gets `meleeDamage` + rand() % 5 (Q3 LCG at 0x5a56f4, seed 0x89abcdef, `>> 17`), no
  flags, MOD_MELEE (7), along the forward axis. TODO(spec): the centre trace's impact (0x5295f0), events 0xad / 0xae.
- 0xa6 offhand throw (0x531d60 → 0x531990): velocity v = forward · `projectileSpeed` + (0, 0, `projectileSpeedUp`),
  grenade entity (0x5275f0) from the eye with `fuseTime`; then v is normalised and the trajectory delta gets
  v̂ · (v̂ · player velocity).
- 0xa8–0xac notify the player with script strings; 0xc8 (200) the cooked grenade.

## Use key and turrets (not implemented)

The use scan (0x534d40 → 0x534490) also offers turrets (entity type 8) through 0x51dac0: turret info flag 0x1000,
not in use (+0x112), 0x51d8a0, no grenade fuse and the player on the ground. The use (0x5342b0 / 0x534350 →
0x5341a0) calls the entity handler's use function: turrets have handlers 20 (think 0x51ced0) and 21 (think
0x51d250), use 0x51db50 (no Ghidra procedure: carve it). On a turret, pressing use again sets +0x112 to 2 when
pm_flags & 0x300. TODO(spec): the player-turret use, view limits, firing and getting off.
