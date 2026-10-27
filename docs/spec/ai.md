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
  weapon index 0 is `none` (0x4dc210). A turret's bullets credit the turret's user, or the world (0x51ada0).
- `getaiarray(team)` (0x4ec9f0) lists actors whose +0x25c is set; the die callback clears it, so the dead are
  not listed. The errors "dospawn can only be called on actor spawners" and "Spawner at (%g %g %g)
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
TODO(spec): the anim-mode switch cases, the turn-pair update rates, the facing-mode → target mapping, and the
override angle + expiry (+0x120/+0x159/+0x15c/+0x160).

## Cover nodes (observed; Ghidra decompiles and the objdump listing)

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
7/9 {285², 330…55}, 12 {285², facing}, 18 {285², turret arcs 0x5159f0 or 315…45}, others {0, facing}. With `check`
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

Implemented in `src/game/actorcover.cpp`. TODO(spec): node take-over 0x518e80/0x518dc0, linked nodes, 0x515390, goal
volumes, 0x505ed0/0x505f70 (goal entity, path to enemy), the rest of 0x503200, the move check's continuation after a
block, the trim state of the reacquire paths (+0x70c/+0x710), `ispathdirect`, actor +0x20b4 in the score, 0x548be0,
0x502960's offset goal (0x542340/0x545670, see "Path planner").

## Path planner (observed)

Node query 0x517f90: candidates of 0x516950 (2D radius, height, ≤ 64, type mask; mode 0: centre 120
below, height 184), linked nodes first then nearest (0x516640); a linked node is taken when the point is on it
(−16…16, −1…73) or a box (−15,−15,17)…(15,15,72) traced to it (mask 0x820011) is clear; plane-excluded and unlinked
ones are traced last. Sentient node 0x515040: radius 192, cached (+0x38, flags +0x3c/+0x3d). A* 0x545840: sorted open list by g + h (h = 2D distance to the goal position), insert before the first not lower,
re-open on cheaper g, links with a team block byte (+8 + team) skipped; 0x545ca0 adds planes. Path points 0x541da0: goal first in the original (+0x330, 0x1c bytes: pos, dir, length, node), at most 32 (+0x6e8 |= 4),
the goal position as an extra point when it differs (2D) from the goal node. Implemented in `src/game/actorpath.cpp`
(goal-first layout, see "Path following"). TODO(spec): negotiation pairs (types 16/17),
0x502960's offset goal (0x542340/0x545670).

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
16). TODO(spec): the flee fallback 0x5429e0 gated by 0x54b610 (0x51faa0…0x51fb05).

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
Bullet_Fire_Extended, the AI fire builtin), `actorstate.cpp` (pain throttle 500 ms, pain reveal, death).
TODO(spec): the pain accumulator's +0x48 add and damage-class gate, the turret entity +0x10c field of the
pain/death handler, the player pain events 2/3 of the client fire path (0x4e786b / 0x4e787b).

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
TODO(spec): +0x318 (the bumped entity: following and 0x500d80), +0x250 move delta vs velocity, the actor clip mask
+0x28c, +0x288, 0x504630 crowding, 0x53ade0, negotiation (+0x244, flag 0x40), the facing blend +0x2150, +0x73c, the
actor physics 0x501xxx.

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

OpenIW2 moves actors along the planner's path ("Path planner") at the root-motion speed of their animations.

## Friendly chain (observed; src/game/friendlychain.cpp, friendlychainworld.cpp)

Nodes (0x78 bytes: +0 type, +4 spawnflags, +6 targetname, +0xc target, +0x14 origin, +0x34 chain id, +0x36 position, +0x38 predecessor,
+0x44 hold time) are chained at load by 0x52d630. A node whose `target` equals another node's `targetname` is that node's
predecessor. Fatal errors: "multiple nodes target node with name '%s'", "node with name '%s' targets itself", "found chain loop including node
with name '%s'". Heads (a predecessor of something, with none of their own), visited in array order, get ids 1, 2, ... and position 1; each
successor gets position + 1 (0x52d4c0). A head or successor with spawnflags bit 2 is excluded (id 0). The nodes are sorted (qsort, 0x52d580)
by chain id descending, position ascending, predecessor ascending, type descending; the nodes before the first id 0 are the chain nodes
(0x1c8da08). A chain node of type 0 is fatal ("bad node ... in friendly chain").

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
entity); modes 0 and 1 take radius max(goalradius, 4) and height max(goalheight, 80). The cover search, `pathToGoal` and the in-goal test read it
(`World::actorGoal`; the script fields stand in until the first update). `isingoal` (0x540c00 -> 0x5026d0) tests a point against it: |dz| <= height and 2D distance <= radius (the goal volume test 0x447830 is TODO(spec)).

The turret think (0x54aba0) is the second caller (0x54ad05), unless the claimed node is kept. Not done: the toucher filter of 0x529140, the g_spawnai dvar.

## Unimplemented actor built-ins: decode status (matmata prediction family, gethitenttype, checkprone)

Sweep with `iw2game <main> <map> 40 --spawn-all` over all 28 SP maps: the only built-in still reported as unimplemented is
`checkprone` (x4 libya, x36 matmata, up to x99 elsewhere). The matmata prediction calls are not reported any more, but there
is no implementation of them in src (the matmata paths that call them are not reached in the sweep time). All are actor
methods (entity from the self object, the "entity" check at 0x468ed0 error path).

- `gethitenttype` 0x5400c0 (0 args, returns string): reads the actor's bumped entity number at gentity +0x318
  (init 0x7ff = none at 0x500d1d; written by the actor move code at 0x501330 and 0x501c6f). 0x7ff gives "none"; else if the
  hit entity's contents word (+0x128) has bit 0x400 set it gives "obstacle"; otherwise "world". Not implemented: the repo
  does not track the bumped entity (see world.cpp note on 0x500d80 and actorfollow.cpp TODO(spec) for +0x2148).
  TODO(spec): wire +0x318 from the actor move trace, then this is a 10-line builtin.
- `checkprone` 0x5406f0 (actor method; args: vector origin (0x46ea90), float (arg1, 0x46e6d0), int (arg2, 0x46e4a0)): calls
  0x4fb530(entnum, &origin, 15.0, 48.0, float, 0, 0, int, 1, 0, 1, 1, 66.0) and returns its bool as an int. 0x4fb530 is a
  capsule/box clearance test: boxes of half extents -6,-6,-6 .. 6,6,6 around points offset from the origin, traced through
  the actor's clip function (table 0x5a9190, indexed by a byte argument), with an extra step-height pass (constants
  0x56fcc8, 0x591e1c, 0x591e20, 0x591ed0, 0x591f30, 0x591f40, 0x5921a0). Not decoded fully (about 400 instructions of x87
  trace math with several early-outs). TODO(spec): decode 0x4fb530 before implementing; not guessed.
- `beginprediction` 0x53f960 / `endprediction` 0x53fb80: snapshot / restore the actor state (the +0x218.. anim-mode words,
  the 0x43-dword AI struct, velocity/origin/angles) plus an animation-state snapshot (0x4796c0 / 0x478db0 / 0x478e10); nested
  through counter 0x1c6104c and a per-depth table at 0x1c7e52c. `predictoriginandangles` 0x53ff50 and `predictanim` 0x540020
  (tail-jumps to 0x5045c0) run an animation step inside such a snapshot and return the resulting origin/angles. They need a
  C++ copy of ActorAi plus the animation-step internals (animscripted domain), so they are not implemented.

### Bumped entity +0x318: writer analysis (not wired)

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
  physics port (0x547db0 slide move, 0x547540 clip velocity, 0x548290 ground probe) first. Not started, no guessing.

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
Tested in tests/test_actorslide.cpp. Not wired into World::moveActor yet: the callers 0x547db0 (the step / ground wrapper),
0x548290 (ground probe), 0x5485a0 / 0x548650 and the mode dispatch of 0x5487c0 are still TODO(spec); the +0x318 copy in the
0x500dc0 step reads the block's +0xf4 after 0x5487c0.

0x547db0 (the actor StepSlideMove, arg = gravity flag) structure, not yet ported: saves the block (0x43 dwords) and the
0x15-dword frame scratch at 0x1cbac58, runs 0x5475e0; result 0 returns 0 (the +0xf4 of the scratch...), 2 returns 0 (all-solid exit);
otherwise (step height 0x1cbaca8 = 10, or 18 when the byte +0x64 of the block is 0) it traces straight down with 0x4db7f0 from the
post-move origin and, when that trace is not a full-fraction (1.0) clear or the normal.z >= 0.7, it skips the step-up; otherwise it
restores the saved block, clears the ground plane flag (0x1cbac60), re-runs the slide move from origin + step height (lerp
with the 0.0 constant 0x591e00 so the origin is kept), traces down by the step height again (0x4db7f0), snaps to the ground via
0x47f390 and accepts the stepped move when the down trace fraction passes 0x591e78 and normal.z passes 0x592018 (then
0x547540 clips the velocity and 0x408ad0 (two args) compared against origin z + 0x59202c decides to store the hit entity in
+0xf4). Helpers 0x4db7f0, 0x47f390, 0x408ad0 and the constants 0x591e78, 0x592018, 0x59202c are not decoded: TODO(spec).

Helpers of 0x547db0, decoded:
- 0x4db7f0: a thin wrapper of G_LocationalTrace 0x44bf00 (pushes 0, 0, 0 for the three trailing arguments, no other work).
  Argument order at the call sites: result, start, mins, maxs, end, pass entity, content mask.
- 0x47f390 (eax = a, edx = b, ecx = out, stack t): out = a + (b - a) · t (vector lerp, per component in x87).
- 0x408ad0 (a, b): max(a, b) (returns a when a - b >= 0, else b).
- Constants: 0x591e78 = 0.0, 0x592018 = 0.3, 0x59202c = 0.1 (0x591e00 = 0.0, 0x591e20 = 1.0).
In the step wrapper: the up-step test compares the down-trace normal.z against 0.3 (0x592018) and the fraction against 0.0
(0x591e78), the final hit-entity rule is max(lerp-corrected z delta, ...) + 0.1 compared against the block origin z. The exact
stack offsets of the two scratch trace results inside 0x547db0 (0x44 / 0x60 / 0x82 relative to esp after the pushes) have not been
mapped to trace_t fields yet, so the wrapper is not ported: TODO(spec). The ground probe 0x548290, 0x5485a0, 0x548650
and the 0x5487c0 mode dispatch are also still undecoded in detail, and World::moveActor, +0x318, gethitenttype and the
follow link stay as documented above.

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
Walkable: z snaps to the trace end only if lower than the origin or mode==5; ground entity = trace entity, touch-add. Mode dispatch (0x5487c0 callees 0x548260/0x548200/0x548110/0x5480d0/0x5485a0/0x548650) is not yet ported: TODO(spec).

### Actor move dispatch (0x5487c0), ported as moveBlock

Block fields: +0x18 ground entity, +0x20 contact flag, +0x24 ground normal.z, +0x28 surface type ((trace +0x10 >> 20) & 0x1f), +0x2c desired displacement, +0x40 mode, +0x48 time ms (dt = ms * 0.001), +0x64 byte (step height 10 if set, else 18), +0x68 trace mask.
Early out: ground entity 0x7fe and zero displacement -> velocity zeroed. Mode 3 adds the displacement; mode 4 (0x548260) sets velocity = displacement/dt and slide-moves without gravity; otherwise the ground probe runs and then mode 5 (0x548200), ground-entity walking (0x548110) or airborne (0x5480d0), see the header comment of moveBlock.
After the move, the contact flag, normal.z and surface type are set when a ground plane exists with normal.z at least 0.3 (0x592018).
Not ported: 0x5485a0 (footstep timer: +0x1c/+0x48 accumulates to 500 ms and plays a sound via 0x535770) and the world wiring. TODO(spec) for the sound call.
