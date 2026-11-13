# Dedicated server: console, rcon, rotation, slots (patch 1.3)

Addresses are VAs in the 1.3 Linux dedicated server. Engine messages quoted here are reproduced by openiw2 for
compatibility (server logs and rcon replies). The protocol itself is in docs/spec/net.md; this file covers what an
operator of a dedicated server sees. openiw2 decisions: no CD-key authorisation, no master server, no PunkBuster.

## 1. Console commands

The server registers its console commands in one place (0x808cd00..0x808ce50). The ones openiw2 implements:

| Command | Handler | Behaviour |
|---|---|---|
| `map <name>` | 0x808bd46 | loads the map with the current `g_gametype` (also `devmap`) |
| `map_restart` | 0x808c0a8 | restarts the current map (calls the shared restart 0x808be3e) |
| `fast_restart` | 0x808c0bc | restarts the current map through the same routine with the other mode |
| `map_rotate` | 0x808c132 | next entry of the rotation (section 2) |
| `kick <name>` / `kick all` | 0x808c55e region | drops the named player / everyone with the reason `EXE_PLAYERKICKED`; usage text without an argument |
| `clientkick <num>` | 0x808c6a0 region | drops by client number, same reason |
| `status` | 0x808c706 | client table (section 3) |
| `serverinfo` | 0x808cba8 | prints the server-info cvars |
| `say <text>` | 0x808c9a6 | chat to everybody |
| `tell <num> <text>` | 0x808ca6c | chat to one client |
| `set`, `seta`, `exec`, `quit` | common command layer | cvar set (seta also marks it for the config), run a config file, stop the process |

All server commands except `map_rotate` print `Server is not running.` when no map is loaded. `map_rotate` has no
such check (0x808c132 goes straight to its prints), so `+exec server.cfg +map_rotate` on the command line starts
the first map of the rotation. Its forced `map_restart` (below) is the console command and so prints `Server is not
running.` when no map is loaded.

At start the server runs its start-up configuration before the command line (VAs in the 1.3 Linux dedicated
server): `default_localize_mp.cfg` when that file exists, else `default_mp.cfg` (the choice at 0x80a32d4, kept at
0x849fc28), then `language.cfg`, then `config_mp_server.cfg` (0x8062036, called once from the start-up at
0x806214d). Each goes through the ordinary `exec` command, so a file that is found prints `execing <name>` and a
missing one prints `couldn't exec <name>` (0x806006a). When the start runs in safe mode it also execs
`safemode_mp_server.cfg` (0x8062090); openiw2 has no safe mode. The game cvars (for example the deathmatch score
limit 50) are therefore those of a 1.3 server.

The server console has no key-binding commands (the table at 0x808cca0 and the executable's strings hold no
`bind`, `unbind` or `unbindall`). An unknown command prints nothing: the command executor (0x80608a6) tries the
registered commands, then a dvar of that name (0x8063054), then, while a game runs, the game's console commands
(0x80926d6), and has no message for a line none of them takes; the executable has no `Unknown command` string. So
the `bind` and `unbindall` lines of the default configuration are skipped silently.

say / tell: without an argument (`say`) or with fewer than two (`tell`) they do nothing. The text starts with
`console: `, followed by the command's argument string; if that string starts with a double quote, the first and the
last character are removed. It is sent as the reliable server command `h "<text>"` (0x68 = `h`, format `%c "%s"`)
to all clients (say) or to one client (tell). tell accepts only a client number in `0 .. sv_maxclients-1` whose
client is fully in the game (state active); other numbers are ignored silently.

Console commands (VAs in the 1.3 Linux dedicated server, registration at 0x808cca0): `heartbeat`, `onlykick`, `banUser`,
`banClient`, `kick`, `tempBanUser`, `tempBanClient`, `unbanUser`, `clientkick`, `status`, `serverinfo`, `systeminfo`,
`dumpuser`, `map_restart`, `fast_restart`, `map`, `map_rotate`, `gameCompleteStatus`, `devmap`, `killserver`,
`scriptUsage`, `stringUsage`, `say`, `tell`.

Player lookup by name (0x808baf8; fewer than two tokens prints `No player specified.`, unused here as the commands check
the usage first): each used slot compares the raw name, then the name with `^N` codes removed, case-insensitively with the
argument as typed (the argument is not cleaned); no match: `Player %s is not on the server`. By number (0x808bc10): the
argument must be all digits (`Bad slot number: %s`), in `0..sv_maxclients-1` (`Bad client slot: %i`), and used (`Client %i is
not active`).

The handlers: `kick` and `tempBanUser` share 0x808c55e (kick by name, or `all`); `onlykick` (0x808c684) is the same
kick without the temporary ban; `tempBanClient` and `clientkick` work by number. A kick refuses the listen-server host
(`e "EXE_CANNOTKICKHOSTPLAYER"`, 0x808c31c). The temporary ban stores the client's GUID for `sv_kickBanTime` (default
300 s) and logs `%s (guid %i) was kicked for cheating` only when the GUID is non-zero. `banUser` / `banClient` append to
`ban.txt` and need a GUID (`Can't ban user, GUID is 0`). `heartbeat` only forces the next master heartbeat. `killserver`
(0x808cc68) passes the reason `EXE_SERVERKILLED` to 0x8060eb8 (`TODO(spec)`: that routine was not read, so whether the process stays up is open).

openiw2 has no authorisation (GUID always 0): `kick`, `onlykick` and `tempBanUser` therefore behave alike, `banUser` /
`banClient` always print the refusal and `unbanUser` has nothing to remove. `killserver` is not implemented.

`addtestclient` (string at 0x815dbe6) adds a test client that occupies a client slot. openiw2 drives test clients
with its bot controller (docs/spec/mp-bots.md).

## 2. Map rotation (0x808c132)

Two cvars: `sv_mapRotation` (the whole list) and `sv_mapRotationCurrent` (what is left of it). The list is a sequence
of tokens: `gametype <name>` and `map <name>`.

1. Prints `map_rotate...`, then both cvars in the form `"sv_mapRotation" is:"%s"` and
   `"sv_mapRotationCurrent" is:"%s"`.
2. If `sv_mapRotationCurrent` is empty it is set to `sv_mapRotation`.
3. Reads the next token from `sv_mapRotationCurrent` and stores the rest back (helper 0x808c0d0). If there is none,
   `sv_mapRotationCurrent` is set to `sv_mapRotation` again and the token is read once more.
4. Loop on the token:
   - none: prints `No map specified in sv_mapRotation - forcing map_restart.` and runs map_restart.
   - `gametype` (case-insensitive): reads the next token; none → `No gametype specified after 'gametype' keyword in
     sv_mapRotation - forcing map_restart.` and map_restart. Else prints `Setting g_gametype: %s.`, sets
     `g_gametype` and continues with the next token.
   - `map` (case-insensitive): reads the next token; none → `No map specified after 'map' keyword in sv_mapRotation -
     forcing map_restart.` and map_restart. Else prints `Setting map: %s.`, runs `map <name>` and stops.
   - anything else: prints `Unknown keyword '%s' in sv_mapRotation.` and continues with the next token.

So the rotation wraps around: after the last `map` the current list is empty and the next rotate starts over.
When a gametype script ends a round with `exitLevel`, the server rotates (openiw2: after the frame in which the
script called it). `TODO(spec)`: the exitLevel argument (keep / reset persistent data) and the intermission delay.

## 3. status (0x808c706)

Prints `map: <mapname>`, then the header line `num score ping guid   name            lastmsg address               qport rate`,
a separator line, one line per used client slot and an empty line. Per client: number `%3i `, score `%5i `, ping as
`CNCT ` while connecting, `ZMBI ` for a dropped client still holding the slot, else `%4i ` (capped at 999), GUID `%6i `,
the name followed by `^7` and padded with spaces to 15 visible columns, ms since the last packet `%7i `, the address
padded to 21 columns, qport `%5i`, rate ` %5i`.
openiw2: GUID is always 0 (no authorisation), test clients show address `bot`, qport 0 and rate 0.

Server info (`serverinfo`, `getstatus`): the server-info cvars sorted by name ignoring case, each printed by
`serverinfo` padded to 20 characters: fs_game (when set), g_antilag, g_gametype, gamename, mapname, protocol,
scr_friendlyfire, scr_killcam, shortversion, sv_allowAnonymous, sv_floodProtect, sv_hostname, sv_maxclients,
sv_maxPing, sv_maxRate, sv_minPing, sv_privateClients, sv_punkbuster, sv_pure, sv_voice; iw2server sets the values a
1.3 server reports by default (g_antilag 1, sv_floodProtect 1, the others 0). `dumpuser <name>` prints `userinfo`, a
line of eight dashes and every key of the client's userinfo padded to 20 characters with its value. Checked byte for
byte against the 1.3 Linux dedicated server (getinfo, getstatus, getchallenge, rcon status / serverinfo / dumpuser and
a bad password): only the natural values differ (ping, addresses, challenges).

openiw2 adds the dvar `sv_snaps` (default 60, 20 to 100, read when the server starts; not a server-info cvar): the
snapshots a second offered to clients that ask for them (docs/spec/net.md section 10 "Snapshot rate").

## 4. Remote console (0x8097188)

A connectionless packet `rcon <password> <command…>`:

1. Rate limit: a request that arrives 500 ms or less after the previously accepted rcon request is dropped without
   any reply (the time of every request that passes this check is remembered, good or bad).
2. The password is valid when `rcon_password` is non-empty and equal (case-sensitive) to the second token. The server
   logs `Bad rcon from %s:\n%s\n` or `Rcon from %s:\n%s\n` with the address and the whole request.
3. Console output is now redirected to the requester: it is buffered and sent as out-of-band packets
   `print\n<text>` (flush 0x8097050).
3a. The command is built from tokens 3..n, each followed by a space; a token that is empty or holds a byte up to space is
   put in double quotes (0x80629ce). It runs as one command. Replies are collected through a redirect buffer of 0x1fff0
   bytes and sent as one `print\n` packet when it is full or at the end; openiw2 sends several packets of at most 1200
   bytes instead, to stay below the usual path MTU.
4. `rcon_password` empty → `The server must set 'rcon_password' for clients to use 'rcon'.`; else an invalid
   password with a non-empty token → `Invalid password.`; an empty token → the `rcon login` hint. Each is printed
   (and therefore sent) with a newline.
5. Valid: tokens 3..n are joined with single spaces (at most 1024 bytes) and executed as a console command; its output
   goes back to the requester. (Commands starting with `pb_sv_` go to PunkBuster; openiw2 has none.)

## 5. Connect: private slots and the game password

- Private slots (0x808eb0d): if the userinfo key `password` equals `sv_privatePassword` (case-sensitive, also when
  both are empty), the free-slot search starts at slot 0, otherwise at `sv_privateClients`. No free slot →
  `error\nEXE_SERVERISFULL`.
- Game password (0x80f8fff): for a client that is not a test client, when `g_password` is non-empty, not `none`
  (case-insensitive) and differs from the userinfo `password` (case-sensitive), the game refuses with
  `GAME_INVALIDPASSWORD`; the server answers `error\n<reason>` (0x8149e91) and logs `Game rejected a connection: %s.`

openiw2: test clients (bots) occupy slots of `sv_maxclients` too, so a network client is refused as full when
network clients plus bots reach it.

## 6. Server queries and LAN

getinfo / getstatus are answered from the live game: `clients` counts network clients and bots, the getstatus
player lines carry each client's score (the player's `score` field), ping and name. The ping of a network client is
the average round trip of its acknowledged snapshots; test clients report 0. `TODO(spec)`: the original's ping
averaging. The server socket is bound to all interfaces, so broadcast getinfo requests on the LAN are answered on
the server port. `TODO(spec)`: which ports the 1.3 client scans on LAN refresh.

## 7. Command-line options of iw2server (openiw2)

The options and their ranges are in docs/dedicated-server.md; the parser is `src/netgame/serverargs.cpp` (test
`serveroptions`). This section records what they set and why.

- Order at start: default configuration (section 1), `--cvar` values, the `+` commands up to the first map command
  (`+set`, `+exec server.cfg`), then the **overrides** (`--weapons`), then the socket, then the map commands. So the
  command line wins over `server.cfg`. The limit options are applied at each map load, see below.
- `g_gametype` in the default configuration is `random`, which names no gametype. A map loaded with `random` or an empty
  gametype runs `dm`; a gametype without a script (`maps/mp/gametypes/<name>.gsc`) prints `Can't find gametype <name>`
  and the map does not load.
- `--weapons` writes `iw2_weaponclasses` (the primary classes of the list) and both grenade cvars
  (`scr_allow_fraggrenades`, `scr_allow_smokegrenades`: 1 when `grenades` or `all` is listed, else 0). Validation in
  docs/spec/mp-bots.md "Weapon classes". Without the option nothing is written and the default configuration's
  `scr_allow_*` (all 1) stand.
- `--scorelimit N`, `--timelimit MIN`, `--roundlimit N` set `scr_<gametype>_scorelimit`, `scr_<gametype>_timelimit`
  and `scr_sd_roundlimit` for the gametype of each map at its load (`Dedicated::loadMap`), so a rotation that changes the
  gametype gets the value for every gametype; `--roundlimit` is ignored for a gametype other than sd, and refused at start
  when the gametype is already known to be another one. The gametype scripts read the cvar at map start and again again
  while the match runs (an `updateGametypeCvars` loop), publish it as `ui_<gametype>_<limit>`, and clamp the time limit to 1440 minutes.

What the limits mean (openiw2 behaviour = the 1.3 scripts; 0 disables the limit in all of them):

| Gametype | Score limit compares with | Source |
|---|---|---|
| dm | the player's `score`, +1 per kill of another player, -1 for a suicide (not when switching teams); the player who reaches the limit ends the map | dm.gsc:336-349, 707-722 |
| tdm | the team score, +1 per kill of an enemy (the killer's own score also drops by 1 for a team kill, the team score does not) | tdm.gsc:445-455, 842-858 |
| ctf | the team score, +1 per flag capture (the capturing player gets +10 personal score) | ctf.gsc:1124-1131, 943-958 |
| hq | the team score, +1 per second while the team holds the radio and has players | hq.gsc:1840-1853, 1007-1022 |
| sd | the rounds won by a team (`game["alliedscore"]` / `game["axisscore"]`, +1 per round win, kept across the rounds of the map) | sd.gsc:1048-1066, 1259-1275 |

Time limit: minutes of the map; in dm, tdm, ctf and hq the clock starts with the match (`dm.gsc:579-593`), in sd the minutes
played are added up over the rounds and compared after each round (`sd.gsc:1241-1257`, the check at `:1099`).
Round limit (sd only, `sd.gsc:1277-1295`): the map ends when the number of rounds played reaches it. The default
configuration also sets `scr_sd_roundlength` (round time), `scr_sd_graceperiod` and `scr_sd_bombtimer`; no option changes them.
