# Player weapon inventory

Source: CoD2SP_s.exe 1.3, disassembled with llvm-objdump where Ghidra had no function (built-in addresses from
`docs/spec/builtins.md`). Weapon-definition offsets come from the weapon field table at 0x5a7e80 (12-byte rows
`{name, offset, type}`, 366 rows): `weaponType` 0x78, `weaponClass` 0x7c, `weaponSlot` 0x80, `offhandClass`
0x84, `slotStackable` 0x88, `startAmmo` 0x1c0, `ammoName` 0x1c4, `clipName` 0x1cc, `maxAmmo` 0x1d4,
`clipSize` 0x1d8, `clipOnly` 0x340, `altWeapon` 0x368 (resolved index at 0x36c).

Enum tables (pointer arrays at 0x5a7e00): weaponType `bullet grenade projectile binoculars`; weaponSlot
`none primary primaryb`; weaponClass `rifle mg smg spread pistol grenade rocketlauncher turret non-player item`;
stance `stand duck prone`; offhandClass `None, Frag Grenade, Smoke Grenade`.

## Player state (offsets in the player state)

- 0x518: bitset of owned weapons; 0x530: a second per-weapon bitset cleared on give (meaning not known yet).
- 0x529 / 0x52a: weapon in the `primary` / `primaryb` slot (byte, 0 = none).
- 0xac: current offhand; 0xb0: cleared by `takeallweapons`.
- 0x118 + 4·ammoIndex: stock rounds; 0x318 + 4·clipIndex: loaded rounds. Ammo and clip indices come from the
  weapon's `ammoName` / `clipName`, so weapons with the same name share ammunition (indexes into the ammo and clip
  definition tables at 0x1c64750 / 0x1c64950).

## Functions

- **give (0x531e30)**: nothing if already owned or the class is `turret` / `non-player`. Sets the owned bit.
  `item`: done. Offhand class ≠ None: becomes the current offhand when none is selected (replacement rule via
  0x4dd580 / 0x4dc6d0 not specified yet). Slot `primary`/`primaryb`: stored in the first empty of 0x529, 0x52a.
  Then the `altWeapon` chain is marked owned.
- **giveweapon (0x5376d0)**: errors "Can not give player weapon without having an empty weapon slot" when the
  weapon is not an item, has no offhand class, goes in a primary slot, and both slots are full. Gives the
  weapon, then adds `startAmmo − stock` rounds (when positive) through *add ammo*, filling the clip if the weapon
  was not owned before.
- **add ammo (0x520e40)**: stock += amount; a `clipOnly` weapon is given as well; the clip is filled (0x520de0:
  move min(clipSize − clip, stock) from stock to clip) when asked or when the weapon was just given; stock is
  clamped to the ammo's `maxAmmo` and the clip to `clipSize`.
- **takeweapon (0x537850)**: zeroes its stock and clip, then removes it (0x4dc8a0, slot details not specified).
- **takeallweapons (0x537960)**: clears 0xb0 and, for every weapon, stock, clip and ownership.
- **givemaxammo (0x538020)**: add ammo `maxAmmo − stock`.
- **getfractionstartammo (0x538130)**: 1 if `startAmmo` < 1, 0 if stock < 1, else stock / startAmmo.
- **getfractionmaxammo (0x538270)**: as above with clip / `clipSize` for `clipOnly` weapons, otherwise
  stock / `maxAmmo`.
- **getweaponslotweapon (0x539df0)** / **setweaponslotweapon (0x539f40)**: slot names `primary`, `primaryb`
  (error "Unknown weaponslot name %s. Valid weaponslots are "primary" and "primaryb""); setting checks the
  weapon's slot ("Weapon %s goes in the %s weaponslot, not the %s weaponslot.", primary and primaryb are
  interchangeable), removes the old weapon, gives the new one, and moves it to primaryb when it was placed in
  primary by the give while primaryb was requested.

Not implemented from evidence yet (marked TODO in `src/game/inventory.cpp`): switching animations/timing for
`switchtoweapon` / `switchtooffhand`, the slot-ammo setters' exact clamping, `getammocount`.
