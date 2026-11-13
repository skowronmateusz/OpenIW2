<p align="center">
  <img src="logo.png" alt="OpenIW2" width="720">
</p>

# OpenIW2

A native macOS engine (Apple Silicon, C++20, SDL3, Metal) that runs the **Call of Duty 2** campaign and multiplayer
from the game files of **your own retail copy**. It was written from scratch, without the original source code: the
original's behavior was written down as specs, then reimplemented from those specs.

> **Status: playable, under active development.** Some details still differ from the original.
> Bug reports are welcome in [Issues](../../issues).

## Play

### You need

- A Mac with Apple Silicon (M1 or newer), macOS 14 or later.
- Your own **Call of Duty 2** for PC, updated to **patch 1.3**. OpenIW2 includes no game files.
  Copy the game's `main` folder (about 3.4 GB) to your Mac, for example into Documents.
  On Windows it is in the install folder, usually `C:\Program Files\Activision\Call of Duty 2\main`.

### Install

1. Download `OpenIW2-<version>.dmg` from [Releases](../../releases).
2. Open it and drag **OpenIW2** into **Applications**.
3. The app is not notarized, so macOS blocks it at first. Once per downloaded version, run this in Terminal:

   ```bash
   sudo xattr -r -d com.apple.quarantine /Applications/OpenIW2.app
   ```

### Start

Open **OpenIW2**. On the first start it asks for your `main` folder and checks that all needed files are there; if
some are missing, it lists them. It remembers the folder and opens the main menu, which shows the OpenIW2 version in its
bottom right corner. Your `main` folder is never changed.

| What | Where |
|---|---|
| Profiles, saved games | `~/Library/Application Support/OpenIW2/OpenIW2/` |
| Screenshots (`F12`) | `~/Library/Application Support/OpenIW2/OpenIW2/screenshots/` |
| Logs (one file per start, `openiw2-<date>-<time>.log`, the last 10 kept; **Options > OpenIW2 > Open log folder**) | `~/Library/Logs/OpenIW2/` |

The language follows the localized files in your `main` folder.

### OpenIW2 settings

**Options > OpenIW2** (also in the multiplayer options) has settings the original game does not have. They are saved
in your profile.

| Setting | What it does |
|---|---|
| Hit marker | Shows a small X at the crosshair when you hit an enemy. Off by default. "Red on kill" turns it red on a kill. |
| Show FPS | Frame counter in the top right corner. |
| Cheats | Single player only. Turns on cheats for the mission and shows three key rows: fullbright (default `L`), god mode (`F10`) and noclip (`N`), all rebindable there. Never works in multiplayer. |
| Open log folder | Opens the folder with the logs in Finder (for a bug report). |

### Options that differ

Hidden because they do nothing here: texture filter (always anisotropic), renderer, SLI, shadows, aspect ratio, sound
quality, sound card, the console, and the multiplayer connection speed.

- **Sync every frame** is on by default.
- **Anti-aliasing** defaults to 4x and applies at the next start.
- **Video mode** and **refresh rate** do nothing. Resize the window instead.

### Multiplayer

**Multiplayer** in the main menu switches to the multiplayer menus.

- **Start new server**: choose gametype (Deathmatch, Team Deathmatch, Search and Destroy, Capture the Flag,
  Headquarters) and map. The **Bots & weapons** page (an OpenIW2 addition) sets up to 15 bots, their skill, and which
  weapon classes are allowed.
- **Join game** lists servers on your local network and joins the one you pick. To join another server, add it to
  **Favorites** by IP address or DNS name (default port 28960).
- A listen server uses UDP port 28960, or the next free one up to 28969. The server list on other computers looks at
  28960 to 28963.
- If the macOS firewall asks whether OpenIW2 may accept incoming connections, choose **Allow**. After **Deny**, others
  cannot see your server. To fix it, check Firewall options in System Settings and start a server again.
- **Dedicated** (LAN or Internet, the same here) closes the game window and opens a server console window. Details in
  [docs/dedicated-server.md](docs/dedicated-server.md#dedicated-server-from-the-menu).

There is no CD key check, no PunkBuster and no Activision master server. Voting, voice chat, mods and automatic updates
are not available. Joining with the original 1.3 game has not been tried yet.

### Dedicated server without a window

`iw2server` is inside the app:

```bash
/Applications/OpenIW2.app/Contents/MacOS/iw2server ~/Documents/main mp_toujane tdm --bots 6 --maxclients 20
```

```
iw2server <main-dir> [<map> [<gametype>]] [options] [+command args ...]
```

| Option | Default | Values |
|---|---|---|
| `<gametype>` | `dm` | `dm`, `tdm`, `sd`, `ctf`, `hq` |
| `--port N` | 28960 | 0-65535 (0 = free port) |
| `--maxclients N` | 8 | 1-20, players and bots together |
| `--bots N` | 0 | 0-19 |
| `--bot-skill N` | 3 | 0 recruit ... 3 veteran |
| `--weapons LIST` | all | `rifle`, `bolt`, `sniper`, `smg`, `mg`, `shotgun`, `grenades` |
| `--scorelimit N`, `--timelimit MIN`, `--roundlimit N` | per gametype | 0 = no limit |
| `--cvar name=value` | | any cvar, repeatable |
| `+command args` | | console command at start-up, for example `+exec server.cfg` |

All options, maps, weapon classes, `server.cfg`, rcon and the map rotation:
[docs/dedicated-server.md](docs/dedicated-server.md). Started from the app, the server also writes a log,
`~/Library/Logs/OpenIW2/iw2server-<date>-<time>-<port>.log`; it prints the path after its start line.

### Smooth play over Wi-Fi

On a Mac, AirDrop and Handoff (AWDL) briefly take the Wi-Fi radio away about twice a second, which makes other players
stand still and then jump. Run the server on a cable and turn off AirDrop and Handoff while you play. More options:
[docs/dedicated-server.md](docs/dedicated-server.md#wi-fi-and-awdl).

### Reporting a bug

Open an [issue](../../issues) with:

- what happened and what you expected, the map and roughly where;
- steps to reproduce, and a screenshot or video if it is visual;
- the log of that start: **Options > OpenIW2 > Open log folder** opens `~/Library/Logs/OpenIW2/`; attach the newest
  `openiw2-<date>-<time>.log` (for a server, `iw2server-...log`), and a `...-trace-...log` next to it if there is one.
  After a crash, also attach the newest `OpenIW2-*.ips` from `~/Library/Logs/DiagnosticReports/`
  (in Finder: `Cmd+Shift+G`, paste the folder path).

The first line of the log names your OpenIW2 version, the next ones your macOS version and Mac model. The log does not
contain your user name, passwords or chat text. Details: [docs/logging.md](docs/logging.md).

## What this repository contains

- Original engine code, tests and specs only. No game files, scripts, text, disassembly or decompiled code.
  The project does not distribute game files, help you obtain them or bypass copy protection.
- The specs in [`docs/spec/`](docs/spec) describe behavior in my own words and cite addresses in the original
  binaries as evidence. They contain no copied code.

## Status

Behavior is specified in [`docs/spec/`](docs/spec); open points are marked `TODO(spec)`.

| Area | State |
|---|---|
| Asset files, textures, maps, models, animation | Done |
| Renderer (Metal) | Maps, lighting, water, fog, bloom, effects; some effect details and a few post effects missing |
| Scripts and game simulation | Mission scripts, movement, weapons, vehicles, turrets |
| AI | Works; some squad and escort behaviors differ from the original |
| Audio, menus, cutscenes | Done; reverb and some audio details missing |
| Campaign | Objectives, saves, difficulty. Nobody has played the full campaign through yet |
| Multiplayer | Five gametypes on all 15 maps, bots, listen and dedicated server, LAN play |

## Build from source

Needs macOS 14+ on Apple Silicon, Xcode command line tools, CMake 3.25+, Ninja, and SDL3 (`brew install sdl3`;
without it only the command-line tools and tests are built). You also need your `main` folder (patch 1.3).

```bash
cmake -S . -B build -G Ninja
cmake --build build
IW2_MAIN=/path/to/main ctest --test-dir build --output-on-failure
```

Tests that need game data find the `main` folder through `IW2_MAIN` or `-DIW2_TEST_MAIN=/path/to/main`; without it
they are not registered.

Run the game from the build folder:

```bash
./build/iw2view /path/to/main --menu        # main menu
./build/iw2view /path/to/main moscow        # one level directly
```

The cheat keys need `--cvar sv_cheats=1` here. Command-line tools (`iw2game`, `iw2fs`, `iw2asset`, `iw2script`,
`iw2roq`, `iw2server`, `iw2netclient`, `iw2netinfo`) are built too. Debug switches:
[docs/debug-flags.md](docs/debug-flags.md).

## How it was built

I built OpenIW2 by directing AI coding agents in [Claude Code](https://claude.com/claude-code) (Claude from Anthropic,
GLM from Z.ai). An agent writes the original's behavior into a spec, then the code is written from the spec, with a
test for every behavior. Method: [docs/reverse-engineering.md](docs/reverse-engineering.md). Thanks to the authors of
[REA](https://github.com/morluto/rea), Ghidra and [LEANN](https://github.com/yichuan-w/LEANN).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). In short: no game files, no copied code, spec first.

## License

GNU General Public License, version 2 or (at your option) any later version; see [LICENSE](LICENSE). Game data is not
covered. The software comes **without any warranty** (license sections 11 and 12).

## Trademarks

Call of Duty is a registered trademark of Activision Publishing, Inc. Infinity Ward is a trademark of Activision
Publishing, Inc. OpenIW2 is an independent project, **not affiliated with, endorsed by or sponsored by** Activision or
Infinity Ward. The names only say which game this engine is compatible with.
