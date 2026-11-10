# Multiplayer bots (OpenIW2 extension)

**This page describes an OpenIW2 design, not the original game.** The original multiplayer server has test clients
(`addtestclient`) that occupy a client slot but never move, aim or shoot; there is no bot movement code to
reimplement. The navigation graph, the bot controller and the skill presets below are our own. Where the design
leans on original behaviour (the test client connect, pmove, bullets and damage, the AI accuracy graphs, the gametype
callbacks) the evidence is cited: VAs in the 1.3 Linux dedicated server ("lnxded") for the MP game module, VAs in the
patch 1.3 SP executable for the shared player and AI code, and `file.gsc:line` for shipped scripts.

Implementation: `src/game/navgen.{h,cpp}` (graph), `src/game/mpbots.{h,cpp}` (controller, `addtestclient`),
`src/game/mpbotgoals.cpp` (objectives, goal API, weapon classes), tests in `tests/test_mpbots.cpp` and
`tests/test_mpgametypes.cpp`.

## What the original provides

- `addtestclient` is a script builtin of the MP server (builtin table entry 0x081885bc → handler 0x08116bd8 → fake
  connect 0x080913da). It connects a client with a generated userinfo (name `bot<N>`, the current protocol) from a
  bot-type address, marks the slot as a test client, lets it enter the game and returns the client's entity to the
  script (0x080916f8).
- The test-client mark is read in two places only: the server skips sending server commands to it (0x08094750) and
  skips its timeout check (0x0809657e). No code synthesises user commands for it: a test client stands where it
  spawned with an empty command.
- MP maps carry no path nodes: the shipped `maps/mp/*.d3dbsp` have no `node_*` entities and an empty lump 38 (the
  path data of docs/spec/ai.md "Path data"), so the SP planner has no graph there.

## Client side of a bot

A bot is an ordinary client slot (docs/spec/mp.md "Local clients") connected by `World::mpAddBot` (also the
`addtestclient` builtin, which returns the entity like the original). From then on it goes through the same steps as a
player, all through the gametype's own code:
1. the frame after the connect it sends begin (`dm.gsc:165` waits for `begin`);
2. while it is a spectator without a team it answers the team menu with `autoassign` (`level.autoassign`,
   `dm.gsc:783-937`), then the weapon menu the gametype opened (`weapon_<nation>`) with an allowed weapon ("Weapon
   classes" below); the gametype spawns it and gives the weapons (`_weapons.gsc`);
3. when dead it presses use (usercmd 0x8, read by `useButtonPressed`) 2.5 s after the death, which respawns it
   (`dm.gsc:526-557`). A spectator that has a team and a weapon in `pers` is a dead player of ctf / hq / sd waiting for
   its respawn (`ctf.gsc:701-735`) or the next round; it presses use the same way and never answers the menus again
   (an `autoassign` would move it to another team).
The bot's name is `bot<N>` with N counting the bots added to this world, as the original's fake connect names them.

The local client should connect first: client 0 is the local player's entity.

## Teams

Bots get no team rule of their own: they answer the team menu with `autoassign`, so the gametype decides
(`sd.gsc:2086-2125`, the same in the other team gametypes): the player counts every client's `pers["team"]` and goes to
the smaller team; with equal teams to the team with the lower score, or a random one on a tie. Five bots therefore split
3 / 2. A player who picks a team by hand is let in while that team has fewer than half of `sv_maxclients` players
(`_teams.gsc:318-335`), so choosing the team with three gives 4 / 2. Only `scr_teambalance` 1 evens the teams: in sd at a
round restart (an unbalanced round marks the next restart to balance), in the other gametypes 15 s after an announcement,
checked every minute (`_teams.gsc:43-80`). With the default
`scr_teambalance` 0 the original keeps 4 / 2 as well.

## Navigation graph

Generated from the collision world (docs/spec/collision.md), never stored in the repository.

1. **Floors.** A grid of columns over the world bounds (lump 35 row 0, docs/spec/d3dbsp.md), 16 units apart. Each
   column is scanned from the top with point traces against the player clip mask without the bodies of players and
   actors. Every hit whose normal has z ≥ 0.7 (pmove's walkable slope) is a floor candidate; the standing player box
   (−15..15 × −15..15 × 0..70) is dropped onto it from 18 units above (from 1 unit above when that starts in solid:
   a room barely taller than the box, as the goods wagons of mp_railyard with 86 units of headroom; then the crouched
   box, top 50), and where it rests without starting in solid on a walkable surface there is a node. Under a floor the scan goes on from the first free point (up to 8 floors per
   column, candidates closer than 16 units in height merged).
2. **Links.** Each node is tested against every node of the 8 neighbouring columns with a height difference between
   −190 and +36 units. The walk test moves the box in 12-unit steps: lift by `jump_stepSize` (18), move flat, drop
   onto the ground (at most 190 below, under the 200-unit fall damage height of docs/spec/pmove.md); when the flat
   move is blocked it retries with a lift of 36 (a jump: `jump_height` 39 minus a margin). The step must land on a
   walkable surface. A link is flagged *jump* when a step rose more than 18 units and *drop* when one fell more than
   18; links are one-way (a drop is not walkable back). The link cost is the 3D distance. When the standing box does
   not pass, the test is repeated with the crouched box (top 50, docs/spec/pmove.md); a link found that way is flagged
   *crouch* (32), and a bot on it, or close to its start node, crouches; such a way that also needs a jump is no link (a
   crouched player does not jump). Towards a neighbouring column the node has no
   link into (a column without a floor node: a sill, a thin beam), the node two columns on in that direction is tried.
2b. **Ladder, mantle and leap links**, found by running pmove (docs/spec/pmove.md) with the controller's own traversal
   input from the node, standing still, in 50 ms steps:
   - *Ladder* (flag 4): from every node the box 6 units narrower than the player, feet at +8 (the box of the ladder grab
     0x50bd90), is swept 24 units in the 8 flat directions; a hit on a surface with flag 0x8 (ladder) that is not a
     floor gives a ladder face, its view yaw facing into the face (−normal). Input up: that yaw, forward, view pitch −60
     (the climb speed follows how far the view looks up, 0x50c070); the step-up of the slide move (0x54f409) gets the
     player off at the top. When pmove used the ladder and stands again, the node within 32 units of the end, if it is
     more than `jump_stepSize` higher or lower, gets the link. Then the way down from that top node: the same yaw,
     backward off the edge; while falling and not on the ladder, forward (the airborne grab sweeps 30 units along the
     facing); on the ladder backward with the view up climbs down.
   - *Mantle* (flag 8): a 0.2-unit box of the player's height swept from 14.9 units behind the node to 40 ahead in the
     8 directions with the mantle contents 0x1000000, as the mantle check (0x5257f0); a hit with MANTLE_ON / MANTLE_OVER
     (surface flags 0x2000000 / 0x4000000) gives a face. Input: the face's yaw, forward; the jump button is pressed once
     the previous step's mantle check found a ledge (hint flag 8 of the mantle state, 0x525010). The mantle needs the
     PLAYER_MANTLE animations (xanim `player_mantle_*`), loaded for the bots' pmove too.
   - *Leap* (flag 16, with 8 when the jump turned into a mantle): towards each of the 8 neighbouring columns that has no
     plain link, a standing jump forward along that direction (jump pressed on the first step, held while in the air or
     mantling). A link is made when the player lands, at most 190 units lower, within 24 units of a node that is either
     more than `jump_stepSize` higher or more than 40 units away. This finds gaps, crates and jump-mantles.
   A link of one of these kinds stores its view yaw. Ladder and mantle links are made before leaps, a pair of nodes has
   one link at most.
3. **Pruning.** The strongly connected components are computed; the component that holds the most spawn points
   (entities `mp_*spawn*`, nearest node within 160 units) is kept as the *main* nodes: every main node can reach every
   other one. The nodes outside it that lie on a way from a spawn point's node into the main part (reachable from that
   node, and a main node reachable from them) are kept after them as *exit-only* nodes (see "Off-graph spawn points");
   everything else is removed. A spawn point's node is the nearest node within 160 units that the knee-to-chest box
   reaches from the spawn in a straight line (else the nearest one), so a spawn inside a room is not given a node
   behind its wall. When the spawn's node has no way into the main part at all (a room the links lead into but not
   out of: the spawn room of mp_farmhouse at about −729 420 64, entered over a window sill by a standing jump), the
   shortest way to the main part over links taken in either direction is used, and every link on it taken backwards
   is added reversed as a *jump* link (the bot runs and jumps where the standing jump came in). (Keeping every node that leads into the main part would add up to half again as many
   nodes and push large maps over the node limit below.)
4. When the graph would have 65536 nodes or more (the planner's links hold 16-bit node numbers) the grid is made
   1.5 times coarser and generated again.

The graph is loaded into the world's `PathGraph`, so the existing A* (docs/spec/ai.md "Path planner") plans on it.

The scanned box is the world model's bounds (lump 35 row 0) widened to every spawn point ± 256 units (mp_dawnville has
spawn points outside them); a column scan steps through solid for at most 512 × 16 units.

Brush entities (`script_brushmodel`, `func_*`: 10 on mp_brecourt, 3 on mp_toujane, 1 on mp_farmhouse, none elsewhere)
are solid in the generator's traces where they stand when the graph is made, so the graph goes around them; a mover that
later moves out of the way does not open a path. Not handled (TODO): cover or camping spots; the way up to the four
off-graph spawn floors.

### Coverage of the shipped maps

`tests/test_navmaps.cpp` generates the graph of all 15 MP maps and counts the spawn points of every spawn class on it
(`mp_dm_spawn`, `mp_tdm_spawn`, `mp_ctf_spawn_*`, `mp_sd_spawn_*`). Every spawn point has a node within 160 units.
On four maps one spawn spot (the dm, tdm and
one ctf point there) is on an upper floor whose only exits the generator finds are drops (a window or a hole in the
floor); the way up is not found, so these spots are exit-only nodes ("Off-graph spawn points" below):

| map | spot (x y z of the floor) |
|---|---|
| mp_dawnville | −26 −16929 176 |
| mp_downtown | 2205 −1846 408 |
| mp_farmhouse | −2383 533 124 |
| mp_trainstation | 5128 −4863 136 |

The generation takes 4–40 s per map (Release build) and is cached.

### Cache file

`<cache dir>/<mapname>.nav`, little-endian: magic `IW2NAV\0\0`, uint32 version (3), uint64 key, float cell, float
grid origin x, y, uint32 node count, the nodes (3 floats each), then per node uint32 link count and per link uint32
target, float distance, uint8 flags (1 jump, 2 drop, 4 ladder, 8 mantle, 16 leap), float view yaw. The key is the generator version (high 32 bits) and the CRC-32
of the map's BSP file (low 32 bits); a file whose magic, version or key differs is ignored and the graph is generated
again (about 2 s for mp_toujane). The cache directory is `$IW2_NAV_CACHE`, else `~/Library/Caches/openiw2/nav` on
macOS, `$XDG_CACHE_HOME/openiw2/nav` or `~/.cache/openiw2/nav` elsewhere.

## Controller

Runs in the multiplayer frame step (after the scripts of the server frame) for every bot that is playing, once per
50 ms server frame, and produces the input of pmove (`PlayerInput`, docs/spec/pmove.md) for the bot's own player state.

- **Player state.** The bot keeps a pmove player state. When the bot is (re)spawned or the script moves its entity by
  more than 64 units, the state restarts at the entity's origin and view yaw with the spawn slide of ClientSpawn
  (pm_time 100, docs/spec/pmove.md). After the move the entity's origin, yaw and velocity are written back, so the
  bullet traces and the entity clip box see it where pmove put it. Bots collide with each other and the local player
  (pmove skips only the bot's own entity).
- **Perception.** An enemy is any other active, living, playing client (dm: everybody; team gametypes: the other
  session team). Candidates within the skill's sight distance are checked nearest first with sight traces from the
  bot's eye (origin + view height) to the target's eye and chest (mask without characters); a new enemy must also be
  inside the skill's view cone, the current one stays known while any trace passes. The last seen position is kept
  for the skill's memory time. No knowledge through walls (but see hearing below).
- **Goals and walking.** Without a visible enemy the bot plans an A* path to the enemy's last seen position, else to a
  random node; the path starts at the node nearest to the bot (within 512 units). Nodes within 24 units are passed;
  the bot steers at the farthest of the next 4 nodes reached over plain links (no jump or drop, height within 20).
  Close (48 units) to a jump link it presses jump for one frame and releases it the next (a held jump does not jump again). With
  no progress of 24 units in a second it jumps; after three such seconds it plans a new goal.
- **Ladders, mantles and leaps.** When the next link of the path is a ladder, mantle or leap link, the bot first stops
  on its start node (within 20 units, 8 for a leap, horizontal speed under 40), then takes the link's view yaw and
  pitch and the same traversal input the generator used (`navTraverseInput`), until it reaches the end node. The
  link's angles go to the move at once (a ladder climbs along the command pitch); the bot's own view, the one the
  killcam replays, turns toward them at the skill's turn rate like anywhere else, so the camera does not snap. The stuck
  jump is off meanwhile (a jump on a ladder pushes the player off it, docs/spec/pmove.md). After 8 s, or when it is
  carried more than the link's length + 64 units from the end node, it plans again. The bots' pmove has the mantle
  animations, so a bot mantles like a player. Forward / right input
  come from the walk direction in the view's frame, scaled so that the larger is 1.
- **Fighting.** With a visible enemy the bot stops following the path, closes in beyond 900 units, backs off within
  250 and strafes, changing side every 0.6–1.5 s. The view turns towards the enemy's chest (eye − 14) at most
  `turnRate` degrees per second in yaw and pitch.
- **Shooting.** The bot fires once the reaction time after first seeing this enemy has passed, the view is within 8
  degrees (yaw plus pitch error) of both the lagging aim point and the enemy's chest (the bullet goes to the enemy, so
  a bot never hits what it does not look at; the killcam replays this view), the trigger is pulled (see "Weapon state" below) and the clip is not empty. As an AI
  shot, hit or miss is decided first: the chance is the weapon's AI-vs-player accuracy graph at the distance (else
  its AI-vs-AI graph; the MP weapons name only the latter) times the skill's accuracy, as the AI hit chance
  0x54fde0 → 0x54fb90 (VAs in the patch 1.3 SP executable) with the shoot accuracy argument 1 (docs/spec/ai.md). A hit aims at the chest with up to
  ±4 / ±8 units of jitter; a miss passes beside the target by the skill's miss offset (1–2 times, random direction
  around the line of fire); a hit on a client aims at its posed `j_spine4` bone (players are hit on bone boxes). The
  view angles become the shot's direction (a player's bullet leaves along its view; the correction is within the 8
  degrees above), so the replayed view has the crosshair on the shot. The bullet is fired from the eye with the weapon's damage through the normal bullet path
  (Bullet_Fire, combat.cpp): damage by distance, MOD by `rifleBullet`, the player damage callback of the gametype.
  After `burst` shots it pauses `burstPauseMs`. An empty clip reloads by itself (weapon state); with no enemy a
  clip under a third also reloads (one press of the reload button).
- **Weapon state.** A bot does not time its shots itself: its trigger, reload button, aim and weapon choice are the
  input of the player's weapon state machine (`PlayerWeapon`, docs/spec/weapons.md), the same one that drives the local
  player and network players (docs/spec/net.md section 13). So a shot leaves only when the state machine fires it:
  `fireTime` apart, a bolt-action weapon (`boltAction`) rechambers for `rechamberTime` after each shot, a reload takes the
  weapon's reload times (segmented for the shotgun), a weapon change drops and raises, and a semi-automatic weapon
  (`semiAuto`) or bolt-action weapon fires once per press: the bot releases the trigger for a frame between shots. The
  clip and the reserve the state machine uses are the inventory's. Every shot it returns is aimed and fired as described
  under "Shooting"; the reload start plays the body's reload event. A new life starts with a fresh weapon state.

### Skill presets

| skill | name | reaction ms | turn °/s | accuracy | miss offset | view cone (cos) | sight | burst | pause ms |
|---|---|---|---|---|---|---|---|---|---|
| 0 | recruit | 900 | 140 | 0.35 | 40 | 0.5 (±60°) | 1500 | 3 | 700 |
| 1 | regular | 600 | 220 | 0.6 | 32 | 0.34 (±70°) | 2500 | 4 | 500 |
| 2 | hardened | 400 | 320 | 0.85 | 26 | 0.17 (±80°) | 3500 | 6 | 350 |
| 3 | veteran | 250 | 450 | 1.0 | 20 | 0 (all around) | 5000 | 8 | 250 |

A random 0–199 ms is added to every reaction and burst pause.

Humanisation and combat presets (same skills):

| skill | aim lag ms | memory ms | hearing | grenade chance | corner checks |
|---|---|---|---|---|---|
| 0 recruit | 300 | 4000 | 1200 | 0.25 | no |
| 1 regular | 200 | 6000 | 1800 | 0.45 | no |
| 2 hardened | 130 | 8000 | 2500 | 0.65 | yes |
| 3 veteran | 70 | 10000 | 3500 | 0.85 | yes |

## Combat behaviour

All of this is our design (the original has no bot code); it reuses pmove's stances and aim (docs/spec/pmove.md), the
inventory of the gametype's loadout and the grenade missile (docs/spec/grenades.md). The pure decisions are
`botChooseStance`, `botWantAds`, `botBurst` and `botGrenadeVelocity` (`src/game/mpbotcombat.cpp`).

- **Ammunition.** The clip and the reserve are the bot's inventory (`World::inventory`), filled by the gametype
  (`_weapons.gsc:276-313` gives the nation's pistol with full reserve, `:315-390` gives frag and smoke grenades by clip
  ammo, `dm.gsc:428-434` the chosen weapon with full ammunition). A shot takes a round from the clip; a reload moves rounds from the reserve into the clip at the weapon state's
  reload times. An empty clip with an empty reserve cannot reload.
- **Pistol.** The pistol is the slot weapon of class `pistol` (`givePistol` asks for slot `primaryb`, but with both slots
  empty `setweaponslotweapon` puts it in the first one, docs/spec/inventory.md). With the clip of the
  main weapon empty and a visible enemy closer than 500 units, or with the main weapon out of ammunition altogether, the
  bot switches to the pistol when it has rounds (an empty pistol clip is reloaded first); the first shot waits for the
  weapon state's drop and raise. Two seconds without a visible enemy it switches back when the main weapon has rounds.
- **Aim lag.** The bot aims at a point that follows the enemy's chest with the skill's aim lag (each frame it moves by
  `min(1, 50 ms / lag)` of the remaining distance). Shots are fired along the view as before; when the followed point is
  more than 16 units off the chest the hit chance is multiplied by `max(0.3, 1 − offset / 64)`.
- **Hit chance multipliers.** On top of the accuracy graph × skill accuracy: aiming down the sights (held for the
  weapon's `adsTransInTime`) × 1.2, crouched × 1.1, prone × 1.2, moving faster than 60 units/s × 0.75; at most 1.
- **Stance** (`botChooseStance`). Standing whenever the bot walks a path or an objective, climbs or is not fighting.
  In a fight: a sniper rifle at 1000 units or more lies prone; when crouching hides the bot from the enemy (a trace
  from 40 units above the feet to the enemy's chest is blocked while the standing eye sees it, checked every 500 ms)
  the bot crouches while it reloads or pauses between bursts and stands up to fire; at 800 units or more a rifle,
  sniper or mg crouches; otherwise it stands.
- **Strafing and dodging.** Only a standing bot strafes (as before); crouched and prone bots stay where they are. A
  combat step is taken only when a main graph node lies within 40 units of the point 40 units ahead (else the other
  strafe side, else no step), so that a fight does not carry the bot off a ledge into a place with no way back. When
  the bot is damaged by an enemy (not a team mate) it knows the attacker (its position becomes the last seen one), turns that way and flips the strafe
  side at once.
- **Aiming down the sights** (`botWantAds`): sniper from 400 units, rifle and mg from 500, smg from 900, never with a
  shotgun or pistol; only in a fight and not while walking a path or climbing. The aim command goes to pmove with the
  weapon allowed to aim, so the move slow-down of an aiming player applies.
- **Bursts** (`botBurst`): automatic weapons (smg, mg) fire the skill's burst within 600 units and half of it (at least 2)
  farther, with 1.5 × the pause; a rifle, sniper or shotgun fires single shots at its `fireTime`; a pistol fires 3 then
  pauses.
- **Grenades.** At most every 2 s (by client number), when the bot has a frag grenade, stands on the ground, is not
  climbing and threw none in the last 6 s: a target is either an enemy seen in the last 3 s but not visible now (behind
  cover, at its last seen position) or a visible enemy with another enemy within 200 units of it, 250..1400 units away,
  with no team mate (team gametypes) and not the bot itself within 256 units of the target. With the skill's grenade
  chance it throws: `botGrenadeVelocity` solves the arc at the weapon's `projectileSpeed` from the eye under the world's
  gravity (the low arc, else the high one), the arc is traced in 50 ms steps against the grenade clip mask and must not
  hit anything more than 120 units before the target; the grenade is spawned with that velocity and the weapon's
  `fuseTime` and one round leaves its clip ammo. The bot's view turns toward the throw at the skill's turn rate (the
  killcam replays that view; the grenade itself leaves along the solved arc at once).
- **Smoke.** The gametype gives smoke grenades only with an smg or the shotgun (`_weapons.gsc:464-492`). In sd (attacking team before the plant) and ctf (the flag carrier, or an attacker) a bot with a smoke
  grenade that sees an enemy 400..1500 units away throws it once per life at the point a third of the way to the enemy.
- **Memory and hearing.** The last seen position is kept for the skill's memory time. A client's weapon fire (the same
  event that pings the compass, docs/spec/mp-hud.md "Compass") is recorded per client; a bot that sees no enemy and is
  within its hearing distance of an enemy's shot of the last 300 ms takes the shooter as its enemy with the shot's
  position as the last seen one. A known but hidden enemy seen in the last 2 s is pre-aimed: the view turns to its last
  position while walking there.
- **Corner checks** (hardened and veteran). Walking with no known enemy, every 1.5–3 s the bot looks for 700 ms along the
  most open of the four directions 45° and 90° left and right of its walk (sight traces of 512 units from the eye).
- **Weapon pickup.** The gametype drops the dead player's weapon and grenades with `dropItem` (`_weapons.gsc:396-428`);
  that builtin is not implemented in our MP game yet, so there is nothing to pick up. TODO.

### Off-graph spawn points

The four upper floors of "Coverage of the shipped maps" are left only by a drop, so they are not in the strongly connected
part. The generator keeps the way down from them as *exit-only* nodes: after the strongly connected part (the *main*
nodes, numbered first, `NavGraph::mainCount`) come the nodes reachable from a spawn point's off-main node that lead into
the main part. A bot that spawns there plans from
its exit-only start node to a main node and drops down; goals (objectives, enemies' positions, random roaming) are only
chosen among main nodes (`nearest(pos, r, true)`), so no bot is sent up there.

### Cost limits

At most 2 A* plans are run per server frame for all bots together; a bot over the limit plans in a later frame (it keeps
standing meanwhile), and the periodic replans are spread by client number. The search for a new enemy (sight traces to
every candidate) runs every other frame per bot (half the bots in each frame); the current enemy is checked every frame.

### Stuck watch

Following a path: the nodes before the steering point are passed for good. Keeping them made a bot run on the spot: the
first frame the box missed the far node by a hair it steered back at a node behind it, the next frame forward again, a
few units to and fro for seconds. The start node of a ladder, mantle or leap link is passed only within the traversal's
reach (8 units for a leap, 20 otherwise); a bot that is not there walks back to it, coasting the last stretch (a run stops
in speed / 5.5 units), and never at the link's far end, which is behind the wall or the gap. Off the path (on the ground
with the next node more than 30 units up, 40 over a Jump link, or 40 down without a Drop link; or a plain link's node
over 48 units away and out of straight reach, as after a fall to the far side of a wall), a bot plans again at once, at
most once a second (`offPathReplans`). The steering box goes from knee height to the top of the standing box.

Measured by `World::mpBotStalls`: a spell is a bot that wanted to walk (not fighting, not on a traversal) and stayed within
48 units of where it began for over a second; `test_mpbotcombat stall <map>` lists them by hot spot, and the test
`mpbotstall_carentan` limits them (under 5% of the walking time, at most 5 over 3 s, none of 8 s). 8 bots, 600 game seconds
on mp_carentan: before these rules 16.8% of the walking time, 16 spells over 3 s, the longest 11 s; after them under 1%.

Short term: a walking bot that has not made 24 units of progress in a second jumps; the second time in a row it also
sidesteps for 600 ms in a random direction, or (every other time) crouch-walks for 1.2 s, half of the times towards its
last path node (low ceilings, crouch-only gaps); the third time it plans a new goal. The steering point is the farthest
of the next four plain-linked nodes that a box from knee height to the head (z 18..70 of the player box) reaches in a
straight line, and a new path starts at its first node (instead of the second) when the second is not reached that way.
The start node of a plan is the nearest of the nodes within 160 units (at most 8 tried, height within 40) that this box
reaches in a straight line, else the nearest within 512 units (a node behind a railing or a thin wall is no start).

A ladder, mantle or leap link whose traversal failed twice (8 s, or carried off) is removed from the map's graph for
the rest of the level (not from the cache); every bot plans again. Sidesteps prefer directions that stay on the graph
(the combat step's test, four random tries).

Long term: a playing bot that has not moved 64 units in 20 s while it is not fighting and not on its objective counts
as stuck (`stuckEvents`); it drops its path and picks a new random goal. Stuck the second time in a row without having
been back on the graph (a node within 24 units) and with no node within 160 units in straight reach of the
knee-to-chest box, it is trapped in a pit
the graph has no way out of (`trapped`): the bot dies as a console kill does (MOD_SUICIDE, its own attacker) and the
gametype respawns it. The soak runs (`test_mpbotcombat soak`) count both and fail on either. Seen before the floor and
spawn-room fixes of "Navigation graph": bots spawned in the goods wagons of mp_railyard (no floor nodes under the low
roof) and in the spawn room of mp_farmhouse (no way out); both are covered now.

Stuck the third time (short term) on the way to the start node of its path, a bot does not take that node as a start
again until it has moved 64 units, and such a node does not count as in straight reach for the trapped test; stuck the
third time on a walk link within 96 units of its start node counts as a failure of that link, and a link that failed
twice is removed as a failed traversal is.

Off the graph (no node within 32 units) for 3 s while not fighting, a bot drops its path, so the next plan starts from
the nearest node in straight reach (`offGraphReplans`), long before the 20 s watch.


## Objectives

Every frame the controller asks what the bot goes for (`World::mpBotObjective`, `src/game/mpbotgoals.cpp`): a point,
a radius within which it counts as there (flat distance, and 72 units of height), and optionally a use trigger to hold
use in. A goal set through the API wins; otherwise the gametype decides from its own script state. The bots of one team
are ranked by client number; with the role `auto` even ranks attack and odd ranks defend (`botSetRole` fixes a role;
`hunt` drops the objective).

- **dm, tdm**: no objective; the bot hunts (Controller).
- **ctf** (`ctf.gsc:1051-1084`: triggers `allied_flag` / `axis_flag` with `home_origin`, `atbase`; a carrier's `flag`
  field, `ctf.gsc:1178`): a carrier runs to its own flag's home (touching it there captures, `ctf.gsc:1099-1130`) and
  does not stop to fight. Attackers go for the enemy flag (radius 8, they touch it) or, when a team mate carries it,
  escort the carrier (radius 200). Defenders guard the own flag's home (radius 350), chase an enemy carrier, or touch
  the dropped own flag to return it.
- **sd** (`sd.gsc:1433-1545`, `1629-1760`; `game["attackers"]`, `level.bombplanted`): before the plant the attacking
  role walks into a bomb zone (zone = rank modulo the zone count) and holds use there until the plant ends; other
  attackers push to a zone (radius 600), defenders guard one (400). After the plant the defenders walk into the bomb
  trigger and hold use to defuse, the attackers guard it (300).
- **hq** (`hq.gsc:1096-1130`, `1244-1262`; `level.radio`, the shown radio has `hidden` false, `radius`, `team`): every bot
  goes within 0.6 × the radio's radius (defenders of a held radio stay within twice the radius).

Walking: a path is planned to the graph node nearest the objective (within 512 units) and planned again when the
objective moves by more than 128 units or after 3 s; past the last node, closer than 256 units, the bot walks straight
at it. On the objective the bot stops. Fighting stops the walk, except for a flag carrier and a bot holding use.
Holding use: the first frame inside the trigger releases the button, from the next frame on it stays pressed (the use
scan fires a use trigger on a press, docs/spec/mp.md "Triggers for every client"); the gametype's `useButtonPressed`
loop does the rest. While planting the gametype disables the weapon, so the bot does not shoot.

### Goal API
C++ (`src/game/world.h`): `mpBotSetGoal(client, pos, radius, use)`, `mpBotSetGoalEntity(client, entity, radius, use)`
(follows the entity; with `use` the entity is the use trigger), `mpBotClearGoal`, `mpBotSetRole`,
`mpBotSetWeaponClass`, `mpBotObjectiveName`. GSC methods on a bot entity (errors "not a bot" on others): `isBot()` (on
any entity: 1 for a bot), `botSetGoal(origin | entity, radius = 64, holdUse = false)`, `botClearGoal()`,
`botSetRole(role)` (error for another string), `botSetWeaponClass(class)`.

## Weapon classes

Weapon menus (`ui_mp/scriptmenus/weapon_<nation>.menu`) answer with weapon names; `_weapons.gsc:570-760` turns a
weapon whose `scr_allow_<name>` cvar is 0 into `restricted` and the gametype reopens the menu. The cvar of each menu
weapon is listed in `_weapons.gsc:93-205` (a missing cvar is set to 1 at game start, `_weapons.gsc:207-217`). Our
classes come from the weapon files (`weapons/mp/<name>`): `weaponClass` `spread` is `shotgun`, `smg` is `smg`; a
`rifle` with an `adsOverlayShader` is `sniper` (all scoped rifles are also bolt-action, so this test comes first), one
with `semiAuto` 0 is `mg` (BAR, Bren, MP44), one with `boltAction` 1 is `bolt`, the rest `rifle` (semi-automatic).
Pistols are given to everybody (`_weapons.gsc:279`); the grenades have their own
cvars.

| nation | menu weapon: class |
|---|---|
| american | m1carbine rifle, m1garand rifle, thompson smg, bar mg, springfield sniper, greasegun smg, shotgun |
| british | enfield bolt, sten smg, bren mg, m1garand rifle, thompson smg, enfield_scope sniper, shotgun |
| russian | mosin_nagant bolt, svt40 rifle, pps42 smg, ppsh smg, mosin_nagant_sniper sniper, shotgun |
| german | kar98k bolt, g43 rifle, mp40 smg, mp44 mg, kar98k_sniper sniper, shotgun |

`mpMenuWeaponClass` holds this table (the test `mpgametypes` checks every row against the weapon file). It follows that
the Americans have no `bolt` weapon and the Russians no `mg` weapon. A list of classes is valid when it contains a
primary class and every nation's menu has at least one weapon in the list (`mpParseWeaponList`, used by `iw2server
--weapons` and the create-server panel); otherwise a player of that nation would have no entry in the weapon menu and
could not join. `bolt,sniper,grenades` is valid for all four nations, `bolt` alone is not (American), `mg` alone is not
(Russian).

`iw2_weaponclasses` (read by `mpStart`) or `World::mpSetAllowedWeaponClasses` writes the `scr_allow_*` cvar of every
menu weapon. A bot answers a weapon menu with a random weapon among the allowed ones of its class (`botSetWeaponClass`,
else the `bot_weaponclass` cvar, else any class), the menu's first allowed one when its class has none.

## Not done yet (TODO)

- The reload and the weapon switch play no weapon animation or sound. No melee, no lean, no weapon pickups (`dropItem`).
- Objectives: no use of cover, no grenades at the objective, no coordination beyond the role split; the ctf
  carrier waits at its base while its own flag is away.
- Rendering in iw2view: the bot entity carries its team's body model and head attachment from the gametype's
  character script (`_teams.gsc` setPlayerModels); whether iw2view draws it (it would be in the bind pose) is not
  verified, and there are no player animations (stand / run / death) yet.
