# Debug flags and environment variables

Developer switches of `iw2view` / `iw2game`. None change gameplay when unset. The full list of `IW2_*` variables is
`grep -rhoE 'getenv\("IW2_[A-Z0-9_]+"\)' apps src tools`; this page covers the ones added recently. Others are
described in the code that reads them.

## Environment variables
| Variable | Tool | Effect |
|---|---|---|
| `iw2_grenadeCook` (cvar, `--cvar iw2_grenadeCook=0`) | iw2view, iw2game | openiw2 option, default 1, a deliberate deviation from the shipped data (no shipped grenade has `cookOffHold`). On: frag-class offhand grenades cook while held (fuse burns, crosshair pulses, goes off in the hand when held past the fuse, the thrown fuse is what is left); smoke grenades do not. Off: shipped behaviour. A weapon file with `cookOffHold 1` always cooks. With the option on, frag / smoke held while on a turret or in a vehicle is ignored until released (getting off with G down primes nothing). |
| `ai_corpseCount` (cvar, `--cvar ai_corpseCount=N`, menu option `ai_corpsecount`) | iw2view, iw2game | Corpses kept, range 0..64. Deliberate deviation: openiw2 default 20, the 1.3 executable's default 5; the eviction rules are the original's (docs/spec/ai.md "Corpse limit"). 20 is the menu's Large entry. |
| `iw2_hitMarker` (cvar, `--cvar iw2_hitMarker=1`; menu row Options > Game Options > "Hit marker") | iw2view | openiw2 addition, default 0, archived in the profile's config.cfg; not original single-player behaviour. Values: 0 off, 1 on (white, as in multiplayer), 2 on with a kill shown red (menu value "Red on kill"; a deviation from multiplayer too: red when that hit killed the enemy, white for a later non-lethal hit). On: the multiplayer damage-feedback material (`damage_feedback`, shipped in the same archives, so single player can draw it) is shown at the screen centre, 24 x 24 virtual units at offset (-12, -12), alpha 1 fading linearly to 0 over one second, restarted by each new hit; no sound (the multiplayer script also plays an alert sound). Look and timing follow the multiplayer script `maps/mp/gametypes/_damagefeedback.gsc` (lines 12-33), which draws it as a script HUD element; here it is drawn after the script elements, so it shows above the scope overlay too. Trigger (`World::damage`, `hitMarkerTrigger` in src/game/hitmarker.h): damage dealt by the player to a living actor of the opposing team; friendlies, neutral actors, corpses, the player, script models, vehicles and world damage do not count. All means of death count (bullets, turret fire, rockets and grenade splash, melee), as multiplayer ignores the means of death (`dm.gsc:274`, `tdm.gsc:344`); the killing blow counts. Hidden while a menu is open, while paused and when the player is dead. The row is added in memory to the game options menu by `addOpenIw2Options` (src/ui/iw2options.cpp), since the shipped menu files are not part of the repository; without that menu the cvar still works. Tests: tests/test_hitmarker.cpp. |
| `cg_hudGrenadeIconInScope` (cvar, `--cvar cg_hudGrenadeIconInScope=1`) | iw2view | Original dvar, default 0, archived. 0: the grenade danger icons are hidden under a scope overlay, as the original does by default (0x4be210). 1: they are drawn there too. |
| `IW2_TURRET_POSE_DEBUG=1` | iw2game, iw2view | Once per second of game time, every AI turret gunner in the pose step (`World::turretPoseStep`, exe 0x549ba0) prints `DBG turretpose <actor> turret <n> node <setturretanim node> probe <i>/<count> h <height> tagYaw <deg> at <origin> pose <posed origin> yaw <deg> gun pitch <deg> want <deg> dropPitch <deg> engaged <0|1>` and its anim tree weights. |
| `IW2_NO_CULL=1` | iw2view | Disables frustum culling of animated-entity skinning (actors are skinned even when off screen) and the near / far depth-plane cull of brush-model entities (viewcull.h). For comparing output/perf. |
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
| `IW2_NO_POSTSUN=1` | iw2view | Disables the post-sun scene copy and the `$floatz` pass: heat distortion sprites are skipped and `effect_zfeather*` sprites draw without feathering (before/after comparison). |
| `IW2_FX_SKIP=<type or name>` | iw2view | Leaves the sprites of effect elements of that type (e.g. `Particle`, `Tail`) or element name out of the draw; the elements still update. For bisecting which element draws an artefact. |
| `IW2_FX_HIST=1` | iw2view | With `IW2_FX_DEBUG`, prints the 12 largest per-element particle counts (type, name, first shader) each frame, and (once per element) a `fx skip element type ...` line for elements whose type is not implemented. |
| `IW2_RIDE_TRACE=1` | iw2view | Per-frame trace of the ridden vehicle (origin, angles, turret, camera link). |
| `IW2_CROSSHAIR_DEBUG=1` | iw2game/iw2view | Prints the crosshair trace eye/yaw and the hit entity/target name. |
| `IW2_PRONE_LOG=1` | iw2game/iw2view | Prints each `checkprone` call (actor, origin, yaw, alreadyProne, the answer and the two pitch outputs of the fit test 0x4fb530), each `enterprone` (length, fit result, pitches) and each `exitprone`. |
| `IW2_NO_PRONE_TILT=1` | iw2view | Draws actors and corpses without the prone / corpse-blend tag_origin control (A/B check of 0x4c1cc0). |
| `IW2_SHOT_ENT=N[,dist,yaw,height]` | iw2view `--shot` | Puts the shot camera `dist` (120) units from entity N at its yaw + `yaw` (90) and `height` (50) above its origin, looking at it. |
| `IW2_SHOT_FREE_CAM=1` | iw2view `--shot` | Keeps the `--pos` camera when a script link carries the player (otherwise the shot is taken from the linked player, e.g. in the duhoc_assault boat). |
| `IW2_FACING_LOG=N` | iw2game/iw2view | Per think of actor N (-1: all): facing mode (override), path count, movemode, yaw after the turn, active target, the motion angle (getmotionangle, 0x53f770), team, whether the actor has an enemy, the trail sum (0x505d80 / 0x504ca0), and the origin, path goal and planned point count. |
| `IW2_FACING_PATH=1` | iw2game/iw2view | With `IW2_FACING_LOG`, also prints the path points (goal first, with node numbers) after each line of an actor that has a path. |
| `IW2_SLIDE_LOG=N` | iw2game | Per path move of actor N (-1: all): the requested delta, origin before and after, hit entity, ground entity, and a probe of the box at the origin (start-solid / all-solid, contents) and along the delta; `[stuck]` lines when the move block failed on the no-path physics (nearest node used by the nudge; `IW2_SLIDE_NODES=1` also lists the candidate nodes); `direct 1` marks a path walk moved by its raw delta (docs/spec/ai.md "Failed actor move block"). For actors that run in place. |
| `IW2_PLAYER_DMG_LOG=1` | iw2game/iw2view | Prints every damage the player takes: time, amount, attacker entity / classname and means of death. |
| `IW2_TOUCH_TEST=<s>` | iw2game | At sim time `<s>` runs the vehicle touch callback (0x512d40) for the first vehicle with collisionDamage > 0 placed 100 units behind the player, moving at 400 units/s at it; prints the target health before and after; `IW2_TEST_ACTOR=1` targets the first living actor instead of the player. |
| `IW2_TEST_MOVER=1` | iw2game | With `IW2_TOUCH_TEST`: targets the first script_model (take-damage on, health 100) to check the eType 5 crush (0x512dd1, 999999 at once). |
| `IW2_ROCKET_TEST=<s>` | iw2game | At sim time `<s>` fires the first vehicle's turret rocket at the player from 300 units away; prints the player health before and 3 s after. |
| `IW2_KILL_VERBOSE=1` | iw2game | With `IW2_KILL_VEHICLES`, also prints each vehicle's model and health before the kill. |
| `IW2_FIRE_VEHICLES=<s>` | iw2game | At sim time `<s>` sends the script notify `turret_fire` to every vehicle (the AI tank fire loop in `_tank.gsc`), to test vehicle weapons headless. |
| `IW2_KILL_VEHICLES=<s>` | iw2view `--shot` | Same as the iw2game variant below, at sim time `<s>` of the shot run (combine with `--follow-vehicle N`, `--sim-seconds`). |
| `IW2_KILL_AXIS=<s>` | iw2game | At sim time `<s>` deals 100000 damage to every living axis actor (leaves the allies without an enemy, e.g. to watch their walking after a mission's fight). |
| `IW2_KILL_VEHICLES=<s>` | iw2game | At sim time `<s>` deals 100000 damage (attacker: the player) to every script vehicle, to test the death chain. |
| `IW2_STATE_HASH=1` | iw2game | After every frame prints `statehash <ms> <hash> threads <n>`: a hash of each live entity's number, origin, angles and health, and the live script thread count. Two runs with the same seed must print the same lines; the first differing line is the first divergent frame (`diff` of the two `grep statehash` outputs). Found the autoplay kill-gate wave times inheriting a freed object's address (tools/iw2game/deathseen.h, tests/test_deathseen.cpp), which made hill400_defend and decoytown runs differ. |
| `IW2_THREADS_PREFIX=<p>` | iw2game | Lists every live script thread whose wait site starts with `<p>`. |
| `IW2_NOTIFY_LOG=level` or `all` | iw2game | Prints every script notify (`level`: only on the level object; `all`: every object) with its sim time and, when the first argument is a string, that argument in quotes (the notetrack name of an animation flag notify). For walking mission flows. |
| `IW2_SOUND_LOG=1` | iw2game | Prints every sound start (`[sound <ms>ms] alias`). Use with `--mixer` so scripts get the real sound lengths (dialogue timing). Also prints `NOT STARTED` lines for refused sounds (with a dump of the busy voices for refused grenade sounds) and a periodic voice dump. Under `--mixer` the mixer advances 50 ms per frame with the listener at the player. |
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
| `IW2_DEPTH_PROBE=1` | iw2view | Replaces the dynamic lights by one pass that paints every lit surface pure red through the l_point pipelines. A surface pixel left unpainted failed LessEqual against its own first pass: its vertex shaders disagree on depth. Use with `IW2_NO_HUD=1 IW2_NO_GLOW=1`. `=2` also leaves out the multiply surfaces, which otherwise darken the red. |
| `IW2_BOLD_MESSAGE=<text>` | iw2view `--shot` | Shows a bold game message (iprintlnbold window). |

## Command-line options
- iw2view: `--menu` (front end without a level), `--cvar name=value`, `--ride F,R` / `--ride-after S` / `--ride-fire`
  (drive/fire a vehicle headlessly), `--follow-vehicle N`, `--follow-dist`, `--follow-yaw`, `--follow-pitch`,
  `--force-turret p,y`, `--force-jolt p,r`, `--spawn-all`, `--noclip` / `--ufo` (run the `noclip` / `ufo` cheat command once the level is up; they
  need `--cvar sv_cheats=1`, otherwise the client prints `GAME_CHEATSNOTENABLED`), `--god`.
  `--force-turret p,y`, `--force-jolt p,r`, `--spawn-all`.
- `--spawn-all` (iw2game, iw2view) also lifts the 32-slot actor pool limit for the rest of the run.
  need `--cvar sv_cheats=1`, otherwise the client prints `GAME_CHEATSNOTENABLED`), `--god` / `--demigod` / `--notarget` (the god / demigod / notarget cheat commands; same sv_cheats + alive gate; F10 cycles off -> god -> demigod -> off; the developer keys V (walk/fly) and L (fullbright) also need `--cvar sv_cheats=1`; the app takes options too: `open OpenIW2.app --args --cvar sv_cheats=1`; `--god` used to skip the gate).
- iw2game: `--spawn-all`, `--cvar name=value`, `--answer-menus`, `--level-notify T name`, `--player-at T x,y,z`, `--player-look T yaw,pitch` (view angles from T on), `--player-use T yaw,pitch` (use key once), `--player-fire T yaw,pitch` (one bullet from the eye), `--player-fire-at T x,y,z`, `--player-weapon T name` (sets $weapon), `--player-ads T frac` (playerads value), `--player-melee T x,y,z`, `--player-throw frag|smoke` (holds +frag / +smoke 1 s..2 s through PM_Weapon; prints the throw and the explosion time: frag 3.5 s, smoke 4 s after the throw on 88ridge), `--lookat-test`.
- iw2game `--autoplay [stallSecs]` (default 25): a god-mode-style bot that follows the level's own gating. When no novel level notify, objective state change, autosave or level change has happened for stallSecs of sim time it (1) teleports the player into the nearest trigger (`trigger_multiple/once/radius`, not AI-only, not disabled; the map's own `maps/<map>*.gsc` waiters before library ones) a script thread `waittill("trigger")`s on, at a point where the player box touches it (`World::debugTriggerEntryPoint`), walking there at 250 u/s along the AI node route (`PathGraph::findPath`; ranking by that walking distance, unreachable x4) and refusing routes that cross a kill / minefield trigger (`--autoplay-skip a,b` adds substrings to the default list `killplayer,kill_player,playerkill,desert,awol,minefield,missionfail` matched against the waiting function name and the targetname); after three fruitless entries since the last progress it escalates: (2) kills (100000 damage from the player) the actors/vehicles a thread waits the `death` of, else every living axis actor, (3) else, with `--autoplay-force`, notifies the level event a `maps/` thread waits for (masks script problems; every forced event is listed), (4) else records a stall with the live `maps/*.gsc` wait sites. The run ends at the first due level change. Use with `--god --answer-menus ok,...`. Prints `[autoplay T] ...` lines and an `autoplay:` summary.
- iw2game `--autoplay` actions besides walking into a trigger (same escalation step): a `trigger_use` / `trigger_use_touch` or a `weapon_*` item is walked to and the use key pressed looking at it (the `--player-use` path), a `trigger_lookat` is looked at from 120 units (`--player-look`), a `trigger_damage` or a non-actor entity whose `damage` a thread waits for is shot twice from 200 units (`--player-fire-at`). Items rank after the map's own trigger waiters and before library ones. A trigger a map thread named `*nade*` / `*grenade*` polls (a timed-wait loop on it: `VM::waiters()` lists those with event `""`), or one entered twice without progress, gets up to five grenade lobs of the offhand weapon (`World::playerThrowGrenade`, stand-offs 60/120/200/300/90 units with pitches 45 down / 35, 25, 15 up / 60 down). A named actor whose `damage` a map thread named `*sniper*`/`*shoot*`/`*kill*`/`*target*` waits for is shot like a model; allies, prisoners and civilians never.
- iw2game `--autoplay` in a vehicle (`usingVehicle`, libya / 88ridge tanks): instead of walking, the bot drives at the target through the player command (forward, view yaw at the target, jump button so the hull turns), backs up for 3 s with the hull turned when it made less than 40 units in 2 s, and after two stuck events follows the nearest moving allied vehicle (`script_team` / `team` allies); with nothing to enter it drives to the current objective marker, or after the allies when there is none. Env `IW2_AUTOPLAY_DEBUG=1` prints the driven vehicle's position every 5 s, the stuck events and, at each kill gate, the entities with a live `death` wait. The kill gate also kills vehicles and other entities with `health > 0` (flak guns), never the player's own vehicle.
- iw2game `--autoplay` pacing and safety rules (no flags): before a map-own trigger entry the bot waits 15 s while no allied actor is within 800 units (at most 4 in a row, 8 per map); escort-chain triggers (`*escort*` waiters) may be entered four times with a 40 s dwell; a vehicle that follows a script path is ridden, never driven; a route through a lethal trigger is retried via the trigger's bounds corners (+96 units) before it is refused, `*warn*` triggers are not lethal; at most six item pickups per map. The kill gate kills a team-less vehicle only when an axis actor is within 200 units and no ally, when the map's own script waits for its death, or when `_flak88.gsc` does (flak guns), and never a vehicle with a map-own `trigger_use` within 200 units (sticky bombs / explosives: it is planted, not shot). Only the player's start vehicle is ever driven; `damage` waits count as shooting gates only from the map's own scripts; named actors are shot once each and at most four per map. The kill gates spare the members of a mortar team (`script_squadname` `mortarteam*`) until the level flag `bunkersealed` is set (hill400_defend counts a team elimination only after the seal but counts the spawn at once, so an early kill makes the "snipe the mortar" stage unwinnable). `IW2_AUTOPLAY_DEBUG=1` also prints the level counters `mortarTeamsDeployed / mortarTeamsEliminated / mortarTeamsRequired / mortarTeamSlots` at each kill gate (they only exist on hill400_defend; `undefined` elsewhere).
- iw2game `--targets FILE`: per-map progress targets, lines `<map>: <level notify>; <level notify>; ...` (`#` comments; `tools/campaign-targets.txt`). The report gets `targets_total`, `targets_reached` and `[targets_missed]`; a stall line ends with `reached k of n stages`. `tools/script_sweep.sh --make-targets --out DIR` prints such a file from a sweep's reports (every notify any run reached, forced ones left out); a real play's `IW2_NOTIFY_LOG=level` is the intended source.
- iw2game `--report FILE`: writes an uncapped, wall-time-free summary of the run for `tools/script_sweep.sh`: `map/seed/seconds/sim_ms/runtime_errors/loop_guard_kills/unimplemented_builtins/live_threads/notifies/saves/stalls/forced/level_change/player_health`, then the sections `[errors]` (count + site), `[builtins]`, `[objectives]` (index, state, text), `[notifies]` (first occurrence time of every level notify), `[autoplay]` (the action log) and `[level_waits]` (live wait sites in `maps/*.gsc`).
- iw2game `--report FILE` soft-lock diagnostics (`src/game/softlock.cpp`; read-only, no engine behaviour changes). The report header gets `softlock_groups` (AI groups some live thread waits on that are soft-locked), `softlock_groups_unwaited` (soft-locked groups nobody waits on yet), `short_spawners`, `short_spawners_antispawn` and `short_spawners_live`, and the section `[softlock]` lists: `untouched_spawners N` (spawners with a count that never saw a spawn attempt); every AI group (`level._ai_group`: `spawnercount`, `aicount`, `waited` / `unwaited`; `waited` means a live thread is in the wait or runs a function that calls it, so a thread still parked on the trigger in front of a `waittill_aigroupcleared` already marks the group waited) with the wait sites of the threads in `waittill_aigroupcleared` or polling `getAIGroupCount` / `getAIGroupAI` on it (the group is read from the wait frame's string locals and literals) and the group's spawners that still have a count; `SOFTLOCK <group>` marks a group whose `spawnercount` is positive while none of those spawners can be started any more. A spawner is `driven` (a live thread with the spawner as self calls dospawn / stalingradspawn: flood_think and similar), `pending` (a thread waits for `trigger` on an entity whose `target` is the spawner's targetname, or another live thread names the targetname / script_noteworthy as a string; the first such site is printed) or `DEAD` (none of that). `spawner ...` lines (every spawner with a spawn attempt plus those in a live group) give the count now, the count at the first attempt, how many AI it spawned, the refusals by reason ("player can see spawnpoint", "spawnpoint would telefrag", "no free actors", "ai_disableSpawn", with the time of the last one) and its liveness. A flood spawner (a `realcount` field, a `flood` script_noteworthy or a `flood_spawner` trigger that targets it) that is `attempted`, still has a count and spawned fewer than its initial count is listed as `SHORT-SPAWNER` when it has no live driver (the case of a lost retry loop); it is only counted in `short_spawners_live` when it has one (retrying or waiting for its AI to die) and in `short_spawners_antispawn` when an `antispawn` trigger names its targetname (the script ends such a flood on purpose once the player passes that trigger; the spawner lines carry `antispawn-trigger`). `DEAD-SPAWN-WAIT` lines (and `dead_spawn_waits`) list map-script threads (not the library's prethink in `_spawner.gsc` / `_load.gsc`) waiting for `spawned` on a spawner that still has a count, had a spawn refused (the refusals are listed) and is `DEAD`, i.e. nothing retries it (the pattern of `waittill_aiAndSpawners_dead` style helpers; override / tweak threads that wait on every spawner show up here too, so each line needs a look at its wait site). `levelwait` lines list the level-script (`maps/`) waits on a notify or flag with their site and how many other live threads name the event as a string; `no-other-reference` marks events nothing alive mentions, `FREED-OBJECT` a wait on an object that was freed. This is a heuristic: a thread that sets a flag later but is not running yet cannot be seen, and a spawner started by a script that holds it only in an array is not seen as named. With `--autoplay`, a stall record also lists the soft-locked groups and short spawners (indented under the stall line). `tools/script_sweep.sh` shows `softlock`, `short` and `dead waits` columns in `summary.md`.
- `tools/script_sweep.sh --reload --maps "a b" [--seconds N]` (needs `IW2_MAIN`): one `--autoplay` run per map keeps its autosaves (`--save-dir`), then one `--autoplay` run starts from each autosave file (`--load`). A level notify the unbroken run reached after the save's time and the reloaded run missed is printed and the exit code is 1. It covers only the stages the bot reaches, and bot timing differs between the runs, so a finding needs a look.
- `tools/script_sweep.sh --smoke | --full | --determinism | --summarize` (needs `IW2_MAIN`): the headless mission-script sweep; `--smoke` is the ctest `script_smoke` target.
- Env `IW2_LOOP_GUARD_ITERS=<n>` (iw2game, default 200000): the VM's "potential infinite loop" guard kills a script that spins without suspending after n backward jumps instead of after 2500 ms of wall time (the exe's rule 0x46aae9, which iw2view keeps). `0` restores the wall clock. The wall clock made toujane runs differ for a fixed `IW2_SCRIPT_SEED`.
- iw2game: `--god` (the player takes no damage, as the `god` cheat), `--spawn-all`, `--cvar name=value`, `--answer-menus`, `--level-notify T name`, `--level-call T file.gsc function[=arg,arg]` (thread on level; the optional comma-separated strings are its arguments, e.g. `maps/_utility.gsc flag_set=move_out`), `--player-at T x,y,z`, `--player-look T yaw,pitch` (view angles from T on), `--player-use T yaw,pitch` (use key once), `--player-fire T yaw,pitch` (one bullet from the eye), `--player-fire-at T x,y,z`, `--player-weapon T name` (sets $weapon), `--player-ads T frac` (playerads value), `--player-melee T x,y,z`, `--lookat-test`.
- iw2game `<main> <map> --item-test [weapon=N] weapon ...`: spawns weapon items at the player and prints stock / clip after the touch pickup (`weapon=N` first sets that weapon's clip, e.g. `mk1britishfrag=0 fraggrenade fraggrenade`).

## Menu debug helpers (iw2view --menu)

| Flag | Effect |
|---|---|
| `IW2_SHOT_MENU=a,b` | Opens the named menus before a `--menu --shot`. |
| `IW2_SHOT_SET=dvar=value,...` | Sets cvars before a `--menu --shot`. |
| `IW2_MENU_KEYS` tokens | `open:name`, `click:X/Y`, `left`, `right` (besides the existing tokens, `quit`). |
| `IW2_EXIT_AFTER=N` | Exits iw2view after N seconds. |
| `IW2_START_TRACE=1` | iw2view | Prints `[start-trace]` lines for the level start: real ms and level ms of the world creation, every script sound start (handle and length), the end of the load, and the first presented frames (with the voices still held). |


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

- Env `IW2_ACTOR_TRACE=<entity number>[,<from s>[,<every n frames>]]` (iw2game): prints `[atrace t] state sub goal script path trav flags stopsoon pos yaw facing orient` (plus `animdelta ... budget plan lookIdx`) and a line `enemy #n classname dist d sight 0|1` (or `enemy none`) for one actor (default every 20 frames). Debug only.

- Env `IW2_ACTOR_TRACE_LINKS=1` (with `IW2_ACTOR_TRACE`) also prints the path points and the pathdata links of their nodes. Env `IW2_ENT_DUMP=<entity number>` (iw2game) prints classname/targetname/target/origin of that entity at the end of the run. Env `IW2_REACH_LOG=1` prints every blocked step of the actor reach test (aiReach: start, target, hit entity, normal, contents).

- Env `IW2_ACTOR_TRACE_THREADS=1` (with `IW2_ACTOR_TRACE`) also prints the wait site of every live script thread whose innermost `self` is the traced actor (battlechatter / coldbreath threads left out; a `wait` shows the milliseconds left, and the calling frames follow after `<`) and its active animation nodes (weight, goal, time, rate, notify flag). Debug only.

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
- Env `IW2_SAVE_DIR=<dir>` (iw2view): directory of the savegame files (default `<main dir>/save`, the directory the save list reads); with no arguments `<pref>/save`).
- iw2view with no arguments (the app started from Finder): stdout/stderr go to `~/Library/Logs/OpenIW2/openiw2.log` (previous log
  kept as `openiw2.log.1`); the `main` folder is read from `mainPath=` in `<pref>/settings.cfg` (`<pref>` = SDL_GetPrefPath
  `~/Library/Application Support/OpenIW2/`), else `<pref>/main`, else chosen in a folder dialog; it must hold `iw_00.iwd`..`iw_15.iwd`
  and one complete `localized_<language>_iw00.iwd`..`iw10.iwd` set (names case-insensitive). Starts as `--menu` at the desktop size;
  profiles go to `<pref>/players`. Any argument keeps the command-line behaviour.
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

Cvar `player_scopeExitOnDamage` (set with `--cvar player_scopeExitOnDamage=1`, default 0): iw2view feeds it to the player weapon; a damaged frame drops the aim of a scope weapon at once.

- `iw2view --language NAME`: UI/subtitle language (`english`, `polish`, ...). Without it the language is detected from the first `localized_<language>_iw*.iwd` pack in the game directory, else `english`.
- iw2game `--autoplay` obeys static-model cover: a shot target is approached from a stand point whose eye-to-target bullet trace is clear (16 directions x 5 distances around the target), a route through a minefield may use two bounds corners, and a kill gate takes the oldest wave of `death` waits first (waits first seen more than 120 s after the oldest candidate wait for the next gate). Env `IW2_AUTOPLAY_WAVE=<seconds>` changes that window (a huge value restores the old kill-everything gate)). When a gate leaves later waves alone, the next gate follows after the stall time (not after two more trigger chases), so a lone early straggler cannot delay the waves behind it.

## On-screen messages for cheat toggles and developer keys

God, demigod, notarget, noclip and ufo queue the client command `print` with the localized GAME_* string (the original sends it to the
console). iw2view also shows each such message in the game-message window (top left, 5 s like con_gamemessagetime, then it fades out),
localized through the loaded strings. The developer keys L and V have no original string: they show the English texts
"fullbright ON" / "fullbright OFF" and "walk mode ON" / "fly mode ON" through the same path. All of these need `sv_cheats` as before; without it
the key shows the localized "cheats not enabled" message. TODO(spec): the original draws console text on screen only with con_minicon
(default 0); the position and fade of that mini console are not proven, so the game-message window is used.

## Brightness
- Cvar `r_gamma` (`--cvar r_gamma=1.8` for iw2view, also the options slider; archived in the profile; 0.5 .. 3, default 1): the brightness ramp, applied to the whole frame by a final full-screen pass. `--cvar r_ignorehwgamma=1` turns the ramp off (the original's software path is not implemented). With `--shot`, `IW2_NO_HUD=1` avoids the level-start fade-in for brightness comparisons.

## Fire rate (iw2game `--report`, tools/script_sweep.sh)
A bug once made AI soldiers play their firing animation without ever calling `shoot`, on many maps, while the god-mode sweep only counted script errors
and objectives. `--report` now has the AI fire counters (the monitor samples every 50 ms frame only when `--report` is given and never changes the run):
- Report fields per team (`axis`, `allies`): `ai_shots_<team>` (bullets and rockets fired by AI: the `shoot` built-in, turret gunners, vehicle turrets, counted
  for the gunner's team or the shooter's `team` field; the player's own shots are not counted), `ai_combat_secs_<team>` (seconds an actor of the team was alive
  with a valid `enemy`, not held in the scripted state: riders and scene actors cannot fire), `ai_sight_secs_<team>` (the part of those seconds in which the
  eye-to-eye line to the actor's own enemy was clear), `ai_shots_per_min_<team>` (shots per combat minute). Section `[firerate]` lists every team (also `neutral`/`undefined` for
  gunless or unteamed shooters such as map-placed turrets) and the silent shooters.
- Silent shooter: an actor whose enemy its own sight record marks visible (the record the `cansee` built-in and so the animscripts read: field of view,
  sight distance, sight traces; the monitor reads it without making a check, and only while it is at most 250 ms old, the age `cansee` without a time
  argument accepts (0x53da00); an older record counts as not visible, so the monitor can undercount sight but never invents it) for 10 s in a row (gaps
  under 1 s ignored) without one shot. A shot ends the stretch (a flagged stretch keeps its seconds up to that shot) and the next stretch starts there. A
  shot of a turret or vehicle gun counts for its gunner. The line gives the entity number, classname, position, start time, the number of shooting
  animations (nodes whose animation has a `fire` notetrack) started meanwhile, the seconds the engine held the actor suppressed (`World::isSuppressed`,
  what `issuppressed` returns, 0x53e6a0) and of those the seconds after the scripts were told so, the seconds in the move script and in the melee state,
  the animscript it spent most of the stretch in, and a reason. Each sample is explained by `running` (the actor is in the `move` animscript: its run
  and walk handlers never shoot, the run script's shoot-while-running test is commented out, animscripts/run.gsc:13-42, walk.gsc has no fire call,
  move.gsc:119-129 picks one of them), else `suppressed` (the engine holds it suppressed and the last answer the animscripts got from `issuppressed`
  was yes: they hide or stop firing on it, e.g. cover_crouch.gsc:1179 / 1431, combat_utility.gsc:302), else `melee` (the animscripts' own state set by
  `trackscriptstate`, 0x540280, is `melee`: melee.gsc:6-25 strikes instead of shooting while the enemy stays in melee range, which against a god-mode
  player lasts for ever). The stretch is `unexplained` when the samples none of these explains still add up to 10 s (the silent threshold itself),
  otherwise the largest explained part names it. Report fields `silent_shooters` (all) and `silent_unexplained`. `SILENT:n` is flagged from 10
  unexplained stretches per run.
- Seed-1 400 s autoplay, all / unexplained stretches: decoytrenches 14 / 6 → 20 / 4, crossroads 4 / 3 → 5 / 1, decoytown 10 / 0 → 12 / 0 (before: the
  1 s sight age, the half-the-stretch suppression rule, the main script for `running`, and stretches that went on through shots; the extra stretches
  are the ones a shot used to hide, nearly all `melee`). The traced remaining cases: an allied Thompson gunner in the decoytrenches trench (#613,
  two stretches) whose eye sees the enemy but whose muzzle line is blocked (`canshoot` fails from both poses, so exposedcombat.gsc only aims,
  exposedcombat.gsc:336; the muzzle trace stops about 110 units out at the trench edge while the eye trace is clear); an axis rifleman (#549) 3400
  units from its enemy in the same aim-only branch between two shots 15 s apart (its muzzle traces were not logged); an axis corner actor (#745) idling in corner_axis.gsc:1208-1215 because its step-out point crosses a team mate's line to its enemy
  (maymovetopoint, 0x53dc50), until it steps out and fires; on crossroads an allied panzerschreck soldier (#958) that holds a script target and only
  fires at it once the sight trace from tag_flash to the target passes (exposedcombat.gsc:61, 1050-1063). The melee stretches are axis soldiers in
  melee range of the god-mode player. A crossroads actor in the bad place state (#794: it runs the `stop` script there, stop.gsc standstillthink) is
  now `running`. Not compared with a run of the original; the muzzle height of the aim poses and `sighttracepassed` were not checked against the exe.
  Debug aids: `IW2_FIRE_TRACE=n,m,...` (iw2game with `--report`: once per second of game time the listed actors' animscript, pose, enemy,
  sight, suppression, the last `issuppressed` answer, clip, weapon, AI state, script state and their script threads' wait sites, on stderr),
  `IW2_MAYMOVE_LOG=n` (-1 for all: each maymovetopoint call of that actor and which test failed, with the team mate for the fire-line test),
  `IW2_CANSHOOT_LOG=n` (each muzzle trace of `canshoot`), `IW2_SIGHTTRACE_LOG=n` (-1 for all: each `sighttracepassed` call of that self, with
  the trace's fraction and hit entity), `IW2_FIRE_TRACE_ANIM=1` (with `IW2_FIRE_TRACE`: also the weighted animation nodes, `#index name(pparent)`, the model and
  attachments and the world positions of tag_weapon_right, tag_flash, tag_eye, j_mainroot, j_wrist_ri), `IW2_FIRE_TRACE_FIELDS=a,b`
  (with `IW2_FIRE_TRACE`: those script fields of the actor), and the `contents` of the hit in `IW2_CANSHOOT_LOG` lines.
- The "muzzle 42 units up" of #613 / #549 is the crouch pose test: canShootEnemyFromPose("crouch") from a standing actor adds (0, 0, -20)
  (animscripts/utility.gsc:436-437) to the real muzzle (about 62 up in the stand aim pose, `IW2_FIRE_TRACE_ANIM`). The stand tests fail
  on the eye line itself: canshoot's trace (mask 0x200c861) stops on solid world (contents 0x8000001) about 330 units out, while the
  monitor's sight record comes from the sight trace with its own mask and timing. No pose or attachment error was found: the shipped
  weapon_thompson hangs its tag_weapon root on the body's tag_weapon_right with tag_flash 9.9 units forward.
- Baseline update with the rocket ping (AI event 9, docs/spec/ai.md "AI events": actors near a flying rocket now take it like a grenade
  and may flee its predicted impact): axis / allies crossroads 43.1 / 12.4 → 49.7 / 11.1, duhoc_defend 50.3 / 67.7 → 46.0 / 71.5, matmata
  83.7 / 5.3 → 77.4 / 6.3, newvillers 13.8 / 25.2 → 17.6 / 16.4, rhine 206.0 / 32.0 → 137.5 / 38.3, silotown_assault 84.4 / 98.4 → 93.6 /
  82.7, toujane_ride 16.9 / 17.1 → 16.6 / 15.7; decoytown moved too (axis 38.4 → 55.8) but its line is owned by the determinism work and
  was left. Silent stretches (all / unexplained): decoytrenches 22 / 7, crossroads 1 / 0, decoytown 11 / 0.
- Crossroads #958 reproduced with seed 5 (#954, 88 s): the allied panzerschreck soldier holds a script target (maps/_spawner.gsc
  panzer_target: `panzer_ent` the Tiger, a script_vehicle about 3600 units away, offset (0, 0, 80)); exposedcombat.gsc:61 loops in
  panzerGuyShootsTarget and ShootIt fires only once sightTracePassed from tag_flash to the target passes (exposedcombat.gsc:1050-1063).
  It never does from its node (the trace stops at 18 % on world geometry), so it never shoots its own enemy either. sighttracepassed
  matches the exe (0x4f2580); not compared with a run of the original.
- Baseline update with the AI event receiver masks (docs/spec/ai.md "AI events": an actor's bullet events no longer reach its own team or
  neutral actors, death events only the dead actor's team): seed-1 rates moved on four maps, axis / allies downtown_assault 87.9 / 31.5 → 98.9 / 41.8,
  hill400_assault 38.8 / 24.0 → 36.0 / 18.6, hill400_defend 11.3 / 39.3 → 13.2 / 38.1, toujane 40.9 / 6.7 → 30.7 / 11.3; all other maps unchanged.
- Baseline update with the per-frame muzzle cache (0x500b40, docs/spec/ai.md "Script sight builtins"): all `canshoot` calls of a frame now use
  the muzzle of the frame's first pose, so the runs change on most maps. Updated where two runs of the build agree: beltot, bergstein, cityhall,
  crossroads, decoytrenches, downtown_sniper, duhoc_defend, hill400_assault, matmata, toujane_ride. decoytown and hill400_defend are not
  deterministic between runs of the same build (main too: decoytown axis 49.2 and 40.6 in two runs), so their lines were left as they were.
  Silent stretches (all / unexplained) then: decoytrenches 22 / 7, crossroads 0 / 0, decoytown 13 / 0 (one run). The decoytrenches ones are the
  aim-only cases: #613 (Thompson, muzzle 42 units above its origin in the stand_aim / stand_shoot_auto blend, the line blocked about 130 units out
  while the eye line, 62 units up, is clear) and #549 (kar98, the same: muzzle traces blocked after 0.9 % of the way, eye trace clear), plus
  corner actors. Whether the original poses put tag_flash that low was not checked against a run of the original.
- Sweep flags (summary.md table column "fire flags" and section "Fire rate"): `NO-FIRE:team` (a team with at least 120 sight seconds fired nothing; reports without
  `ai_sight_secs_<team>` fall back to combat seconds),
  `LOW-RATE:team(rate<baseline)` (an autoplay run under 65% of `tools/firerate-baseline.txt`, the rate of a clean 400 s autoplay run of that map and team,
  produced by `tools/script_sweep.sh --make-firerate-baseline --out DIR` from the `-s1-auto.txt` reports; only maps listed there are compared),
  `SILENT:n` (unexplained stretches; older reports without `silent_unexplained`: all).
- Data (seed 1, 400 s autoplay, axis / allies shots per combat minute, fix present vs the `noteSource` line of `AnimState::updateTime` removed):
  decoytrenches 36.8 / 32.8 vs 34.2 / 14.7, toujane 48.0 / 8.3 vs 27.0 / 4.6, hill400_defend 12.1 / 38.1 vs 7.1 / 36.0, moscow 0.0 / 42.7 vs 0.0 / 8.4.
  Without the fix one or both rates fall to 20-59% of the clean value on all four maps (so the sweep flags each of them), but the clean rates differ too
  much between maps (8 to 48) for a global threshold, hence the per-map baseline. The silent-shooter count (then from an eye trace) did not separate the two builds
  (decoytrenches 26 vs 37, others 1..9 vs 3..5): it points at actors that never shoot a visible enemy for whatever reason, not at this bug.
- Why NO-FIRE counts sight seconds (seed 1, 400 s, plain run: the player stands at the start in god mode). An `enemy` is also set from knowledge alone (the
  player as goal entity of attackers, events), so "combat seconds" include actors that hold cover with an enemy they cannot see, and the shipped cover
  animscripts then wait by design until the enemy can be seen from the exposed position (cover_crouch.gsc main loop, idleUntilEnemyWouldBeVisible; utility.gsc
  canSeeEnemyFromExposed). Sight seconds per team (axis / allies): beltot 4.7 / 47.4 (axis 545 combat s), crossroads 0.3 / 0.3 (1953 / 2734 combat s),
  demolition 0.0 / 0.0 (1545 / 749 combat s). crossroads allies: the intro group holds position in the building, its enemy is an axis actor 2200 units away
  behind walls. demolition: the German and Russian squads sit in cover 423 units apart without a line (cover_left / cover_crouch), waiting for each other.
  beltot axis: the wave runs along its path with the player (3000 units away) as enemy and is shot by the allies within about 1.5 s of the first hit; the
  survivors hold a cover node far from the allies. The demolition axis "35 fire animation starts" are the corner look animation
  (corner_nowall_l_stand_alert2look, which carries a `fire` notetrack), not shooting. Autoplay runs of the same maps fire (axis 27..44 shots per minute).
- Moscow `NO-FIRE:axis` (seed 1) is not an engine gap. The run ends at 1426 s with a mission failure (maps/moscow.gsc:2598 melee_scene_damage, after the
  autoplay kill gate), before any German infantry fight. Nearly all of the 433 axis combat seconds belong to the four riders of the `smoke_halftrack`
  vehicle (map entity at 1170,200,-171, `script_vehicleride 2`, `script_vehiclestartmove 2`). They sit in the halftrack's idle loop
  (maps/_halftrack.gsc:366 guy_idle, animscripted until `unload`, state `scripted`, which only waits for `killanimscript`: animscripts/scripted.gsc:41)
  and never fire by design. `unload` comes at the end of the vehicle path (maps/_vehicle.gsc:1159-1164), and the path starts only when the player
  enters the startmove-2 `trigger_radius` at 598,-128,-165, which the seed-1 bot never reaches. The rest are three `last_guys` with no line of sight
  (27 s). In that section the halftrack's own MG fires as an unmanned turret (counted under `undefined`). With seed 2 the bot gets to 1852 s and the
  axis fire 1134 shots in 3010 combat seconds (22.6 per minute). The only silent shooters are the same riders, so moscow has no axis baseline.

## Inventory across level changes (iw2game `--carry-out` / `--carry-from` / `--inventory-check` / `--end-level`)
Checks that the player's weapons survive the hand-over of a persistent level change (the path iw2view takes with `World::carriedPlayerState` /
`setCarriedPlayer`):
- `--end-level T`: at sim time T runs the mission-complete call of the shipped scripts (`_endmission.gsc` `nextmission`), so the real next map and the
  persistent flag come out; the run stops at the level change (like `--autoplay` does).
- `--carry-out FILE`: at a persistent level change writes the carried player state (current weapon, owned weapons, primary / primaryb, offhand, ammo and clip per
  name, health) as text. Nothing is written for a non-persistent change (the next map starts from its own loadout).
- `--carry-from FILE`: starts the map with that state (as iw2view would) and `--inventory-check S` (default 8) seconds into the run compares it with the
  player's inventory, after the map's start scripts. Output: console block `[inventory ...]`, report section `[inventory]` and fields `inventory_checked`,
  `inventory_lost`, `inventory_unexplained`. Lines: `lost weapon`, `gained weapon`, `slot`, `offhand`, `current weapon`, `ammo`, `clip` differences, then
  `level scripts set the loadout: yes|no` with every `giveweapon` / `takeweapon` / `takeallweapons` / `setweaponslotweapon` call the scripts made
  (time, argument, `file:line function`). A lost weapon is `EXPLAINED-LOSS` when the scripts took it (`takeallweapons`, `takeweapon` of it, or a
  `setweaponslotweapon` into the slot it held) and `UNEXPLAINED-LOSS` otherwise: that is the real finding (a weapon that vanished with nothing taking it).
  Gains, offhand and ammo changes are information (maps/_loadout.gsc and the level scripts hand out fixed weapons). A `giveweapon` the engine refuses
  (both primary slots full) is a script runtime error in the same run.
- Sweep: `tools/script_sweep.sh --carry [--maps "a b"] [--seconds 120]` autoplays each map, ends it 20 s before `--seconds` with `--end-level`, and for every run that
  ended in a persistent change starts the next map with `--carry-from` (30 s, tag `carry-from-<map>`); `--full` does the same for the autoplay runs that
  reached a persistent change by themselves. summary.md has the section "Inventory across level changes" and the table column "inventory" (lost/unexplained).
- Manual: `iw2game <main> decoytrenches 120 --god --end-level 30 --carry-out a.carry`, then `iw2game <main> decoytown 30 --god --carry-from a.carry`.
- Limits: only map pairs the scripts actually link through a persistent change are checked; the carried state is what the bot has at the change time, so the
  bot's pickups are carried like a player's; a map with several exits only follows the one `nextmission` takes.
