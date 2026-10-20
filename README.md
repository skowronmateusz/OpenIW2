# OpenIW2

A from-scratch, native macOS (Apple Silicon) engine that plays the **Call of Duty 2** single-player campaign
using the game data from your own retail copy.

> Status: early. Asset loading (packs, textures, maps, materials) and a first native Metal map viewer work. See [docs/ROADMAP.md](docs/ROADMAP.md).

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
