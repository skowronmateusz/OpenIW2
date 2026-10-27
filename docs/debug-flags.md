# Debug flags and environment variables

Developer switches of `iw2view` / `iw2game`. None change gameplay when unset. The full list of `IW2_*` variables is
`grep -rhoE 'getenv\("IW2_[A-Z0-9_]+"\)' apps src tools`; this page covers the ones added recently. Others are
described in the code that reads them.

## Environment variables
| Variable | Tool | Effect |
|---|---|---|
| `IW2_NO_CULL=1` | iw2view | Disables frustum culling of animated-entity skinning (actors are skinned even when off screen). For comparing output/perf. |
| `IW2_PROF=N` | iw2view | Prints average per-frame CPU timings every N frames and quits after two reports. |
| `IW2_NO_HUD=1` | iw2view `--shot` | Draws no script HUD and no HUD fade. |
| `IW2_FX_DEBUG=1` | iw2view | Prints effect sprite debug lines, including the view-model flash tag. |
| `IW2_FX_HIST=1` | iw2view | With `IW2_FX_DEBUG`, prints the 12 largest per-element particle counts (type, name, first shader) each frame. |
| `IW2_RIDE_TRACE=1` | iw2view | Per-frame trace of the ridden vehicle (origin, angles, turret, camera link). |
| `IW2_CROSSHAIR_DEBUG=1` | iw2game/iw2view | Prints the crosshair trace eye/yaw and the hit entity/target name. |
| `IW2_KILL_VEHICLES=<s>` | iw2game | At sim time `<s>` deals 100000 damage (attacker: the player) to every script vehicle, to test the death chain. |
| `IW2_THREADS_PREFIX=<p>` | iw2game | Lists every live script thread whose wait site starts with `<p>`. |
| `IW2_NOTIFY_LOG=level` or `all` | iw2game | Prints every script notify (`level`: only on the level object; `all`: every object) with its sim time. For walking mission flows. |
| `IW2_SOUND_LOG=1` | iw2game | Prints every sound start (`[sound <ms>ms] alias`). Use with `--mixer` so scripts get the real sound lengths (dialogue timing). |
| `IW2_DEV_SNAP_GOAL_YAW=1` | iw2game/iw2view | DEV-ONLY BYPASS, not original behaviour: an actor whose goal was set with `setgoalnode` is turned to that node's angles right before it is notified "goal". Lets moscow's `letlev.angles != node.angles` loop (moscow.gsc:5031, the commissar yaw stall) pass so later stages can be walked. |
| `IW2_MENU_KEYS=a,b,...` | iw2view `--menu` | Scripted menu keys (up/down/enter/esc, `pause`, `wait`, `set:name=value`). |
| `IW2_BOLD_MESSAGE=<text>` | iw2view `--shot` | Shows a bold game message (iprintlnbold window). |

## Command-line options
- iw2view: `--menu` (front end without a level), `--cvar name=value`, `--ride F,R` / `--ride-after S` / `--ride-fire`
  (drive/fire a vehicle headlessly), `--follow-vehicle N`, `--follow-dist`, `--follow-yaw`, `--follow-pitch`,
  `--force-turret p,y`, `--force-jolt p,r`, `--spawn-all`.
- iw2game: `--spawn-all`, `--cvar name=value`, `--answer-menus`, `--level-notify`, `--lookat-test`.

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
