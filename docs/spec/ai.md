# AI actors

Sources: `CoD2SP_s.exe` (1.3) analyzed with REA: `FUN_00524570` (per-class script loading),
`FUN_00524270` (engine animscripts), `FUN_005241b0` (script+label lookup).

## Script loading (observed)

- For every map entity whose classname starts with `actor_`, the engine loads `aitype/<classname without
  "actor_">` and looks up three labels in it (the shipped files define `main`, `spawner` and `precache`).
- Entities with an `animscript` key load `animscripts/traverse/<value>`. `misc_mg42`/`misc_turret` load the
  animscript named by their weapon info.
- The engine loads these animscripts: `combat`, `concealment_crouch`, `concealment_prone`, `concealment_stand`,
  `cover_crouch`, `cover_left`, `cover_prone`, `cover_right`, `cover_stand`, `cover_wide_left`, `cover_wide_right`,
  `death`, `grenade_return_throw`, `init`, `pain`, `move`, `scripted`, `stop`, `grenade_cower` (strings at
  0x587e50–0x587f4c). These are the AI state scripts the engine starts as the actor's state changes.

## Spawning (implemented; partly inferred)

- `actor_*` entities are spawners. Those without spawnflag 1 (`SPAWNER`, described in the aitype headers as
  "makes this a spawner instead of a guy") spawn an AI at load.
- `dospawn()` / `stalingradspawn()` on a spawner create an AI. It copies the spawner's key/values, decrements
  `count`, runs `aitype main()` with `self` = the AI, notifies the spawner `"spawned"` with the AI, and starts
  `animscripts/init::main()`. The errors "dospawn can only be called on actor spawners" and "Spawner at (%g %g %g)
  is in solid" exist in the executable.

## Not yet specified

The engine's AI state machine (when `stop`, `move`, `combat`, `cover_*`, `pain` and `death` run), path finding,
and target selection.

## Native actor fields (observed)

Table at 0x5703f0–0x5707ec (read with `read_bytes`), rows of
`{ const char* name; int32 offset; int32 type; setter; getter }` (20 bytes), terminated by a zero row. Offsets are
into the actor struct (0x21a4 bytes, 32 slots), not the entity. Types: 0 int, 1 int (`chainfallback`), 2 byte, 3
float, 4 string, 5 vector, 6 and 8 entity, 0xa path node. Setter 0x536580 means read-only ("actor field %s is
read-only"); 0x536480 clamps to [0, 1] and 0x536520 to ≥ 0 (strings "actor field %s clamped from %g to 1/0");
0x536660/0x5366a0 store seconds × 1000 as int milliseconds and read them back × 0.001.

| field | offset | type | | field | offset | type |
|---|---|---|---|---|---|---|
| accuracy | 0xb8 | float ≥ 0 | | grenadeweapon | 0x20c8 | int |
| accuracystationarymod | 0xbc | float ≥ 0 | | grenadeammo | 0x20d8 | int |
| lookforward / lookright / lookup | 0xe0 / 0xec / 0xf8 | vector, ro | | favoriteenemy | 0x2038 | entity |
| fovcosine | 0x176c | float [0,1] | | allowdeath | 0xb0 | int |
| maxsightdistsqrd | 0x1770 | float | | useable | 0x211b | byte |
| followmin / followmax | 0x714 / 0x718 | int | | dropweapon | 0x211c | int |
| chainfallback | 0x738 | 1 | | drawoncompass | 0x2120 | int |
| interval | 0x71c | float | | scriptstate / lastscriptstate / statechangereason | 0x2140 / 0x2142 / 0x2144 | string, ro |
| damagetaken / damagedir / damageyaw | 0x1e4 / 0x1ec / 0x1e8 | ro | | groundtype | 0x24c | string, ro |
| damagelocation / damageweapon | 0x1f8 / 0x1fa | string, ro | | anim_pose | 0x146 | string |
| proneok | 0x154 | int, ro | | goalradius | 0x770 | float ≥ 4 |
| walkdist | 0x708 | float | | goalheight | 0x774 | float ≥ 80 |
| desiredangle | 0x124 | float, ro | | lastenemysightpos | 0x203c | vector |
| pacifist | 0x790 | int | | pathenemylookahead | 0x780 | float |
| pacifistwait / suppressionwait | 0x794 / 0x20b0 | seconds | | pathenemyfightdist | 0x784 | float |
| name / weapon | 0xd0 / 0xd2 | string | | chainnode | 0x78c | node, ro |
| dontavoidplayer | 0x734 | int | | movemode | 0x214 | string, ro |
| grenadeawareness | 0x20c0 | float [0,1] | | keepclaimednode / keepclaimednodeingoal | 0x740 / 0x741 | byte |
| grenade | 0x20c4 | entity, ro | | badplaceawareness / goodshootpos / goodshootposvalid | 0x212c / 0x2130 / 0x213c | float [0,1] / vector / int |

A second table follows at 0x570800 with `threatbias` (0x8, int), `node` (0x2c, ro), `enemy` (0x1c, ro) and
`ignoreme` (0xc, byte), offsets into a separate struct. The entity table at 0x570868 has `health` (0x13c) and
`maxhealth` (0x140).

There is no native `fightdist` or `maxdist` actor field: the only occurrences of these words in the executable (1.3
and the protected original) are the weapon-file keys `maxDist`/`fightDist` and the built-ins `weaponmaxdist`,
`weaponfightdist`, `pathenemyfightdist`. Scripts that read `ai.fightdist` (e.g. `toujane_ride.gsc::spawners_tweak`,
`spawned.fightDist * 0.5`) therefore read an ordinary script variable that is undefined unless a script set it,
and the original engine raises the same runtime error there.

## Native actor field defaults (observed)

The actor slot is zeroed when allocated (`FUN_004fc8d0`), then `FUN_004fdde0`
stores: `fovcosine` 0.01, `maxsightdistsqrd` 8192² (0x4c800000), `pacifistwait` 20000 ms, `suppressionwait`
5000 ms, `followmin` −5, `followmax` −1, `walkdist` 128, `interval` 96, `grenadeawareness` 0.33,
`badplaceawareness` 0.75, `goodshootposvalid` 0.

The actor spawn function `FUN_004fc640` (called by `G_CallSpawn` 0x4f9770 for `actor_*` after the
spawn key/values are parsed, and by the spawner path 0x532200) then sets entity `health` = `maxhealth` = 100,
bounds (−15, −15, 0)–(15, 15, 72), and on the actor `dropweapon` 1, `drawoncompass` 1, `accuracy` 0.2,
`accuracystationarymod` 1, `keepclaimednode` 0, `keepclaimednodeingoal` 0, the goal position to the entity origin,
and goal radius and height through `FUN_005061f0(0)`/`FUN_00506220(0)`, which
clamp to the minimums: `goalradius` 4, `goalheight` 80 (instructions at 0x4fc80d–0x4fc82a).

No instruction in `.text` stores to actor offsets 0x780/0x784 (byte scan for `mov`/`fstp` with those
displacements), so `pathenemylookahead` and `pathenemyfightdist` start at 0 and change only from script.
`FUN_00505f70` uses it while the actor paths to its enemy (byte at actor+0x788 set): goal radius
= max(`pathenemyfightdist`, 4), goal height = 512 if `pathenemyfightdist` > 80, else 80.

TODO(spec): the initial values of the engine-maintained string fields `movemode` and `anim_pose` (zero string
index after allocation; getter 0x536960 for `movemode`). OpenIW2 uses `"run"` and `"stand"` (inferred);
`animscripts/init.gsc` sets `anim_pose = "stand"` itself.

## Path data (lump 38, observed)

Loaded by `FUN_0052e3c0`: `int32 version = 8; int16 nodeCount` (must equal the number of
`node_*` entities: "Path node count mismatch in bsp file"), then per node `int16 linkCount` and
`linkCount × { uint16 targetNode; float distance }`. Node-to-node visibility follows as `(n − 1)·n` bits,
run-length coded: a byte `b < 0x80` means `b` zero bytes then one literal byte, and `b ≥ 0x80` means `~b` literal
bytes. `toujane`: 2,571 nodes, 21,583 links; the lump is consumed exactly.

OpenIW2 moves actors along an A* path over this graph at the root-motion speed of their current animations.
