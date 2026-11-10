# Multiplayer scoreboard

The scores list the MP client draws while `+scores` is held and during intermission. There is no menu file for it:
the client code draws it. Evidence: VAs in the patch 1.3 MP executable (client) and in the 1.3 Linux dedicated
server (server), the shipped `default_mp.cfg`, `maps/mp/gametypes/_scoreboard.gsc` and the localized strings
(`localizedstrings/cgame.str`). Implementation: `src/game/mpscoreboard.cpp` (order, command, parse, layout),
`World::mpScoresCommand` (`src/game/mpspectate.cpp`), iw2view (drawing), iw2server (the request).
Tests: `tests/test_mpspectate.cpp`.

## Request and server command

- The client asks with the client command `score`: when `+scores` goes down (0x4c14b0) and while the scoreboard is
  drawn (0x4dabf0), at most once every 2000 ms. A fresh `+scores` press also clears the client's list and the scroll
  position.
  The 2000 ms are level time, which starts again with every map (the client game starts anew), so the first
  scoreboard of a new map asks at once and shows nothing of the previous map's list. TODO(spec): the address where the
  client game clears its request time at a level start.
- The server answers with the reliable server command `b` (0x80fdf08):
  `b <count> <axis score> <allies score>` followed, for each listed client, by ` <client> <score> <ping> <deaths>
  <status icon>`. At most 64 clients, and the per-client part stops before it would pass 1024 bytes. A client that is
  still connecting gets the ping -1. The status icon is the index of the client's `statusicon` among the precached
  status icons (`precacheStatusIcon`, `dm.gsc:79-80`), 0 for none.
- Order (the sorted client list built at 0x81097e6, qsort with the compare at 0x810968e): connecting clients come
  last; after the others come the spectators (session team spectator), among themselves by client number; the rest by
  score, higher first, and on equal score by deaths, fewer first. Equal clients have no defined order (qsort); this
  port keeps the slot order.

## Client copy (0x4d02b0)

The count is capped at 64. The axis score and the allies score are kept for the team banners. For each row a client
number outside 0..63 becomes 0; a status icon 1..8 selects the icon material, anything else none. The row's team is
not in the command: it is read from the client's info (team number, docs/spec/mp-hud.md: 0 none, 1 axis, 2 allies,
3 spectator; a client slot not in use counts as 0). Per team the client counts the rows and averages their pings
(integer division; 0 when no rows or when the summed ping is not above 0).

The client info of a network game is the client state list of the snapshots (docs/spec/net.md), which holds every
connected client, also one whose entity is not in the snapshot (out of view, e.g. the listen host's own player); the
row client numbers are the server's. Implemented: iw2view reads the name and team of a row from that list
(`netgame::RemoteView::clientInfo`), and the title's gametype from the server info's `g_gametype`.

## Visibility

- `+scores` shows it, `-scores` hides it and stamps the release time; after the release it fades out over 100 ms.
  Nothing is drawn while `cl_paused` is set.
- During intermission (the local player state's pm_type 5) the 2D pass of the client draws only the scoreboard, at
  full alpha (0x4cbca0 -> 0x4cb810): no HUD, no crosshair, no script HUD elements.
- Scrolling: while the scoreboard is shown, three keys move the first drawn line up and three down by
  `cg_scoreboardScrollStep` lines (0x4c6b30). TODO(spec): the key numbers (0xce / 0xa4 / 0xb8 up, 0xcd / 0xa3 / 0xbe
  down) in this port's key names; scrolling is not bound here.

Dvars (registered at 0x4bee8e..0x4bef7b): `cg_scoreboardScrollStep` 3 (1..8), `cg_scoreboardBannerHeight` 24 (1..100),
`cg_scoreboardItemHeight` 18 (1..100; the drawing code uses a fixed row height of 12 and does not read it).

## Layout

All positions are in the 640x480 screen; text positions are baselines. White means (1, 1, 1) with the scoreboard alpha.

Frame (0x4d95a0):
- backdrop: rectangle (120, 22, 400 x 430), black with alpha x 0.8;
- six lines, white with alpha x 0.1 (rectangles of 0x4d8e10): (123, 25, 394 x 2), (123, 447, 394 x 2),
  (123, 27, 2 x 420), (515, 27, 2 x 420), (125, 51, 390 x 1), (125, 432, 390 x 1);
- header left (0x4d8fa0): the gametype's display name (the localized name from `maps/mp/gametypes/<g_gametype>.txt`)
  at x 129, scale 0.41, baseline 51 - (24 - text height) / 2;
- header right (0x4d90b0): the map name, right-aligned so that it ends 4 units before x 511. Its scale starts at 0.41
  and drops by 0.025 while the two header texts measured at that scale, plus 4, are wider than 386 (not below 0.075).
  TODO(spec): the map display name lookup (0x532cd0); the port shows `mapname`;
- footer (0x4d9360): the server's host name at x 129 and on the right the server address (`ip:port`) or the
  localized `CGAME_LISTENSERVER` for a listen server, the same 386 rule with scales from 0.2 down in steps of 0.01,
  baseline 447 - (14 - text height) / 2.

List (0x4da980):
- The objective text (`cg_objectiveText`, 0x4d95f0) starts the list at y 52 when present: 4 units down, then lines
  wrapped at 374 units with scale 0.24, 12 units each (baseline + 10.08), and a white separator line (alpha x 0.1)
  at (125, y + 2, 390 x 1); the list continues 3 units below it.
- The needed height (0x4d9d60) is 10, plus two banners and 8 when the axis or allies have rows, plus banner + 4 for
  the team "none" and for the spectators, plus 12 per row. When it does not fit above y 432 the list width is 374 and
  a scroll bar is drawn; otherwise the width is 382.
- Column headers (0x4d9c10): 4 units below, scale 0.3, baseline + 11.76, the block is 14 high.
- Columns (table 0x59e628), from x 129, each a fraction of the list width: status icon 0.05, voice 0.05, name 0.47,
  score 0.13 (`CGAME_SB_SCORE`, right-aligned), deaths 0.13 (`CGAME_SB_DEATHS`, right-aligned), ping 0.17
  (`CGAME_SB_PING`, right-aligned). Right-aligned texts end at the column's right edge.
- Blocks: when the axis or allies have rows, the team with the higher score first (axis on top when its score is
  higher, allies when theirs is); on equal scores the block order of the previous frame stays (initially allies
  first, 0x5ddad0). Each block is its banner and its rows; 4 units between the two team blocks and after them. Then
  the team "none" (DM players) and last the spectators.
- Lines and the scroll position: banners and rows count as lines. Lines before the scroll position are counted and
  not drawn (and take no space); a line that would end below y 432 ends the list. After drawing, a scroll position
  past the last line is pulled back to it.

Banner (0x4d9ec0), `cg_scoreboardBannerHeight` high, the list width wide at x 129, white, material from the dvar
`g_ScoresBanner_None` / `_Axis` / `_Allies` / `_Spectators` (client defaults `mpflag_none`, `mpflag_german`,
`mpflag_american`, `mpflag_spectator`; `_scoreboard.gsc:9-34` sets them per map). Text at x 133, baseline
y + height x 0.84, scale 0.32, in the team colour (docs/spec/mp-hud.md, white for none and spectators):
- team none: the player count only, `CGAME_SB_PLAYER` for one row, else `CGAME_SB_PLAYERS` (`&&1` = the count);
- axis / allies: the team name (localized value of `g_TeamName_Axis` / `g_TeamName_Allies`) and the count in brackets;
  in the score column the team score and in the ping column the team's average ping, right-aligned, same scale;
- spectators: `CGAME_SPECTATORS` and the count in brackets.

Row (0x4da210), 12 high:
- the local client's row has a white rectangle with alpha x 0.2 behind it; otherwise every second row of a block
  (the second, fourth, ...) has the `black` material with alpha x 0.15;
- status icon: 13 x 13 at the column's left, white;
- voice: `voice_on` / `voice_off` 13 x 13 while the client talks or is muted. TODO(spec): no voice chat in this port;
- name, score, deaths, ping as text at scale 0.22, baseline + 10.08; a text wider than its column is shrunk in steps of
  0.02 until it fits, and below 0.16 it loses the shadow. Spectators show no score and no deaths.

Scroll bar (0x4da770): a track at x 505 from the list top down to y 431 (8 wide, `black`, alpha x 0.5); a thumb at
x 506, 6 wide, white with alpha x 0.25, its top and length proportional to the scroll position and the drawn lines
over all lines; with lines above, `hudscoreboardscroll_uparrow` (521, top) and `hudscoreboardscroll_upkey`
(521, top + 18); with lines below, `hudscoreboardscroll_downkey` (521, 397) and `hudscoreboardscroll_downarrow`
(521, 415), all 16 x 16.

TODO(spec): the renderer's font height query used for the header baselines (this port uses 48 x scale) and the
choice of font by scale; the colour of the black backdrop's material.
