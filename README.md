# OpenIW2

A from-scratch, native macOS (Apple Silicon) engine that plays the **Call of Duty 2** single-player campaign
using the game data from your own retail copy.

> Status: early. Only the asset filesystem works so far. See [docs/ROADMAP.md](docs/ROADMAP.md).

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

## Point it at your game data

Copy the `main` folder from an installed, patched (1.3) game anywhere on your Mac, then:

```bash
./build/iw2fs /path/to/main packs --lang english
./build/iw2fs /path/to/main list maps/ --lang english
./build/iw2fs /path/to/main cat maps/_utility.gsc
```
