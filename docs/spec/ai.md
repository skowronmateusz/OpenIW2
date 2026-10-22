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
  `count`, runs `aitype main()` with `self` = the AI, starts `animscripts/init::main()` (handle 0x1c7e0f8, 0x532558)
  and the bottom AI state, and then notifies the spawner `"spawned"` with the AI (0x5325ad).
- An actor's `damageweapon` is the attacker's weapon name, set by the pain and die callbacks (0x4ff3b0 / 0x4ff530)
  from the attacker entity's weapon index. G_Damage (0x528050) uses the world entity when there is no attacker, and
  weapon index 0 is `none` (0x4dc210). A turret's bullets credit the turret's user, or the world (0x51ada0). The errors "dospawn can only be called on actor spawners" and "Spawner at (%g %g %g)
  is in solid" exist in the executable.

## Perception (observed)

Sentients (player and actors; at most 33, 0x44 bytes from 0x1c60518, enumerated by team mask 0x515230 / 0x515270) have
`threatbias` (+8), `ignoreme` (+0xc), the current enemy (+0x1c) and an attacker count (+0x24). Each actor keeps per
sentient: visible (+0), last check time (+4), last seen (+8), last attacked (+0xc), last known time (+0x10), ignore-until
(+0x14), last known position (+0x18), and its own position then (+0x28) (actor +0x1774 + index · 0x44).

Once per frame (0x4fe890):

1. Sight (0x54d0a0): sentients of the enemy team
   (axis ↔ allies, table {0, 2, 1, 0, 0}) are sorted by (1 / distance) · max(now − last check − 100 ms, 0), highest first.
   The first one with no check yet or an older check is checked (0x54ca60); candidates are tried until one sight trace was
   made (trace counter actor +0xd4).
2. Check: from the actor's eye (`tag_eye` world position and its horizontal forward, 0x500910; origin + 64 without the tag)
   to the target's eye (actors: `tag_eye`; player: origin + view height, 0x4e8b70). Distance/cone test (0x54c840): fail
   beyond `maxsightdistsqrd` (when ≠ 0) or the fog limit 0x1c61038; with a cone (`fovcosine` ≠ 0; the current enemy and
   `favoriteenemy` get none) fail behind or when dot² < dist² · fov². A pass makes a sight trace (0x54c760, mask
   0x280d803); a clear trace still needs a
   visibility value ≥ 0.2 from 0x48b090.
3. Update: visible, last check = now; if visible: last seen = now, ignore-until = 0 (not on a turret), last known time,
   position and own position (0x500c50). If the target is an actor not on a turret, it gets the same update about this
   actor when this actor is inside its own distance/cone limits (no second trace).
4. Enemy (0x54e230): over enemy-team sentients with
   a known position, not `ignoreme`: aware = (on a turret or seen within 5 s) and ignore-until ≤ now. Unaware candidates
   are skipped once an aware one is the best. The best has the highest threat, aware beats unaware, and `favoriteenemy`
   always wins. The enemy is set (0x515150): attacker counts follow and the actor is notified "enemy".
5. Threat (0x54dea0): `pacifist` actors ignore
   sentients that did not attack them within `pacifistwait`. Not visible: the current enemy counts as aware when its last
   known position is within 64 units of where it is and both stand near path nodes (192 units) that see each other
   (0x517e40: bit n·min + max of the node visibility), else as "recent" when known within 10 s; others are aware when known
   within 5 s. Score = threatbias + (visible 1000 | aware 500 | recent 250) + clamp(−100 · accuracy difference, −500,
   1000) + (dist < 2000 ? round((2000 − dist)² · 0.00125) : 0) + max(−100 · attackers (minus itself when current), −400)
   + (current enemy: visible/aware 1000, recent 500, else 100).

TODO: the accuracy term (0x550da0: weapon accuracy graphs, player accuracy cvars), the 0x48b090 visibility value, the fog
limit, `notarget` (entity flag 4 at +0x128), the player's real view height, and
using a turret's eye for gunners (0x51b940).

## AI states (observed; objdump where Ghidra has no function)

State table 0x56f720, 0x1c bytes per state: {start, finish, suspend, resume, think, touch, damaged}. States: 0 none,
1 exposed, 2 turret, 3 grenade, 4 badplace, 5 death, 6 pain, 7 animscripted, 8 animcustom, 9 negotiation (traverse).
Suspend is "finish the current state" (0x5498a0) and resume "start the current state" (0x5498b0) except exposed
(resume 0x548d40) and turret (suspend 0x549b70).

**Stack and requests.** Stack of 5 at actor +0x8 (depth +0x1c, +0x20 = time the bottom entry was last set). Requests
{type, state} queue at +0x30 (count +0x88): 1 replace (0x4feb00), 2 push (0x4feb40), 3 pop (0x4feba0). Each enqueue
merges the last two (0x4fe980) with the 4×4 table 0x56f838 [previous][new]: −1 keep both, 0 drop both,
else the previous takes that type and the new state (replace+replace→replace, replace+pop→pop, push+replace→replace,
push+pop→nothing, pop+push→replace); with +0x25c = 0 (dead) only the first request is kept. A predicted stack
(+0x8c, depth +0xa0) follows the queue. Push of 6…9: refused while levels 1…depth of the predicted stack hold a
blocker (0x4fea00, table 0x56f878: pain ← 7, 9; animscripted ← none; animcustom ← 6, 9; negotiation ← 7); then the
predicted stack is popped down below the first level holding a replaced state (0x4fea60, table 0x56f8b8: pain → 6;
7, 8, 9 → 6, 7, 8, 9). Kill (0x4febd0): finish states depth…1, queue only a replace with the state.
Apply (0x4fe4e0): replace = finish current, set, start (a failed start sets exposed and starts it);
push = suspend current, depth+1, start (failure → 0x4fe460); pop = finish current, 0x4fe460. 0x4fe460: step down and resume until a resume succeeds, finishing the ones that fail; at depth 0 the state
becomes exposed and starts.

**Actor think** (0x4ff030, every 50 ms): loop {apply requests if any; r = think(current)} while
r = 1. r = 2 → corpse (0x4fdba0: entity type 0xb, contents 0x4000000, notify "death"); otherwise
when 0x4fef50 holds the actor is notified "goal": inside the goal (0x5026d0: |dz| ≤
goalheight, 2D distance ≤ goalradius, inside the goal volume if any) and the path, if any, ends inside it (facing 6 at
a cover node also needs the yaw near the node's angle). At spawn animscripts/init runs as a plain thread, then the
bottom state starts (0x4fdee0).

**Animscript** (0x53ac90): actor +0x204 current script handle, +0x214 movemode byte (field getter
0x536960: 0 "stop", 1 "stop_soon", 2 "walk", 3 "run", constant strings 0x1c7f112…0x1c7f118), +0x218 anim mode
(+0x21c override). The same script keeps running: anim mode updated, a changed movemode notifies "movemode".
Another script: 0x53ac40 (notify "killanimscript" if a script thread exists, current = none), then
the new one starts. The current script is not cleared when its thread ends. Choices: 0x53aed0 the
claimed node's cover script when at the node (0x54bb30), else `stop`; 0x53ae40 that script (0, 1); 0x53b050 the node's cover script (0, 1) or `combat` (0, 4); 0x53afe0 / 0x53af20 while moving: the stop choice with movemode stop_soon when +0x704 − now ≤ 200 (af20: < 200), else
`move` with movemode run / walk (anim mode 1); 0x53ade0 (followed actor +0x2148): stop choice if |velocity| < 1,
else `move` walk.

**Path follow** (0x552350): entering movement sets anim mode 1, movemode stop; step 0x551960; run →
0x53afe0, else 0x53af20; on entering, +0x704 is cleared. Stop soon (0x503fe5…0x5040ad): when the path end is
within |velocity|² · 0.0625 (a quarter second) +0x704 = now + 500; when it has passed the path is cleared.

**Exposed** (think 0x5496f0; start/resume set substate 100): grenade (+0x20c4) → replace 3; bad
place (+0x2128) → 4; a recorded turret (+0x2114) that is free → replace 2 (in use → forget it); then perception
(0x4fe890) and the substate (0x548d50): no enemy → 0x65 noncombat; noncombat and the enemy seen
(0x54ce10: the last sight check if within 250 ms, else a new one) → 100 combat; reacquire 0x66 →
combat without a path (or with +0x728/+0x2148), or when seen and 0x54d010 passes.
- Combat 100: path update 0x503490; with a path and the enemy seen: clear the path when inside the goal (always with
  +0x728/+0x2148). No path → 0x548c70 (0x53b050; inside the goal, with the enemy not seen for 5 s, at a claimed
  cover node: facing 6, else 3); else facing 4 and path follow (run).
- Noncombat 0x65 (0x549520): path update; no path → facing 3 and, with a known enemy, 0x53b050, else
  the stop choice (cover node with spawnflag 0x8000 at the goal: facing 6); with a path → follow, run when the
  distance to the path goal exceeds `walkdist`, facing 2 (4 inside the goal; 6 near flagged nodes).
- Reacquire 0x66 (0x5494a0): facing 5, path follow (run).

**Damage.** G_Damage notifies "death" (attacker) when health ≤ 0 and calls the die callback, else the pain callback.
Pain (0x4ff3b0): damage fields, the state's damaged handler, the attacker's "last attacked" (+0x1780)
= now, push 6; when pushed, 0x53ac40. Die (0x4ff530): unless the state is death: damage fields, kill
→ 5, +0x25c = 0, 0x53ac40.
- Pain: start 0x54d2a0 (+0xa4 = now, clear path, `pain` (0, 4)); think 0x54d340: after the start
  frame, once the script thread has ended → pop and think again; else notify "pain", perception, facing 1, `pain`
  (0, 4); finish 0x54d300 broadcasts the pain AI event (0x535770).
- Death: start 0x548980 (substate 200); think 0x548ab0: substate 200 and 800 ms
  since the state was set → 0x548a10 (death AI event, grenade, sentient release, clear path), substate 201; the
  `death` script's thread ended → 2 (corpse); else facing 1, `death` (0, override or 4); 500 ms after death the
  contents become 0x4002000.
- animcustom(func) (0x540450): push 8; when pushed 0x53ac40 and +0x208 = func. Think 0x54b240:
  clear path, run func as the animscript `<custom>` (0, override or 4); when its thread ended → pop.
- Turret 2: start 0x549960, finish 0x549b20 (release), think 0x54aba0: leaving → release, replace
  with exposed, return 1 (see docs/spec/turrets.md).

Implemented in `src/game/actorstate.cpp` / `actorturret.cpp`. TODO(spec): grenade (0x5205b0/0x520c30: flee,
takecover, cower, combat, coverattack, acquire, throwback substates 300…0x132), badplace (0x524060), animscripted
(0x4f6240, 0x54b0a0), negotiation (0x54b720/0x54b760), node claiming and cover scripts by node (0x54bb30), the path
planner 0x503490 and follower 0x551960 (a node-graph placeholder is used), facing modes, AI events (0x535770), touch
and damaged handlers, followed/blocking actors (+0x2148, +0x728).

## Not yet specified

Path finding (0x503490, 0x551960) and the remaining states listed above.

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
`spawned.fightDist * 0.5`) therefore read an ordinary script variable. `animscripts/init.gsc` sets it
(`self.fightDist = WeaponFightDist(self.weapon)`), and init runs before the spawner's `"spawned"` notify (0x5325ad),
so the value is defined there.

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

`movemode` (actor +0x214) is a byte; its getter 0x536960 (raw slice) returns the constant strings 0x1c7f112…118
for values 0–3: "stop", "stop soon", "walk", "run" (constants set in 0x5064a0). The zeroed actor therefore starts with
`movemode` "stop". The engine's movement code updates it; OpenIW2 sets "run" while moving to a goal and "stop" on
arrival (inferred placeholder).

TODO(spec): `anim_pose` (+0x146) is a string index, 0 after allocation; what the getter returns for 0 is not read.
OpenIW2 uses "stand"; `animscripts/init.gsc` sets `anim_pose = "stand"` itself.

## Path data (lump 38, observed)

Loaded by `FUN_0052e3c0`: `int32 version = 8; int16 nodeCount` (must equal the number of
`node_*` entities: "Path node count mismatch in bsp file"), then per node `int16 linkCount` and
`linkCount × { uint16 targetNode; float distance }`. Node-to-node visibility follows as `(n − 1)·n` bits,
run-length coded: a byte `b < 0x80` means `b` zero bytes then one literal byte, and `b ≥ 0x80` means `~b` literal
bytes. `toujane`: 2,571 nodes, 21,583 links; the lump is consumed exactly.

OpenIW2 moves actors along an A* path over this graph at the root-motion speed of their current animations.
