# Debug flags and environment variables

Developer switches of `iw2view` / `iw2game`. None change gameplay when unset. The full list of `IW2_*` variables is
`grep -rhoE 'getenv\("IW2_[A-Z0-9_]+"\)' apps src tools`; this page covers the ones added recently. Others are
described in the code that reads them.

## Environment variables
| Variable | Tool | Effect |
|---|---|---|
| `IW2_NO_CULL=1` | iw2view | Disables frustum culling of animated-entity skinning (actors are skinned even when off screen). For comparing output/perf. |
| `IW2_SPEC_SCALE=x` | iw2view | r_specularColorScale (DLL dvar 0x101ce638, default 1, clamped 0.01..100): scales lightSpecular0 of the lmap_s path. |
| `IW2_COS_SHIFT=x` | iw2view | r_cosinePowerMapShift (DLL dvar 0x101ce680, default 0, clamped -1..1): shifts the `$specularity` power ramp (t = clamp(x/30 + shift)). |
| `IW2_NO_SPEC=1` | iw2view | Disables the lmap_s specular path (world batches of `phong_*_spec*` materials fall back to the plain lightmap shader, no normal map). For A/B comparison. |
| `IW2_NO_DETAIL=1` | iw2view | Disables detail-map blending (lib/shade.hlsl USE_DETAIL) on the non-spec lightmap/model path (`phong_*_detail`: lmap_dtl, lmap_a_dtl, lprobe_dtl). For A/B comparison. |
| `IW2_NO_GLOW=1` | iw2view | Disables the glow/bloom post chain (glow_setup, two blur layers, glow_apply_bloom/sky_bleed); the scene then goes straight to the swapchain. For A/B comparison. |
| `IW2_NO_PROBE=1` | iw2view | Entities and the view model use the old single up-normal light-grid colour instead of per-pixel ProbeLighting(N). For A/B comparison. |
| `IW2_PROF=N` | iw2view | Prints average per-frame CPU timings every N frames and quits after two reports. |
| `IW2_SHOT_SCRIPT=t:action,...` | iw2view `--shot` | Scripted weapon input before the shot (actions `reload+/-`, `fire+/-`, `ads+/-`, `frag+/-`, `smoke+/-`, `melee+/-`, `binoc+/-` (binoculars 0x4000), `use+/-` (0x28, binocular fire), `weapon=name`, `turn=<deg/s>` (view yaw change per 20 ms step from then on; a script with it also plays each shot's view flash / brass at its step and updates the effects every step like client frames, logging `turn flash yaw ... tag ...`, so firing while turning can be checked); times in seconds) for `IW2_SHOT_SECONDS` (default 3); logs `shot-script` lines (state, anim, clip, stock, ads every 0.26 s, events). Melee and throw events act on the world. |
| `IW2_VM_TRACE=1` | iw2view | Prints the view model anim, frame, time and hand/gun models each time it is added. |
| `IW2_DEBUG_ITEM=name[@seconds]` | iw2view `--shot` | Zeroes the player's ammo, then drops weapon_<name> on the player at that simulation time (default 0) and prints the inventory 1 s later (touch pickup check). |
| `IW2_NO_HUD=1` | iw2view `--shot` | Draws no script HUD and no HUD fade. |
| `IW2_FX_DEBUG=1` | iw2view | Prints effect sprite debug lines, including the view-model flash tag. |
| `IW2_FX_TEST=<efx>` | iw2view `--shot` | Plays the effect 150 units ahead of the camera, aged `IW2_FX_MS` ms (use with `IW2_NO_HUD=1`, otherwise the script fade-in can make the shot black). |
| `IW2_FX_TEST=file:<path>` | iw2view `--shot` | As `IW2_FX_TEST`, but parses an effect file from the local disk (for element types no shipped effect uses, e.g. Cylinder). |
| `IW2_FX_STEP=<ms>` | iw2view `--shot` | With `IW2_FX_TEST`, updates the effects every `<ms>` from the play time to the shot (client-like frames), so element updates such as emitFx trails and bounces run with that frame time. |
| `IW2_FX_TEST_DIST=<units>` | iw2view `--shot` | Distance ahead of the camera at which `IW2_FX_TEST` plays its effect (default 150). |
| `IW2_OUTDOOR_DUMP=<file.pgm>` | iw2view | Writes the outdoor map (game/outdoor.h, 512 x 512 heights, row 0 = world mins.y) when it is built for the first Cloud. |
| `IW2_NO_OUTDOOR=1` | iw2view | Skips the outdoor-map test of particle_cloud_outdoor clouds (flakes also show under cover). |
| `IW2_TRACER_TEST=N` | iw2view | With `--shot`, N tracer attempts (the 0.4 chance applies) from beside the view to 1500 units ahead, aged `IW2_FX_MS` (default 100). |
| `IW2_FIRE_TURRETS=N` | iw2view | With `--shot`, fires every turret entity (`World::debugFireTurrets`, prints each) in the last 3 simulation frames and puts the camera 150 units beside and 60 above turret N (toujane: 4 is a ground MG42); use `IW2_FX_MS=40` so the 100 ms muzzle flash has grown. |
| `IW2_FIRE_VEHICLES=1` | iw2view | With `--shot`, sends the script notify `turret_fire` to every vehicle in the last 3 simulation frames (combine with `--follow-vehicle N --follow-dist D`; elalamein 2 is a flak88). The iw2game variant takes a time in seconds. |
| `IW2_FX_HIST=1` | iw2view | With `IW2_FX_DEBUG`, prints the 12 largest per-element particle counts (type, name, first shader) each frame, and (once per element) a `fx skip element type ...` line for elements whose type is not implemented. |
| `IW2_RIDE_TRACE=1` | iw2view | Per-frame trace of the ridden vehicle (origin, angles, turret, camera link). |
| `IW2_CROSSHAIR_DEBUG=1` | iw2game/iw2view | Prints the crosshair trace eye/yaw and the hit entity/target name. |
| `IW2_PRONE_LOG=1` | iw2game/iw2view | Prints each `checkprone` call (actor, origin, yaw, alreadyProne, the answer and the two pitch outputs of the fit test 0x4fb530), each `enterprone` (length, fit result, pitches) and each `exitprone`. |
| `IW2_NO_PRONE_TILT=1` | iw2view | Draws actors and corpses without the prone / corpse-blend tag_origin control (A/B check of 0x4c1cc0). |
| `IW2_SHOT_ENT=N[,dist,yaw,height]` | iw2view `--shot` | Puts the shot camera `dist` (120) units from entity N at its yaw + `yaw` (90) and `height` (50) above its origin, looking at it. |
| `IW2_SHOT_FREE_CAM=1` | iw2view `--shot` | Keeps the `--pos` camera when a script link carries the player (otherwise the shot is taken from the linked player, e.g. in the duhoc_assault boat). |
| `IW2_FACING_LOG=N` | iw2game/iw2view | Per think of actor N (-1: all): facing mode (override), path count, movemode, yaw after the turn, active target, motion yaw and the trail sum (0x505d80 / 0x504ca0). |
| `IW2_PLAYER_DMG_LOG=1` | iw2game/iw2view | Prints every damage the player takes: time, amount, attacker entity / classname and means of death. |
| `IW2_TOUCH_TEST=<s>` | iw2game | At sim time `<s>` runs the vehicle touch callback (0x512d40) for the first vehicle with collisionDamage > 0 placed 100 units behind the player, moving at 400 units/s at it; prints the target health before and after; `IW2_TEST_ACTOR=1` targets the first living actor instead of the player. |
| `IW2_TEST_MOVER=1` | iw2game | With `IW2_TOUCH_TEST`: targets the first script_model (take-damage on, health 100) to check the eType 5 crush (0x512dd1, 999999 at once). |
| `IW2_ROCKET_TEST=<s>` | iw2game | At sim time `<s>` fires the first vehicle's turret rocket at the player from 300 units away; prints the player health before and 3 s after. |
| `IW2_KILL_VERBOSE=1` | iw2game | With `IW2_KILL_VEHICLES`, also prints each vehicle's model and health before the kill. |
| `IW2_FIRE_VEHICLES=<s>` | iw2game | At sim time `<s>` sends the script notify `turret_fire` to every vehicle (the AI tank fire loop in `_tank.gsc`), to test vehicle weapons headless. |
| `IW2_KILL_VEHICLES=<s>` | iw2view `--shot` | Same as the iw2game variant below, at sim time `<s>` of the shot run (combine with `--follow-vehicle N`, `--sim-seconds`). |
| `IW2_KILL_VEHICLES=<s>` | iw2game | At sim time `<s>` deals 100000 damage (attacker: the player) to every script vehicle, to test the death chain. |
| `IW2_THREADS_PREFIX=<p>` | iw2game | Lists every live script thread whose wait site starts with `<p>`. |
| `IW2_NOTIFY_LOG=level` or `all` | iw2game | Prints every script notify (`level`: only on the level object; `all`: every object) with its sim time and, when the first argument is a string, that argument in quotes (the notetrack name of an animation flag notify). For walking mission flows. |
| `IW2_SOUND_LOG=1` | iw2game | Prints every sound start (`[sound <ms>ms] alias`). Use with `--mixer` so scripts get the real sound lengths (dialogue timing). |
| `IW2_MELEE_LOG=1` | iw2game, iw2view | Prints every player melee trace (`[melee] trace i from .. to ..: fraction, entity, classname, surface flags`). |
| `IW2_ACTOR_DUMP=<team\|all>` | iw2game | At the end of the run prints `actor #N classname (origin) script_noteworthy` for every living actor of the team (`axis`, `allies`, ...) or of all. Used to aim `--player-fire-at`. |
| `IW2_CHAIN_LOG=1` | iw2game | Prints a line whenever a `trigger_friendlychain` changes the player's friendly chain node (trigger entity number, node name, time). Also prints `setgoalentity at <ms> from <file:line function>` for every `setgoalentity` call. |
| `IW2_TRIGGER_DUMP=<prefix>` | iw2game | At the end of the run prints the world bounds of the brush entities whose targetname or `target` starts with the prefix (printed as `targetname -> target`) (`trigger <classname> <name> mins .. maxs ..`), for routing `--player-at` through triggers. |
| `IW2_GRENADE_HINT_DEBUG=1` | iw2game/iw2view | Logs the grenade hint node decisions (docs/spec/grenades.md "Grenade hint nodes"): `grenade hint: <method> blocked throw (start) -> (target) aims at hint (h)` / `..., no hint of N` for a 0x51f490 throw whose line of sight is blocked, `MagicGrenade lob over (h) lands (p)` (0x51f050) and `throw back from (landing) at hint (h)` (0x51f320). |
| `IW2_MENU_KEYS=a,b,...` | iw2view `--menu` | Scripted menu keys (up/down/enter/esc, `pause`, `wait`, `set:name=value`, `call:file.gsc:function` runs a thread on level, `exec:console text`). |
| `IW2_NO_DEPTH_HACK=1` | iw2view | Draws the view weapon without the depth hack (depth range 0..0.2, near plane 0.1): it clips through walls and the near plane again. |
| `IW2_SHOT_HEALTH=<hp>[,<trail>[,<phase ms>]]` | iw2view `--shot` | Sets the player health (a value <= 1 is a fraction of maxhealth), the health-bar trail fraction and how far into its pulse period the low-health background is. |
| `IW2_SHOT_HINT=<n>` | iw2view `--shot` | Shows the invalid-command hint state `<n>` (1 no ammo, 2 no frag, 3 no smoke, 4 stand blocked, 5 crouch blocked) 300 ms into its blink cycle. |
| `IW2_SHOT_OBJECTIVES=1` | iw2view `--shot` | Shows the objectives screen (menu objectiveinfo) as if +scores were held. |
| `IW2_BOLD_MESSAGE=<text>` | iw2view `--shot` | Shows a bold game message (iprintlnbold window). |

## Command-line options
- iw2view: `--menu` (front end without a level), `--cvar name=value`, `--ride F,R` / `--ride-after S` / `--ride-fire`
  (drive/fire a vehicle headlessly), `--follow-vehicle N`, `--follow-dist`, `--follow-yaw`, `--follow-pitch`,
  `--force-turret p,y`, `--force-jolt p,r`, `--spawn-all`.
- iw2game: `--spawn-all`, `--cvar name=value`, `--answer-menus`, `--level-notify T name`, `--player-at T x,y,z`, `--player-look T yaw,pitch` (view angles from T on), `--player-use T yaw,pitch` (use key once), `--player-fire T yaw,pitch` (one bullet from the eye), `--player-fire-at T x,y,z`, `--player-weapon T name` (sets $weapon), `--player-ads T frac` (playerads value), `--player-melee T x,y,z`, `--player-throw frag|smoke` (holds +frag / +smoke 1 s..2 s through PM_Weapon; prints the throw and the explosion time: frag 3.5 s, smoke 4 s after the throw on 88ridge), `--lookat-test`.
- iw2game: `--god` (the player takes no damage, as the `god` cheat), `--spawn-all`, `--cvar name=value`, `--answer-menus`, `--level-notify T name`, `--level-call T file.gsc function` (thread on level), `--player-at T x,y,z`, `--player-look T yaw,pitch` (view angles from T on), `--player-use T yaw,pitch` (use key once), `--player-fire T yaw,pitch` (one bullet from the eye), `--player-fire-at T x,y,z`, `--player-weapon T name` (sets $weapon), `--player-ads T frac` (playerads value), `--player-melee T x,y,z`, `--lookat-test`.
- iw2game `<main> <map> --item-test [weapon=N] weapon ...`: spawns weapon items at the player and prints stock / clip after the touch pickup (`weapon=N` first sets that weapon's clip, e.g. `mk1britishfrag=0 fraggrenade fraggrenade`).

## Menu debug helpers (iw2view --menu)

| Flag | Effect |
|---|---|
| `IW2_SHOT_MENU=a,b` | Opens the named menus before a `--menu --shot`. |
| `IW2_SHOT_SET=dvar=value,...` | Sets cvars before a `--menu --shot`. |
| `IW2_MENU_KEYS` tokens | `open:name`, `click:X/Y`, `left`, `right` (besides the existing tokens, `quit`). |
| `IW2_EXIT_AFTER=N` | Exits iw2view after N seconds. |


- Env `IW2_ACC_DEBUG=1` (iw2game/iw2view): prints one `ACC #<ent> (<origin>) <weapon> <ai|player> dist <d> chance <c> hit|miss clip <n>` line per AI/player shot (combat.cpp); `clip` is the shooter's script field `bulletsInClip` (the animscripts ammo count, decremented by the scripts after each shot; `undefined` before the first refill).
- Env `IW2_NO_DLIGHT=1` (iw2view): disables the dynamic point-light (l_point) pass, for before/after comparisons of explosion lighting. The count is the `r_dlightLimit` cvar (default 4, max 32).
- Env `IW2_PMOVE_FORWARD=1` (iw2game `--pmove` / fall test): the test pmove holds forward, so footstep events reach the AI (stats line "player noise events").
- Env `IW2_PMOVE_JUMP=1` (iw2game `--pmove`): jump for 0.1 s every 1.5 s; with `--pmove` the player-state events now also start their sounds (World::applyPlayerEvent, visible with `IW2_SOUND_LOG=1`).
- iw2game stats line also prints "actor noise events" (the 500 ms movement noise of actors, 0x5485a0).
| `IW2_SHOT_KEYS=a,b,...` | Applied before the screenshot of a `--shot` run when `--menu` or `IW2_SHOT_MENU` is set. Tokens: `open:name`, `click:X/Y`, `hover:X/Y`, `up`, `down`, `esc`, `left`, `right`, `enter`, `bindkey:NAME` (feeds the key to a BIND item that waits for one, e.g. `bindkey:PGUP`; also valid in `IW2_MENU_KEYS`). Also in both: `text:abc` (typed into an edit field), `bs`, `del`, `home`, `end`, `ins`, `pgup`, `pgdn`, `tab`, `wheelup`, `wheeldown`, `dbl:X/Y` (double click), `uiscript:name` (run a uiScript, e.g. `uiscript:addPlayerProfiles`). |
- Env `IW2_GROUND_DEBUG=1` (iw2game): counts the actor ground-trace hits/misses of the movement noise (stats line "ground trace hit/miss"); toujane 40 s: 24444/1123.
- iw2game stats line also prints "grenade away-flee/cower/plane-retry" (grenadeReact outcomes; duhoc_assault 150 s: 34 grenades, 6 flee, 0 cower, 0 retry).
- Env `IW2_PLAY_CINEMATIC=<name>` (iw2view): plays `video/<name>.roq` (e.g. `iw_logo`) before the level starts; any key skips it. Scripted `cinematic()` calls play automatically before the next level (docs/spec/video.md).
- Env `IW2_PLAY_CINEMATIC=<name>[!]` (iw2view): plays `video/<name>.roq` before the level (`!` = unskippable); `IW2_CINEMATIC_LETTERBOX=1` / `IW2_CINEMATIC_LOOP=1` set the letterbox / loop flags of that run (docs/spec/video.md).
- Env `IW2_CINEMATIC_STATS=1` (iw2view): after every movie prints wall time, last frame, dropped frames, audio length and the picture-minus-audio-played offset (min / mean / max).
- `--no-intro` / env `IW2_SKIP_INTRO=1` (iw2view `--menu`): no start-up movies (legal, atvi, IW_logo, cod_intro).
- Env `IW2_EXIT_AFTER=N` also leaves a playing movie after N seconds.

- Env `IW2_COMBAT_SCENE=<s>` (iw2game): at sim time `<s>` takes the closest living axis actor and allied actor (not in the scripted state 7), re-places the allied one 120 units in front of an axis actor with a clear shot line (hit boxes, before every shot, because scripted animation moves it back; entity clips are resynced), sets its health to 1000 (`IW2_SCENE_LETHAL=1` keeps it) and makes them enemies. Rifle bullets through the real `fireBullet` path (events 10 and 16, danger planes, damage, pain) every 0.3 s for 3 s; prints `[scene t] #n state sub hp planes pain` for both actors for 8 s and `shot hit entity` per shot. Debug only.
- Env `IW2_ACTOR_SLIDE=0` (iw2game/iw2view) opts out of the default: path-walking actors otherwise move with the ported actor slide move (actorslide.cpp, exe 0x5487c0: ground probe, step slide move, wall clipping) instead of the ground snap (opt-out restores the snap), and track the bumped entity (+0x318) for `gethitenttype`.
- Env `IW2_SCENE_COVER=1` (with `IW2_COMBAT_SCENE`): the scene is built around a cover node (victim beside it with the goal volume set, shooter in front) and logs the distance to the node. Env `IW2_COVER_LOG=1`: stderr `[cover ms] #n ...` lines for findcovernode / findbestcovernode / usecovernode, and the scene log adds the victim's animscript thread wait site and animation nodes. Debug only.
- Env `IW2_TRAVERSE_SCENE=<s>[,<pair>]` (iw2game): at sim time `<s>` takes the first living non-scripted actor, places it at the outer link node before the pair on the shortest route, freezes its goal (`$goalfrozen`, setgoal* ignored) on the far side, neutral team, and prints `[trav]` setup and `[trav t]` state/path/traversal lines every `IW2_TRAVERSE_EVERY` frames (default 10). Debug only.

- Env `IW2_FLOAT_LOG=1` (iw2game): an actor whose box (mask 0x820011) has no ground within 16 units below its origin for 10 frames in a row, outside the negotiation state, is printed every second (`[float ms] #n classname state script animmode at x y z ground d`, plus `linked` / `animscripted`); the end line counts them as `floating actor reports`. Debug only.

- Env `IW2_NODE_DROP_STRICT=0` (iw2game/iw2view): the level-start node drop (`World::dropPathNodes`, 0x515d70) keeps the type of a node it finds "in solid" or "floating" instead of making it a BAD NODE (type 0) as the exe does (the default since the drop test was checked against 0x416480, docs/spec/ai.md "Node drop"). With the default, breakout and silotown_assault print "Bad node in friendly chain" and clear their chain fields. Debug only.

- Env `IW2_ACTOR_TRACE=<entity number>[,<from s>[,<every n frames>]]` (iw2game): prints `[atrace t] state sub goal script path trav flags stopsoon pos yaw facing orient` (plus `animdelta ... budget plan lookIdx`) for one actor (default every 20 frames). Debug only.

- Env `IW2_ACTOR_TRACE_LINKS=1` (with `IW2_ACTOR_TRACE`) also prints the path points and the pathdata links of their nodes. Env `IW2_ENT_DUMP=<entity number>` (iw2game) prints classname/targetname/target/origin of that entity at the end of the run. Env `IW2_REACH_LOG=1` prints every blocked step of the actor reach test (aiReach: start, target, hit entity, normal, contents).

- Env `IW2_ACTOR_TRACE_THREADS=1` (with `IW2_ACTOR_TRACE`) also prints the wait site of every live script thread whose innermost `self` is the traced actor (battlechatter / coldbreath threads left out) and its active animation nodes (weight, goal, time, rate, notify flag). Debug only.

- `IW2_ACTOR_FOLLOW=0` (iw2game): keeps the yield step 0x501620 and the clear scan 0x501380 from setting the followed link (+0x2148) of other actors; the yield move itself still runs. On by default.
- `IW2_FOLLOW_SCAN=0` (iw2game): disables the follow retarget scan 0x504630 (`scanFollowed`), so no actor starts following a blocker from the scan. On by default.
- `IW2_FOLLOW_SCENE=<s>` (iw2game): at that time the first path-walking actor is made to follow a second actor placed ahead, to exercise the yield step 0x501620; prints `[follow ...]` lines.
- Env `IW2_FOLLOW_SCENE=<s>` (iw2game): at sim time `<s>` takes a path-walking actor A and a second actor B pinned 20 units ahead, forces A.followed = B one second later (the link cannot fire naturally) and prints `[follow t]` lines: positions, speed, followed, bumped entity, link/release counters.
- `IW2_ACTOR_FOLLOW=1`: lets a bumped actor start following the bumper (0x501c8b). Off by default: the follower trail (0x505520) that moves a followed actor is not ported, so a followed actor would stand still.

- Env `IW2_SCRIPT_SEED=<n>` (iw2game/iw2view): seeds the script `randomint`/`randomfloat` generator with n instead of 1. Debug only; used to vary a
  headless run (the moscow melee assert only showed with some seeds).
- Env `IW2_FIELD_LOG=<text>`: prints `[field T ms] kind#id.field = value at file:line` for every script store to a field whose name contains
  <text>. Debug only. `IW2_SCRIPT_BACKTRACE=1` now also lists each frame's self and the object locals (`{kind#id removed}`).

## Savegames (docs/spec/savegame.md section 5)
- `iw2game <main> <map> <secs> --save-dir DIR`: writes the level's saves (script `savegame` autosaves, `--save-at`) to `DIR/<name>.svg`
  (autosaves land in `DIR/autosave/`). Without it the saves are built and only reported.
- `iw2game ... --save-at T name`: queues a manual save (type 0, flags 6) at sim time T seconds: an openiw2 stand-in, the exe's console
  `savegame` / `devsave` handlers (0x4474xx) are not decoded.
- `iw2game <main> <map> <secs> --load FILE`: starts from that save (map must match) instead of running the level script.
- Env `IW2_MAIN=<game main dir>` (test_savestate): also runs the World round trip on a real map (save after 5 s, load into a
  fresh world, compare `World::stateDigest()`, run 40 more frames on both and compare time and live thread count).
  `IW2_SAVE_MAP=<map>` picks the map (default toujane); `IW2_SAVE_DIFF=1` prints the first lines where the two continuations differ.
- Env `IW2_SAVE_DIR=<dir>` (iw2view): directory of the savegame files (default `<main dir>/save`, the directory the save list reads).
- iw2game `<main> <map> --ladder-test x,y,z`: finds a ladder surface (flag 8) within 64 units around x,y,z in the running level's collision,
  puts a pmove player 20 units in front of it and holds forward looking up for 4 s; exit 0 when it climbed > 40 units. Env
  `IW2_LADDER_RAISE=targetname` first puts that entity back at its realOrigin (maps/_utility.gsc triggerOn, e.g. moscow's `regroup_ladder`),
  `IW2_LADDER_DEBUG=1` prints the probe hits and the ladder clips, `IW2_LADDER_MASK=0x..` the probe mask (default 0x2810011),
  `IW2_LADDER_SCAN=1` lists spots with headroom above, `IW2_LADDER_JUMP=1` holds jump from 1.4 s (exit at the top), `IW2_LADDER_SHAFT=1` / `IW2_LADDER_FLOORZ=z` print moscow shaft / floor probes. Example: `iw2game <main> moscow --ladder-test 644.5,785,-240` with `IW2_LADDER_RAISE=regroup_ladder`.

## Level end / victory screen
| Flag | Tool | Effect |
|---|---|---|
| `IW2_SHOT_LEVEL_CALL=<s>:file.gsc:function` | iw2view `--shot` | Starts the function as a thread on level at sim time `<s>`; a level change that falls due ends the simulation (a victory screen is opened for the shot), e.g. `20:maps/_endmission.gsc:nextmission`. |
| `IW2_SHOT_ALIASES=<map>` | iw2view `--shot` | Loads the material aliases of `maps/<map>.csv` (`$levelBriefing`, `$victoryBackdrop`) for the shot, e.g. with `--menu` and `IW2_SHOT_MENU=briefing` or `pregame`. |
- iw2game prints `[<ms> ms] level change to <map> ...` when a change falls due (with `: openvictoryscreen, nextlevel loads it` for
  `missionsuccess`) and `player weapons: ... ; current ..., offhand ...` at the end of every run.
- iw2view logs `victory screen (next map X)`, `changing level to X (persistent)` and `player weapons: ...` after each load.
- `--shot` runs open all menus with their fades already finished (`MenuStack::settleFades`).
- Env `IW2_CANSHOOT_LOG=<entity number>` (or `-1` for all; iw2game/iw2view): prints every muzzle trace 0x54cf40 (`canshoot`, 0x54d010) of that actor: `[canshoot <ms>] #n muzzle (..) target (..) fraction f hit <ent> -> 0|1`. Debug only.
