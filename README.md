# OpenIW2

A from-scratch, native macOS (Apple Silicon) engine that plays the **Call of Duty 2** single-player campaign
using the game data from your own retail copy.

> Status: playable but unfinished. Mission scripts run, AI, vehicles, menus and save games work; see [Status](#status).

## Status

State as of 2026-10-29. Behavior is specified in [`docs/spec/`](docs/spec); open points are marked `TODO(spec)` in the specs and code.

| # | Area | Works | Missing |
|---|---|---|---|
| M0 | Asset filesystem (`.iwd` packs, search order, case-insensitive paths) | Lists and reads every file of a retail install (`iw2fs`) | – |
| M1 | Asset decoders (textures, maps, materials, models, animations) | `.iwi` textures, `.d3dbsp` maps with lightmaps, materials/techsets, `xmodel*`, `xanim` (`iw2asset`) | Remaining rare texture variants and unusual material combinations (see `TODO(spec)` in `src/assets/`) |
| M2 | Renderer + map viewer (SDL3, Metal; `iw2view`) | Textured, lightmapped maps with sun, light grid, water, fog, glow/bloom, distortion, dynamic lights, effects; MSL shaders are own code that reproduces the behavior of the shipped HLSL shaders | Some effect sorting details, a few post effects, performance tuning |
| M3 | Models and animation | Static and animated models, characters and weapons with animation trees, notetracks and scripted animation | Rarely used blend and notetrack corner cases |
| M4 | GSC compiler + VM | All shipped `maps/*.gsc` compile; level scripts run; built-in function table implemented (`iw2script`, `docs/spec/builtins.md`) | A few built-ins and error paths still marked `TODO(spec)` |
| M5 | Game simulation | Entities, collision, player movement, weapons, grenades, damage, mantling, turrets, player-driven vehicles | Remaining weapon and movement corner cases |
| M6 | AI | Pathnodes, animscripts (GSC), cover, squads, grenades, vehicles | Some squad and escort behaviors differ from the original |
| M7 | Audio, UI, cinematics | Sound aliases and mixing, subtitles, `.menu` UI, main menu and mission select, RoQ cinematics (`iw2roq`) | Reverb and some audio details |
| M8 | Save/load, objectives, campaign | Objectives, HUD, compass, level changes, save games, difficulty | The full campaign has not been verified end to end by a human; scripted autoplay reaches mission success on 7 of 28 maps ([`tools/iw2game`]) |

## What this repository contains, and what it does not

- It contains **original engine code only**.
- It contains **no game assets, executables or code** from the original game. You need your own copy of the game.
- Engine behavior is specified in [`docs/spec/`](docs/spec). Each spec comes from black-box and static analysis
  of the original binaries and cites its evidence. Specs describe behavior; they never copy code.

Call of Duty is a trademark of Activision Publishing, Inc. This project is not affiliated with or endorsed by
Activision or Infinity Ward.

## Requirements

- macOS 14+ on Apple Silicon
- Xcode command line tools, CMake 3.25+, Ninja
- `brew install sdl3` (needed from the renderer milestone on)

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

If your `cmake` is an Intel build running under Rosetta, the project still targets `arm64` (set in `CMakeLists.txt`).

## Point it at your game data

Copy the `main` folder from an installed, patched (1.3) game anywhere on your Mac, then:

```bash
./build/iw2fs /path/to/main packs --lang english
./build/iw2fs /path/to/main list maps/ --lang english
./build/iw2fs /path/to/main cat maps/_utility.gsc
```

Fly through a map (mouse + WASD, Shift = faster, F = fullbright, P = print camera position):

```bash
./build/iw2view /path/to/main toujane
```

## License

OpenIW2 is licensed under the GNU General Public License, version 2 or (at your option) any later version.
See [LICENSE](LICENSE). This keeps the door open to reusing code from the GPL-licensed id Tech 3 (Quake III Arena)
source release, the engine family CoD2's engine descends from. Game data is not covered: you need your own copy.
