# Multiplayer bots (OpenIW2 extension)

**This page describes an OpenIW2 design, not the original game.** The original multiplayer server has test clients
(`addtestclient`) that occupy a client slot but never move, aim or shoot; there is no bot movement code to
reimplement. The navigation graph, the bot controller and the skill presets below are our own. Where the design
leans on original behaviour (the test client connect, pmove, the weapon state, bullets and damage, items, turrets, the
gametype callbacks) the evidence is cited: VAs in the 1.3 Linux dedicated server ("lnxded") for the MP game module, VAs in the
patch 1.3 SP executable for the shared player and AI code, and `file.gsc:line` for shipped scripts.

Implementation: `src/game/navgen.{h,cpp}` (graph), `src/game/mpbots.{h,cpp}` (controller, `addtestclient`),
`src/game/mpbotsense.cpp` (perception), `src/game/mpbotaim.cpp` (aim and shots), `src/game/mpbotcombat.cpp` (the pure
decisions, grenades, the damage reaction), `src/game/mpbotitems.cpp` (weapons from the ground, cover),
`src/game/mpbotturret.cpp` (mounted machine guns), `src/game/mpbotgoals.cpp` (objectives, goal API, weapon classes),
tests in `tests/test_mpbots.cpp`, `tests/test_mpbotcombat.cpp` and `tests/test_mpgametypes.cpp`.

**Design goal.** A bot plays like a person at a keyboard and a mouse. It acts only through what a client's command
carries (move, view angles, the fire / aim / reload / melee / grenade / use buttons, the weapon choice), it knows only
what a player at its place could see on a screen or hear, and its hand is not exact. Nothing a bot does reads another
client's state beyond that: no knowledge through walls, no aim that snaps, no shot that is decided to hit.

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

## Bot limit

Bots are capped at 31 on a server (`kMaxBots`, `src/game/mpbots.h`) although `sv_maxclients` goes to 64 as in the original. `World::mpAddBot` refuses the 32nd, which covers `--bots` (0..31), the console's `addtestclient [N]` (it prints a message at the cap), the script builtin (returns undefined), the create-server list (up to 31) and the listen server. Bots also need free slots (`sv_maxclients` - 1 for the host). Tests: test_serveroptions, test_svconsole, test_mpui, test_mp64.

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
   A node whose box rests higher than the floor under its middle (a rim) and has under 2 units to spare on both sides
   of one axis (a slot as wide as the box, bridged by the box) is dropped. mp_matmata at about 4912 7696: the box
   rested at 45 on the two lips of a crack whose floor is at 37; a bot that steps off the node by a hair slides into
   the crack (see "Stuck watch", "Held in a crevice"). 33 nodes of mp_matmata.
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
     0x50bd90), is swept 24 units (on a grid coarser than 16 units 1.5 cells: the column nearest a ladder can stand
     farther from it; on mp_farmhouse, a 24-unit grid, the ladder up to the loft with the machine gun at about
     −2300 1430 0 was found from no column) in the 8 flat directions; a hit on a surface with flag 0x8 (ladder) that is not a
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

`<cache dir>/<mapname>_<crc>.nav`, little-endian: magic `IW2NAV\0\0`, uint32 version (4), uint64 key, float cell, float
grid origin x, y, uint32 node count, uint32 main node count, the nodes (3 floats each), then per node uint32 link count and per link uint32
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
  (pmove skips only the bot's own entity). The bot's clip box is moved there at once, with the top of its stance (70
  standing, 50 crouched, 30 prone, as a network client's `$clipheight`, docs/spec/mp-pmove.md "Players against each
  other"): the clips are otherwise rebuilt once per server frame, so a bot moved later in the same frame collided with
  where an earlier one had stood, and two bots stepping into one place in one frame ended up inside each other.
- **Perception.** An enemy is any other active, living, playing client (dm: everybody; team gametypes: the other
  session team). The bot notices what is inside the view of its screen at the current zoom with a clear line, after a
  time; what it hears or is hit by gives it a believed place only. See "Perception" and "Hearing" below.
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
- **Fighting.** With an enemy in sight the bot stops following the path, closes in beyond 900 units, backs off within
  250 and strafes, changing side every 0.6–1.5 s. Its view is steered at the enemy's body ("Aim" below).
- **Shooting.** The bot pulls the trigger once the reaction time after noticing this enemy has passed and its view is on
  the point it aims at ("Aim"). The shot leaves along the bot's view, from its eye, with the weapon's own spread, through
  the fire path every client's shot takes (`World::fireWeaponShot`, combat.cpp: damage by distance, MOD by `rifleBullet`,
  hit location on the bone boxes, the player damage callback of the gametype). Nothing decides beforehand whether it
  hits. After `burst` shots it pauses `burstPauseMs`.
- **Weapon state.** A bot does not time its shots itself: its trigger, reload button, aim and weapon choice are the
  input of the player's weapon state machine (`PlayerWeapon`, docs/spec/weapons.md), the same one that drives the local
  player and network players (docs/spec/net.md section 13). So a shot leaves only when the state machine fires it:
  `fireTime` apart, a bolt-action weapon (`boltAction`) rechambers for `rechamberTime` after each shot, a reload takes the
  weapon's reload times (segmented for the shotgun), a weapon change drops and raises, and a semi-automatic weapon
  (`semiAuto`) or bolt-action weapon fires once per press: the bot releases the trigger for a frame between shots. The
  clip and the reserve the state machine uses are the inventory's. Every shot it returns is fired as described
  under "Shooting"; the reload start plays the body's reload event. A new life starts with a fresh weapon state. The
  bot's pmove gets the weapon side of a client's move as a network client's does (docs/spec/net.md section 13): the aim
  fraction, the weapon state, the weapon's move speed scale and its hip spread fields, so the aim spread scale of the
  bot's player state grows with its moves and turns and decays at rest (docs/spec/pmove.md).
- **Command.** At the end of its frame the bot hands the server what a client's command carries besides the move:
  its buttons (use; attack while on a turret) and its view angles and view height (`World::mpBotCommand` →
  `mpSetButtons`, `mpSetView`). The use search and the turret code of the server read them as any client's
  (docs/spec/mp.md "Use search of every client", docs/spec/turrets.md "Multiplayer").

### Skill presets

| skill | name | reaction ms | notice ms | turn °/s | aim error ° | spring ω / damping | lead | fire tolerance | sight | burst | pause ms |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | recruit | 450 | 500 | 180 | 5.5 | 5 / 0.55 | 0.3 | 2.2 | 1500 | 3 | 700 |
| 1 | regular | 320 | 350 | 260 | 3.4 | 7 / 0.65 | 0.5 | 1.6 | 2500 | 4 | 500 |
| 2 | hardened | 230 | 250 | 360 | 2.0 | 9.5 / 0.75 | 0.75 | 1.25 | 3500 | 6 | 350 |
| 3 | veteran | 160 | 160 | 480 | 1.25 | 12 / 0.85 | 0.9 | 1.0 | 5000 | 8 | 250 |

The columns are explained under "Perception" (notice, sight), "Aim" (turn, aim error, spring, lead, fire tolerance)
and "Bursts". A random 0–149 ms is added to every reaction and 0–199 ms to every burst pause.

| skill | aim lag ms | memory ms | hears fire | hears steps | grenade chance | corner checks | breaks off for cover at |
|---|---|---|---|---|---|---|---|
| 0 recruit | 300 | 4000 | 1200 | 300 | 0.25 | no | never |
| 1 regular | 200 | 6000 | 1800 | 400 | 0.45 | no | 30 health |
| 2 hardened | 130 | 8000 | 2500 | 500 | 0.65 | yes | 40 health, or to reload |
| 3 veteran | 70 | 10000 | 3500 | 600 | 0.85 | yes | 45 health, or to reload |

**Traits.** The bots of one skill are not alike: each bot gets three factors between 0.85 and 1.2 when it is added
(from its number, not from the game's random numbers): `reaction` scales its reaction and notice times, `aim` its aim
error, `aggression` the health at which it breaks off (a bold bot stays longer).

## Perception

`World::mpBotSense` (`src/game/mpbotsense.cpp`). The measured fault of the first controller: it kept the enemy it
fought known whatever the view, a veteran saw all around, and 13–43% of the enemies noticed by sight were outside the
picture a player would have had (table under "Measurement"); a bot aiming through a scope turned at once to somebody
at its side.

- **The view.** What a player sees is the picture of a screen: `botViewAngles(fov)` takes the horizontal field of view
  for a 4:3 picture and a 16:9 screen, so the half angles are atan(tan(fov / 2) · 4 / 3) across and
  atan(tan(fov / 2) · 3 / 4) up and down: 48.2° and 32.2° at the hip (fov 80). At the sights the field of view is the
  client's own zoom rule (`adsFov`, docs/spec/weapons.md: 80 towards the weapon's `adsZoomFov` by the aim fraction of
  the bot's weapon state): about 31° across for a rifle (50). Through a scope (a weapon with `adsOverlayShader`, aim
  fraction ≥ 0.5) the picture outside the scope's circle is black, so both half angles are the vertical one: 5.6° for
  `adsZoomFov` 15. On a turret the field of view is the turret view's 55. Nothing outside the view is noticed, the enemy
  the bot fights included: when it leaves the view it is out of sight.
- **Lines.** An enemy inside the view (its middle, widened by half a body's width at that distance) within the
  skill's sight distance is seen when a sight trace from the bot's eye (mask without characters) reaches its head, its
  chest (70% of its height) or its legs (25%); for the enemy the bot fights all three are traced, and the parts with a
  clear line are remembered for the aim. A smoke grenade's cloud hides what is behind it: a line that passes within
  150 units of the middle of a cloud (60 units above where the grenade went off) between 1.5 s and 20 s after it went
  off is blocked.
- **Noticing.** Seeing is not noticing. Each enemy in sight is noticed after `botNoticeMs`: the skill's notice time ×
  (1 + 2 · e²) where e is how far from the middle of the view it is (0 middle, 1 edge: three times as long at the edge)
  × (1 + (d · fov / 80 / 1200)²) for the distance d (a zoomed view brings it closer) × 0.4 when it fired in the last
  300 ms (the muzzle flash), else 0.7 when it moves faster than 50 units/s, 2 when it lies prone, 1.3 when it
  crouches × 0.5 when the bot expects it (the enemy it knows, or the source of a sound or hit), at least 50 ms. The
  progress is kept per enemy; out of sight it falls back over 1.5 s, but an enemy once noticed stays known for 3 s and is
  noticed again at once.
- **Choice.** Among the noticed enemies in sight the bot fights the nearest; the one it already fights counts as 30%
  nearer, so it is not dropped for one a little closer. A newly seen enemy starts the reaction time (half of it for
  the same enemy back in sight within 1.5 s). The last seen place is kept for the skill's memory time; a dead enemy is
  forgotten (the obituary names it).
- **Attention.** A search for new enemies runs every other frame and looks only at the five nearest enemies inside the
  view; the enemy the bot fights is looked at every frame ("Cost limits").

Tests (`test_mpbotcombat behaviour`, scenario 9): an enemy standing still 400–700 units away is noticed in 0.2 s
straight ahead and in 0.4 s at 30° off the middle, and not at all at 60°, 90° or behind; with a scoped rifle aimed it is
noticed straight ahead and not at 15°, 30° or 60°; scenario 10: an enemy behind a smoke cloud is seen only when the
cloud is gone.

## Hearing

What a bot hears or feels gives it a believed place, never the source's place, and only after a delay
(`World::mpBotAlert`); the enemy is then known there (`enemyHeardOnly`) and the bot turns to look and walks there.

- **Weapon fire.** A client's shot (the event that pings the compass, docs/spec/mp-hud.md "Compass"; a client's turret
  fire counts as its own) within the skill's hearing distance of a bot that fights nobody in sight. The believed place
  is the shooter's place turned around the bot by a random angle of up to 12° (veteran) to 25° (recruit) and moved to
  0.75–1.25 times its distance (`botHeardPosition`). The bot knows it after 1.2 × its reaction time plus 0–199 ms; twice
  the reaction time while it looks through a scope.
- **Steps.** An enemy running (faster than 130 units/s) on its feet, not crouched or prone and not aiming, within the
  skill's step distance, checked every fourth frame: the same with an error of up to 30°–45°. Steps are not followed
  while another enemy was known in the last 4 s.
- **A reload.** An enemy whose reload begins (the reload events of its weapon state, for a bot, the listen server's
  player and a network client alike: `MpClient::lastReloadMs`) within the skill's step distance: as steps (30°–45°, the
  same delay), and like steps not followed while another enemy is known.
- **A grenade that lands.** An enemy's grenade bouncing within 500 units of the bot (the bounce sound): the thrower is
  known at a place in the direction the grenade came from, up to 30°–45° off and 0.6–1.4 times its distance, after 1.2 ×
  the reaction time plus 0–199 ms; not while the bot fights an enemy in sight. A frag grenade heard landing is also a
  danger (an enemy's, the bot's own, or one whose thrower is dead or gone; not a team mate's): from the reaction time
  plus 0–149 ms after the bounce until it goes off, the bot runs from where the grenade lies while it is within 350
  units of it, standing and without aiming, also in a fight (not while planting or defusing, not on a traversal):
  straight away from it when a main graph node lies within 40 units of the point 40 units ahead and the body (knee to
  head) gets 32 units that way, else 45°, 90° or 135° to either side. So a walk towards the thrower does not lead over
  the grenade.
- **A grenade that goes off.** An enemy's frag grenade exploding within the skill's hearing distance: an enemy is
  known at the place of the blast, as imprecise as gunfire. The thrower is not where its grenade went off; the bot looks
  and goes where the fight is.
- **A near miss.** An enemy's bullet that passes within 40 units of the bot's eye without hitting it (the closest point
  of the bullet's way from the muzzle to its first impact, more than 64 units from the muzzle): the crack shows where it
  came from about as a hit does, less precisely: after 120 ms plus half the reaction time the shooter is known at a place
  up to 12°–25° off and 0.7–1.3 times its distance, whatever that distance (a shot from beyond the hearing distance is
  noticed this way only).
- **A hit.** The bot dodges at once (its strafe side flips). The hit shows where it came from, as the damage indicator
  does: after 120 ms plus half its reaction time the attacker is known at a place within 8°–15° of its direction and
  0.8–1.2 times its distance. A bot that fights another enemy in sight turns to the attacker on every second hit; for
  1.5 s, or until it sees the attacker, it then fights nobody else.
- A bot that fights an enemy in sight does not turn to fire, steps, reloads, grenades or near misses of others.
- **Which one.** A bot holds one pending cue. A new one replaces it only when it is more urgent: a hit before gunfire,
  grenades and near misses, these before steps and reloads (`BotCue`, `botCueUrgency`); in a fight the shots of several
  enemies come one after another, and an equal one replacing it would put the pending cue off again and again. Another
  cue of the source of the pending one (the next bullet of a burst, the next bounce of a grenade) is not taken. A team
  mate's sounds are never taken for an enemy's (team gametypes).
- **The walk.** A cue makes the bot plan its way to the believed place, unless the way it walks already ends within 300
  units of it, or within a third of its distance (the sounds of one fight each carry their own error; planning anew for
  each sent bots to and fro: on mp_carentan a bot stood 17 s within 64 units, hearing 46 shots).

Tests: `test_mpbotcombat` (an enemy out of sight fires: not known in the same frame, known 150–700 ms later at a
place up to 13° and a quarter of the distance off, not the shooter's place; "more sounds": a veteran hears an enemy out
of sight 250–450 units away reload, later than the reload began, up to 30° off; a recruit looking away is told by a near
miss from 1300–2000 units, beyond its hearing of gunfire, 150–700 ms later, up to 26° and 35% off, and not by a team
mate's, and three bullets 100 ms apart are one cue; a frag grenade landing 100 units in front of a bot is heard, the bot
runs and is more than 150 units farther from it when it goes off; one going off 650–950 units away is heard; three
enemies out of sight 900–1300 units away firing in turn every 150 ms are heard at least four times in 8 s, the bot walks
more than 400 units towards them and plans its way at most twice); `behaviour` scenario 9 (a scoped bot with
an enemy at 60° that fires: heard, then seen 1.3–4 s after the start; a bot hit from behind sees the attacker 5–40
frames after the hit), 15 (an enemy 250–350 units behind the bot is not heard standing or crouched, and is heard
running, at a place up to 30° off, after which the bot turns and sees it) and 17 (a bot that fights an enemy in front
of it and is hit from the side turns to the attacker and fights that one).

## Aim

`src/game/mpbotaim.cpp`. The first controller turned the view at a fixed rate straight at the enemy and decided each
shot's hit or miss with a die (the AI accuracy graph times the skill's accuracy); the view was then set on the shot.
Now the view is all there is: where it points when the weapon fires is where the shot goes.

- **The steered angles.** The bot steers two angles toward the angles it wants like a hand moves a mouse
  (`botAimStep`): a spring with the skill's stiffness ω (rad/s) and damping ratio ζ, acceleration
  ω² · error − 2 ζ ω · rate, the rate limited to the skill's turn rate, in two half steps per 50 ms frame. With ζ below
  1 the view swings past the target and settles: a recruit's 90° turn is there after 0.6 s and overshoots by about 8°,
  a veteran's after 0.45 s with almost none. Turning to an enemy known in the last 3 s but not in sight (heard, hit by,
  just lost), or to a throw, uses 0.8 ω; looking around and along the walk 0.6 ω and at most 60% of the turn rate.
- **What it wants.** Fighting, the angles to the aimed point: the enemy's chest when a line to it is clear, else the
  head, else the legs ("Perception"), taken from the enemy's posed body (`j_spine4`, `j_head`, `j_mainroot`; clients are
  hit on their bone boxes, docs/spec/mp-players.md "Hit locations"), read from the pose every fourth frame. The bot
  follows that point with the skill's aim lag (each frame the followed point moves by min(1, 50 ms / lag) of the
  remaining distance), so it trails a moving enemy by velocity × lag; it makes up the skill's lead fraction of that by
  adding the enemy's velocity × lag × lead. With a grenade in the hand the angles of the throw ("Grenades"); at a weapon
  or turret it is about to use, that; an enemy known in the last 2 s but out of sight, its believed place; else along
  its walk, with corner checks; standing, it looks around ("Looking around").
- **The hand's error.** On top of the steered angles the view carries an error the bot does not see or correct: a
  point that wanders (an Ornstein–Uhlenbeck process, time constant 0.35 s, up and down 0.6 of sideways) with a standard
  deviation of the skill's aim error × the bot's `aim` trait × `botAimNoiseScale`: × 1.5 moving, × 0.8 crouched, × 0.6
  prone, × (1 − 0.4 · aim fraction), × 2 for 600 ms after a hit, × 1.5 while the view turns faster than 100°/s, and
  × (0.5 + e^(−t / 700 ms)) for the time t on this target (1.5 when it is new, 0.5 once settled). Without a target it
  fades. On a turret it is half as large.
- **View kick.** Each shot kicks the view as a player's is kicked: the weapon's `hip/adsViewKick*` impulse and the
  centring spring of docs/spec/weapons.md "Recoil" (`RecoilKick`, the same code as the local player's view). The kick is
  added to the view; it comes back by itself.
- **The view** is the steered angles + the hand's error + the view kick. It is what the server reads, what the killcam
  shows, and the direction of every shot and melee blow.
- **The trigger.** The bot fires when the error it sees is small: the difference between the wanted angles and the
  steered angles plus the view kick (it waits for the sight to come back from the kick; the hand's error it does not
  see) within `botFireTolerance`: 1.3 × half a body's width at that distance (atan(13 / d), at least 0.5°) × the
  skill's fire tolerance, × 1.5 for an automatic weapon within 500 units.
- **The shot** (`World::mpBotFireShot`): along the view, with the spread angle a client's shot has
  (`fireSpreadDegrees` with the weapon's hip spread fields, the aim spread scale of the bot's player state, the aim
  fraction and `adsSpread`, as the network server computes it), and the shot adds `hipSpreadFireAdd` to the aim spread
  scale. A spread weapon fires its pellets, a projectile weapon a rocket, through `World::fireWeaponShot`.

Accuracy is therefore not a parameter: it follows from the aim error, the lag, the spread and the kick against the size
and movement of the target. Measured hit rates are under "Measurement". Tests (`behaviour` scenarios 11, 12 and the pure
tests): a 90° turn of the spring gets there, overshoots more with less damping, settles and never exceeds the turn
rate; with the same semi-automatic rifle against a still enemy at 500–700 units a recruit hits about 23% of its shots
and a veteran about 55%, the view never turns faster than the turn rate plus the kick's motion, and the shots kick the
view; the aimed point is the chest, or the head or the legs when only those are in sight.

## Combat behaviour

All of this is our design (the original has no bot code); it reuses pmove's stances and aim (docs/spec/pmove.md), the
inventory of the gametype's loadout, the weapon state machine (docs/spec/weapons.md) and the grenade missile
(docs/spec/grenades.md). The pure decisions are in `src/game/mpbotcombat.cpp` and tested in
`tests/test_mpbotcombat.cpp`: `botChooseStance`, `botWantAds`, `botBurst`, `botWeaponAction`, `botWantMelee`,
`botItemWant`, `botWantRetreat`, `botGrenadeAngles`, and `botTurretLeave` (`src/game/mpbotturret.cpp`).

- **Ammunition and weapon choice** (`botWeaponAction`). The clip and the reserve are the bot's inventory
  (`World::inventory`), filled by the gametype (`_weapons.gsc:276-313` gives the nation's pistol with full reserve,
  `:315-390` gives frag and smoke grenades by clip ammo, `dm.gsc:428-434` the chosen weapon with full ammunition). A
  shot takes a round from the clip; a reload moves rounds from the reserve into the clip at the weapon state's reload
  times. The bot looks at both slot weapons, whatever their classes (a weapon from the ground may have replaced the
  pistol): the held one and the other one.
  - The held weapon has no round left (clip and reserve): the other one when it has a round. Always, also in a fight
    and at any distance.
  - In a fight with an empty clip: reload; but with the enemy closer than 500 units, a loaded second weapon and a
    reload of more than a second, the second weapon (it is faster).
  - In a fight with two loaded weapons, at most every 3 s: the pistol or smg when the enemy is within 300 units of a
    bolt-action or sniper rifle; the rifle, sniper rifle or mg when the enemy is beyond 1000 units of an smg, shotgun
    or pistol.
  - No enemy in sight: an empty clip is reloaded; after 2 s back to the weapon the bot prefers (one with rounds, a main
    weapon before a pistol, the one it chose in the menu); a tactical reload under half a clip once the enemy has been
    out of sight for 1.5 s, under 70% of a clip when nothing has been known for 3 s; with nothing known for 6 s and
    the held weapon full, the second weapon's empty clip is filled too (change, automatic reload, change back).
  - A weapon change sets the client's weapon; the weapon state drops and raises. The weapon state reloads an empty clip
    by itself (PM_Weapon's automatic reload); the reload button is pressed for the tactical ones.
  The fault this replaces: the first controller knew one case, main weapon to pistol. A bot with an empty pistol in its
  hand and rounds for its main weapon kept the pistol while an enemy was in sight, and a bot without a round in either
  weapon stood strafing in front of its enemy: on mp_brecourt with three recruits, 32% of the bots' time alive.
- **No round left at all.** The bot does not stand and fight: it goes on its way (its objective, a weapon on the
  ground, a turret) and looks along its walk. An enemy within 300 units it charges straight for the melee blow.
- **Melee** (`botWantMelee`). An enemy in sight within 58 units of the eye and within 20° of the view: the melee
  button (a press, released the next frame; the weapon state's melee, the blow's traces along the bot's view as for a
  network client) when the clip is empty, the weapon is reloading or being changed, the weapon is a rifle, bolt-action
  or sniper rifle or a pistol, or the enemy is within 40 units; a loaded smg, mg or shotgun fires instead.
- **Stance** (`botChooseStance`). Standing whenever the bot walks a path or an objective, climbs or is not fighting.
  In a fight: a sniper rifle at 1000 units or more lies prone; when crouching hides the bot from the enemy (a trace
  from 40 units above the feet to the enemy's chest is blocked while the standing eye sees it, checked every 500 ms)
  the bot crouches while it reloads or pauses between bursts and stands up to fire; at 800 units or more a rifle,
  sniper or mg crouches; otherwise it stands. Behind cover ("Cover") it crouches.
- **Strafing and dodging.** Only a standing bot strafes; crouched and prone bots stay where they are. A
  combat step is taken only when a main graph node lies within 40 units of the point 40 units ahead (else the other
  strafe side, else no step), so that a fight does not carry the bot off a ledge into a place with no way back. A hit
  by an enemy flips the strafe side at once ("Hearing").
- **Aiming down the sights** (`botWantAds`): sniper from 400 units, rifle and mg from 500, smg from 450 (from the hip
  its spread is wider than a body beyond that), never with a shotgun or pistol; only in a fight and not while walking
  a path or climbing. The aim command goes to pmove with the weapon allowed to aim, so the move slow-down of an aiming
  player applies; the aim fraction is the weapon state's. A bot that loses its enemy from sight while aiming (out of
  the scope's circle, behind a post) keeps its aim for 0.7 s while its view follows to where the enemy was, and only
  then lowers the weapon to look; otherwise a scoped bot would go in and out of its scope on every sidestep of its
  enemy.
- **Bursts** (`botBurst`): automatic weapons (smg, mg) fire the skill's burst within 600 units and half of it (at least 2)
  farther, with 1.5 × the pause; a rifle, sniper or shotgun fires single shots at its `fireTime`; a pistol fires 3 then
  pauses. The trigger rule of "Aim" also stops a burst while the kick has the sight off the target.
- **Grenades.** At most every 2 s (by client number), when the bot has a frag grenade, stands on the ground, is not
  climbing and threw none in the last 6 s: a target is either an enemy known in the last 3 s but not in sight now
  (behind cover, at its believed place) or an enemy in sight with another enemy the bot also sees within 200 units of
  it, 250..1400 units away, with no team mate (team gametypes) and not the bot itself within 256 units of the target.
  With the skill's grenade chance it throws as a player does: `botGrenadeAngles` finds the view angles of a throw that
  lands there (a grenade leaves along the view at the weapon's `projectileSpeed` plus `projectileSpeedUp` upwards,
  docs/spec/grenades.md; the low arc, else the high one, the view at most 85° up), the arc is traced in 50 ms steps
  against the grenade clip mask and must not hit anything more than 120 units before the target. The bot then holds the
  grenade button (the weapon state's offhand states: put the weapon away, pull back), turns its view to those angles,
  and lets go once the view is within 4° of them or 1.2 s have passed; the grenade leaves along the bot's view through
  the client's throw (`World::clientThrowGrenade`, with the bot's velocity and the cooked fuse). A view that is not there
  yet throws short or wide.
- **Smoke.** The gametype gives smoke grenades only with an smg or the shotgun (`_weapons.gsc:464-492`). In sd
  (attacking team before the plant) and ctf (the flag carrier, or an attacker) a bot with a smoke grenade and an enemy
  in sight, or known in the last 3 s, 400..1500 units away throws it once per life at the point a third of the way to
  the enemy. An sd attacker on its way to a bomb zone (plant, push zone) 500..1200 units ahead throws it two thirds of
  the way to the zone, to cover its approach. Smoke hides what is behind it from every bot ("Perception").
- **Corner checks** (hardened and veteran). Walking with no known enemy, every 1.5–3 s the bot looks for 700 ms along the
  most open of the four directions 45° and 90° left and right of its walk (sight traces of 512 units from the eye).
- **Looking around.** A bot that stands with nobody known (a guard on its objective, a bot planting or defusing)
  does not stare along the way it came: every 1.5–4 s it turns to another open direction about it (sight traces of
  600 units in twelve directions 30° apart from a random start; one of those at least 30% free, the more open the
  likelier, not within 40° of the one it just looked along). Without this the guards of sd never saw the attackers come.
  Test: `behaviour` scenario 16.

### Detours

A detour is a place the bot leaves its way for (`BotDetour`): a weapon on the ground, the place of its last kill, a
turret, cover. While one is on, it is the bot's objective (the walk, the replans and the stuck watch are those of
"Objectives"). No detour for a flag carrier, while holding use, within 600 units of a plant or defuse, or with a goal
set through the API; on a ladder / mantle / leap a detour goes on but none is begun. Cover comes before a weapon, a
weapon before a turret. A detour the bot makes no way to (a second without 24 units of progress while walking to it) is
given up at once, and the item's place (45 s) or the turret (60 s) is left alone, so that a bot does not press against
what stands between it and the thing. After a map restart the bots gather the turrets and items of the map again.

### Weapons from the ground

The gametype drops a dead player's weapon with `dropItem` (`_weapons.gsc:396-428`; docs/spec/mp.md "Dropped weapons");
the shipped maps place no weapon items of their own. A bot takes them as a player does, through the server's item
touch and use of every client (`World::mpClientItems`, `World::mpClientUseSearch`).

- **What it sees.** Twice a second the weapon items on the ground are listed for all bots; each bot without a detour
  looks at those within 700 units that are inside its view at the hip (or within 64 units) with a clear line from its
  eye and a main graph node within 48 units (an item on a roof or in a pit is left alone). How many rounds an item
  carries a player cannot see, and neither does the bot.
- **What it wants** (`botItemWant`, a desire 0..1; the bot leaves its way for an item within desire × 700 units):
  ammunition for a weapon it holds when that weapon has no round (0.9) or its reserve is under half of `maxAmmo` (0.6):
  taken by walking over the item (the touch gives an owned weapon's ammunition). A weapon it does not have, taken with
  the use button: into a free slot (0.8); in place of a slot weapon without a round (0.9; 1.0 when neither has one); in
  place of the pistol, a second main weapon (0.5; 0.7 when it covers the other distance than the main weapon: a
  rifle, bolt-action rifle, sniper rifle or mg against an smg or shotgun); in place of a main weapon with less than a
  clip left (0.6); in place of its worse weapon when the item is two ranks better (mg 4, smg and rifle 3, bolt-action
  rifle, sniper rifle and shotgun 2, pistol 0; 0.4). Grenades are never sought.
- **Taking it.** The bot walks to the item (onto it for ammunition). For a new weapon it stops within 26 units, looks
  at the item, holds the weapon the new one is to replace (in multiplayer the new weapon takes the place of the held
  one, docs/spec/inventory.md), and when the use search of its client offers that item (the hint a player would see) it
  presses use for one frame. The server swaps the weapons; the bot's weapon state takes the swap as a network client's
  does (the held weapon was taken: no put-away). The place is then left alone for 30 s, so the weapon the bot just laid
  down is not taken back. An item not reached within 4 s plus the walk at 100 units/s is left alone for 45 s. In a team
  game a bot leaves an item to a team mate that is already on its way to it.
- **The place of the last kill.** A bot with a slot weapon without a round and no item in sight walks to where its
  last kill of the past 30 s fell (within 1500 units, a main graph node within 64 units of it; given up after 6 s plus
  20 ms a unit): the dead player's weapon lies there.

Tests (`behaviour` scenarios 5, 6, 18): a bot whose main weapon has no round takes a dropped weapon 200–400 units ahead
with exactly one press of use and leaves its own there; a bot with an empty reserve walks over a weapon like its own
and has the rounds without pressing use; a bot whose main weapon has no round walks to the place of its last kill.

### Turrets

`src/game/mpbotturret.cpp`. A bot uses a map's mounted machine gun as a player does (docs/spec/turrets.md
"Multiplayer": the server's turret use of every client).

- **What the bots know of the map** (once per map, as a player knows it): for every usable turret (turret flag 0x1000)
  the middle and half width of its arcs (the setup arcs a user gets), the place to use it from, and how much it
  overlooks. The place is the main graph node nearest the point 40 units behind the gun that passes the use's own
  tests: behind the gun (within the half arc of the arc's middle, as `turretUsableBy`), the standing eye within 68
  units of the turret's box centre, a clear sight line to it. The coverage is the number of sampled main nodes (about
  600 per map) 200–3000 units from the gun inside its arcs with a clear line from the gun. A turret without such a place,
  or with fewer than 12 covered samples, is not used; after two bots stood at the place and were not offered the turret
  it is given up for the map. Every usable turret of the 15 shipped maps has a place (`test_navmaps` checks it): the one
  that had none, the gun in the loft of mp_farmhouse (−2236 1329 293), stood above floor nodes the graph had pruned,
  because the ladder up to the loft was not found on that map's coarse grid ("Ladder, mantle and leap links").
- **When a bot goes for one** (checked once a second; not for 5 s after a spawn, not for 20–30 s after getting off,
  not the turret it just left for 60 s; only a free turret, and in a team game one no team mate is on its way to),
  within 600 units:
  an enemy known in the last 8 s, more than 400 units from the bot, at a place the gun overlooks (inside the arcs
  narrowed by 5°, 150–3000 units, a clear line from the gun); a guard (`guard flag`, `guard zone`, `guard bomb`,
  `hold radio`) whose objective the gun overlooks; with nothing known, 30% of the checks for a gun within 350 units
  that covers 25 samples or more. In a fight only against an enemy beyond 500 units, for a gun within 200 units that
  overlooks it. A bot without a round goes up to 1200 units for a gun (it has endless ammunition).
- **Getting on.** The bot walks to the place, looks at the gun, and presses use for one frame when the use search of
  its client offers the turret (behind the gun, on the ground, no grenade in the hand).
- **On the gun** the turret code holds the bot behind the gun; the bot's own movement, hand weapon and detours rest
  and the stuck watches are off. Its view, kept inside the turret's arcs (the view clamp of the use), is steered as in
  "Aim" at the enemy in sight, with half the hand's error; an enemy known in the last 3 s is looked at where it was;
  with nobody known the view sweeps slowly over three quarters of the arcs. The bot holds attack while the steered view
  is within 4° of the enemy and the reaction time has passed, in bursts of 0.6–1.6 s with the skill's pause (plus
  0–299 ms), and for a second more at the place an enemy just left. The gun fires along its barrel with its own spread
  (the turret code's shot).
- **Getting off** (`botTurretLeave`; a press of use): hit from a direction more than 5° outside the arcs (flanked); health
  under 35; an enemy in sight where the gun cannot turn to for more than a second; nobody seen for the hold time (12–25
  s, a guard 30–60 s); on the gun for three hold times. The server puts the bot back where it got on.

Tests (`behaviour` scenarios 7, 8, 21): a bot near a turret that overlooks an enemy gets on with one press of use, fires 20
shots within 10 s and hits, fires no hand weapon meanwhile, gets off within 3 frames of a bullet from outside the
arcs and stands where it got on; alone on a gun it gets off after 11–27 s without a shot; at the foot of the ladder of
the mp_farmhouse loft, knowing an enemy out where the loft gun overlooks, a bot climbs up and gets on that gun.

### Cover

`botWantRetreat`, `World::mpBotFindCover`, `World::mpBotCoverDetour`. A bot (not a recruit) that was hit in the last
3 s and is down to 30 (regular), 40 (hardened) or 45 (veteran) health × (2 − its `aggression` trait), or a hardened
bot or veteran with an empty clip and an enemy in sight within 900 units, breaks off: it goes to the nearest main graph
node 48–320 units away (within 60 of height, of the 24 nearest) that the enemy does not see: the lines from 64 units
above the enemy's believed place to the top of a crouched head (50 units), the knees (18) and both shoulders (15 units
to either side at 46) there are blocked; a node the bot reaches in a straight line before the others. It keeps
fighting on the way, crouches there looking at where the enemy was, reloads, and comes out when its health is back
to 75 with a loaded weapon, after 8 s, or when the enemy has come round into its sight (after 1.5 s); a place not
reached in 5 s is given up. Then no cover for 8 s.

Test (`behaviour` scenario 14): a hardened bot hurt to 25 health in a fight is out of its enemy's sight within 5 s,
crouches there, and with its health back leaves and fires again.

## Measurement

`test_mpbotcombat stats <map> <gametype> <bots> <skill> <seconds> [scripted]` runs bots of one skill and prints what
the counters of `MpBotStats` and the game log hold. With `scripted` a scripted player is among them: a client that is
no bot, walks the graph from node to node at 190 units/s and, when a bot is within 1500 units in front of it (60° to
either side) with a clear line between the eyes, turns to it and half a second later fires its weapon at the bot's
chest every 150 ms with an aim error of up to about 1.5°; it is the same before and after a change of the bots.
*Defenceless* is the time a bot had an enemy in sight and nothing to fight with (the held weapon ready with an empty
clip, or no weapon; not a reload, a weapon change, a melee or a throw). *Outside the view* is the share of the enemies
noticed by sight that were outside the picture of the bot's screen at that moment.

*Hit rate* is the share of the bots' hand-weapon shots that hit an enemy (`MpBotStats::hits` / `shots`): a shot counts
once whatever its pellets or passes, a killing hit once, a team mate hit not at all; `classShots` / `classHits` and
`bandShots` / `bandHits` split it by weapon class and by the distance to the enemy fought (under 500, 500–1000, over
1000 units). Until this counting, every bullet damage of a hand weapon counted as a hit: a shotgun's pellets each, a
killing bullet twice (its damage and the death) and team mates too. On the same mp_carentan game (440 kills, 4085 shots)
that gave 1284 hits (31.4%) where 646 shots (15.8%) hit an enemy; the rates of 31–51% that looked too strong were this.
The runs print the number of rounds: an sd game is many rounds, and a round's restart (a fast restart of the level)
keeps what belongs to a bot: its counters, skill, traits, role, weapon class and a goal at a place
(`World::mpFastRestart`). Tests: `behaviour` scenario 22 (a shotgun blast is one shot and at most one hit; a dummy that
dies of the first hit gives one hit and one kill; a team mate just in front of the enemy takes the bullets and they are
no hits); `test_mpbotcombat` (in three minutes of sd with at least one round restart the counters only grow and every
bot keeps its weapon class).

Before = the first controller (omniscient within its cone, hit or miss by a die; its hit rates by the old counting,
marked *), after = this page.

| run | | kills | hit rate | defenceless | weapons / ammunition picked up | turret: mounts, time, kills | noticed outside the view | stuck events, longest still |
|---|---|---|---|---|---|---|---|---|
| mp_toujane dm, 8 recruits + scripted, 600 s | before | 161 | 8.1%* | 0 s | 0 / 3 | 0 | 19% | 0, 11.8 s |
| | after | 188 | 9.3% | 0 s | 62 / 5 | 10, 135 s, 5 | 0% | 0, 7.0 s |
| mp_toujane dm, 8 regulars + scripted, 600 s | before | 226 | 17.2%* | 0 s | 0 / 2 | 0 | 26% | 0, 8.5 s |
| | after | 232 | 12.5% | 0 s | 76 / 5 | 6, 50 s, 4 | 0% | 0, 7.7 s |
| mp_toujane dm, 8 veterans + scripted, 600 s | before | 360 | 24.0%* | 0 s | 0 / 1 | 0 | 43% | 0, 10.5 s |
| | after | 309 | 25.0% | 0 s | 66 / 1 | 9, 56 s, 6 | 0% | 0, 9.8 s |
| mp_brecourt dm, 3 recruits, 3600 s | before | 51 | 2.8%* | 3371 s (31.6% of the time alive) | 0 / 13 | 0 | 14% | 0, 8.8 s |
| | after | 166 | 10.1% | 0 s | 40 / 26 | 4, 47 s, 1 | 0% | 0, 6.2 s |
| mp_carentan tdm, 12 regulars, 900 s | before | 335 (190 by grenade) | 20.1%* | 0 s | 0 / 12 | 0 | 14% | 3, 29.9 s |
| | after | 381 (54 by grenade) | 13.7% | 0 s | 217 / 30 | 11, 282 s, 29 | 0% | 0, 10.5 s |
| mp_dawnville ctf, 10 hardened, 900 s | before | 139 | 15.1%* | 0 s | 0 / 12 | 0 | 13% | 2, 27.4 s |
| | after | 162 | 19.1% | 0 s | 106 / 7 | 9, 205 s, 1 | 0% | 0, 12.0 s |
| mp_decoy dm, 31 regulars, 300 s | before | 278 | 23.6%* | 0 s | 0 / 2 | 0 | 31% | 0, 6.5 s |
| | after | 283 | 15.4% | 0 s | 58 / 2 | 14, 49 s, 10 | 0% | 0, 5.7 s |
| mp_toujane sd, 10 hardened, 900 s (14 rounds) | after | 88 | 21.8% | 0 s | 27 / 1 | 8, 134 s, 1 | 0% | 0, 7.8 s |
| mp_matmata sd, 10 regulars, 900 s (15 rounds) | after | 86 | 14.7% | 0 s | 28 / 1 | 1, 12 s, 0 | 0% | 1, 24.8 s |
| mp_farmhouse tdm, 12 hardened, 900 s | after | 373 | 18.3% | 0 s | 225 / 17 | 1, 13 s, 0 | 0% | 0, 6.8 s |

Against the scripted player on mp_toujane (bots killed it / it killed bots): recruits 4 / 80 before and 3 / 88 after,
regulars 15 / 78 and 13 / 101, veterans 34 / 51 and 28 / 63: the bots do not know of it before they see it. No bot was
trapped in any run. The stuck events of mp_dawnville ctf (before 2, then 5 with the longest spell 36.5 s) were flag
carriers behind escorts on the rubble ("Giving way"); none now. The one on mp_matmata sd was a bot that fell into a
crevice at about 4913 7695 37, 8 units under a node (fixed since, see "Held in a crevice"; the table row is from before and
was not measured again). The runs differ by a few percent from
one version of the controller to the next: every change shifts the game's random numbers.

### Hit rates

Targets, from what people hit in this kind of game: a typical player hits roughly 15–30% of the shots of a rifle (a
weak player below that, a good one at the top), fewer with an smg at range. Measured (all hand weapons; rifles alone;
by the distance to the enemy fought):

| skill | mp_toujane dm, 8 bots + scripted | mp_carentan tdm, 12 bots | rifles (toujane / carentan) | toujane: < 500, 500–1000, > 1000 units |
|---|---|---|---|---|
| recruit | 9.3% | 11.1% | 9.4% / 10.3% | 8.2%, 10.2%, 13.5% |
| regular | 12.5% | 13.7% | 11.2% / 13.0% | 12.9%, 14.5%, 9.4% |
| hardened | 18.5% | 20.4% | 22.4% / 19.1% | 18.7%, 21.8%, 14.5% |
| veteran | 25.0% | 24.6% | 22.5% / 25.0% | 27.9%, 27.7%, 21.5% |

An smg hits less than a rifle of the same skill in every run (toujane: 7.9%, 8.8%, 15.5%, 20.8%; carentan: 8.0%, 10.4%,
16.6%, 18.5%), and beyond 1000 units every skill hits less than closer (carentan regulars 17.7% under 500 units,
10.4% beyond 1000). With scoped rifles only (`IW2_BOT_CLASS=sniper`, the pistol too): regulars 16.4% / 15.7%, veterans
26.8% / 29.6% (toujane / carentan). The aim errors of the skill presets are unchanged: with the counting above,
recruit..veteran run from about 9–11% to 25%, hardened and veteran inside the 15–30% of a typical to a good player,
recruit and regular below it (their rifles 9–13%).

### Off-graph spawn points

The four upper floors of "Coverage of the shipped maps" are left only by a drop, so they are not in the strongly connected
part. The generator keeps the way down from them as *exit-only* nodes: after the strongly connected part (the *main*
nodes, numbered first, `NavGraph::mainCount`) come the nodes reachable from a spawn point's off-main node that lead into
the main part. A bot that spawns there plans from
its exit-only start node to a main node and drops down; goals (objectives, enemies' positions, random roaming) are only
chosen among main nodes (`nearest(pos, r, true)`), so no bot is sent up there.

### Cost limits

At most 2 A* plans are run per server frame for all bots together; a bot over the limit plans in a later frame (it keeps
standing meanwhile), and the periodic replans are spread by client number. The search for a new enemy runs every other
frame per bot (half the bots in each frame) and traces only to the five nearest enemies inside the view; the enemy the
bot fights is checked every frame. What perception reads of every client (playing, team, place, eye, speed, stance) is
gathered once per frame for all bots, and the weapon file fields a bot reads every frame once per weapon
(`BotClientInfo`, `BotWeaponInfo`). The enemy's pose is read for the aimed point every fourth frame. The item list is
refreshed twice a second, a bot's item check runs twice a second, its turret check once a second, a cover search at
most once a second. Measured on mp_decoy with 31 bots (the limit): the bots' part of a server frame is 0.9–1.2 ms on
average (0.7–0.9 ms with the first controller), 2% of a 50 ms frame; `test_mpbotcombat perf` checks 15 bots.

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

A traversal that does not move the bot 8 units in 1.5 s has failed too (its way is blocked, e.g. by a turret in the
window the leap goes through), like one that takes 8 s (test: `behaviour` scenario 19, somebody standing between the bot
and the ladder). A ladder, mantle or leap link whose traversal failed twice (8 s, 1.5 s without moving, or carried off) is removed from the map's graph for
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

#### Held in a crevice

The crevice of mp_matmata at about 4913 7695 37 held a bot for the whole 20 s watch (a recorded run: stuck 24.8 s). Its
box rests on the floor of a slot as wide as itself (the slot's two faces are slanted, one by 21 degrees) and starts inside
the faces by the trace epsilon, so the 0.25-unit ground trace of pmove starts in solid and finds no ground
(`PM_GroundTrace`, docs/spec/pmove.md), every walk direction is clipped to nothing against the faces, and a jump needs the
ground. A bot reached the spot by a leap that landed on the rim (45) short of its target and walked on towards the
target across the crack. The same box lifted by 0.03 units walks out in every direction, so this is a numerical trap of
the box against the faces, not a wall.

The graph generator drops the rim node over this crack ("Navigation graph"). The recovery (`World::mpBotUnwedge`) is for any
spot of the kind: when the stuck watch's second finds a walking bot (not fighting, not on a traversal) without ground, not
having moved a unit, it lifts the box to the first of 1, 2, 4, 8, 12, 18 and 24 units above where it stands in which the
box is clear (nudged sideways by up to 2 units when the faces hold it at every height), gives it a jump's upward speed
(270) and 150 units a second along the first of eight directions, the walk direction first, in which the box raised by a
step gets 40 units, and plans the path again (`unwedges`, "UNWEDGE" with `IW2_BOT_STUCKDBG`). A run of mp_matmata sd with 14
bots for 1800 s at four skills: 3 to 9 lifts per run at other spots of the rock fields, no stuck event, the
longest still spell 8.7 to 15.8 s (it was 25 s). Those spots are not removed from the graph: TODO(spec) the nodes there.
Test: `test_mpbotcombat crevice` (ctest `mpbotcrevice_matmata`): no node on the rim; a bot put into the crevice is held
(no ground) and is 64 units away within 8 s, with a lift and no stuck event (2.3 s measured).

### Giving way

`botPushedBy`, `World::mpBotGiveWay`. In a team gametype a bot steps out of the way of a team mate that wants to walk
into it: the mate stands within 44 units (flat; heights within 60) and its move points within 60° of the line to the
bot (a bot's walk of its last frame; a player's command move turned by its view yaw: the listen server's own player, a
network client). The bot steps across the mate's way on the side it already stands, else the other side, else ahead
along the mate's way (each when a main node lies within 40 units of the point 40 units ahead and the body, knee to head,
gets 24 units that way) for a second. A bot that walks itself gives way only to a mate it walks into head-on (its own
walk within 60° of the line to the mate) and only when its client number is the higher. Not in a fight, on a
traversal, while holding use or behind cover. Enemies are not asked.

The fault this mends: on the rubble of mp_dawnville (about −340 −16780 50, a passage one body wide) ctf carriers stood
stuck for 20–37 s: escorts (objective "escort", reached within 200 units of the carrier) stood in the passage ahead of
it, and a carrier that could not pass planned the same way again. The graph there was sound (its walk links pass pmove).
Test (`behaviour` scenario 20): there a team mate parked on its objective steps aside and the bot is through within 8 s;
an enemy parked there is not asked.


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

- The reload and the weapon switch play no weapon animation or sound. No lean, no hold breath through a scope.
- A bot does not hold a place with a long view on its own (a sniper's post), does not peek around a corner step by
  step, and does not act together with its team beyond the role split; the ctf carrier waits at its base while its own
  flag is away. No grenades at the objective.
- Sounds a bot does not hear: a team mate's death, a weapon change.
- Rendering in iw2view: the bot entity carries its team's body model and head attachment from the gametype's
  character script (`_teams.gsc` setPlayerModels); whether iw2view draws it (it would be in the bind pose) is not
  verified, and there are no player animations (stand / run / death) yet.
