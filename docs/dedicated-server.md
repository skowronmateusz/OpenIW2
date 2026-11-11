# Running a dedicated server

`iw2server` is OpenIW2's multiplayer server without a window. It lives inside the app, next to the game:

```bash
/Applications/OpenIW2.app/Contents/MacOS/iw2server <path to main> mp_toujane tdm \
    --bots 6 --bot-skill 1 --maxclients 20 --weapons bolt,sniper,grenades --scorelimit 50
```

`<path to main>` is the `main` folder of your own copy of the game (the folder with the `iw_*.iwd` files). The server
loads the map, adds the bots, prints `iw2server: mp_toujane tdm on UDP 28960` and waits for players. Stop it with
Ctrl+C or the `quit` command.

There is no CD key check, no PunkBuster and no Activision master server: players find the server on the local
network or join it by address.

```
iw2server <main-dir> [<map> [<gametype>]] [options] [+command args ...]
```

Options go before the first `+command`; everything after a `+command` belongs to it.
Options on the command line win over `server.cfg`. A wrong option prints an error and the usage text and exits with
code 2, so nothing starts half configured.

## Options

| Option | Type | Default | Valid values |
|---|---|---|---|
| `<main-dir>` | folder | (required) | the `main` folder with the `.iwd` files |
| `<map>` | name | none | a map of the table below. Without a map, `+map_rotate` (or `+map <name>`) must load one |
| `<gametype>` | name | `dm` | `dm`, `tdm`, `sd`, `ctf`, `hq`. Only after a map |
| `--port N` | whole number | 28960 | 0..65535 (0 picks a free port). Also `+set net_port N` |
| `--maxclients N` | whole number | 8 | 1..20. Players and bots share these slots. Also `+set sv_maxclients N` |
| `--bots N` | whole number | 0 | 0..19. Added on every map, as many as the slots allow |
| `--bot-skill N` | whole number | 3 | 0..3, see [Bots](#bots) |
| `--weapons LIST` | list | everything allowed | `rifle`, `bolt`, `sniper`, `smg`, `mg`, `shotgun`, `grenades`, `all`, comma separated. See [Weapon classes](#weapon-classes) |
| `--scorelimit N` | whole number | 50 dm, 300 tdm, 10 sd, 5 ctf, 600 hq | 0..100000, 0 = no limit. See [Score and time limits](#score-and-time-limits) |
| `--timelimit MIN` | number | 30 (sd: 0) | 0..1440 minutes, decimals allowed (7.5), 0 = no limit |
| `--roundlimit N` | whole number | 0 | 0..1000, only for `sd`, 0 = no limit |
| `--cvar name=value` | text | none | sets a cvar before the first map. Can be repeated |
| `--seconds N` | whole number | run until quit | 0..31536000. Stops the server after N seconds |
| `--no-console` | flag | off | do not read commands from the terminal (for a server started by another program) |
| `--help` | flag | | prints the usage and exits |
| `+command args` | console command | none | run at start-up in order, for example `+exec server.cfg +map_rotate` |

The defaults for the limits are those of the game's own default configuration, which the server runs first.
Exit codes: 0 after `quit`, Ctrl+C or the end of `--seconds`; 1 when the port cannot be bound or no map could be
loaded; 2 for a wrong command line.

## Maps and gametypes

All 15 maps run in all five gametypes. A map named without a gametype starts as `dm`.

| Map | Name | Allies | Axis |
|---|---|---|---|
| `mp_breakout` | Villers-Bocage, France | British | German |
| `mp_brecourt` | Brecourt, France | American | German |
| `mp_burgundy` | Burgundy, France | American | German |
| `mp_carentan` | Carentan, France | American | German |
| `mp_dawnville` | St. Mere Eglise, France | American | German |
| `mp_decoy` | El Alamein, Egypt | British | German |
| `mp_downtown` | Moscow, Russia | Russian | German |
| `mp_farmhouse` | Beltot, France | American | German |
| `mp_harbor` | Rostov, Russia | Russian | German |
| `mp_leningrad` | Leningrad, Russia | Russian | German |
| `mp_matmata` | Matmata, Tunisia | British | German |
| `mp_railyard` | Stalingrad, Russia | Russian | German |
| `mp_rhine` | Wallendar, Germany | American | German |
| `mp_toujane` | Toujane, Tunisia | British | German |
| `mp_trainstation` | Caen, France | American | German |

| Gametype | Name | What is played |
|---|---|---|
| `dm` | Deathmatch | every player for themselves |
| `tdm` | Team Deathmatch | two teams, kills count for the team |
| `sd` | Search and Destroy | rounds, one life per round, the attackers plant a bomb, the defenders stop them |
| `ctf` | Capture the Flag | take the enemy flag to your own |
| `hq` | Headquarters | hold the radio that moves around the map |

## Score and time limits

A match ends when a limit is reached (then the next map of the rotation follows, or the same map again without a
rotation). `0` switches that limit off for every gametype.

| Gametype | What `--scorelimit` counts | Who wins on reaching it |
|---|---|---|
| `dm` | the kills of one player (a suicide or a self-inflicted death costs a point) | the first player to reach it |
| `tdm` | the kills of a team (a team kill takes a point from the killer, not from the team) | the first team to reach it |
| `sd` | the rounds a team has won | the first team to reach it |
| `ctf` | the flags a team has captured | the first team to reach it |
| `hq` | the seconds a team has held the radio (one point per second while the radio is held and the team has a player) | the first team to reach it |

- `--timelimit` is in minutes of play on the map. When it runs out the best player or team wins. In `sd` the time is
  added up over the rounds of the map and checked at the end of each round, so a round is never cut short.
- `--roundlimit` exists only in `sd`: the map ends after that many rounds have been played, whatever the score. It is
  refused for another gametype given on the command line and ignored for the other gametypes of a rotation.
- The limits apply to the gametype of every map the server loads, including the following maps of a rotation.
  For a mixed rotation put the limits of each gametype into `server.cfg` instead
  (`set scr_tdm_scorelimit 100`, `set scr_dm_timelimit 15`, ...; the cvar names are `scr_<gametype>_scorelimit`,
  `scr_<gametype>_timelimit` and `scr_sd_roundlimit`). Other settings of `sd` in the default configuration:
  `scr_sd_roundlength` (minutes per round), `scr_sd_graceperiod`, `scr_sd_bombtimer`.
- A value given with `--scorelimit` or `--timelimit` wins over `server.cfg`.

## Bots

Bots are an OpenIW2 addition: the original game has none. They count against `--maxclients`; a human who joins
needs a free slot, so keep a few free (`--maxclients 20 --bots 6`). Bots obey the weapon list, play the objectives of
the gametype and use the navigation graph of the map.

| `--bot-skill` | Name | Reaction | Aim | Sight |
|---|---|---|---|---|
| 0 | recruit | slow (about 0.9 s) | poor, wide misses | short |
| 1 | regular | about 0.6 s | fair | medium |
| 2 | hardened | about 0.4 s | good, checks corners | long |
| 3 | veteran | about 0.25 s | best accuracy, sees all around | very long |

The server's default is 3 (veteran); the Bots & weapons page of the game's menu starts at 1. Details of the
presets are in `docs/spec/mp-bots.md`.

**Navigation graph.** The first time a map is loaded with bots, the server generates the graph the bots walk on. That
takes between a few seconds and about 40 seconds depending on the map, and the server does not answer players
until it is done. The graph is cached in `~/Library/Caches/openiw2/nav` (one `.nav` file per map and gametype family,
because the spawn points differ) and the next start takes about two seconds. Set `IW2_NAV_CACHE` to use another
folder; delete the folder to make the graphs again.

## Weapon classes

`--weapons` lists the weapon classes that players and bots may choose from the weapon menu. Without the option
everything is allowed. Exactly the listed classes are allowed, so `--weapons bolt,sniper,grenades` means
bolt-action rifles and sniper rifles, plus frag and smoke grenades, and nothing else. Pistols are given to everybody
whatever the list says. The list is not case sensitive; commas or spaces separate the classes.

| Class | Meaning | American | British | Russian | German |
|---|---|---|---|---|---|
| `rifle` | semi-automatic rifles | M1A1 Carbine, M1 Garand | M1 Garand | SVT-40 | G43 |
| `bolt` | bolt-action rifles without a scope | | Lee-Enfield | Mosin-Nagant | Kar98k |
| `sniper` | scoped rifles | Springfield | Lee-Enfield (scoped) | Mosin-Nagant (sniper) | Kar98k (sniper) |
| `smg` | submachine guns | Thompson, Grease Gun | Sten, Thompson | PPS-42, PPSh-41 | MP40 |
| `mg` | automatic rifles and light machine guns | BAR | Bren | | MP44 |
| `shotgun` | the shotgun | yes | yes | yes | yes |
| `grenades` | frag and smoke grenades | all | all | all | all |

(`smg` is what the game calls machine pistols; `mg` is what the Bots & weapons page calls automatic rifles / MG.)

A player of a nation needs at least one weapon to pick, otherwise they could not join the game. So a list is refused
(with a message, before the server starts) when

- it names no weapon class: `grenades` alone, `none`, or an empty list;
- it contains an unknown name; or
- a nation would have no weapon at all. The table shows where that can happen: the Americans have no `bolt` weapon
  and the Russians have no `mg` weapon, so `--weapons bolt` (nothing for the Americans) and `--weapons mg`
  (nothing for the Russians) are refused, naming the nation. `--weapons bolt,sniper,grenades` works for all four
  nations because every nation has a sniper rifle.

The Bots & weapons page of the game's menu follows the same rules: it has a Bolt-action rifles switch and does not
start a server with only grenades or with nothing allowed; it shows a message instead.

## server.cfg

`+exec server.cfg` reads the file from the main folder, the folder you pass as `<path to main>` (next to the `.iwd`
files): `<path to main>/server.cfg`. `exec` without an extension adds `.cfg`, and a file that is not found prints
`couldn't exec <file>` and the server goes on. The server always runs the game's own default configuration first, so a
setting in your file replaces the default.

A working example (`<path to main>/server.cfg`):

```
// Name shown in the server list
set sv_hostname "Toujane night"
// Remote console: without a password rcon is off
set rcon_password "change-me"
// Optional game password for players
set g_password ""
// The maps in order; the list starts over at the end
set sv_mapRotation "gametype tdm map mp_toujane map mp_carentan gametype dm map mp_leningrad"
set scr_friendlyfire 0
set scr_killcam 1
set scr_tdm_scorelimit 100
set scr_dm_timelimit 15
```

Start it with:

```bash
/Applications/OpenIW2.app/Contents/MacOS/iw2server <path to main> --maxclients 20 --bots 4 \
    --bot-skill 2 --weapons all +exec server.cfg +map_rotate
```

`+map_rotate` loads the first entry of `sv_mapRotation`, so no map is given on the command line. Options such as
`--bots` stay before the first `+command`.

- `sv_mapRotation` is a list of `gametype <name>` and `map <name>`; a `gametype` stays in force for the following
  maps. When a match ends the next entry loads; at the end of the list it starts over. Without a rotation the same
  map restarts.
- `sv_hostname` is the name in the server list (default `openiw2`). `g_password` protects the server;
  `sv_privateClients` and `sv_privatePassword` reserve slots for players who give that password.
- `rcon_password` enables remote console: a client sends `rcon <password> <command>` to the server's address and gets
  the output back. Wrong passwords get `Invalid password.`; replies are limited to two per second per address.
- Set the cvars of the options in the file too if you like (`set sv_maxclients 20`), but the command-line options win.

## Console commands

The server reads commands from the terminal (unless `--no-console`), from `+command` arguments, from `server.cfg`
and from rcon.

| Command | What it does |
|---|---|
| `status` | the map and a table of players and bots (number, score, ping, name, address) |
| `map <name>` | loads a map with the current `g_gametype` (also `devmap`) |
| `map_rotate` | loads the next entry of `sv_mapRotation` |
| `map_restart`, `fast_restart` | loads the current map again |
| `kick <name>`, `kick all` | removes a player or bot by name (colour codes ignored, case insensitive) |
| `clientkick <number>` | removes the client with that number from `status` |
| `say <text>` | message to everybody, shown as `console: <text>` |
| `tell <number> <text>` | message to one player |
| `serverinfo` | the cvars the server publishes in its status |
| `dumpuser <name>` | the connection details a player sent |
| `addtestclient [N]` | adds N bots (up to the free slots) |
| `set <cvar> <value>` (also `seta`, `sets`) | sets a cvar, for example `set sv_hostname "New name"` |
| `<cvar>` | prints the cvar's value; `<cvar> <value>` sets it |
| `exec <file>` | runs a config file from the main folder |
| `quit` | stops the server |

`status`, `map_restart`, `kick` and the like print `Server is not running.` before a map is loaded.
Changing `sv_hostname` takes effect at once. The running match picks up a changed score or time limit and a
changed `scr_allow_<weapon>` within a few seconds; the weapon list (`iw2_weaponclasses`) and `g_gametype` are read
when a map loads, so they apply from the next map or `map_restart`.

## Network

- The server listens on UDP port 28960 (change it with `--port`). It binds all interfaces, so it takes players from
  the local network and, with a port forward on your router, from the internet.
- The first time you run `iw2server`, macOS asks whether it may accept incoming network connections. Choose Allow;
  otherwise other computers cannot reach it. The choice can be changed later in System Settings, Network, Firewall.
- Players on the same network see the server in **Join game** of the multiplayer menu; the list refreshes with a LAN
  broadcast. Otherwise add the server under **Favorites** in that screen (a numeric address or a DNS name, the port can be left out
  when it is 28960), or start the client with the address, for example from Terminal
  `/Applications/OpenIW2.app/Contents/MacOS/OpenIW2 <path to main> +connect cod.example.com:28960`; the console
  command `connect` takes the same form.
  For a password-protected server set the password with the **Password** button of the join screen first; a wrong one shows an error.
- Start several servers on one computer with different `--port` values.
- A server computer on two networks at once (for example cable and Wi-Fi) answers from the interface its route to
  the player goes through. Players must connect to the server's address on that same network: if they use its other
  address, the server list still shows it, but joining times out, because the answers come from an address the game did
  not connect to.
- On Wi-Fi, see [Wi-Fi and AWDL](#wi-fi-and-awdl): a Mac's AirDrop and Handoff radio pauses make players stand
  and jump. A server on a cable avoids it.
- No CD key, no PunkBuster and no master server: the server is never listed on the internet, and nothing is
  reported anywhere.

## Dedicated server from the menu

**Multiplayer > Start new server** with **Dedicated** set to LAN or Internet (both are the same here, there is no
master server) closes the game window and opens a server console window instead. The server runs with the chosen map,
gametype, settings, bots and weapon classes, without a player of its own.

- The window shows the server log and has a command line (`status`, `map <name>`, `map_rotate`, `kick <name>`,
  `say <text>`, `set <cvar> <value>`, `quit`, ...). Up and down arrows recall earlier commands; Page Up / Page Down
  and the mouse wheel scroll the log.
- Closing the window (or `quit`) stops the server and the app.
- rcon works once `rcon_password` is set, for example `set rcon_password <password>` in the window.
- Started from Terminal, the log also goes to the terminal, which takes console commands too. Started from Finder, the
  log goes to `~/Library/Logs/OpenIW2/openiw2.log`.

## Wi-Fi and AWDL

Players who stand still and then jump forward are usually a Wi-Fi link that pauses. On a Mac, AWDL (AirDrop, Handoff,
Sidecar, Universal Control, AirPlay) takes the radio off your network's channel for about 90 ms roughly twice a second,
so every packet in that time is held back, even with a strong signal. A one-per-second `ping` does not show it.
Measured on one Mac: pings to the router every 20 ms peaked at 96 ms with AWDL on and at 14 ms with it off.

- Best: run the server on a cable.
- Turn off AirDrop and Handoff while you play.
- Set the router to channel 149, the channel AWDL uses, so the radio does not switch.
- Or turn AWDL off until the next restart: `sudo ifconfig awdl0 down` (`up` turns it back on; macOS may turn it on
  again by itself).

OpenIW2 also evens out such a bursty path on the client and the server, at the cost of a little more delay for the
player on that link (docs/spec/net.md section 10).
