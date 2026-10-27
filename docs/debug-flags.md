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
| `IW2_PROF=N` | iw2view | Prints average per-frame CPU timings every N frames and quits after two reports. |
| `IW2_NO_HUD=1` | iw2view `--shot` | Draws no script HUD and no HUD fade. |
| `IW2_FX_DEBUG=1` | iw2view | Prints effect sprite debug lines, including the view-model flash tag. |
| `IW2_TRACER_TEST=N` | iw2view | With `--shot`, N tracer attempts (the 0.4 chance applies) from beside the view to 1500 units ahead, aged `IW2_FX_MS` (default 100). |
| `IW2_FX_HIST=1` | iw2view | With `IW2_FX_DEBUG`, prints the 12 largest per-element particle counts (type, name, first shader) each frame. |
| `IW2_RIDE_TRACE=1` | iw2view | Per-frame trace of the ridden vehicle (origin, angles, turret, camera link). |
| `IW2_CROSSHAIR_DEBUG=1` | iw2game/iw2view | Prints the crosshair trace eye/yaw and the hit entity/target name. |
| `IW2_TOUCH_TEST=<s>` | iw2game | At sim time `<s>` runs the vehicle touch callback (0x512d40) for the first vehicle with collisionDamage > 0 placed 100 units behind the player, moving at 400 units/s at it; prints the target health before and after; `IW2_TEST_ACTOR=1` targets the first living actor instead of the player. |
| `IW2_ROCKET_TEST=<s>` | iw2game | At sim time `<s>` fires the first vehicle's turret rocket at the player from 300 units away; prints the player health before and 3 s after. |
| `IW2_KILL_VERBOSE=1` | iw2game | With `IW2_KILL_VEHICLES`, also prints each vehicle's model and health before the kill. |
| `IW2_FIRE_VEHICLES=<s>` | iw2game | At sim time `<s>` sends the script notify `turret_fire` to every vehicle (the AI tank fire loop in `_tank.gsc`), to test vehicle weapons headless. |
| `IW2_KILL_VEHICLES=<s>` | iw2game | At sim time `<s>` deals 100000 damage (attacker: the player) to every script vehicle, to test the death chain. |
| `IW2_THREADS_PREFIX=<p>` | iw2game | Lists every live script thread whose wait site starts with `<p>`. |
| `IW2_MENU_KEYS=a,b,...` | iw2view `--menu` | Scripted menu keys (up/down/enter/esc, `pause`, `wait`, `set:name=value`). |
| `IW2_NO_DEPTH_HACK=1` | iw2view | Draws the view weapon without the depth hack (depth range 0..0.2, near plane 0.1): it clips through walls and the near plane again. |
| `IW2_BOLD_MESSAGE=<text>` | iw2view `--shot` | Shows a bold game message (iprintlnbold window). |

## Command-line options
- iw2view: `--menu` (front end without a level), `--cvar name=value`, `--ride F,R` / `--ride-after S` / `--ride-fire`
  (drive/fire a vehicle headlessly), `--follow-vehicle N`, `--follow-dist`, `--follow-yaw`, `--follow-pitch`,
  `--force-turret p,y`, `--force-jolt p,r`, `--spawn-all`.
- iw2game: `--spawn-all`, `--cvar name=value`, `--answer-menus`, `--level-notify`, `--lookat-test`.
- iw2game `<main> <map> --item-test [weapon=N] weapon ...`: spawns weapon items at the player and prints stock / clip after the touch pickup (`weapon=N` first sets that weapon's clip, e.g. `mk1britishfrag=0 fraggrenade fraggrenade`).

## Menu debug helpers (iw2view --menu)

| Flag | Effect |
|---|---|
| `IW2_SHOT_MENU=a,b` | Opens the named menus before a `--menu --shot`. |
| `IW2_SHOT_SET=dvar=value,...` | Sets cvars before a `--menu --shot`. |
| `IW2_MENU_KEYS` tokens | `open:name`, `click:X/Y`, `left`, `right` (besides the existing tokens, `quit`). |
| `IW2_EXIT_AFTER=N` | Exits iw2view after N seconds. |


- Env `IW2_ACC_DEBUG=1` (iw2game/iw2view): prints one `ACC <weapon> <ai|player> dist <d> chance <c> hit|miss` line per AI/player shot (combat.cpp).
- Env `IW2_PMOVE_FORWARD=1` (iw2game `--pmove` / fall test): the test pmove holds forward, so footstep events reach the AI (stats line "player noise events").
- iw2game stats line also prints "actor noise events" (the 500 ms movement noise of actors, 0x5485a0).
| `IW2_SHOT_KEYS=a,b,...` | Applied before the screenshot of a `--shot` run when `--menu` or `IW2_SHOT_MENU` is set. Tokens: `open:name`, `click:X/Y`, `hover:X/Y`, `up`, `down`, `esc`, `left`, `right`, `enter`, `bindkey:NAME` (feeds the key to a BIND item that waits for one, e.g. `bindkey:PGUP`; also valid in `IW2_MENU_KEYS`). |
- Env `IW2_GROUND_DEBUG=1` (iw2game): counts the actor ground-trace hits/misses of the movement noise (stats line "ground trace hit/miss"); toujane 40 s: 24444/1123.
- iw2game stats line also prints "grenade away-flee/cower/plane-retry" (grenadeReact outcomes; duhoc_assault 150 s: 34 grenades, 6 flee, 0 cower, 0 retry).
- Env `IW2_PLAY_CINEMATIC=<name>` (iw2view): plays `video/<name>.roq` (e.g. `iw_logo`) before the level starts; any key skips it. Scripted `cinematic()` calls play automatically before the next level (docs/spec/video.md).
- Env `IW2_PLAY_CINEMATIC=<name>[!]` (iw2view): plays `video/<name>.roq` before the level (`!` = unskippable); `IW2_CINEMATIC_LETTERBOX=1` / `IW2_CINEMATIC_LOOP=1` set the letterbox / loop flags of that run (docs/spec/video.md).
- Env `IW2_CINEMATIC_STATS=1` (iw2view): after every movie prints wall time, last frame, dropped frames, audio length and the picture-minus-audio-played offset (min / mean / max).
- `--no-intro` / env `IW2_SKIP_INTRO=1` (iw2view `--menu`): no start-up movies (legal, atvi, IW_logo, cod_intro).
- Env `IW2_EXIT_AFTER=N` also leaves a playing movie after N seconds.
