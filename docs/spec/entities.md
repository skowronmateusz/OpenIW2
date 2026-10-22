# Map entities

Source: the class spawn table in `CoD2SP_s.exe` (1.3) at 0x56f598, read with REA. Each row is
`{ const char* classname; void (*spawn)(entity) }`. Classes that share a spawn function are listed together.

| Classname | Spawn function |
|---|---|
| `info_player_start` | 0x4e84a0 |
| `info_null`, `func_group` | 0x52f580 |
| `info_notnull` | 0x52f590 |
| `info_notnull_big` | 0x52f5f0 |
| `info_grenade_hint` | 0x51e6a0 |
| `info_volume` | 0x52f660 |
| `trigger_use` | 0x52cfd0 |
| `trigger_use_touch` | 0x52cfe0 |
| `trigger_multiple` | 0x528e90 |
| `trigger_radius` | 0x528ee0 |
| `trigger_disk` | 0x529070 |
| `trigger_friendlychain` | 0x529170 |
| `trigger_hurt` | 0x5292a0 |
| `trigger_once` | 0x529300 |
| `trigger_damage` | 0x529540 |
| `trigger_lookat` | 0x529930 |
| `light` | 0x52f650 |
| `misc_model`, `misc_prefab`, `script_struct` | 0x4d7c30 |
| `misc_mg42`, `misc_turret` | 0x51e650 |
| `corona` | 0x52f7e0 |
| `sound_blend` | 0x52f6c0 |
| `script_brushmodel` | 0x533440 |
| `script_model` | 0x533480 |
| `script_origin` | 0x5334d0 |
| `script_vehicle` | 0x512fa0 |
| `script_vehicle_collmap` | 0x512fd0 |

Classes not in this table include path nodes (`node_*`, handled by the path-node loader, string table at 0x5a946c),
`actor_*` spawners, `worldspawn` and vehicle nodes. Their handling is still to be specified.
`misc_model`, `misc_prefab` and `script_struct` share one function. For `misc_model` this matches the map compiler
baking static models into the level; that the entity is then dropped is *inferred*.

## Map keys as script fields

- Native entity fields (table at 0x56f4a8: classname, origin, model, spawnflags, target, targetname, count,
  health, dmg, angles, script_linkname, script_noteworthy, maxhealth, anglelerprate) exist on every entity; the
  integer ones read 0 when the map gives no key. Path nodes have their own table (0x5a93b8: targetname, target,
  animscript, script_linkname, script_noteworthy, origin, angles, radius, spawnflags, type) in zero-filled memory.
- Other keys are typed by `radiant/*.txt` ("float|int|string|vector name" lines, parsed at 0x4686c0, looked up
  by 0x4682d0): int → atoi, float → atof, vector → sscanf "%f %f %f", string → text (entity 0x4f8ff0, node
  0x515bf0). The original sets no field for a key that is neither native nor listed; here such keys stay, typed by
  their text, because engine code reads spawn keys from the same object (TODO).
- `weapon_<name>` with a known weapon spawns an item entity (0x4f96d0 → 0x522c70); item details are TODO.

