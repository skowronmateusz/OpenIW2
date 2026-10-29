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

### aitype `spawner()` / `precache()` call sites (decoded)

- Loading (0x4eae00, per `actor_*` entity): the three labels `main`, `precache`, `spawner` (strings 0x573c90 / 0x587ff4 /
  0x587fec) of `aitype/<name>` are resolved into a handle struct {+0 main, +4 precache, +8 spawner}, cached in the hash table at
  0x744d18 keyed by the aitype name.
- Running (0x4fdf30, called from G_InitGame at 0x4da4fa, after the map entities are spawned and before 0x516b70): for each
  in-use entity of type 9 or 10 it looks the struct up by the entity's aitype name; for type 10 (actor spawner) it executes the
  `spawner` handle (+8) once with self = that entity (0x46db10 then 0x464ea0 frees the thread), and, for either type, executes
  the `precache` handle (+4) once and clears it. This is why `setspawnerteam` (0x4f8750) works on the spawner: it is only valid
  for an actor spawner. The port does not run `spawner()`; it uses the team named by the class name as fallback in
  `getspawnerteamarray` (every shipped aitype `spawner()` sets exactly that team, see builtins.md). A second loop (0x4fe03c)
  then runs `main` of each remaining aitype struct with the animscripts init handle 0x1c7e0f8 (not decoded further).
  TODO(spec): the exact condition of the second loop; whether any shipped `spawner()` does more than `setspawnerteam`
  (only the 100 team calls were counted).

## Spawning (implemented; partly inferred)

- `actor_*` entities are spawners. Those without spawnflag 1 (`SPAWNER`, which the aitype headers describe as
  turning the entity into a spawner rather than a placed AI) spawn an AI at load.
- `dospawn()` / `stalingradspawn()` on a spawner create an AI. It copies the spawner's key/values, decrements
  `count`, runs `aitype main()` with `self` = the AI, starts `animscripts/init::main()` (handle 0x1c7e0f8, 0x532558)
  and the bottom AI state, and then notifies the spawner `"spawned"` with the AI (0x5325ad).
- An actor's `damageweapon` is the attacker's weapon name, set by the pain and die callbacks (0x4ff3b0 / 0x4ff530)
  from the attacker entity's weapon index. G_Damage (0x528050) uses the world entity when there is no attacker, and
  weapon index 0 is `none` (0x4dc210). A turret's bullets credit the turret's user, or the world (0x51ada0).
- `getaiarray(team)` (0x4ec9f0) lists actors whose +0x25c is set; the die callback clears it, so the dead are
  not listed. The errors "dospawn can only be called on actor spawners" and "Spawner at (%g %g %g)
  is in solid" exist in the executable.

## Spawn checks (observed; src/game/spawnchecks.cpp, world.cpp spawnActor, tests/test_spawnchecks.cpp)

- Builtins 0x4edff0 (dospawn) / 0x4ee120 (stalingradspawn): a non-spawner (eType != 0xa) is an error; a spawner whose +0x158
  is >= level.time returns undefined (one spawn per spawner per level time; +0x158 = level.time after a success, 0x4ee0f2);
  with exactly one parameter (0xa0189c == 1) its const string (0x46e760) is passed on as the new actor's targetname.
- Core 0x532200 (spawner, targetname, force): ai_disableSpawn prints "Attempted spawn prevented by ai_disableSpawn." (not
  default 0, registered at 0x4d89d7, dvar 0x1c14880); count +0x148 == 0 returns undefined. Without force: the telefrag test 0x4e8500, then, only once the level is
  loaded (0x1c61048 == 0), the sight test: the player (G_Find classname "player") eye 0x514f70 against 0x532100(eye,
  spawner origin, player, spawner) -> "^3couldn't spawn from %s because player can see spawnpoint" (0x58f850).
  0x1c61048 is set by G_InitGame (0x4d99cb: 1 new level, 2 savegame load) and cleared by 0x4da540 (called at 0x448aab,
  after the entity spawn, the level main thread and the first G_RunFrame). The spawner is copied (0x4f95b0), the copy's
  targetname set to the argument (0x53233c, none without one), its spawnflags bit 0 cleared (0x53234e), then 0x4fc640.
- Telefrag 0x4e8500: area query 0x419ea0 (box origin + (-15,-15,0)..(15,15,70) from 0x5a5bdc, up to 0x800 entities, contents
  mask 0x200c000) against each linked entity's absolute box, which SV_LinkEntity widens by 1 on every side (0x44b555..0x44b5c8,
  0x591e20), touching included. Blocks when one is a client with ps pm_type < 4 (client +0xfc, +4) or an actor (+0x100) with
  health (+0x13c) > 0. The player's r.mins / r.maxs are the pmove box of the last ClientThink (0x4e7d68..0x4e7da2, stance
  height 70 / 50 / 30); spawn sets (-15,-15,0)..(15,15,70) (0x4e9100). The port uses that height for the player clip, the
  trigger touch box and this test.
- Sight probe 0x532100 (eye, pos, pass1, pass2): false when |eye - pos|^2 * 0.685584 (0x592298) > the fog sight limit
  0x1c61038; else true when any of 11 points pos + (x * 30 - 15, y * 30 - 15, z * 72) (table 0x5701e8: (.5,.5,.8),
  (.5,.5,.5), (.5,.5,.2), then the eight box corners) is not sight-blocked from the eye (0x44c3c0, point, mask 0x801).
- Actor spawn 0x4fc640: when no actor slot is free (0x4fc8d0, 32 slots) and the entity has spawnflags 2, 0x4fcaf0 frees one
  and the slot is allocated again (0x4fc687..0x4fc691); otherwise the entity is freed (0x4fc6a9), and dospawn prints
  "^3couldn't spawn from %s because there are no free actors" (0x58f814).
- Eviction 0x4fcaf0: while 0x1c61048 is set, Com_Error "too many actors in bsp file" (0x589e50). Else eye and view forward
  of the player (ps viewangles +0xc4 / +0xc8, AngleVectors) and up to four passes over the actors in use (0x4fe2c0 /
  0x4fe310, all teams), skipping spawnflags 4 (undeletable) and the entity G_RunFrame is running (0x1c61064); the first pass
  with a candidate wins: (1) farthest the player cannot see (0x532100), (2) farthest behind (forward . delta < 0),
  (3) farthest with (forward . delta)^2 < 0.5 * distance^2 (0x591e10), (4) farthest (>=). None: "Tried to force spawning
  of an AI when all AI slots are used by undeletable AI" (0x589e00). The victim is notified "death" (const 0x1c7f05c,
  string 0x587ea4) and freed (0x4d7c30), like the script delete.
- A real spawner (spawnflags 1) is never freed by a spawn (only the positive count is decremented, 0x5325ca..0x5325d8); a
  map-start placed actor is the entity itself, which the port replaces by a new entity.
- TODO(spec): the eviction iterates actor slots; the port iterates entities (differs only for equal distances).

## Server frame order (observed; G_RunFrame 0x4da970, src/game/world.cpp World::frame)

G_RunFrame is a resumable state machine (stage 0x1c6105c, jump table 0x4db27c, optional rdtsc time budget); the frame
start 0x44a6a0 adds 50 ms to level.time and resets the stage. Stages:
1. 0x4da99f: 0x4e8160 on the player: if in use, the queued client commands and usercmds (0x409ed0 -> 0x4e7ee0 ->
   ClientThink 0x4e7bc0) and the eye trail 0x5154b0; then 0x4e8180: the moved byte (+0x86b6, 0x4e8194..0x4e81e5) and
   G_TouchTriggers 0x4e7320 (triggers and weapon items, Touch_Item 0x521a90) for a player with pm_type < 4 and neither
   noclip (client +0x8628) nor ufo (+0x862c); then the level change 0x4d9f90 and the friendly chain error print.
   The eye trail 0x5154b0: every g_playerTrailTime ms (int dvar 0x1c13f08, default 500) the player's eye and the time go
   into a two-entry ring (0x1c8d998 / 0x1c8d9b0, index 0x1c8d994); 0x515630 reads the older entry for 0x54fa40, the target
   motion term of the hit chance 0x54fb90, which ai_vsPlayerUseAccuGraphOnly (default 1) skips for the player
   (0x54fbd3..0x54fbe2). Not ported (inert with the default dvar).
2. 0x4daa14: 0x447ec0 for every entity from 1: the animation weight update 0x476960 (dt 0.05) of the entity's tree; the
   queued touch records (0x1c610b4, count 0x1c628b4, filled by 0x528cb0) are taken over (0x1c61cb4 / 0x1c628b8).
3. 0x4dab00: each record with valid spawn ids sends "trigger" (const 0x1c7f0e4) with the other entity; a trigger already
   notified in this pass keeps its record for the next pass; after each pass the woken threads run (0x46d560).
   0x528cb0 notifies at once when the queue is full (256).
4. 0x4dac9c: the animation step of every entity from 1, running the notetrack-woken threads after each entity.
5. 0x4daefc: woken threads, then the script frame 0x467150; frame counter 0x9bef20 + 1.
6. 0x4daf94: bad places 0x523a60.
7. 0x4dafc6: 0x409ed0 and 0x5154b0 again (usercmds that arrived since stage 1), and with ps +0x7c bit 0x20000 (in a
   vehicle: VEH_LinkPlayer 0x511624 refuses a second vehicle on it) G_RunEntity of the vehicle at entity +0xf4.
8. 0x4db048: G_RunEntity 0x4da7e0 (once per frame, +0x130 = frame number) of every entity in number order, its link parent
   (+0x1c8) first; 0x1c61064 holds the running entity number (-1 outside). Dispatch 0x4da720: eType 3 G_RunMissile
   0x526c30, eType 2 (or physics +0x110) G_RunItem 0x523040, eType 5 0x52ccb0, clients nothing, else the think 0x4da090.
Then (0x4db11d) for each client in use 0x530cf0 and ClientEndFrame 0x4e7f90, then 0x4d9710, 0x517c30, 0x52b070, 0x4da2d0,
0x4da0e0. The server frame end 0x44a900 finishes a budgeted G_RunFrame (0x44a870), processes the save queue (0x44a230) and
builds the snapshot (0x4db2b0).
Port (World::frame): stage 1 = the use part of ClientThink (pmove itself runs in the front end before the frame), the
moved byte, trigger and item touches, then the threads the trigger notifies woke (stage 3); the animation step (stages 2
and 4, weights inside each entity's update); the script frame; bad places; the ridden vehicle (stage 7); stage 8 =
World::runEntity for every entity in number order (once per frame, link parent first; link follow, mover, vehicle,
turret, missile, item, script model, actor); ClientEndFrame (player link), death screen, saves.
The first frame and the level's main run at level time 50: the server spawn runs the frame start 0x44a6a0 (0x448985)
before 0x4da470.
TODO(spec): the exe's G_RunEntity runs one dispatch per eType; the port runs every part that applies, in a fixed order.
The weight update of stage 2 is done per entity inside the stage-4 update. Which parts of the entity spawn run at time 0
(G_InitGame) and which at 50 (0x4fa5c0): the port spawns everything at 0.

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
   to the target's eye (actors: `tag_eye`; player 0x4e8b70: ps origin + view height ps+0xd4, moved by the lean
   0x440380(…, 16, 20), or the turret's `tag_player` while ps pm_flags & 0x300 — so a crouched (40) or prone (11) player
   is checked at that height, not at a standing 60). Distance/cone test (0x54c840): fail
   beyond `maxsightdistsqrd` (when ≠ 0) or the fog limit 0x1c61038; with a cone (`fovcosine` ≠ 0; the current enemy and
   `favoriteenemy` get none) fail behind or when dot² < dist² · fov². A pass makes a sight trace (0x54c760, mask
   0x280d803); a clear trace still needs a
   visibility value ≥ 0.2 from 0x48b090. That value is the FX sight blockers only (smoke): 1.0 (0x591e20) with no
   blockers (count 0xc47a20) or when the segment is shorter than `fx_visMinTraceDist` (dvar 0x617eb0); else, with
   dir = normalize(end − start) and half = length · 0.5 (0x591e10), every blocker (0x14 bytes from 0xc3ed88: origin,
   radius², visibility) whose projection t = (origin − start) · dir has |t − half| ≤ half and whose closest-point
   distance² is below radius² multiplies the result by its visibility. World geometry plays no part in it.
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
   + (current enemy: visible/aware 1000, recent 500, else 100). The node visibility table (0x1c8da44) is the
   lump-38 bit set loaded by 0x52e3c0 (0x516c86); only with the developer dvar `g_connectpaths` (0x1c148b4,
   registered at 0x4d9339) does 0x516c03 → 0x52ee40 regenerate links and visibility at load ("Map does not have paths
   generated. Run the game with "g_connectpaths 1" …", 0x58ef50). Shipped maps therefore use the compiler's bits as
   data; a null table makes 0x517e40 return 0 (not visible), the same node returns 1. The awareness here only
   ranks enemies (score); it does not make the actor fire (see "Firing at an unseen enemy").
6. Last sight position (0x4fe830, after step 4 at 0x4fe8bf / 0x54d45b): when the current enemy's record is visible
   and its last seen time is the current level time, the actor field `lastenemysightpos` (+0x203c, defined flag
   +0x2048) becomes the enemy's eye (0x514f70: actor `tag_eye` 0x500910, player 0x4e8b70). Losing sight does not
   clear it. An enemy change (0x515150, 0x5151ff..0x515217) and the enemy-reference clears of the sentient release
   paths (flag writes at 0x514d83, 0x514de6, 0x514e3d) clear the flag (0x515150 and 0x514d50 also actor +0x1738 /
   +0x1764, not modelled) and then run 0x4fe830 again (0x515217, 0x514d89). Two more clears (0x4fe23d, followed by
   0x4fe830, and 0x540db2) are in callers not decoded yet. Its setter
   0x5368d0 stores a script vector or clears the flag for undefined; the getter 0x536940 returns undefined while the
   flag is clear.

### Firing at an unseen enemy (observed)

The `shoot` builtin path 0x550a70 has no visibility test: without a position it rolls the hit chance (0x54fde0) and
aims at the current enemy's body (0x54ff90 / 0x550390), with a position (0x550af6) it fires at that point. Whether an
actor fires is decided by the animscripts' gates, all of which trace: `cansee` (0x53d930 → 0x54ce10 / 0x54ca60, sight
mask 0x280d803), `sighttracepassed` (sight mask), `canshoot` (0x53d800 → 0x54cf40: muzzle to the point, mask
0x200c861, also passing when the first hit is a sentient of another team; `canShootEnemyFrom` adds the eye offset for
its sight check), and suppression (`hasSuppressableEnemy` / `foundGoodSuppressSpot` in utility.gsc), which needs
`lastenemysightpos` (step 6) and aims `shoot(1, spot)` at points between that position and the enemy that the muzzle
can sight-trace to. Behind sight-only cover (rhine's fallen trunk, contents 0x8031640: bits 0x1000 and 0x40) `cansee`
and `canshoot` fail, so an exposed actor falls back to suppression of the last sight position; its bullets (mask
0x280e831) are not stopped by such cover.

TODO: the accuracy term (0x550da0: weapon accuracy graphs, player accuracy cvars), the fog
limit, `notarget` (entity flag 4 at +0x128), the actor fields +0x1738 / +0x1764 cleared with an enemy change, the
`clearenemy` builtin (OpenIW2 clears the field directly, bypassing 0x515150), the node visibility derivation of
0x52ee40 (developer path only), and using a turret's eye for gunners (0x51b940).

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
when 0x4fef50 holds the actor is notified "goal": inside the SCRIPT goal block (0x4fef53: 0x5026d0 with actor +0x764 = setgoalpos / setgoalnode
position (spawn origin at spawn, 0x4fc80d), +0x770 goalradius, +0x774 goalheight, +0x778 goal volume; not the working goal
+0x748: `World::inScriptGoal`) and the path, if any, ends inside it (facing 6 at
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
combat without a path (+0x6b0 <= 0) or with +0x728/+0x2148 set, else when the enemy was seen within 250 ms (0x54ce70) and
0x54d010 passes (0x54cf40: a point trace, mask 0x200c861, from the actor's muzzle (0x500b40, tag_flash; no tag fails) to the
enemy's eye - 16 (G_Trace 0x44bf00 start EBX, end EDI), clear or ending on a sentient of another team; beyond the fog
limit 0x1c61038 it fails). The +0x728/+0x2148 shortcut is not modelled.
- Script sight builtins: `canshoot(target [, offset])` (0x53d800) is 0x54cf40 from tag_flash + offset (script error
  "Couldn't find tag_flash in entity %d" without the tag); `cansee(ent [, seconds])` (0x53d930) on a sentient is the cached
  sight 0x54ce10 no older than seconds × 1000 (truncated; default 250 ms), refreshed by 0x54ca60 (TODO(spec): non-sentient
  targets, 0x54cc87); `geteye()` (0x4eee60) of an actor is tag_eye (0x500910, scr const 0x1c7f156) looked up in the whole
  DObj, so the head model's tag_eye counts; without one, origin + 64 (warning 0x589b84).
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
  `death` script's thread ended → 2 (corpse); else facing 1, `death` (0, override or 4); while the script runs, from 500 ms
  after the state was set (+0x20) the contents (+0xc0) are 0x4002000 and the entity is relinked (0x548bac..0x548bcd). Until then
  the dying actor keeps its living contents 0x4000 and box. Which traces that hits, from the masks in the exe: 0x2000 is in the
  bullet / melee / bullettrace mask 0x280e831, the grenade and rocket clip 0x280e891 and the use trace 0x280e801, not in the actor
  move (0x2820011), player move (0x281c011) or sight (0x280d803) masks; 0x4000 is in all of these. The corpse (0x4fdba0) gets
  0x4000000 (0x4fdc75), which no mask in the exe contains. Ported: `syncEntityClips` keeps a clip for an actor in the death state
  with 0x4000, then 0x4002000 (`ActorAi::dyingContents`, set by `deathThink`); a corpse has none (toujane lethal combat scene:
  death state at 8.15 s, 0x4000 to 8.65 s, 0x4002000 to the corpse at 10.0 s).
- "Script ended" (death, pain, animcustom, negotiation, grenade return, 0x53ac10): the animscript thread id +0x200 is 0, or the VM
  variable of that id (table 0x8bee88, stride 16, type = low 5 bits) has type 0x17, which the VM's thread-end paths 0x464990 /
  0x46ca60 / 0x46d1f0 write (`& ~8 | 0x17`). The seven actor-code readers (0x520973, 0x53ac25, 0x548b09, 0x54b122, 0x54b2d1,
  0x54b7c3, 0x54d377) test only 0x17; every other type of that table belongs to the VM (`actorAnimScriptDone`).
- animcustom(func) (0x540450): push 8; when pushed 0x53ac40 and +0x208 = func. Think 0x54b240:
  clear path, run func as the animscript `<custom>` (0, override or 4); when its thread ended → pop.
- Turret 2: start 0x549960, finish 0x549b20 (release), think 0x54aba0: leaving → release, replace
  with exposed, return 1 (see docs/spec/turrets.md).

Implemented in `src/game/actorstate.cpp` / `actorturret.cpp`; grenade state in `actorgrenade.cpp`
(docs/spec/grenades.md), badplace state in `badplace.cpp` ("Bad places" below). TODO(spec): animscripted
(0x4f6240, 0x54b0a0), negotiation (0x54b720/0x54b760), facing modes ("Facing and desiredangle" below),
touch and damaged handlers, followed/blocking actors (+0x2148, +0x728). The AI events (0x535770) are implemented
("AI events" below).

## Facing and desiredangle (observed)

The facing mode is actor +0x104 (int): the state code writes it (0x548c70 combat-no-path → 6 at a covering
claimed 0x8000 node when the enemy was not seen for 5 s inside the goal, else 3; reacquirestep 0x548f10 → 3
with substate 0x66). Each actor movement frame FUN_00504160 switches on the anim mode (+0x218) and either
moves along the path or turns toward a 2D direction: FUN_00423900 turns a direction into a yaw (· 57.2957764 =
180/π, so degrees), FUN_00423f50 is AngleNormalize180 (180/360/−180 constants at 0x591e1c/0x591f08/0x591f0c),
and FUN_00504aa0 applies the turn, maintaining the two angle pairs (+0x108/+0x110 and +0x118/+0x120, selected
by +0x114) and driving the entity angles. Then FUN_00504ca0 — the only store to actor +0x124 in the
executable (0x504cb7/0x504cc5; also 0x532356, but that one is dospawn's entity spawnflags `&= ~1`) — sets
`desiredangle` to the active pair's target: degrees, the units of `angles[1]` (animscripts/stop.gsc:555
ProneStill turns while `AngleClamp(desiredangle − angles[1])` is > 5). Reacquirestep side-steps around
desiredangle + 90° (constant 0x5920bc).

Implemented in `src/game/world.cpp` (`moveActor` writes `desiredangle` from the look direction while
path-moving) and `src/game/actorfields.cpp` (0.0 from the zeroed allocation, so scripts always read a float).
The anim-mode switch, the turn rates, the facing-mode targets and the look turn: see "Facing blend"; the +0x159..+0x160 slow
turn is the prone block (0x514b30 / 0x514c40), not an override angle.

## Cover nodes (observed; Ghidra decompiles and the objdump listing)

**Node drop** (0x515f80, called by G_InitGame at 0x4d9c3e right after the entity spawn 0x4fa550): the contents (+0xc0) of
every entity whose flags (+0x128) have 0x100 (DYNAMICPATH) are saved and zeroed, every node goes through 0x515d70, then the
contents are restored. 0x515d70(node): G_Trace 0x44bf00 of the box (-15,-15,0)..(15,15,30) from origin + (0,0,1) to origin −
(0,0,256), pass entity 0x7ff, mask 0x820011. allsolid or startsolid (+0x22 / +0x23): "^3WARNING: Pathnode (%s) at (%g %g %g) is
in solid" (0x58d790, type name from 0x5a9368); fraction 1: "... is floating" (0x58d75c); both set the type (+0) to 0 (BAD NODE).
Otherwise the hit point (z = start + fraction·(−257)) is tested with the actor box 0x56f704 / 0x56f710 (start = end); solid gives
the "in solid" case, else the hit point becomes the origin (+0x14..+0x1c). Implemented in `World::dropPathNodes` (before the
path graph and the chains are built); the entity clips there are the non-DYNAMICPATH script_brushmodels (vehicle collmaps are
linked later, 0x50dc40 at 0x4d9c6f). TODO(spec): the other entity classes linked at 0x4d9c3e. The 39-map sweep flags 29 nodes
"in solid" and 2 "floating"; all become BAD NODEs (type 0) as in the exe. The rejections are real overlaps under the exe's
own brush test (0x416480: axial sides pushed out by the trace extents (15, 15, 15), the other sides by |offsetZ·n.z| + radius,
d ≤ 0 inside, epsilon 0x591f80 = 0.125; the port's `traceThroughBrush` does the same): for breakout's negotiation End node
(194.331 8859.31 2) the drop sphere (radius 15, centre (194.3, 8859.3, 18)) is 2.3 inside the AI clip brush's y face and 3.5
inside its slanted side, 13.5 units from the brush's vertical edge at (199, 8872); newvillers (−1648 2096 −48) is 7 units
from a clip wall. An earlier note here assumed the exe could not reject such nodes because breakout's friendly chains would
then be emptied; that was never checked against the exe, and a bad chain node clears the chain fields (0x52d8b0 →
0x52d5d0: +0x34/+0x36 = 0, +0x38 = −1; it also writes +0x30/+0x32 = −1 and +0x3a/+0x54 = 0, of which the path data load
0x52e3c0 rewrites +0x3a (0x52e473); TODO(spec): +0x30/+0x32/+0x54 there, not modelled); the chain node count is kept, so trigger_friendlychain (0x518bf0)
still finds its node by name. breakout and
silotown_assault therefore print "Error: bad node [%d] in friendly chain" and "Bad node in friendly chain" (0x42c7f0 channel
1, not fatal), as the exe does. Debug opt-out: `IW2_NODE_DROP_STRICT=0` keeps a rejected node's type.

**Runtime node** (0x78 bytes from 0x1c8da04; script value = node index): +0 type 1…18, +4 spawnflags (0x40 priority,
0x8000 "cover script optional", 1 never a cover candidate), +0x14 origin, +0x20 yaw, +0x24 forward, +0x30/+0x32 linked
nodes, +0x40 owner sentient, +0x44 hold time (0x7fffffff while claimed), +0x44 + 4·team per-team block time (1 axis,
2 allies), +0x56 claims of linked nodes. A sentient's claimed node is +0x2c (script field `node`).
- Usable (0x518f20): own node → yes; now ≤ team block → no; hold < now → yes; claimed nodes can be taken over
  (0x518e80) and priority nodes shared (0x518dc0: owner dying, or a distance rule).
- Claim (0x5152f0): release the old one (0x5191a0: owner and hold 0); a normal node, or a priority node whose hold
  expired, is taken (0x518fa0: owner, hold 0x7fffffff); a held priority node is taken over (0x519050), evicting the
  claiming owner (0x515330: its path is dropped if it ends at the node, then 0x519300).
- 0x519300 (path failed / node given up): an owner of that team leaves with a 5 s hold (0x519200), owner cleared,
  the team blocked 5 s. Give up (0x4fe670): drop a path ending at the node, then 0x519300. The sentient release
  (0x514d50, death) frees the node.
- Node update after each exposed/pain/death think (0x4fe8d0 → 0x4fe740): unless keepclaimednode(ingoal), keep the
  claimed node while it is near (`nearnode` 0x502660: |dz| < 80, 2D < 30) the actor or its path end, or before the
  plan time; else release it and claim the sentient's node (0x515040, within 192) when it is a cover type (mask
  0x41ffc = types 2…12, 18) or flagged 0x8000, near and usable.

**Cover script** (0x54bb30, `check`): type → `cover_stand` 2, `cover_crouch` 3/4/18, `cover_prone` 5, `cover_right` 6,
`cover_left` 7, `cover_wide_right` 8, `cover_wide_left` 9, `concealment_stand/crouch/prone` 10/11/12 (slots
0x1c7e090 + 8·i, names pushed at 0x4eb233); other types only with flag 0x8000 (no script). The node "does not cover"
(true, no script) when the enemy (not ignoreme/notarget, last known time > 0; with `check`, else only when seen
within 250 ms) is covered neither at its origin nor at its last known position, or without an enemy (with `check`)
the grenade is not covered. Covered (0x54b960, arcs 0x54ba00): 2D distance² ≥ distSq, then either in front of the
node (dot with the forward ≥ 0) or the yaw from the node minus the node yaw, normalised through 16 bits, inside
[min, max] (wrapping): types 2/3/10/11 {285², facing}, 4 {285², facing}, 5 {800², facing}, 6/8 {285², 305…30},
7/9 {285², 330…55}, 12 {285², facing}, 18 {285², arcs of the node's turret or 315…45; `setturretnode(node, turret)` 0x515910 stores the turret entity at node +0x58, -1 at spawn and after `unsetturretnode` 0x5159a0; 0x5159f0 reads turret info +0x38 / +0x40 as (int)(deg·65536/360) & 0xffff · 360/65536}, others {0, facing}. With `check`
the facing test becomes the arc test when the enemy was not seen within 5 s. Arc limits for the facing types:
2/3/10/11 315…45, 4 350…10, 5/12 330…30, default 305…55.
- Faces the enemy (0x54c000): the enemy is in front of the node, and the actor is not farther from the node than
  the enemy on the enemy's side. Good claimed node (0x54c0c0): usable, faces, covers.
- Animscript choice: stop (0x53aed0) and combat (0x53b050) use the claimed node's script when at the node (0x502910:
  keepclaimednode or near); a node that does not cover is given up; else `stop` / `combat`. Combat without a path
  (0x548c70): facing 6 inside the goal when the enemy was not seen for 5 s and a claimed 0x8000 node covers.

**Search** (0x54c3f0(better, ignore, prefer, max)): candidates within goal radius (2D) and goal height of the goal
position (0x516950/0x516820, node tree, at most 256, type in mask, not spawnflag 1); dropped (0x54c2a0): `ignore`,
unusable, not facing, not covering, outside the goal volume; `prefer` scores 16, the others need score ≥ score(better)
+ 1.25e-5 when `better` is good and inside the goal. Ascending sort (qsort, 0x54c3b0), the last `max` are returned.
Score (0x54bf10): (1 − min(d², 1200²)/1200²) + (enemy node visible from the node, 0x517ed0, else 1: 1) + 2·(not
concealment) + 4·(enemy yaw inside the arcs, 0x54bdd0) + 8·priority. 0x517ed0 indexes the node visibility bits as
(max − 1)·n + min (0x517e40, used by perception and `canattackenemynode`, as n·min + max).

**Path update** (0x503490, exposed combat/noncombat): runs with a path or once the plan time (+0x720) passed.
keepclaimednode → clear path, plan time +500. Fighting (0x503080): enemy known within 5 s (or, without a path, its
node visible from the claimed node). Node (0x502da0): the claimed node if inside the goal and good, else the best
cover node, else the claimed node if inside the goal. No node → path into the goal (0x502960; 0x502c70 on failure:
plan time +500). Node → path to it (0x503650/0x502d00: a path ending within 2 units is kept, standing there needs
none; with no previous path a non-corner node (types ≠ 6…9) that is near inside the goal needs none); failure →
0x519300, plan time +500. Then 0x503200 (stop for the enemy within `pathenemyfightdist`) or claim the node.

**Built-ins**: `findcovernode` (0x54c510: 1000 best into +0x798, enemy and exposed only), `getcovernode` (0x54c580:
pops the best while there is a path or the plan time passed), `findbestcovernode(["prefer"|"ignore"|"better", node])`
(0x53c730), `usecovernode(node)` (0x53c900/0x54c5e0: errors with keepclaimednode(ingoal); exposed or turret state;
usable and facing → claim, turret replaced by exposed, substate combat), `findreacquirenode`/`getreacquirenode` (10
best, +0x173c), `usereacquirenode` (0x548e60: usable, enemy eye visible from node origin + 64, path → claim, substate
reacquire), `reacquirestep(d)` (0x548f10: side steps ±d perpendicular to the enemy direction, random order; eye +
offset sees the enemy eye within maxsightdistsqrd; move check 0x543d30 (box −15,−15,step…15,15,72, step 10 crouched
else 18; drop 72; ground normal z ≥ 0.7; within 72 of the target height) and inside the goal; path → substate
reacquire, facing 3), `findreacquiredirectpath`/`findreacquireproximatepath` (0x549280/0x549340: path towards the
enemy, cut to the goal 0x5434e0), `trimpathtoattack` (0x543580), `reacquiremove` (0x53ccb0: with a path → substate
reacquire), `flagenemyunattackable` (0x53cd90: ignore-until 0x7fffffff, substate noncombat), `withinapproxpathdist`
(0x545030: no path, or the rest of it shorter), `ispathdirect` (path flag +0x6e8 & 1), `nearnode` (0x502660),
`canattackenemynode` (0x540500), `isnodeoccupied` (0x5158c0: now ≤ +0x44), `setnodepriority` (0x515810: flag 0x40,
types 18, 2, 3, 5, 10, 11, 12, 6, 7). `findbestcovernode` options are the constant strings set at 0x506e95…: 0x1c7f12a
"prefer", 0x1c7f128 "ignore", 0x1c7f12c "better".

Implemented in `src/game/actorcover.cpp`. Done since: node blocking 0x515390 (blockFacingNodes), goal volumes
(goalVolumeHolds), the 0x502960 offset goal (dead: +0x73c is only ever written 0), the damaged handler 0x5498c0, the trim
state set by the reacquire paths, trimpathtoattack 0x543580 (with the sight test 0x54c7d0), the planners 0x503a50 (reuse
rule) / 0x503be0 / 0x546980, the turret damaged handler 0x54af30, the node kd-tree order (0x52de40 build, 0x516820 walk) and
the node take-over 0x518e80 / priority rule 0x518dc0. Node links (+0x30 / +0x32 / +0x56:
overlapping nodes within 31 xy / 73 z are linked at load by 0x52d990; a claim holds the linked nodes for good without an
owner and counts on +0x56, release / block 0x5191a0 / 0x519200 / 0x519300 undo it) are ported (Follow-up 23). The fog sight limit
0x1c61038 = ((far - near) * 0.828 + near)² of linear fog (setcullfog, parsed by 0x4e96e0) applies in 0x54c7d0.
**Stop for the enemy** (0x503200, called at the end of the path update 0x503490 with `fighting` 0x503080, the node
chosen this update, and whether there was a path before it; `World::stopForEnemy`): 0x503080 = an enemy and, without a
path, the claimed node sees the enemy's node (0x502fc0: 0x515040 + 0x517ed0), else its position known within 5 s (record
+0x10). 0x503200 returns 0 without an enemy or path, when the enemy is not nearer (2D) than pathenemyfightdist, or when
the actor stands at the node (0x502660); 0x503000 then needs the enemy seen within 5 s (record +0x8) or, without a path
before, the claimed node seeing its node. Stop when the lookahead point L = origin + lookDist·lookDir (+0x6dc, +0x6c8) is
past the enemy ((L − enemy)·dir > 0) and nearer to it than lookDist (starting pathToEnemy +0x788 and recomputing the working
goal 0x505f70 when fighting); stop when inside the goal with the enemy seen within 250 ms (0x54ce70 → 0x54ce10) and there is
no node or the path ahead (0x543ae0: lookDist + the rest of its segment, +128 with a traversal point, then segment
lengths, the first one by +0x3c4) is longer than the node's 2D distance to the enemy (0x420ec0). Otherwise, when fighting,
not yet pathing to the enemy and outside the goal, 0x502e80 (reach = pathenemylookahead, +256 without a path before:
within it of the actor, or of the segments L…actor, the lookahead's segment rest, and further segments while the covered
length is below pathenemyfightdist; segment test 0x502e10) starts pathing to the enemy, and the update stops unless the
claimed node is in the new goal and is the chosen node (with no claimed node in it: unless the chosen node is). A stop
drops the path, waits 500 ms (0x503e10, +0x720) and does not claim the node (else 0x5152f0 claims it).
The goal entity (0x505ed0) and the working goal (0x505f70) are in `friendlychainworld.cpp` (re-checked against the exe: the chain node is cleared when there is no sentient goal entity, and the working goal volume +0x75c is the script volume only in mode 0). TODO(spec): the move
check's continuation after a block, `ispathdirect`, actor +0x20b4 in the score, 0x548be0, the other 0x505f70 caller
0x54ad05, `withinapproxpathdist` (ported: 0x545030 sums the lookahead +0x3ac/+0x3b4, +0x3c4 on the first segment and the segment lengths down to +0x386).

## Path planner (observed)

Node query 0x517f90: candidates of 0x516950 (2D radius, height, ≤ 64, type mask; mode 0: centre 120
below, height 184), linked nodes first then nearest (0x516640); a linked node is taken when the point is on it
(−16…16, −1…73) or a box (−15,−15,17)…(15,15,72) traced to it (mask 0x820011) is clear; plane-excluded and unlinked
ones are traced last. Sentient node 0x515040: radius 192, cached (+0x38, flags +0x3c/+0x3d). A* 0x545840: sorted open list by g + h (h = 2D distance to the goal position), insert before the first not lower,
re-open on cheaper g, links with a team block byte (+8 + team) skipped; 0x545ca0 adds planes. Path points 0x541da0: goal first in the original (+0x330, 0x1c bytes: pos, dir, length, node), at most 32 (+0x6e8 |= 4),
the goal position as an extra point when it differs (2D) from the goal node. Implemented in `src/game/actorpath.cpp`
(goal-first layout, see "Path following"). Negotiation pairs (types 16/17) are ported.

**Offset goal** (0x502960, 0x502a5b..0x502c58; `World::pathToSideGoal`): runs instead of the plain path to the goal when
s = min(|+0x73c|, goal radius − 15) > 0; s takes the sign of +0x73c. +0x73c is only ever stored as 0 (0x4fc7ab; no actor
field row, table 0x5703f0) and its other readers are 0x50533d / 0x505387 / 0x5053a4, so the branch is dead in SP; it is
ported for completeness with `ActorAi::goalSide` = 0. Point = goal + s·(dir y, −dir x) with dir = the lookahead direction
(+0x6c8), goal z. 0x542340 seeds {point, best node = the goal node of the working-goal query, best = its 2D distance²}
and runs 0x545670: the list search of 0x545840 without a goal (until the list is empty, returns 0); each neighbour must
first pass 0x5422c0 (2D distance² to the point below the best, (z − point z)² <= 6400; passing makes it the best), so a
non-improving neighbour is never queued. The result is the best node (`nearestReachableNode`, pathnodes.cpp, unit test
`test_nodesearch`). A move check 0x543d30 from that node's origin to the point (mask 0x820011, no pass entity, step 10
with the tilt byte +0x288, else 18) gives the target: its end if inside the goal, else the node origin if inside (both make
the node the goal node), else the working goal with the queried goal node. Then 0x542550 → 0x545840 (planes: 0x5428a0)
from the sentient's node (cache flag +0x3c cleared first; 0x5150d0 with planes). Returns whether there is a path.

0x541da0 arguments (read at every call site; the search variants 0x545840 / 0x545a70 / 0x545ca0 / 0x545f30 / 0x5461a0 /
0x546980 / 0x546c10 pass their sixth and seventh arguments as its fifth and sixth): the fifth says whether the goal position is
used (0: the path starts at the goal node's origin, no extra goal point); the sixth allows traversals (flag 0x10). When it is 0,
the first negotiation pair met in the chain walk returns 0 (the block was already reset by 0x541780 and its counts zeroed): no
path. Per caller: 0x503650 passes its third argument (0x502960 path to the goal and 0x502d00 path to a node: 1; reacquirestep
0x548f10 at 0x549229 and the grenade throw-back path 0x51fb20 at 0x51fd89: 0); 0x503890 and 0x503a50: goal point 1, traversals
1; the capped planner 0x503be0 -> 0x542eb0 / 0x542fb0 -> 0x546980 / 0x546c10: goal point 0 (0x542f16 / 0x54300f), traversals 1
(the 1 pushed first at 0x503c26); the flee search 0x503ca0: goal point 0 and traversals = its fifth argument (1 from 0x51fa85),
in both the plain (0x5429e0 -> 0x542a60 -> 0x545f30) and the plane variant (0x542b70 -> 0x542bf0 -> 0x5461a0); the second flee
attempt 0x51faf9 passes 1; the truncated-path replan 0x542410 (from 0x544ca0 at 0x544d13) and 0x5037e0 keep the old path's flag
0x10. Ported in `buildActorPath(…, useGoalPos, allowTraversal)` and its callers (the capped planner path no longer ends with the
target point). Flee fallback: the plain search keeps the farthest node seen (result block +0x14 = -1.0, +0x18 node, 0x542abd);
when no node is far enough, 0x542a60 builds the path to that node unless it is the start node (0x542ae7: no path); the plane
variant has no fallback (0x542bf0 returns the search's 0). Ported in `World::pathAwayFrom`.

**Danger planes** (observed; Ghidra decompile and objdump call sites): actor
+0x2050 holds four 0x18-byte slots {time, source, nx, ny, dist, short-lived}. 0x54b3b0 (slot choice 0x54b370: the
source's own slot, else the oldest) adds the half plane through the line a–b of enemy fire: n = (a.y − b.y, b.x − a.x),
c = n·a, flipped so the actor's origin stays on the kept side (n·origin ≤ c); only a source of the enemy team adds one
(entity +0xfc is the player's client pointer — the team gate is skipped for player fire, which marks the slot
short-lived and drops the actor's path; confirmed by 0x53145c's G_Damage client-or-actor pair). 0x54b4d0 (each actor
think) expires a short-lived slot after 300 ms, the rest after `suppressionwait`
(+0x20b0); 0x54b5a0 (per-sentient release 0x4fe170) drops the slots of a released source; 0x54b610 reports a free or
not short-lived slot (the grenade flee gate 0x51faa2); 0x54b670 collects the active planes. Planning with planes
(0x503650 / 0x503890 third argument, 0x503a50): the start node comes from the uncached 0x5150d0, the goal query keeps
the planes, 0x5428a0 requires both end nodes on the kept side (0x542710: n·p > dist fails) and searches 0x545ca0.
Implemented in `src/game/actorpath.cpp` (`addDangerPlane`, `expireDangerPlanes`, `removeDangerPlanesFrom`,
`dangerPlanesOf`, `hasFreeDangerPlane`, `sentientNodePlanes`; `pathToPoint` / `pathToNodeDirect` pick the plane
variant; the grenade take-cover path passes planes, 0x51fa49). The producers are live — the AI events below (type
16). The flee fallback (the second plain search 0x5429e0 gated by 0x54b610, 0x51faa0…0x51fb05, and 0x542a60's farthest-node
path) is ported ("Path planner" above).

## AI events (observed; call sites in the spec below)

0x535770 broadcasts a point event of `type` at `pos` to every live actor within the type's radius² (table 0x57031c,
stride 8: 3 → 256², 4/6/13 → 1024², 10 → 2048², 11 → 4096², 16 → 128², 20 → 256², else 512²); 0x535860 the line
variant along a–b (closest point, behind `a` does not count) with its handler 0x535f20; the per-actor dispatch is
0x535d70. Unless the broadcast is forced (producers pass 1 or 1 | 1 << team), the source must be a sentient of a team
with an enemy (axis ↔ allies). Handlers: 2/3 pain/death — the actors around learn the source if stale (> 2 s; on a
turret only if < 3 s); 4 weapon fired (0x550a70 → 0x535ba0) — the player gives a plain position update, an actor
passes on its own knowledge of its enemy (copied per-sentient when the target's is staler); 5 pain 500 ms in
(0x54d340) — the attacker (sentient +0x28, set by G_Damage 0x5284dd) is revealed and then forgotten; 6 death
(0x548a10) — the killer is learned; 10 bullet fired (0x531443, 2048²); the suppression line 0x531489 type 16
(0x535860 → 0x535f20) marks the attacker (+0x1780 = now), teaches the source's position and adds a danger plane
(0x54b3b0). Types 7/12/13 print "not yet handled" in the exe, 11 does nothing, 8/9 (grenade ping) go through
`grenadePing` instead of this dispatch. Knowledge is kept per sentient (0x500c50 learns position + own position then,
0x54ddc0 "known within ms"). Implemented in `src/game/actorevents.cpp`; producers wired in `combat.cpp` (G_Damage,
Bullet_Fire_Extended, the AI fire builtin), `actorstate.cpp` (pain reveal, death).
The +0x48 accumulator is not a pain accumulator: it is the movement-noise accumulator 0x5485a0 (`actorMoveNoise`). TODO(spec): the turret entity +0x10c field of the pain/death handler. The player's events 2/3 are the footstep and
landing events of G_ClientEvents 0x4e75f0 (`World::playerPmEvents`).

## Bad places (observed 0x524060 0x5199f0)

32 slots at 0x1c90e00 (0x28 bytes: expire, last event, name, type, team mask, origin, radius, half height, left,
right). badplace_cylinder / badplace_arc / badplace_delete (0x4f5880 / 0x4f5990 / 0x4f5800); teams axis 2, allies 4,
neutral 8, default 0xe. Adding marks links into the volume per team (0x5199f0) and drops paths using them (0x503d60).
Every 250 ms (0x523a60) actors of its teams inside (0x425c90) set +0x2128 with probability badplaceawareness; exposed →
state 4 (start 0x524030 flee target 0x523f10, think 0x524060). Implemented in `src/game/badplace.cpp`.

## Path following (observed; objdump of 0x550f10…0x552350, 0x541780…0x542600, 0x543ae0…0x5454d0, 0x505230 and
carved Ghidra decompiles)

Path layout (actor +0x330, cleared by 0x5436a0: budget 32768, avoided sentient 0x7ff): 32 points of 0x1c bytes (pos,
unit 2D dir from the next point, 2D length, node), goal first; count +0x6b0, planned +0x6b2, detour points +0x6b4,
traversal +0x6b6, lookahead index +0x6b8, avoided sentient +0x6ba, goal +0x6bc, lookahead dir +0x6c8 (z = slope), far
dir +0x6d4, lookahead distance +0x6dc, budget +0x6e0, lookahead segment part +0x6e4, flags +0x6e8, plan time +0x6ec,
start length +0x6f4, start +0x6f8. 0x541da0 builds it (start = last point; the budget carries over via 0x541780;
then 0x541a70). Lookahead update 0x544ca0 (each frame from the move 0x505230): start behind → 0x544310 advances it
(dropping passed points), ahead → 0x544520 backs it up; a truncated path with count · 4 ≤ planned is replanned
(0x542410); then 0x5447d0 puts the lookahead where the area between actor and path reaches the budget, checked by
0x5440e0 (0x543d30 move check, or 0x543c30 past the avoided actor): blocked shrinks the budget (0x543b70: · 9/16 or
3/4), clear grows it (0x543be0); 0x544a10 the far dir. Move 0x505230: distance · max(dot(far, look)^0.333, 0.6), at
most the lookahead distance, along the lookahead dir. Stop soon 0x503ec0.

Follower 0x551960 (from 0x552350; CL = use the interval, 1 only from combat 0x5494c0; result 1 → "stop" script
0x1c7e118 with movemode stop, 2 → walk 0x53af20): waits on +0x720 (+50 ms with a followed actor +0x2148); move =
velocity (+0x230) or 190 · lookahead dir (0x550fd0; no dir → path cleared when traversal < 1); radii 0x550f60.
Sentients (0x551100): not enemy actors, not itself, not ones waiting on it; |dz| < 80, within max(interval²,
speed²), ahead of the move or the far dir; moving ones are yielded to by entity number or walked behind (0x551740:
running, or walking beyond the half radius). In the way (0x551490): within 30 of the move line up to the lookahead
(+37.5 circle), special cases for the avoided sentient, the 60-unit lookahead point and the far dir. No blocker:
0x551800 lets the avoided sentient go beyond 60 units. Enemy blocker < 37.5: stand 500 ms (dropping the path if it is
the enemy). Otherwise wait out the blocker's own wait (0x504720 chain, +0x728/+0x72c), detour on either side 37.5
off its centre (0x5451b0, corners 75 apart, stepping back to 37.5 first when closer; +0x724 = now + 1000), or stand
blocked (+0x728 = self, 500 ms); when a detour was already tried the path is dropped every 2 s. Implemented in
`src/game/actorfollow.cpp` and `src/game/actorpath.cpp`.
The bumped entity +0x318, the clip mask +0x28c (pushplayer), +0x288 (prone tilt), negotiation (+0x244, flag 0x40) and the
actor physics (slide move / ground probe / dispatch) are ported (sections below). TODO(spec): +0x250 move delta vs velocity,
0x504630 crowding, 0x53ade0.

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
| interval | 0x71c | float | | scriptstate / lastscriptstate / statechangereason | 0x2140 / 0x2142 / 0x2144 | string, ro; set only by trackscriptstate (0x5402d6: last = old state, state, reason), read by a debug print (0x5005d6), released by the animscript switch 0x53ac90; not modelled (trackscriptstate is a no-op, the shipped scripts only copy `self.scriptState` into an unused local) |
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

**Actor spawn drop** (0x4fc550; `World::dropActorToFloor`, test_spawndrop). 0x4fc640 tests g_spawnai (0x1c1487c), then
spawnflags bit 0 (SPAWNER, +0x124): a spawner goes to the spawner init 0x532740 (eType 0xa, +0xa2 = 1, contents 0) and is
not dropped here; any other actor entity is dropped by 0x4fc550 (0x4fc66c) before the actor slot is allocated (0x4fc8d0).
0x4fc550: origin z += 1 (0x591e20 = 1.0, written to the entity), then G_Trace 0x44bf00 of the box (-15,-15,0)..(15,15,30)
from that origin to (x, y, original z − 128) (0x591e3c = 128.0), pass entity 0x7ff, mask 0x2820011. Only allsolid (+0x23,
0x4fc5e9) is tested: then the result is 1 and the origin stays 1 higher. Otherwise the origin becomes start + (end −
start)·fraction on every axis, which is the end point when nothing is hit (there is no "floating" case). dospawn
(0x532312..0x53235c) spawns a fresh entity, copies the spawner's key/values, clears spawnflags bit 0 (0x53234e) and calls
0x4fc640, so every spawned actor is dropped again. At level start 0x5325f0 (G_InitGame 0x4d9c43, right after the node drop)
drops every in-use eType 0xa entity (the spawners) with 0x4fc550, with the node drop's DYNAMICPATH rule (contents of
entities with flags 0x100 saved, zeroed and restored, 0x532620..0x532660 / 0x532705..0x53272e); an allsolid spawner prints
"^3Spawner at (%g %g %g) is in solid" (0x58f7ec, its origin before the drop) and gets bit 0 of +0xa2 cleared (0x5326d9;
TODO(spec): the reader of that bit, not modelled). The actor physics does not lower a stationary actor: the move block
starts with ground entity 0x7fe (0x500d27) and 0x5487c0 returns at once while the ground entity is 0x7fe and the move delta
is zero (0x5487c3..0x548823), so before this drop was modelled actors without a delta kept their spawner height (cityhall
spawner (4386 −4304 64) over terrain at 35.4). Map-start placed actors are dropped by the exe inside the entity spawn (0x4fa550 → G_CallSpawn 0x4f9770 → 0x4fc640) in
entity-string order, with only the entities before them linked; of those only script_brushmodels are in the mask (script_model
0x2080, actors 0x4000, the player and vehicle collmaps later). The port spawns them at the end of `prepareStart` and drops them
against the script_brushmodels with a lower entity number. On the 39 maps (127 placed actors) this gives the same heights as
dropping against all clips.

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

OpenIW2 moves actors along the planner's path ("Path planner") at the root-motion speed of their animations.

## Friendly chain (observed; src/game/friendlychain.cpp, friendlychainworld.cpp)

Nodes (0x78 bytes: +0 type, +4 spawnflags, +6 targetname, +0xc target, +0x14 origin, +0x34 chain id, +0x36 position, +0x38 predecessor,
+0x44 hold time) are chained at load by 0x52d630. A node whose `target` equals another node's `targetname` is that node's
predecessor. Fatal errors: "multiple nodes target node with name '%s'", "node with name '%s' targets itself", "found chain loop including node
with name '%s'". Heads (a predecessor of something, with none of their own), visited in array order, get ids 1, 2, ... and position 1; each
successor gets position + 1 (0x52d4c0). A head or successor with spawnflags bit 2 is excluded (id 0). The nodes are sorted (qsort, 0x52d580)
by chain id descending, position ascending, predecessor ascending, type descending; the nodes before the first id 0 are the chain nodes
(0x1c8da08). A chain node of type 0 is not fatal: each prints "Error: bad node [%d] in friendly chain at (%g %g %g)" (0x52d8e9, 0x42c7f0
channel 1); then 0x52d5d0 keeps the first path error "\nBad node in friendly chain" (0x1cb2bac; G_RunFrame 0x4da9b9 prints it
every 100 frames while the dvar at 0x1c1487c is set) and clears every node's chain fields (+0x34 / +0x36 = 0, +0x38 = -1); the
returned chain node count (0x1c8da08) is kept. The chain build runs from the client begin (0x4e8f40 → 0x516bc0), after the
node drop below.

`setfriendlychain(node)` (0x4eeb10): the entity must be a player, one argument, and the node's chain id must be >= 1 ("Node %d (%.2f, %.2f,
%.2f) is not a friendly chain node."); it is stored as the sentient's chain node (+0x30). `trigger_friendlychain` (type 5, spawn 0x529170,
contents 0x405c0008, spawnflags |= 1, needs `target`) does the same on touch (0x529140 -> 0x518bf0) for the chain node named by its `target`,
unless the sentient already is on a node of that name; errors "Node '%s' is not part of a friendly chain" and "Friendly chain node '%s' does
not exist". It is gated by g_spawnai (default 1, not modelled).

An actor with a goal entity (`setgoalentity`, +0x77c) that is a sentient runs 0x505ed0 from the path update (0x503490, before the fighting test):
for an enemy-team goal entity it records the enemy (0x500c50) and clears its chain node (+0x78c); with followmin > followmax it clears it;
otherwise 0x506250: the followed sentient's chain node moves to the nearest node of its chain (0x518270) once per frame (sentient +0x34); no
chain node there is fatal ("Sentient %s at (...) is following the sentient %s at (...) that is not on a friendly chain"; the port logs it and
skips the update). The actor keeps its chain node while on the leader's chain, within [followmin, followmax] positions of the leader and within
64 units of the node; otherwise 0x518a70 picks one: window lo/hi = leader position + followmin/followmax, scanning descendants of the current
node (0x518580, 0x5186f0), the ancestors of the leader (0x5187e0), then the nearest usable node outside the window (0x518860; 0x518960 for nodes
whose hold time has not passed). A node is a candidate when usable by the actor (0x518f20) and no other actor of a non-enemy team has it as
chain node (0x518510); among equal positions one is sampled with the exe's own LCG (0x5184c0; the stream is not reproduced). A chain node
without a claimed node is claimed (0x5152f0).

Working goal (0x505f70, run after the fighting test of the path update; actor +0x748 pos, +0x754 radius, +0x758 height, +0x760 mode,
`Entity::workGoal`): pathing to the enemy (+0x788): the enemy's last known position, mode 3, radius max(pathenemyfightdist, 4), height 512 when
pathenemyfightdist > 80 else 80. Else with a goal entity (+0x77c): a sentient on a friendly team (not an enemy, followmin <= followmax) gives
mode 2: the actor's own origin without a chain node (radius and height untouched); with one the chain node's origin, and a non-zero node
radius sets radius = max(node radius, 4) and height = max(goalheight, 80) (a zero one leaves them as the last update set them). Any other goal
entity gives its origin (mode 1); no goal entity the setgoalpos / setgoalnode position (mode 0, `setgoalpos` and `setgoalnode` clear the goal
entity); modes 0 and 1 take radius max(goalradius, 4) and height max(goalheight, 80). The working volume (+0x75c) is the script
volume (+0x778) in mode 0 and none otherwise; the in-goal test, isingoal and the cover filter read it. Spawn sets the working goal
to (origin, 4, 80) (0x4fc80d..0x4fc837). Without a sentient goal entity 0x505ed0 clears the chain node (+0x78c). The cover search, `pathToGoal` and the in-goal test read it
(`World::actorGoal`; the script fields stand in until the first update). `isingoal` (0x540c00 -> 0x5026d0) tests a point against it: |dz| <= height and 2D distance <= radius (plus the goal volume, 0x447830 = World::triggerContact on the point).

The turret think (0x54aba0) is the second caller (0x54ad05), unless the claimed node is kept. Not done: the toucher filter of 0x529140, the g_spawnai dvar.

## Unimplemented actor built-ins: decode status (matmata prediction family, gethitenttype; checkprone done)

Sweep with `iw2game <main> <map> 40 --spawn-all` over all 28 SP maps: the only built-in still reported as unimplemented is
`checkprone` (x4 libya, x36 matmata, up to x99 elsewhere). The matmata prediction calls are not reported any more, but there
is no implementation of them in src (the matmata paths that call them are not reached in the sweep time). All are actor
methods (entity from the self object, the "entity" check at 0x468ed0 error path).

- `gethitenttype` 0x5400c0 (0 args, returns string): reads the actor's bumped entity number at gentity +0x318
  (init 0x7ff = none at 0x500d1d; written by the actor move code at 0x501330 and 0x501c6f). 0x7ff gives "none"; else if the
  hit entity's contents word (+0x128) has bit 0x400 set it gives "obstacle"; otherwise "world". Implemented (world.cpp), with
  +0x318 written from the slide move's hit entity (`linkBumpedActor`).
- `checkprone`: decoded and ported, see "Prone" below.
- `beginprediction` 0x53f960 / `endprediction` 0x53fb80: snapshot / restore the actor state (the +0x218.. anim-mode words,
  the 0x43-dword AI struct, velocity/origin/angles) plus an animation-state snapshot (0x4796c0 / 0x478db0 / 0x478e10); nested
  through counter 0x1c6104c and a per-depth table at 0x1c7e52c. `predictoriginandangles` 0x53ff50 and `predictanim` 0x540020
  (tail-jumps to 0x5045c0) run an animation step inside such a snapshot and return the resulting origin/angles. They are implemented
  (src/game/actorpredict.cpp: a copy of ActorAi plus `AnimState::snapshot()` / `updateTruncated`). `gethityaw`,
  `isdeflected` and `lerpposition` are implemented; full decode in docs/spec/builtins.md "Prediction built-ins".

### Bumped entity +0x318: writer analysis (historical; now wired, see the slide move sections)

+0x318 is written only inside the actor move-obstruction step that spans 0x500dc0..0x501359 (callers 0x504228 / 0x505255
read it in the move code and re-call 0x500d80; init at 0x500d1d). That step copies the actor origin into the +0x224 vector, runs
the forward probe 0x5487c0 (returns 0 = clear: the actor's contents bits 0x18 at +0x128 are cleared and the function returns 0;
else the entity number found is in the local at esp+0x14), checks 0x5442a0 on the +0x330 block, and stores the found entity
number to +0x318 at 0x501330 (or 0x7ff at 0x501346 when nothing remains, and at 0x501c6f in the second writer). The producer of
the entity number is the probe 0x5487c0 plus the +0x6b0 / 0x5037e0 per-node state, neither of which is modelled in
`World::moveActor` (which does not run the actor physics 0x501xxx). Wiring +0x318 faithfully therefore needs 0x5487c0 and
0x5037e0 decoded first; guessing the entity from the repo's own clipping would not reproduce the exe, so gethitenttype,
the actorfollow +0x2148 link and the 0x500d80 lookahead shrink stay TODO(spec).

### Libya start view: pale flat surface

Not a death or mission-fail view and not a terrain bug. At sim 16..30 s the player is alive, looking out of the commander
hatch (hatch overlay) of the player tank, vehicle 13 `xmodel/vehicle_crusader2_viewmodel` at -53265,5736,-198 (turret
-4.4/-0.6). The pale plane with the straight far edge is that viewmodel's own hull/turret top seen from the hatch eye point
(the same pale camo seen from outside in a follow shot). It is unchanged with IW2_NO_SPEC, IW2_NO_DETAIL and IW2_NO_PROBE.
TODO(spec): the near-field look of that surface (texture detail) has not been compared with the original.

### Decode attempt: 0x5487c0, 0x5037e0, 0x5442a0 (stalled; findings)

- 0x5487c0 (edx = the +0x224 actor move block "pm") is the actor physics entry, not a small probe: it stores the block pointer
  in 0x1cbacac, clears hit entity +0xf4 to 0x7ff, zeroes +0x70 / byte +0x109; with ground entity +0x18 == 0x7fe and a zero
  desired move (+0x2c..0x34 equal to 0.0 at 0x591e00) it clears the velocity (+0xc..0x14) and returns 1. Otherwise it zeroes a
  0x15-dword scratch at 0x1cbac58, stores frame dt·scale (+0x48 as int · 0x591e30) in 0x1cbac58, copies the position / velocity,
  and dispatches on the mode word +0x40: 3 = teleport add of the move delta (+0x2c..) to the position and return 1; 4 =
  0x548260 (velocity = delta / dt, then 0x5475e0); 5 = 0x548200 (same, vz = 0, then 0x547db0 slide move); others 0x548290
  (G_LocationalTrace 0x44bf00 based ground probe), 0x548110, 0x5480d0, followed by the ground / blocker handling (0x5485a0,
  0x548650, 0x5191a0, 0x514970, 0x4fd9e0, 0x535770, 0x520470, 0x514d50, 0x541780, 0x548a10, 0x53ac90, 0x504450, 0x4fe740,
  0x502660, 0x4fe670). It returns the move result and the per-move hit entity in the scratch (+0xf4 of the block).
- 0x5037e0 (arg: actor): when the path point count +0x6b0 > 0, resets the +0x330 path block via 0x541780, zeroes +0x380/+0x382
  (path index words), sets +0x6e8 |= 0x80 and clears +0x724, clears byte 0x3c of the actor goal block (+0x4), asks 0x515040
  for a goal and, when non-zero, rebuilds the path with 0x5424d0(+0x330, goal block, goal, actor origin (+0xdc), flag
  (+0x6e8 >> 4) & 1) and sets +0x6e8 |= 0x80 again. This is the path re-plan, not an entity probe.
- 0x5442a0 (edx = path block +0x330): returns 0 (path ok) when the index at +0x380 minus 1 is not past +0x386, the point
  at index-1 has non-negative fields, flag 0x20 of +0x3b8 is clear, 0x543bc0 is zero and the index words +0x380 / +0x382 differ
  or 0x415c50(+0x3c8) is false; otherwise 1 (re-plan needed).
- Conclusion: the +0x318 entity comes from the actor slide move result (0x547db0 / 0x44bf00 hit entity stored at block +0xf4 and
  copied by the 0x500dc0 step), i.e. from the actor physics that World::moveActor does not model (it moves along the path
  lookahead without clipping). Wiring +0x318, gethitenttype and the +0x2148 follow link therefore needs the full actor
  physics port (0x547db0 slide move, 0x547540 clip velocity, 0x548290 ground probe) first. (Superseded: that port is done, see
  the next sections; +0x318 is written from the slide move's hit entity by `linkBumpedActor` and read by `gethitenttype`.)

### Actor slide move port (0x5475e0, 0x547540)

`src/game/actorslide.{h,cpp}` ports the actor PM_SlideMove (0x5475e0) and its clip velocity (0x547540) from the asm, with the trace
(G_LocationalTrace 0x44bf00: start = block origin, mins +0x4c, maxs +0x58, pass entity +0x3c, mask 0x1cbaca4) supplied by the
caller. Matches Q3's algorithm with these exe specifics: gravity branch half-steps vz and clips on the ground plane (1.001);
at most 4 bumps and 5 planes (5 planes: velocity zeroed, return 1); duplicate plane (dot > 0.99) nudges the velocity by
the normal; interact threshold 0.1 (impact speed 0x1cbac88 keeps max(-into)); crease slide uses cross(j, i) for both
directions; third plane test zeroes the velocity. Hit bookkeeping on the block: the first wall hit (normal.z < 0.7 and
(nx != 0 or ny != 0)) stores the entity number (+0xf4), origin xy (+0xf8) and normal xy (+0x100); a frac 0 duplicate plane
hit and the final clipped move (normal = clipVel - vel) record it the same way; all-solid start records it with byte +0x108 = 1
and returns 1 (2 when bump == 0 and byte 0x1cbac87). Touched entity numbers (not world 0x7fe) go in a list of 0x20 at +0x74
(count +0x70). ClipVelocity: normal.z >= 0.7 and vz² <= vx²+vy² gives (vx·nz, vy·nz, -(vx·nx + vy·ny)); otherwise the usual
backoff (divide by the overbounce when moving away, multiply into the plane). Normalize (0x421270) leaves the destination
untouched for a zero vector.
Tested in tests/test_actorslide.cpp. Wired into World::moveActor / moveActorKind through `moveBlock` (0x5487c0), on by
default (`IW2_ACTOR_SLIDE=0` restores the old ground snap); the step wrapper 0x547db0, the ground probe 0x548290 and the mode
dispatch are ported (sections below); the +0x318 copy in the 0x500dc0 step reads the block's +0xf4 after 0x5487c0.

0x547db0 (the actor StepSlideMove, arg = gravity flag) structure, not yet ported: saves the block (0x43 dwords) and the
0x15-dword frame scratch at 0x1cbac58, runs 0x5475e0; result 0 returns 0 (the +0xf4 of the scratch...), 2 returns 0 (all-solid exit);
otherwise (step height 0x1cbaca8 = 10, or 18 when the byte +0x64 of the block is 0) it traces straight down with 0x4db7f0 from the
post-move origin and, when that trace is not a full-fraction (1.0) clear or the normal.z >= 0.7, it skips the step-up; otherwise it
restores the saved block, clears the ground plane flag (0x1cbac60), re-runs the slide move from origin + step height (lerp
with the 0.0 constant 0x591e00 so the origin is kept), traces down by the step height again (0x4db7f0), snaps to the ground via
0x47f390 and accepts the stepped move when the down trace fraction passes 0x591e78 and normal.z passes 0x592018 (then
0x547540 clips the velocity and 0x408ad0 (two args) compared against origin z + 0x59202c decides to store the hit entity in
+0xf4). Helpers and constants: decoded right below.

Helpers of 0x547db0, decoded:
- 0x4db7f0: a thin wrapper of G_LocationalTrace 0x44bf00 (pushes 0, 0, 0 for the three trailing arguments, no other work).
  Argument order at the call sites: result, start, mins, maxs, end, pass entity, content mask.
- 0x47f390 (eax = a, edx = b, ecx = out, stack t): out = a + (b - a) · t (vector lerp, per component in x87).
- 0x408ad0 (a, b): max(a, b) (returns a when a - b >= 0, else b).
- Constants: 0x591e78 = 0.0, 0x592018 = 0.3, 0x59202c = 0.1 (0x591e00 = 0.0, 0x591e20 = 1.0).
In the step wrapper: the up-step test compares the down-trace normal.z against 0.3 (0x592018) and the fraction against 0.0
(0x591e78), the final hit-entity rule is max(lerp-corrected z delta, ...) + 0.1 compared against the block origin z. The exact
stack offsets of the two scratch trace results inside 0x547db0 (0x44 / 0x60 / 0x82 relative to esp after the pushes) have not been
mapped to trace_t fields in this pass; the mapping and the port follow in the next section.

### Actor step slide port (0x547db0), stack offsets mapped

Both scratch trace results sit at esp0 + 0x44 (fraction +0x0, normal +0x4, entity word +0x1c, startsolid +0x22, allsolid +0x23, the
same trace_t as in 0x5475e0); saved origin O0 at +0x10, saved velocity V0 at +0x34. Flow (src/game/actorslide.cpp, stepSlideMove):
slide move; result 0 returns 1, result 2 returns 0. When the post-slide vz > 0 and the ground-entity flag 0x1cbac5c is clear, a
down trace from O0 by the step height (0x1cbaca8) with fraction 1.0 or normal.z < 0.7 returns 1. Then an up trace from O0 to
O0 + step: startsolid returns 0 when fraction 0 else 1. Save the block and scratch (the whole ActorSlide), hit entity := 0x7ff,
ground plane flag := 0, origin = (O0.x, O0.y, lerp z at the up fraction), velocity := V0, slide move again (2 restores the
saved state, returns 1). Then trace down from the new origin by (O0.z - up z) (0x4db7f0 from the block origin), snap the origin
to the end point unless allsolid, return 1 when fraction >= 1.0; normal.z >= 0.3 clips the velocity (1.001) and clears the hit
entity when max(O0.z, saved z) + 0.1 < origin z; otherwise the saved state is restored. Helper identities are in the previous
section (0x4db7f0 trace wrapper, 0x47f390 lerp, 0x408ad0 max; 0x591e78 is the double 1.0). Tested with a step of height 10.

### Actor ground probe (0x548290), ported

`groundProbe` in `src/game/actorslide.cpp`: trace from origin+0.25 to origin-D (D = step height when vz<=0 and a ground entity exists, else 0.25).
All-solid: touch-add the entity and retry without content bit 0x2000000 if set, then retrace from the origin; still all-solid -> ground entity = hit entity, groundPlane=0.
Fraction 1 -> no ground. vz>0 and dot(normal, vel)>10 (0x591e8c) -> no ground. Normal z<0.7 -> groundPlane=1, no ground entity, hit record if none yet (normal.x stored in both slots, as at 0x548521/0x54852c).
Walkable: z snaps to the trace end only if lower than the origin or mode==5; ground entity = trace entity, touch-add. The mode dispatch (0x548260/0x548200/0x548110/0x5480d0) is `moveBlock`, next section.

### Actor move dispatch (0x5487c0), ported as moveBlock

Block fields: +0x18 ground entity, +0x20 contact flag, +0x24 ground normal.z, +0x28 surface type ((trace +0x10 >> 20) & 0x1f), +0x2c desired displacement, +0x40 mode, +0x48 time ms (dt = ms * 0.001), +0x64 byte (step height 10 if set, else 18), +0x68 trace mask.
Early out: ground entity 0x7fe and zero displacement -> velocity zeroed. Mode 3 adds the displacement; mode 4 (0x548260) sets velocity = displacement/dt and slide-moves without gravity; otherwise the ground probe runs and then mode 5 (0x548200), ground-entity walking (0x548110) or airborne (0x5480d0), see the header comment of moveBlock.
After the move, the contact flag, normal.z and surface type are set when a ground plane exists with normal.z at least 0.3 (0x592018).
0x5485a0 is not a sound: it is the movement noise (block +0x1c accumulates the step +0x48 to 500 ms, then AI event 2 through
0x535770), ported as `World::actorMoveNoise`. 0x548650 is the actor foliage sound (`World::actorFoliageSound`): the player's
PM_FoliageSounds rule with the same bg_foliagesnd_* dvars (0x1c7efb4 minspeed 40, 0x1c7f03c maxspeed 180, 0x1c7f000
slowinterval 1500, 0x1c7efd0 fastinterval 500, 0x1c7eff4 resetinterval 500; registered at 0x4fa6f3..0x4fa7de), block +0x6c the last
event time, box 0.75 x the actor box (maxs z 0.9 x), contents mask 2, start solid -> event 0x8b on the actor ("movement_foliage").
Both run from the think (0x4ff1b3 / 0x4ff1b8) only when the origin differs from the one stored at the previous think (sentient
+0x10, compared at 0x4ff0b5..0x4ff0f3; the noise additionally needs +0x25c alive). An actor that stands still makes neither, and
its noise sum is kept. Before this gate the port added noise every frame for standing actors (beltot 30 s --spawn-all: 1514 noise
events before, 296 after).

### Actor trace box, followed actor (+0x2148) and 0x500d80: decode status

- Trace box: the actor r.mins/r.maxs (gentity +0xa8/+0xb4) are written once at spawn (0x4fc709..0x4fc731: -15,-15,0 .. 15,15,72) and by no other code in the actor range 0x4f0000..0x560000 (the only other writer, 0x4f8318, is the same box 72 high for makefakeai). The slide move wiring now uses the 72-high box (the old ground snap used 70).
- Writer of +0x2148 from the bump: 0x501c6f..0x501c8b (second move step, after the call 0x5487c0 at 0x501bda and the scan 0x501380 that returns early when another actor is close ahead): +0x318 = the hit gentity number; when that gentity has an actor struct (gentity +0x100) whose +0x2148 is still 0, the BUMPED actor's +0x2148 is set to the bumping actor's gentity (the bumped one follows the one that pushed it). The first writer (0x501330) does not touch +0x2148.
- Other writers of +0x2148: 0x504714 (0x504630: when unset, scan the 32 actors for one within range via 0x5025f0), 0x501480 (the 0x501380 scan), 0x4fee3f/0x4fef0d/0x4fedc0 region; clears/consumers at 0x4fe6ca, 0x4fe8eb, 0x50081d, 0x5013b0, 0x504630. Setting the repo's `ActorAi::followed` from the bump alone would freeze the actor (stepActorPath returns early while followed) because the lifecycle (0x504630 scan, the reset sites, 0x53ade0 consumer) is not ported. TODO(spec): decode that lifecycle, then add the 0x501c8b link.
- 0x500d80 (arg: bumped entity number, this = actor): if the bumped gentity has +0x104 set the result is (level time < actor +0x724); else 0 when its contents (+0x128) have bit 0x400; else (actor +0x6ec > gentity +0x1ec). It is called only from 0x504228 in the branch of 0x504160 that does NOT run the path walk (0x505230, which World::moveActor models), as the third argument of 0x544ca0 before 0x505520. +0x6ec and +0x1ec have no writer in the actor range, so the last branch is undecoded. TODO(spec); World::moveActor's own lookahead call is not affected.

## Follow lifecycle (+0x2148): decoded part (0x4fedc0, link at 0x501c6f)

Ported (src/game/actorfollow.cpp `updateFollowed`, `linkBumpedActor`):
- 0x4fedc0, called at the start of each think (0x4fef20): no-op without a followed gentity; the followed entity is dropped when removed. With d2 the squared XY distance: d2 >= 1406.25 (0x591fd0) clears this actor's link and the followed actor's back-link; closer, the link is cleared when the followed actor's blocker (+0x72c) is this actor. If still set and d2 >= 900 (0x591fd4), a moving actor (anim mode 1, path count > 0, not blocked by the followed one) clears it when (me - followed) . lookDir > 0.
- 0x501c6f..0x501c8b: this is NOT a link from the hit entity. It is the last fallback of the yield step 0x501620 (below): +0x318 = the followed entity and an actor followed that follows nobody gets +0x2148 = this actor.

TODO(spec): the retarget to the player (0x4fec50, 0x447830, 0x504610, 0x519050), the scans 0x501380 and 0x504630 (0x5025f0/0x502660), the follower trail (0x500d80 + 0x544ca0 + 0x505520/0x504f60, path walk skipped at 0x5041a3), the 0x504160 branch not running 0x505230 (0x504228), and the consumers 0x4fe6ca, 0x4fe8eb, 0x50081d, 0x5013b0, 0x53ade0, 0x501e06 are not decoded/ported.

Status: stepActorPath returns early for a followed actor (as the exe skips the path walk, 0x5041a3); the follower is moved by the yield step 0x501620 from the physics step 0x501d00 (next section), so it does not freeze.

### Followed-actor branch of 0x504160 (decoded and ported)

0x50418b (anim mode 1): the path walk runs only when movemode (+0x214) is set, the path count (+0x6b0) > 0 and +0x2148 is 0. Otherwise, with path count > 0, 0x504228 runs: 0x544ca0 (the lookahead update, `World::updatePathLook`) with shrink = 0x500d80(+0x318) and correct = 0, then the trail 0x505520; the mode word +0x264 = 1 and the desired delta +0x250..0x258 = 0. The path walk (0x505230) and its stop-soon check 0x503ec0 do not run. So a followed actor does not walk its path; the physics step 0x501d00 runs the yield step 0x501620 for it (anim mode 1 or 7), which moves it out of the followed entity's way. `World::moveActor` models this (no step, no angles write, no stop soon); `followActorPath` calls it for a followed actor.
0x500d80 is `World::followShrink`: a bumped gentity with a sentient (actor or player) -> level time < +0x724; otherwise contents 0x400 gives 0, else signed (+0x6ec > gentity +0x1ec). TODO(spec): +0x6ec has no writer and +0x1ec is zero for non-actors, so the last branch is taken as false.
0x505520 (trail): every think, at (level time / 100) % 10 != 0 the lookahead direction * 0.1 (0x59202c) is added to ring[+0x214c] (8 bytes per entry at +0x2150), at the tenth boundary 0x504f60 advances the ring (index 0 wraps to 10, a weighted smoothing of the other entries by 0x591e20-stepped divisors) and the entry is set to lookDir * 0.1. Decoded and ported (`src/game/actortrail.h`, test_actortrail): the ring is 10 (x, y) float entries at +0x2150, index +0x214c. 0x505520: time/100 not a multiple of 10 -> entry[idx] += look * 0.1; else advance (0x504f60) and entry[idx] = look * 0.1. 0x504f60: with prev = idx-1 and pp = prev-1 (mod 10), when cross(pp,prev) * cross(prev,cur) <= 0 and prev.cur >= 0 and prev.pp >= 0, entry[pp] = entry[cur]; then entries idx+1..9, 0..idx-1 are multiplied by k/(k+1), k = 1, 2, ... in that order; idx = (idx+1) % 10. 0x504e70 sums the entries into a vector (x, y, 0); 0x504de0 zeroes the ring and the index (at spawn 0x4fc806 and in the move script 0x5523b6). The ring is written only on the followed branch (0x50424a); its only reader is the facing 0x505630 of the path walker (calls 0x505c3a, 0x505d5e, 0x505dad; with a zero sum or the path walk it turns by 0x5055d0 / 0x504950 / 0x5048b0), so it has no effect here: TODO(spec): the facing modes / turn (0x504aa0, 0x505630) are not modelled (angles = lookDir yaw), and the 0x5523b6 clear is not wired.
Open: the consumers 0x4fe6ca, 0x4fe8eb, 0x50081d, 0x501e06, the ring advance 0x504f60 and facing blend, 0x519050 / +0x86b6.

### Yield step 0x501620 and clear scan 0x501380 (decoded, ported: `yieldFollowed`, `followClear`)
- 0x501d00 (actor physics): with +0x2148 set and anim mode 1 or 7 it calls 0x501620; nonzero ends the step, zero continues to the path mover 0x501e48 (path count > 0) or 0x501f59. So the follower moves, with or without a path.
- 0x501620 moves the actor block (0x5487c0) up to three times, each from the actor's origin with the entry velocity/ground/contact. 1: away from the followed entity (player: perpendicular to its horizontal velocity, or its view direction when still, on the actor's side, 0x5014b0, speed max(60, speed) * 0.05; actor: directly away by the animation frame distance, at least 1.5 (0x592334), a random direction from the game LCG when co-located). Accepted when it ended no closer than it started, moved over half the intended distance and 0x501380 finds nobody in the way. 2/3: when the move hit something (ground entity not none) the delta is the perpendicular of the unit hit normal at 0.75 then 0.5625 of the distance; accepted above 0.375 / 0.281 of the distance. The side taken is remembered in the actor gentity flags (+0x128) bits 0x10 / 8; when the opposite bit is set the delta is added to 0.33 of the perpendicular instead of replacing it. Nothing worked: +0x318 = the followed entity, a followed actor that follows nobody follows this one, velocity zeroed, the actor stays. Returns 0 when 0x5487c0 failed (flags bits 0x18 cleared).
- 0x501380: every other actor (not the followed one) that the move brought within 30 units (2D) of the new position while at least 30 from the actor's old origin and within 80 vertically blocks the move; one that follows nobody and is not at its claimed node (0x504610) starts following this actor.
- CORRECTION (supersedes the earlier "natural bumps" reading): bits 8 / 0x10 are in the gentity FLAGS word at +0x128 (the same word as notarget 4 and the 0x400 obstacle bit of 0x500d80), not in the contents word at +0xc0. Every trace reads contents from +0xc0 only (area query 0x419ea0 `testl mask, 0xc0(ent)`, link 0x44b2d0, entity clip 0x44bb8c / 0x44c8dd); +0x128 is read back only by the yield step itself (0x501927 tests 8, 0x501978 tests 0x10 on the other entity). Actor contents is 0x4000 (0x4fc74f), which is in none of the actor / player clip masks, so a sidestepping actor is NOT solid to traces and `sideBits` no longer enters the entity clips. Actors and the player therefore do not block each other in traces; what makes the +0x318 / link bump fire in the exe is TODO(spec) (the actor touch record 0x500c50 is a candidate, undecoded).
- 0x504610: claimed node set and dz^2 <= 6400, 2D distance^2 <= 900.
- `IW2_FOLLOW_SCAN=0` disables the 0x504630 scan, `IW2_ACTOR_FOLLOW=0` the link writes (docs/debug-flags.md).

- groundtype (actor +0x24c = move block +0x28, written by the ground probe 0x548290 each move): getter 0x5367b0 returns undefined for 0, the table 0x5a0344 (stride 20, same names as the pmove surface aliases bark..asphalt) for 1..22, "default" otherwise. Implemented in World::moveActor (slide path only).

### Retarget 0x4fec50 (decoded, ported as `World::retargetFollowed`)
0x4fedc0 (every think) first releases as documented above, then, unless +0x2148 is already the player gentity (g_entities[0], 0x1c60504), calls 0x4fec50(actor, player): return unless dontavoidplayer (+0x734) == 0 and +0x28c has 0x2000000 and the state (stack[depth]) is not 2 (turret); the actor's team must not be the team the player's team fights (table {0,2,1,0,0} at 0x4fec9d..0x4fecd2); the box 0x447830 test: player origin + velocity (ps+0x20) * 0.1 (0x59202c) with the player's mins/maxs (ps+0x540 / +0x54c) against the actor's box (an AABB test for an actor model); then 0x504610 (claimed node set and 0x502660 close) and, if that holds, the client byte +0x86b6 must be 0, and a claimed node is handed to the player's sentient (0x519050); finally +0x2148 = the player gentity. +0x86b6 (decoded, 0x4e8180 per player frame: sentient +0x10 is the previous origin; the byte is set when the squared xy distance to the current origin is >= 0.01 (0x591ea0), then the previous origin is updated): standing at its claimed node (0x504610) an actor is skipped (not linked) while the player moves; otherwise 0x519050 (release the player's sentient's old node 0x519200, take the actor's node over: the claiming owner is evicted by 0x515330 - its path ending at the node dropped, the node blocked for its team 0x519300 - then claimed 0x518fa0) runs before +0x2148 = the player; modelled as `claimNode(player, node, takeOver)`. TODO(spec): the eviction of the owners of the linked nodes in 0x519050; the player's stance box (standing box used).
Scene check: decoytown (player standing on a friendly actor's box edge): the link is re-established every think while the boxes overlap, as in the exe order (release check, then retarget); the actor stands until the player leaves.
`IW2_FOLLOW_SCENE` forces a link to exercise the yield step.

### Scan 0x504630 and the followed-stand script 0x53ade0 (decoded, ported: `scanFollowed`, `standOrFollow`)
- 0x504630(actor): only when +0x2148 is 0. flag = 1 when the animscript is grenade_cower (handle 0x1c7e120, string 0x587e50); else 0 without a claimed node, else (node close, 0x502660). Over the 32 actor slots (in use, not itself): with the flag set the other's +0x318 hit entity must have a sentient (+0x104); then 0x5025f0(me, other) (dz^2 <= 6400 at 0x591fdc, 2D distance^2 < 900) -> +0x2148 = the other's gentity (first match).
- 0x53ade0(actor, script): horizontal speed^2 (+0x230/+0x234) > 1.0 (0x591e20) plays handle 0x1c7e108 ("move", string 0x587e74) at movemode 2 / anim mode 1; otherwise `script` at movemode 1 / anim mode 1. Callers: 0x53ae40 (stop), 0x5523d2 (path step result "stand": "stop" 0x1c7e118), 0x5206c0 (grenade_cower), 0x53b040 (the run/walk scripts 0x53afe0 / 0x53af20 when followed), each after 0x504630 and with the unfollowed fallback (script, movemode 0, anim mode 1).
- 0x53b050 (combat script) runs 0x504630 first, clears +0x728/+0x72c, then the cover/combat choice; followed -> ("combat", 0, anim mode 1) else (0, 4).
Wired in: exposedNoncombat / badplace flee (stop), followActorPath result 1 (stop), grenadeCower, combatScript. TODO(spec): the walk/run scripts 0x53af20/0x53afe0 for a followed actor are reached only through the path step result 1 here.
Open: the second scan 0x501380 (callers 0x5018a2, 0x501a6a, 0x501c38 in the actor move 0x501xxx), consumers 0x4fe6ca, 0x4fe8eb, 0x50081d, 0x501e06, the ring advance 0x504f60 and facing blend 0x505630/0x504e70/0x504de0.

## Prone

Source: CoD2SP_s.exe 1.3, disassembly of 0x4fb530..0x4fbff6 read instruction by instruction (Ghidra pseudo-code of a carved
image used as a guide; it drops the outputs of the register-argument helpers 0x421270, 0x47f390 and is not trusted alone).
Port: `src/game/prone.cpp` (fit test), `src/game/proneworld.cpp` (built-ins), test `tests/test_prone.cpp`.

### Fit test 0x4fb530

Arguments (cdecl, 14): a1 entity number (pass entity), a2 origin, a3 radius, a4 height, a5 yaw, a6/a7/a8 optional outputs,
a9 already prone, a10 strict, a11 optional ground normal, a12 index into the sweep table 0x5a9190 (12-byte entries, first
dword the sweep: 0 -> 0x4b9810, 1 -> 0x4db7f0 = G_Trace 0x44bf00 with (result, start, mins, maxs, end, pass, mask)),
a13 adds 0x10000 to the mask 0x810011, a14 body length. Constants (.rdata): 6 (0x56fcc8), 10 (0x591e8c), 0.7 (0x591f30),
180 (0x591e1c), 1 (0x591e20), 2 (0x591ed0), 48 (0x591f40), 22 (0x5921a0), 2.5 (0x591e44), -0.75 (0x59219c), 0.5
(0x591e10), 18 (0x591f44), -50 (0x592030), 70 (0x592198), 5 (0x591ef0). "Probe" below is the box -6..6 on all axes.

1. Unless already prone: the box (-r,-r,0)..(r,r,h) swept from the origin 10 units up must not end all-solid (trace byte
   +0x22), else false.
2. Strict with a ground normal whose z < 0.7: false.
3. Body direction F = AngleVectors(0, yaw - 180, 0) (forward of 0x421da0, which takes angles in edx and forward/right/up in
   esi/edi/ebx). Probe height z0 = h - 6, reach = length - 6. Probe sweep from origin + (0,0,z0) to that + F * reach.
   - Clear (fraction >= 1): body length D = length, not shortened.
   - Blocked: non-strict callers get false here. D = reach * fraction + 6; D < r + 2 gives false. If D < z0 * 0.7 + 48
     (always with checkprone's numbers: 77.4 > 66) the end is raised 22, F becomes the unit vector of (end - start)
     (0x421270 writes it through esi, which still points at F) and the sweep is repeated: clear gives D = length and
     "not shortened"; blocked gives D = |end - start| * fraction + 6, false when still below 77.4, else "shortened".
   - The foot point K = start + (end - start) * fraction of the last sweep.
4. Hip drop: from origin + F * 48 + (0,0,z0) straight down by r * 2.5 + z0 - 6. Fraction exactly 1 goes to the fallback
   (below); hit normal z < 0.7 gives false. Hip B = hit point - (0,0,6); hip drop distance hd = drop * fraction + 6.
5. Only when shortened: D - hd < hd * -0.75 goes to the fallback. Direction V = unit(F * 6 + (K - B) + (0,0,6)); with
   rest = reach - 48 the sweep goes from the hip probe start S2 to (xy: midpoint of origin + F * reach and S2 + V * rest,
   z: S2.z + V.z * rest). Blocked: start = hit point, start and end raised 18, swept again; blocked again goes to the
   fallback. K = end point of the last of these sweeps (0x47f390 with eax = start, edx = end, ecx = out).
6. Foot drop: from K down by (K.z - B.z) * 2 + r. Fraction exactly 1: fallback; normal z < 0.7: false. K = hit point -
   (0,0,6).
7. Bend: pitch(K - B) - pitch(B - origin), wrapped into -180..180 (0x423f50 = a - b, then +-360 until inside), where pitch
   (0x421990) is -atan2(z, |xy|) in degrees plus 360 when negative (vertical: 270 up, 90 down). Outside [-50, 70] fails.
   Point sweeps (zero box) from origin + (0,0,5) to B + (0,0,5) and from B + (0,0,5) to K + (0,0,5) must be clear.
8. Outputs: a6 = 0 (int); a7 = pitch(origin - B), a8 = pitch(B - K), both through 0x424040 (16-bit angle grid via _ftol2
   truncation, then -180..180). All checks passed: true. Fallback (and a failed step 7): strict gives false; non-strict
   zeroes the outputs and returns true.

`checkprone(origin, yaw, alreadyProne)` 0x5406f0: actor only ("not an actor"), reads vector / float / int, calls the test
with (entity, origin, 15, 48, yaw, -, -, -, alreadyProne, strict 1, no normal, table 1, +0x10000, 66) and returns the int.
Ported exactly; on toujane / moscow (`--spawn-all`, 60 s) 187 / 226 calls, 168 / 90 fit, no runtime errors. Note the
trace pass entity is the actor itself, and our G_Trace stands in for 0x44bf00 (capsule sweep, docs/spec/collision.md).

### Prone state and built-ins (ported)

Actor fields (offsets from the actor struct, `ActorProne` in src/game/world.h): +0x140/+0x142/+0x144 leg nodes down /
straight / up (0 = unset), +0x148 = 1 / down angle, +0x14c = 1 / up angle, +0x150 bend of the last leg-weight update,
+0x154 last fit result (script field `proneok`), +0x158 corpse-blend byte, +0x159 prone, +0x15c transition start (level
ms), +0x160 transition length in ms (negative while leaving, 0 once settled), +0x164 / +0x168 / +0x16c the fit outputs a6 /
a7 / a8, +0x288 a tilt byte (set on entry, cleared by the think when not prone; reader not found: TODO(spec)).

- Bookkeeping shared by 0x514970 / 0x514b30 / 0x514c40 / 0x53d430: length < 0 and start - length < now: prone = 0 (the
  leave finished); length > 0 and start + length < now: length = 0.
- Fraction 0x5149e0 (edx = block +0x158, ecx = now): 0 when not prone; 1 when length is 0; leaving: 1 - (now - start) /
  -length (0 once finished); entering: (now - start) / length (1 once finished).
- `setproneanimnodes(down, up, a, b, c)` 0x53d2f0: floats read before the actor check; down must be < 0, up > 0 (the two
  error strings at 0x5916ac / 0x591670); stores 1/down, 1/up and the three nodes (0x46e530).
- `enterprone(sec)` 0x53d1b0: "Must call SetProneAnimNodes before calling EnterProne" (0x5916e4) when both reciprocals
  are 0 or any node is 0; then 0x514b30 with _ftol2(sec * 1000). 0x514b30: when not prone, a corpse blend, or a leave
  that has finished: corpse byte 0, prone 1, tilt 1, start = now, length = ms, fit (already prone 0, strict, outputs into
  +0x164..+0x16c) into +0x154. Otherwise only when the length is non-zero and differs from ms: with f = fraction < 1 the
  start becomes now - _ftol2(ms * f); the length becomes ms.
- `exitprone(sec)` 0x53d280 -> 0x514c40(ms): only while prone and not a corpse blend; after the bookkeeping (a finished
  leave returns), a non-zero length different from ms sets start = now - _ftol2(ms * f), else start = now; length = -ms.
- `updateprone(a, b, weight, blend, rate)` 0x53d430: only while prone and not a corpse blend; after the bookkeeping (a
  finished leave clears prone and returns) both animations get setanim 0x4795e0 with (weight, blend, rate), then 0x5022b0
  with eax = 0.
- 0x5022b0 (edi = actor): eax != 0 runs the fit with already prone = 1 (into +0x154 and +0x164..+0x16c); eax = 0 sets
  +0x150 = 360 instead. Bend d = wrap(+0x16c - +0x168). When d differs from +0x150: d < 0 gives down = min(d * +0x148,
  0.99), d >= 0 gives up = min(d * +0x14c, 0.99) (0x592208 = 0.99), straight = 1 - that; each weight under 0.001
  (0x591e30) becomes 0 and goes to its node through 0x478f10 (goal, blend 0.05, rate 1) + 0x4794b0 (ancestors); +0x150 = d.
- Actor think 0x4ff030: after the state loop it compares the entity origin and angles with those of the previous think
  (moved / turned); at 0x4ff1de, when 0x514970 says prone (prone, no corpse blend, with the bookkeeping) and the actor
  moved or turned, 0x5022b0 runs with eax = 1; when not prone the tilt byte +0x288 is cleared. Ported as
  `World::actorProneFrame` after the facing and movement-noise steps.
- Client 0x4c1cc0 (per centity, prone block from the snapshot): when prone (0x514970 on the client copy) or the corpse
  byte is set, f = 0x5149e0; corpse byte: mode 2 (pitch f * a7, second rotation f * a8, lift f * a6); otherwise mode 1:
  pitch f * a7, lift f * a6. 0x4c1b90 applies it to the actor's `tag_origin` (scr const 0x1c7f144 = "tag_origin", set at
  0x506fed; error "actor (entnum %d) does not have a tag_origin") as a DObj control: rotation 0x423970 = quaternion (0,
  sin(a/2), 0, cos(a/2)) (a pitch about y, the same as `anglesQuat(a, 0, 0)`), translation (0, 0, lift). Mode 1 is drawn by
  iw2view through `World::actorProneTilt`.

Verified: 27-map sweep (all SP maps, 40 s, `--spawn-all`, `IW2_PRONE_LOG=1`): no unimplemented built-ins; checkprone / enterprone /
exitprone run on 24 of the 27 maps; runtime errors only the known ones (elalamein / matmata setbattlechatter, matmata roof_guys,
silotown_assault field_charge spawn), none from prone code.
Actor +0x288 readers (every `0x288(%reg)` byte access; no script field: the 51-entry actor field table at 0x570530 has
no 0x288 entry): 0x502b42 (offset goal 0x502960), 0x53defc (maymovetopoint 0x53dce0), 0x543ca4 (0x543c30, per listed
actor), 0x5491bf (reacquirestep 0x548f10). Each takes a step height of 10 (0x41200000 / 0x591e8c) while it is set and 18
otherwise. Ported in reacquirestep, aiReachPast and maymovetopoint (replacing the crouch-pose guess); 0x502960's offset
goal is dead code (see actorcover.cpp).

### Corpse prone blend (ported)

- Death start 0x548980 (0x54899f..0x5489f9): anim mode (+0x218) not 8 ("nophysics"), not linked (gentity +0x1c8 tagInfo
  0) and not prone (0x514970): start = now, prone 1, corpse byte 1, length 500, +0x168 = +0x16c = 0, then 0x4fd9e0(ent, 0).
- Death think 0x548ab0 (0x548b96): while the corpse byte is set, 0x4fd9e0(ent, 1) every think.
- Corpse conversion 0x4fdba0: the 24-byte block is copied to the corpse slot (0x1c7e5ac + 0x20 * i, block at +8, entity
  number at +4). With "not prone, mode not 8, not linked" the blend starts (only when the corpse byte is still 0) and
  0x4fd9e0(corpse, 0) runs. 0x523040 (item/corpse trajectory) runs 0x4fd9e0(corpse, 0) when a falling corpse lands (corpse
  falling is not modelled here).
- Snapshot 0x4db2b0: every frame the blocks of the actors in use and of the corpse slots are copied to 0x1c638bc (24
  bytes each) with the index per entity at 0x1c630bc; the client reads them there.
- 0x4fd9e0(edx = entity, smooth): block = corpse slot (eType 0xb) or actor +0x158; nothing unless the corpse byte is set;
  mask = clip mask & 0xfdff3fff (0x820011 for actors and corpses). smooth 0: 0x4fd890 straight into +0x168 / +0x16c /
  +0x164. smooth 1: into temporaries; each angle moves towards its target by at most 6 (0x56fcc8) per call (0x423f50
  wrap), the lift by at most 0.6 (0x592308).
- 0x4fd890(pass, mask, yaw, outRoll, outLift; eax = outPitch, ecx = origin): forward / right from yaw (0x421f00: (cos,
  sin, 0) / (sin, -cos, 0)); pitch probe along forward; |pitch| >= 30 (0x591e08) gives roll 0 and height h1, else a roll
  probe along right and height (h1 + h2) / 2. Lift = height - origin z; when negative and the box (-15,-15,0)..(15,15,72)
  (0x56f704 / 0x56f710) cannot move 1 unit down (0x44c1b0, pass entities actor and 0x7ff) it becomes 0.
- 0x4fd460(pass, mask, origin, fallback = origin, dir, outSlope): box (-4,-4,0)..(4,4,8); from origin + (0,0,30) a sweep of
  10.9 (0x5922d4) along dir (fraction 0: slope 0, return fallback z); from where it stopped straight down to origin z - 30
  (start-solid or no hit: the fallback point); the same along -dir. Slope = 0x424040(pitch(front - back)), 0 when the two
  points are equal; returns (back z + fallback z + front z) / 3 (0x592250).
- Client 0x4c1cc0 / 0x4c1b90: corpse byte set gives mode 2: rotation = 0x423970(f * +0x168) (pitch about y) times
  0x423a10(f * +0x16c) (quaternion (sin(a/2), 0, 0, cos(a/2)), roll about x), product 0x4235e0 (Hamilton, pitch first) =
  `anglesQuat(pitch, 0, roll)`; translation (0, 0, f * +0x164).
- Client time: f uses cg.time (0xf708dc), set from the snapshot server time (0x4a4070) and lying between the previous and
  next snapshot (frame interpolation 0x4a4c10). iw2view passes its effect time, server time - 50 + lerp * 50.
- Visual check (iw2view, toujane `--spawn-all` 50 s, corpse #1139 with pitch -4.2 / roll 11.7, `IW2_SHOT_ENT=1139,110,0,40`,
  A/B with `IW2_NO_PRONE_TILT`): with the control the body lies on the rising ground beside the wall (right side raised),
  without it the right arm sinks into the slope. Live prone pitches seen so far are a few degrees (not visible).

TODO(spec): the corpse prone blend (0x4fdba0 copies the block, sets the corpse byte, 500 ms, zero pitches; client mode 2
with the second rotation 0x423a10 / product 0x4235e0) is not ported; the reader of +0x288; the client draws with its own
time (0xf708dc), here the server time is used. Not visually checked in iw2view yet.
Consumers (decoded): 0x4fe8d0 (the end of the exposed / pain / death / grenade / flee thinks: 0x504450, 0x4fe740 = `updateClaimedNodeCore`) and 0x548b60.. then give up the claimed node (0x4fe670) when the FOLLOWED entity stands at it (0x502660: dz^2 <= 6400, 2D distance^2 <= 900; 0x4fe6ca / 0x4fe8eb) - `updateClaimedNode` does this after the core. 0x4fe6c0 is the same check with no caller found. 0x50081d is only the debug text of the actor debug draw ("close: %d" for +0x2148, with "blockee: %d, blocker: %d" and "dontavoidplayer" next to it): no gameplay. 0x501e06 is the physics dispatch already modelled (anim mode 1, movemode set, path count > 0, not followed -> the path mover 0x501e48; followed with anim mode 1 or 7 -> 0x501620). TODO(spec): 0x504450 (placing the entity from the linked parent +0x1c8) before the node update; a path-less / movemode-0 actor goes through the block move at 0x501f22 (clears contents bits 0x18 first), not modelled.
Resolved: the facing blend (see "Facing blend"). Open (from before): the ring advance 0x504f60 and facing blend 0x505630/0x504e70/0x504de0.

## Facing blend (ported; src/game/actorfacing.cpp, World::actorFacing)

- Look blocks: base at actor +0x104 (mode, +0x108 pitch, +0x10c yaw, +0x110 turn target), override at +0x114 (orientmode;
  +0x118, +0x11c, +0x120). 0x5048b0 stores trunc(a * 65536/360) & 0xffff back in degrees for pitch / yaw; 0x504950(pitch,
  yaw) also sets the target to the quantised yaw; 0x5055d0(dir) does the same from vectoangles(dir) (0x421b30). The spawn
  (0x4fc7dd) sets the base block from the spawn angles.
- Selector 0x505d80 (jump table 0x505dec, mode - 2): 2 -> 0x505630, 3 -> 0x505960, 4 -> 0x505b80, 5 -> 0x505cb0, 6 ->
  0x504950(0, claimed node yaw) when the node has flag 0x8000 (0x505dcc); mode 1 and others: nothing. The override block is
  used when +0x114 is set.
- 0x505630 (motion): only with a path (+0x6b0 > 0) in anim mode 1. Moving (movemode +0x214 != 0), path direct (+0x6e8 & 1),
  lookahead distance (+0x6dc) < 60 (0x591e4c) and lookahead direction (+0x6c8) behind lookforward (+0xe0, dot < 0): keep
  (0x504950 with the entity pitch / yaw). Else the trail sum (0x504e70); zero: keep; moving: 0x5055d0(sum); standing: the
  sum normalised (0x421160) and compared with (cos, sin) of the block target: dot < 0.9 (0x59200c) faces it, else keep.
- 0x505b80 (mode 4): path and a non-zero velocity (+0x230 / +0x234): with an enemy seen within 250 ms (0x54ce10) +0x130 =
  now + 500 and, within 256 (dist² <= 65536, 0x591e24) and 0x545580(unit direction) true, 0x505960; every other case
  0x505630. Without a path or standing: 0x505960.
- 0x505cb0 (mode 5): path and moving: 0x545580(unit direction to the enemy) ? 0x505960 : 0x505630 (the exe reads the enemy
  without a null check; the port faces the motion without one); otherwise 0x505960.
- 0x545580: true when the path was planned (+0x6ec) under 300 ms ago, or 0x543ae0 says the path ahead is not longer than
  128; else with h = (1 + lookDir . dir) / 2: false when 72 - 48 h (0x591f3c, 0x591f40) > lookahead distance, else
  age < 1500 + 3500 h (0x591f34, 0x591f38).
- Trail ring: 0x505230 (the path walk) writes every frame ring[idx] = F, then 0x504f60 advances. F = lookahead direction
  when w = (dist * 20 - 95) / 95 <= 0 (0x591e18, 0x592364, 0x592360), the far direction (+0x6d4) when w >= 1, else the
  normalised blend w * far + (1 - w) * look (the side offset branch +0x73c is dead). The followed branch (0x504248) adds
  through 0x505520. 0x5523a2..0x5523c4: when the walk script starts from standing in anim mode 1 with movemode set, the ring
  is cleared (0x504de0), gentity +0x128 &= ~0x18 and +0x704 = 0. The spawn clears it too (0x4fc806).
- 0x504ca0 (turn): +0x124 = the active target; yaw turns by at most rate * 50 (0x592030 / 0x592034), rate 0.4, or 0.036
  (0x3d1374bc) while prone (with the 0x514970 bookkeeping inline); pitch 0; re-quantised. The port no longer writes the yaw
  in moveActor (it was inferred); the selector + turn do it. Measured on toujane (30 s, IW2_FACING_LOG=-1, moving actors
  after 3 s): yaw within 3 degrees of the motion in mode 2, 6 in mode 4 (where the enemy facing applies).
- Re-read for the move frame (cluster-animmode), where it differs from the earlier decode:
  - 0x505960 with an enemy and a path does not return early (0x5059f6 jg 0x505a22): when the path is older than 300 ms
    (+0x6ec + 300.0, 0x591f4c, compared as floats) and the enemy was not seen for more than 500 ms, the node from 0x5432f0
    (with the sight record) is faced, else the enemy's origin (0x505a80). The direction is 3D through 0x5055d0, so the look
    pitch follows it (the earlier port used yaw only, pitch 0).
  - 0x505960 without an enemy calls 0x505790 first and only then the claimed-node test (0x505973..0x5059b7); never-seen and
    seen-long-ago enemies go to 0x505920 = 0x505790, then the node test (0x505920..0x505959).
  - 0x505790 (nearest threat): the fought team ({0, 2, 1, 0, 0}[team]); none -> false. While +0x204c - now > 0 -> true (the
    block stays). Else the nearest sentient of that team (0x515230 / 0x515270 over the team mask; 3D distance², a client's
    * 0.25 (0x591fe0), strictly smaller wins); none -> false; +0x204c = now + 3000; 0x5432f0 without a record; none -> false;
    the block faces that node (0x5055d0) when it is 1 or more away in 2D; true. Ported as `World::faceThreat`.
  - 0x5432f0 (sentient a, sentient b, record or none): a's node 0x515040 (none -> 0); b's node = record +0x24 (b's node
    +0x38 when the record was written, 0x500ca2; `SentientInfo::lastKnownNode`) or 0x515040(b) (none -> 0); when they see
    each other (0x517e40) b's node; else 0x547330: a search from b's node over the links (b's team block bytes skipped,
    cost = link length, no heuristic) that stops at the first node seeing a's node; that node or 0. Ported as
    `World::enemyPositionNode`.
  - 0x505b80 / 0x505cb0 read the block velocity +0x230 inside 0x504160, before this frame's physics: the previous step's
    velocity (`ActorAi::blockVel`), not the one moveActor just set.
- 0x504aa0 (anim yaw, `World::turnActorByDelta`, called by 0x504160 when the yaw is not 0, args (0, yaw)): both blocks: pitch
  re-quantised, look yaw and target = target + yaw (rebuilt from +0x110 / +0x120); entity yaw += yaw (pitch, roll 0); look
  angles += (0, yaw) through 0x5049a0.
- 0x504be0 (look turn, `World::actorLookTurn`, after 0x504ca0): look pitch / yaw (+0xd8 / +0xdc) each turn by at most 40
  (0x591f78 / 0x592110) towards the active block's look pitch / yaw (0x423f50 = a - b normalised to [-180, 180]); 0x5049a0
  stores them quantised and derives the look axes (0x421da0: +0xe0 forward = the script field lookforward, field table
  0x570418, +0xec right, +0xf8 up).
- 0x504160 (anim-mode switch, jump table 0x504428 over +0x218 - 1): 1 the path walk (0x505230) or the followed branch
  (0x504228), yaw 0; 2 delta xy, z 0, kind (+0x264) 2, yaw 0; 3 ("angle deltas") delta zeroed, kind 1, yaw of the delta;
  4 delta xy, z 0, kind 2, yaw; 5 3D delta, kind 3, yaw; 6 3D delta, kind 4, yaw; 7 delta xy, z 0, kind 5, yaw; others yaw 0.
  The delta is 0x503e40 -> 0x478550 (goal weights per +0x220), the yaw 0x423900 of its rotation (atan2 with 2 / (a² + b²)).
  Then (0x5042ea) a non-zero yaw -> 0x504aa0(0, yaw), the selector 0x505d80, 0x504ca0, 0x504be0. 0x501d00 rotates the delta
  of kinds other than 1 by +0x124 (desiredangle, 0x421f00) before the block move. +0x220 writers: death 0x548b59 0, pain
  0x54d47c 0, animcustom 0x54b2c2 1, negotiation 0x54b7b4 1, the move-script choices 0x53ae15..0x53b12e 0.
- 0x504450 is now one frame (`World::actorMoveFrame`, from thinkActors after the state think, for every state but the
  turret and animscripted, whose thinks do not reach it): mode 8 nothing; modes 2..7 the delta / yaw above, then the
  facing tail, then `moveActorKind` with the rotated delta. The mode-1 path walk stays in the state code (moveActor); a frame
  that already walked is treated as mode 1 so an animmode override does not move the actor twice (TODO(spec): whether
  0x552350 stores mode 1 directly or through 0x53ac90, which applies +0x21c). Pain and death animations now move the actor by
  their deltas (mode 4), as animcustom and the negotiation (whose ad hoc copy is removed) did. The linked branch (parent tag
  frame) and 0x4e7150 stay TODO(spec).
- orientmode (0x53f510) sets the override block through the same helpers: "face angle" 0x5048b0(0, angle) and +0x120;
  "face current" +0x120 = +0x124 (desiredangle, not the entity yaw); "face direction" / "face point" 0x5055d0 (look pitch from
  the vector); "face default" 0x5063f0. teleport with angles (0x53e237) and the animscripted think (0x54b1f3, every frame) set
  the base block from the entity angles (teleport also the look angles).
- getmotionangle (0x53f770): block velocity (+0x230) of more than 1 (2D squared) -> its yaw (0x421890) - entity yaw; else
  with a path the lookahead direction's yaw - entity yaw; else 0. Replaces the inferred `$motionyaw` reader.
- TODO(spec): +0x130 has no reader found; the turret user's angles from the turret (0x54a7e6, then 0x54a829) are not set.
- Elalamein check (after the facing blend): the player dies by 40 s where the build before it (1b1b4a0) survived. The
  bisect (ring feed, modes 2/4/5, prone turn rate, the removed moveActor yaw write) shows the death only goes away with
  both the selector and the old yaw write restored; it is not tied to one piece. The damage (`IW2_PLAYER_DMG_LOG`) comes
  from the same standing kar98 riflemen (#1396, #1405, #1409, #1411; 80 per hit) in both builds, and #1396's facing state
  is identical frame by frame (mode 3 override, no path, yaw 12.9). Only the hit timing differs: 7 hits within 7.8 s
  versus 5 spread out, against 500 health that regenerates. The cause is the shared random sequence shifting when other
  actors move differently, not a facing fault, so this is not a regression. breakout, duhoc_defend and hill400_assault
  end with the player dead in both builds.

## Failed actor move block (0x501d00)

When the move block (0x5487c0) returns 0, the physics step discards the block origin and nudges the entity origin
vertically towards the nearest node (0x516950, radius 384, height 128, at most 64, any type; nearest by 3D distance²).
With dz = node z - origin z (constants 0x592010 = 8, 0x591e00 = 0, 0x592390 = -18): dz > 8 gives +8, 0 < dz <= 8 gives dz,
dz < -18 gives -8, otherwise 0. Ported as `World::stuckRecovery`. It does not free the demolition actors that wall_hop
leaves in the actor-clip slab near (6383, 523): they sit on the node height (dz in [-18, 0]), so the nudge is 0.
TODO(spec): the escape from that slab is still unknown (the move block there returns 0 every frame).

### Negotiation trigger snap (0x503ec0, 0x503fb0..0x503fda)

The trigger runs inside the path step, before the physics move, on the pre-move origin and the previous frame's block velocity (+0x230..0x238): 2D distance² to the begin point <= |velocity|² · 0.005 + 1e-6. When it pushes state 9 the move delta (+0x250..0x258) is replaced by (begin point - origin), so the move block carries the actor exactly onto the begin point. The port used to test after the move, starting wall_hop up to ~13 units early; the hop then ended inside the 15-thick AI-clip wall of demolition (x 6383) and the actors never reached `end_left`. Now `World::moveActor` triggers first and overrides the delta. Failed move block (0x501d00): block vz (+0x238) is cleared; with no node in range the block's partially moved origin stays.

### Stop soon (0x503fe5..0x50414b) and actor use

Stop soon runs in the same pre-move function (0x503ec0), on the pre-move origin and the previous block velocity.
vec = path end point - origin (3D); d2 is its 2D distance squared. Start: if d2 != 0 and the actor flag 1 is not set,
return; if 2D speed² * 0.0625 (0x5920cc) < d2, return; otherwise stopSoon = now + 500. While left = stopSoon - now > 0
the move delta (+0x250..0x258) is vec * k, with k = (1 / contactNormalZ if the block reported a contact (+0x244), else 1)
* (2 - r) * r and r = 50 (0x592034) / left (2.0 = 0x591ed0). When left <= 0: path clear (0x541780), path count and planned
= 0, detour (+0x724) = 0 and the delta is vec. Ported in `World::moveActor` (`stopSoonScale` in actorslide.cpp).

Player use of actors (scripts such as demolition's offerExplosives set `useable`): the scan (0x534490) takes an entity with
an actor (+0x100) whose field +0x211b is non-zero; the pick (0x534d40, byte table 0x534fb4, case 3 at 0x534ec5) gives the
hint (type 4, string +0x2124) for sentient team 2 or 3 and ends the search; the press (0x5341a0) hands the actor's held node to the player (0x519050, as the follow retarget) and notifies "trigger" with
the player.
