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

Rows of `{ const char* name; int32 offset; int32 type; … }` (20 bytes), around 0x570774.
Types seen: 0 int, 2 byte/bool, 3 float, 4 string, 5 vector, 6 and 8 entity, 0xa path node.
Names include `fovcosine`, `maxsightdistsqrd`, `followmin`, `followmax`, `chainfallback`, `interval`,
`damagetaken`, `damagedir`, `damageyaw`, `damagelocation`, `damageweapon`, `proneok`, `walkdist`,
`desiredangle`, `pacifist`, `pacifistwait`, `suppressionwait`, `name`, `weapon`, `dontavoidplayer`,
`grenadeawareness`, `grenade`, `grenadeweapon`, `grenadeammo`, `favoriteenemy`, `allowdeath`, `useable`,
`dropweapon`, `drawoncompass`, `scriptstate`, `lastscriptstate`, `statechangereason`, `groundtype`, `anim_pose`,
`goalradius`, `goalheight`, `lastenemysightpos`, `pathenemylookahead`, `pathenemyfightdist`, `chainnode`,
`movemode`, `keepclaimednode`, `keepclaimednodeingoal`, `badplaceawareness`, `goodshootpos`, `goodshootposvalid`,
`threatbias`, `node`, `enemy`, `ignoreme`, `accuracystationarymod`. Their default values are not specified yet;
OpenIW2 uses `movemode = "run"`, `anim_pose = "stand"` and `goalradius = 64` (inferred).

## Path data (lump 38, observed)

Loaded by `FUN_0052e3c0`: `int32 version = 8; int16 nodeCount` (must equal the number of
`node_*` entities: "Path node count mismatch in bsp file"), then per node `int16 linkCount` and
`linkCount × { uint16 targetNode; float distance }`. Node-to-node visibility follows as `(n − 1)·n` bits,
run-length coded: a byte `b < 0x80` means `b` zero bytes then one literal byte, and `b ≥ 0x80` means `~b` literal
bytes. `toujane`: 2,571 nodes, 21,583 links; the lump is consumed exactly.

OpenIW2 moves actors along an A* path over this graph at the root-motion speed of their current animations.
