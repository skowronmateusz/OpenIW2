# Multiplayer front end: create a server

How the multiplayer menus lead from the main menu to a running map: the menu set, the gametype and map lists of the
"start new server" screen, its commands, and openiw2's extension for bots and weapon classes. Evidence: the shipped
menus under `ui_mp/` and `ui/` (cited as `file:line`, not reproduced), the data files `mp/CoD2maps.arena` and
`maps/mp/gametypes/*`, and VAs in the patch 1.3 MP executable (marked "MP") or the patch 1.3 SP executable ("SP").
Implementation: `src/ui/mpserver.{h,cpp}`, `src/ui/frontend.cpp`, `src/ui/menurun.cpp`, iw2view (`apps/iw2view/main.cpp`).
Test: `tests/test_mpui.cpp` (ctest `mpui`, and `mpui_world` with the game files).

## From the single-player menu (openiw2 extension)

- In the original game, single player and multiplayer are separate programs. The SP main menu's multiplayer entry opens
  a confirmation popup (`ui/main.menu:330`, popup `multi_popmenu` at `ui/main.menu:622`) whose "yes" runs
  `uiScript startMultiplayer` (`ui/main.menu:734`), which starts the other program. The MP menu set has the inverse,
  `ui_mp/single_player.menu`.
- openiw2 runs both in one program. `startMultiplayer` swaps the front end's menu set: every open menu is dropped, the
  menus of `ui_mp/menus.txt` are loaded (the list the MP UI loads at start, MP 0x538109), `ui_multiplayer` is set to 1
  and the MP `main` menu opens. `startSingleplayer` goes back to `ui/menus.txt` + `ui/ingame.txt` and `ui_multiplayer`
  0. The swap is done between frames, not inside the menu script that asked for it (the script's items belong to the
  old set).
- While the multiplayer menu set is active, the console commands `map X` / `devmap X` start the listen server on X
  (iw2view's `mp/X` mode, docs/spec/mp.md). The original runs the same command text in its MP program.
- `cl_ingame` reads 1 while a map runs (the MP main menu shows its in-game items by it, `ui_mp/main.menu:152`).
  TODO(spec): the MP client's own `cl_ingame` rules.

## Menus of the create-server screen

`ui_mp/main.menu:270`–`279`: the "start new server" button closes `main_text` and opens `createserver`.
`createserver` (`ui_mp/createserver.menu:10`) is a title strip; its onOpen (line 24) runs `uiScript loadArenas` and opens
three non-fullscreen menus:

- `createserver_maps` (line 93, rect 404 137 200 280): an ownerdraw `UI_STARTMAPCINEMATIC` (255) preview (line 110) and
  the map list, a listbox with `feeder FEEDER_ALLMAPS` (4, line 133).
- `createserver_buttons` (line 157, the bottom strip): Back (closes `createserver`), "game type settings"
  (`openForGameType` with the format `settings_%s`, line 231) and "server settings" (`closeForGameType`, line 263), which swap
  the left column, and Start (`uiScript StartServer`, line 300).
- `createserver_serversettings` (line 304): the server cvars. The gametype row is a MULTI item with
  `ownerdraw UI_NETGAMETYPE` (245, line 728) and no value list; the other rows edit `sv_hostname`, `ui_dedicated`
  (no / LAN / internet), `sv_pure`, `sv_maxclients`, `sv_minping`, `sv_maxping`, `sv_maxrate`, `sv_voice`,
  `g_password`, `scr_drawfriend`, `scr_friendlyfire`, `scr_forcerespawn`, `scr_killcam`, `scr_teambalance`,
  `g_allowvote`, `scr_spectateenemy`, `scr_spectatefree` (lines 751–1121).
- `settings_<gametype>.menu` (`settings_dm.menu` …): the gametype's score and time limit as NUMERICFIELD items
  (`settings_dm.menu:82`, `:104`, cvars `scr_<gt>_scorelimit` / `scr_<gt>_timelimit`, `maxChars 4`).

The settings menus have no weapon or bot options. Weapons are limited by server cvars only (see the extension).

## UI dvars

MP registrations: `ui_currentMap` (0x531aa0, int), `ui_netGametypeName` (0x531b49, string), `ui_dedicated` (0x531b8b,
int 0..2, flags 0x1001), `ui_currentNetMap` (0x531bd0, int, flags 0x1001), `ui_netGametype` (0x5380c5, int, max =
gametype count - 1). `g_gametype` is a string dvar with default `dm` (MP 0x457af5 / 0x4596d7).

## Gametype list (MP 0x537ce0)

Built at UI start from the file list of `maps/mp/gametypes` with extension `gsc`:

- names starting with `_` are skipped (the shared helpers such as `_callbacksetup.gsc`); the `.gsc` suffix is cut;
- at most 32 entries; the 33rd prints "Too many game type scripts found! Only loading the first %i" (with 31) and the
  scan stops; an empty list is an error ("No game type scripts found in maps/mp/gametypes folder");
- each entry has the internal name and a display name: the first token of `maps/mp/gametypes/<name>.txt` (a localized
  string key, e.g. `MPUI_DEATHMATCH` in `dm.txt`), or the name itself when the file gives none (0x532610 read,
  0x449850 token);
- a second list for the server browser filter starts with "All" and holds the same entries; the create screen uses the
  first list only.

The shipped files give ctf, dm, hq, sd, tdm (in file-list order). TODO(spec): the sort order of the engine's file list;
openiw2 uses its sorted list.

## Arena list (MP 0x547650, uiScript loadArenas)

- Every `mp/*.arena` file is read; each `{ … }` block of key/value pairs is one map (at most 128). Keys:
  `map` (bsp name, entry +4), `longname` (list text, entry +0), `gametype` (space-separated gametype names).
  The entry also gets `loadscreen_<map>` (+8, the preview material name).
- Each word of `gametype` is compared case-insensitively with the gametype list; a match sets bit i of the entry's mask
  (+0x14). A block without `gametype` gets every bit.
- `mp/CoD2maps.arena` lists 15 maps, all with `dm tdm sd hq ctf`.

loadArenas (MP 0x5352ce) then:
1. (0x536110) when `g_gametype` names a gametype in the list, `ui_netGametype` = its index and `ui_netGametypeName` = its
   name;
2. (0x536320) marks the arenas whose mask has bit `ui_netGametype` as listed (+0xa0);
3. selects row 0 of `FEEDER_ALLMAPS` in `createserver_maps` (Menu_SetFeederSelection, which runs the feeder's select);
4. (0x536380) when the client is connected, selects the running map if it is listed. The front end has no running
   server, so openiw2 skips this step.

## FEEDER_ALLMAPS (4)

Rows are the listed arenas in list order. Text (0x537550): the long name. Select (0x537b00): the row's arena index goes
to `ui_currentMap` and `ui_currentNetMap`.

## UI_NETGAMETYPE (245)

Key handler (MP 0x533b70): mouse 1, Enter and key 0xbf step forward, mouse 2 back, wrapping over the gametype list;
`ui_netGametype` and `ui_netGametypeName` (the internal name) are set, the arenas are re-marked (0x536320). When the
arena of `ui_currentNetMap` is still listed, the list keeps it selected (0x5362d0 row of an arena, row 0 when not
listed); otherwise row 0 is selected (0x536220). The item shows the gametype's display name. TODO(spec): the ownerdraw's
paint handler (openiw2 draws the display name as the MULTI value text).

## openForGameType / closeForGameType (MP 0x53af80 / 0x53b020)

Menu commands of the MP UI. The argument is a format string filled with the current value of the dvar of the item whose
action runs (item +0x2c0); the menu of that name is opened / closed. `createserver_buttons` uses them with
`ui_netGametypeName` to show `settings_dm` and so on.

## StartServer (MP 0x534d93)

1. PunkBuster gate: with `sv_punkbuster` on and `cl_punkbuster` off it opens `startpb_popmenu` and stops. openiw2 has
   no PunkBuster; the gate never fires.
2. `cg_thirdPerson` = 0.
3. `dedicated` = `ui_dedicated` ("%i").
4. `g_gametype` = name of gametype `ui_netGametype`.
5. Console text `wait ; wait ; map <map of arena ui_currentNetMap>`.

Without the dedicated choice iw2view hosts a listen server for network clients (docs/spec/net.md section 11): the
window keeps its own player and bots, LAN clients join through the join-server list. The gate of step 1 is the only use of `startpb_popmenu` on this path; with no PunkBuster in openiw2 it stays unreachable.

## Dedicated start

VAs in the patch 1.3 MP executable. The dedicated row of the server settings edits `ui_dedicated` with three values:
0 no, 1 LAN, 2 internet (ui_mp/createserver.menu:796-797; a comment there notes that the UI handles this dvar
specially because setting it turns the game into a console-only server). StartServer copies it into `dedicated`.

- `dedicated` (string at 0x5a7b04) is registered at start as an int 0..2 (0x434040, called from 0x43455d).
- 0x434dc0 runs every frame (call at 0x43503d) and before a map starts (0x451ba6): when the latched value of
  `dedicated` differs from the current one it registers the dvar again with the new value (read-only when non-zero)
  and calls the client shutdown 0x4116d0 (CL_Shutdown). It then shows the system console window (the handle at
  0xd76af8, ShowWindow) and scrolls its log to the end. That is why StartServer's text waits two frames before `map`.
- The console window has a splash image at the top, a log box under it and a one-line command field at the bottom;
  on a resize (0x468451) the log box is placed 70 units from the top, 15 narrower and 100 lower than the window, and
  the 20 unit high command field 22 units above the bottom. Closing it while `dedicated`
  is non-zero queues the console command `quit` (0x468532: the string at 0x5a79f4); there is no confirmation.
- Value 2 also reports to the master server (0x4b8860); openiw2 has no master server, so 1 and 2 behave the same.

openiw2 (iw2view): StartServer with a non-zero `dedicated` loads no local game. The game window, renderer, audio and
menus are closed and the process goes on as the server of src/netgame/dedicated.cpp (the loop of `iw2server`) with the
menu's settings: map, gametype, the menu cvars (host name, password, slots, gametype settings, weapon classes) and the
bots of the extension panel; UDP port as for the listen server. A console window of its own replaces the Win32 one:
the log (standard output, which still goes to the terminal or the log file), a command line for the server console
commands (docs/spec/dedicated.md section 1) with a history (up / down), page up / down and the wheel to scroll back.
Its close runs `quit` without a question, as above. Started from a terminal the server reads stdin too. rcon works
as in `iw2server` once `rcon_password` is set (for example from the console window). TODO(spec): the original console
window's size, font, colours and title; whether it echoes a typed command (openiw2 echoes `]<command>`).

## Load screen (MP 0x5390c0)

VAs in the patch 1.3 MP executable. While the client connects and loads (client states 3..7, the screen switch at
0x414975), the screen is the UI refresh (0x532540) plus the connect screen 0x5390c0; the MP UI has no `briefing` menu.
The connect screen first calls 0x4d3390, which reaches 0x5332c0: with no map name known yet (UI string 0x196ffa0) it fills
the 640x480 screen; once the map name is known it paints the menu named `connect` (Menus_FindByName 0x53ac30, Menu_Paint
0x542550). When the map name is set (0x4349fb..0x434a1d) the client also loads the material aliases of
`maps/mp/<map>.csv`, so `$levelBriefing` is the map's `loadscreen_mp_<map>` material.

`ui_mp/connect.menu` (loaded by `ui_mp/menus.txt`) is a FILLED menu over the 896-wide rect with a white back colour and
the background `$levelBriefing`, two translucent black bars at the top and bottom with thin light lines, and the two
`loadbar` items (the frame material and a WINDOW_STYLE_LOADBAR item, both hidden while `com_expectedhunkusage` is "0").
Window_Paint's FILLED case (0x539f9c..0x53a081) draws the background material in the back colour when the window has
one, and fills the rect with the back colour otherwise.

Over the menu the connect screen draws, with the map name known (0x539211..0x53926c), two centred lines at x 320
(0x538a70: x minus half the text width), scale 0.5, white (0x59d038), text style 6:
- y 89: the gametype's display string, looked up from the server's gametype name (0x19953b0) in the gametype table of
  `maps/mp/gametypes/<name>.txt` (0x532d90; the raw name when not listed), then localized (0x539770);
- y 119: the map's long name from the arena files (0x532cd0: `longname` of the `mp/*.arena` block whose `map` matches,
  else the map name itself); not localized.
There is no description line. Before client state 5 the screen instead shows `EXE_CONNECTINGTO` and the server name at
y 55, `EXE_AWAITINGCHALLENGE` / `EXE_AWAITINGCONNECTION` by state, and the server's message text wrapped from y 299 in
yellow (0x539277..0x539385).

openiw2 (`ui::mpLoadScreenText`, iw2view): a map under `mp/` opens `connect` instead of `briefing` for the one frame
before the synchronous load (menu start, rotation, restart and a joined server alike); the gametype is the server's
`g_gametype` (configstring 0 for a joined server), the arena and gametype files are read for every load.
TODO(spec): the LOADBAR fill (style 7) and `com_expectedhunkusage`; the connecting texts (the connect loops block and
draw nothing); the font choice of 0x538a70 (thresholds at 0x5390f4; here the menu font rule at scale 0.5); an MP map
started from the command line outside the MP front end has the SP menu set and no MP load screen.


After the load the MP client goes straight into the game: the MP executable has no `pregame` menu, no
`ui_autoContinue` dvar and no `playerstart` UI script (none of those names exists in it), so unlike SP nothing waits for
a click and the client is not paused. Implemented: `ui::loadWaitsForClick` is false for every map under `mp/` (command
line, menu start, map rotation, restart).

## Shared front-end rules used here

- Pointer routing (SP 0x4d0060): when the focused menu is a popup (menu flag 0x1000000) it alone gets the pointer;
  otherwise the open menus are tried from the top of the stack down and the first with an item under the pointer takes
  it. The create screen depends on it: its map list and buttons are in menus below `createserver_serversettings`.
- `show` / `hide` / `fadein` / `fadeout` / `setitemcolor` act on the items of the menu whose script runs (the buttons of
  `createserver_buttons` hide and show their own items while another menu is on top). TODO(spec): confirm the menu
  argument of 0x4c77d0 / 0x4c78a0.
- NUMERICFIELD (type 9) is an edit field that takes digits only (SP 0x4ca9aa → 0x43f620).
- Value column of edit, numeric, yes/no and multi items: the value starts 8 units after the end of the label's text
  rect (SP 0x4cd29d: text rect x + width + 8; 0 when the item has no text). The text rect (SP 0x4cc170) starts at
  `textalignx` for left alignment, `textalignx` − width for right (2) and `textalignx` − width / 2 for centre (1, 3),
  so right-aligned labels such as `serverinfo_dm.menu:71`–`104` put their values in one column. TODO(spec): 0x4cc170
  adds the value's width to the centred width for some edit items (0x4c3720).

## Server dvars

The MP executable registers the server dvars the create screen edits at start (dvar lookups ignore case; the menus use
`sv_minping` for `sv_minPing`). Defaults: `sv_maxclients` int 20 (1..64, 0x45982b), `sv_hostname` string "CoD2Host"
(0x459869), `sv_punkbuster` 0 (0x4598b0), `sv_maxRate` int 0 (0..25000, 0x4598c8), `sv_minPing` and `sv_maxPing` int 0
(0..999, 0x4598fa / 0x45993f), `sv_floodProtect` 1, `sv_allowAnonymous` 0, `sv_voice` 0, `sv_voiceQuality` 1 (0..9),
`sv_pure` 1 (0x459a9f), `sv_mapRotation` and `sv_mapRotationCurrent` "" (0x459e94 / 0x459eba). openiw2 answers these
defaults for a cvar nobody has set while the multiplayer menus or a multiplayer game are active
(`ui::mpRegisteredDefault`). The gametype settings (`scr_<gt>_scorelimit` …) are not registered by the executable:
the gametype scripts create them at game start, so they are empty before the first game.

### Server cvars of the create screen

`scr_killcam`, `scr_friendlyfire`, `scr_teambalance`, `scr_drawfriend`, `scr_forcerespawn`, `scr_spectateenemy` and
`scr_spectatefree` are not registered by the engine as far as found: the dedicated server reads `scr_friendlyfire` /
`scr_killcam` only for the server info keys `ff` / `kc` (VAs in the 1.3 Linux dedicated server 0x80956ef / 0x8095731),
and the patch 1.3 MP executable has a single reference to `scr_killcam` (0x45b1b5 in 0x45ae80). 0x45ae80 (called at 0x45b7e5) builds the same server info in the
MP executable: it reads `fs_game`, `g_password`, `scr_friendlyfire`, `scr_killcam` and `sv_referencedIwdNames` and formats them with "%i"; it is the listen server's copy of that builder. openiw2's YESNO rows of `createserver.menu`
(lines 751-1121) show an empty dvar as "no"; TODO(spec): the original's YESNO read of a dvar that does not exist. The gametype scripts set the empty ones to their
defaults when the game starts: `scr_killcam` 1 (`_killcam.gsc:8-15`), `scr_drawfriend` 1 (`_friendicons.gsc:4-5`),
`scr_spectatefree` / `scr_spectateenemy` 1 (`_spectating.gsc:3-8`), `scr_teambalance` 0 (`_teams.gsc:26-27`),
`scr_forcerespawn` 0 (`dm.gsc:138-139`). So before the first game the screen shows "no" for the kill camera while the
game plays it, in the original too. The original has one dvar store for the menus and the server, so after a game the
screen shows the values the scripts set. openiw2 gives each world a copy of the menu cvars and, when a multiplayer world
ends, copies its cvars back (`ui::carryWorldCvars`); a value chosen on the screen reaches the gametype unchanged.

## Extension: bots and weapon classes (openiw2, not in the original)

The original create screen has neither. openiw2 adds two menus of its own (menu text in `src/ui/mpserver.cpp`, parsed
with the front end's menu parser):

- `iw2_createserver_extbutton`: a "Bots & weapons" button in the bottom strip, opened by loadArenas. It closes the
  settings column (`createserver_serversettings` and `settings_<gametype>`) and opens `iw2_createserver_ext` in its
  place. The original buttons bring their column back (the panel closes when one of them is open). Both close with
  `createserver`.
- `iw2_createserver_ext`: `ui_iw2_bots` (0–15), `ui_iw2_botskill` (0 easy – 3 veteran, the bot skill of
  docs/spec/mp-bots.md) and one yes/no per weapon class, `ui_iw2_allow_<class>` (rifle = semi-automatic rifles, bolt,
  smg, sniper, mg, shotgun, grenades). Defaults when unset: classes on,
  0 bots, skill 1.

On StartServer the panel writes one list, `iw2_weaponclasses` (the enabled classes among rifle, bolt, smg, sniper, mg,
shotgun, comma separated). The game layer is the only owner of the weapon-to-class mapping
(`World::mpWeaponClass`, docs/spec/mp-bots.md "Weapon classes"): at game start it sets the `scr_allow_<weapon>` cvar of
every menu weapon that `maps/mp/gametypes/_weapons.gsc` reads (`_weapons.gsc:94`–`204`, checked again at
`:576`–`:755`; an empty cvar means allowed, `:212`), and the bots choose among the allowed weapons only. "Grenades" is
a panel class of its own that sets `scr_allow_fraggrenades` and `scr_allow_smokegrenades` (`_weapons.gsc:368`, `:378`).

Start is refused when the toggles make a list without a primary class (grenades alone, nothing) or one that leaves a
nation's weapon menu without a weapon (the same check as `iw2server --weapons`, `mpParseWeaponList`; the player of
such a nation could not choose a weapon and join): `com_errorTitle` is the localized `MENU_ERROR`, `com_errorMessage` the
openiw2 string `IW2_ERR_NO_WEAPON` (no primary class) or `IW2_ERR_NATION_WEAPON`, and `error_popmenu` opens over the
create-server screen (its close runs `clearError`; the popup's "open main" is skipped for this one use, so the player stays on the screen); no cvar is written and no map loads.

The panel texts are openiw2's own localized strings (keys `IW2_*`, English and Polish in `src/ui/mpserver.cpp`,
Polish in the Windows-1250 code page of the Polish game's strings). They are added to the loaded localization and never
replace a key of the game files.

The bot count is clamped to `sv_maxclients - 1` when `sv_maxclients` is set. The bots join the listen server after the
local client, one per frame, as with `IW2_MP_BOTS` (which, with `IW2_MP_BOT_SKILL`, still takes precedence; see
docs/debug-flags.md). The gametype of the started map is `g_gametype` unless `IW2_MP_GAMETYPE` is set.

## Main menu navigation and the Escape key

Evidence: ui_mp/main.menu:37-101 (menus `main` and `main_text`), main.menu:206-214 and 269-277 (join / new server buttons),
ui_mp/joinserver.menu:35-41 and 971-977, ui_mp/createserver.menu:19-50, ui_mp/scriptmenus/ingame.menu:18-53 and the
`leavegame` menu below it, default_mp.cfg:49. VAs in the patch 1.3 MP executable.

- `main` (full screen) opens `bg` and `main_text` and has an empty onESC. `main_text` has the focus; its onESC closes the
  options/mods side menus and runs `ingameclose main`.
- `ingameclose <menu>` (handler 0x53b170 in the command table at 0x5dcc70) closes the named menu (lookup 0x53ac30, close
  0x53acd0) only when the client connection state (0x609fe0) is 8, the active state. In the front end it does nothing, so
  ESC on the MP main menu leaves every menu open; it never quits. The SP handler (0x4c8320) has the same shape with the
  test "dvar cl_paused non-zero" instead, so ESC on the SP main menu outside a level does nothing either; in a level it
  closes `main` and the game unpauses once no menu is left.
- Scripts run token by token (0x53bd30): a `;` token is skipped, each command handler reads only its own arguments, and the
  next token starts a new command. A missing `;` therefore does not merge two commands: joinserver's onESC
  (`uiScript closeJoin` directly followed by `close joinserver`) closes joinserver and reopens `main_text`. The port splits
  a command at the end of its fixed argument count when the next token is a command name (open, close, show, hide,
  setfocus, play, ingameclose, ingameopen, fadein, fadeout, exec, execnow: one argument; setdvar: two; uiScript names
  without arguments such as closeJoin, stopRefresh, UpdateFilter). The names taking no arguments are in `fixedArgs` (src/ui/menu.cpp); `ServerSort` takes one.
- Expected stacks (all checked by tests/test_mpmenunav.cpp): front page `bg main main_text`; join -> `bg joinserver main`,
  its Back or ESC -> front page; new server -> `bg createserver createserver_buttons createserver_maps
  createserver_serversettings main`, Back or ESC (createserver onClose closes the three panels and reopens `main_text`) ->
  front page; options, profile, single-player and quit popups -> ESC -> front page.
- In an MP level, default_mp.cfg binds ESCAPE to `togglemenu`; the key goes through that binding only (one press = one
  toggle: open `g_scriptMainMenu`, e.g. `ingame`, or close the top menu). With a menu open, ESC goes to the menu's onESC.
  `ingame` closes on ESC; its key 5 opens `leavegame`, whose yes button closes it and runs `disconnect`.
- The port treats every MP level as connection state 8, a listen server with bots and a network client alike, so
  `ingameclose` acts there (in SP the cl_paused rule stays). togglemenu reads g_scriptMainMenu wherever the client got it:
  the local server's setClientCvar or, on a network client, the `v` server command; without it the client falls back
  to `main`, which `ingameclose main` closes in a game. Menus the server opened (`t`) close through their own onESC.

## Join server

Evidence: `ui_mp/joinserver.menu`, `ui_mp/createfavorite.menu`, `ui_mp/filter.menu`, `ui_mp/serverinfo.menu`, `ui/menudefinition.h`, VAs in the
patch 1.3 MP executable. Implementation: `src/net/lanbrowser.{h,cpp}`, `src/ui/mpserver.cpp`, iw2view. Test: `tests/test_mpbrowser.cpp` (ctest `mpbrowser`).

- Sources (`ui_netSource`, table at 0x5dcb4c: `EXE_LOCAL`, `EXE_INTERNET`, `EXE_FAVORITES`): 0 local broadcast, 1 internet (the
  master server's list), 2 favorites (list at 0x9619d0, 0x94-byte entries, at most 0x80). The key handler of `UI_NETSOURCE` (220, 0x533cb0) wraps over
  0..2, mouse 2 goes back, rebuilds the list and starts a refresh (0x539540) unless the new source is internet. openiw2 has no master
  server, so internet stays empty.
- Entry: the screen's onOpen runs `UpdateFilter` (`joinserver.menu:22`, 0x534d60): with source 0 it refreshes, in any case it rebuilds the list (0x5365c0)
  and selects row 0 (0x537b00). Favorites are not refreshed there in the original: their answers are stored (servercache.dat).
  openiw2 keeps no such cache, so it refreshes the favorites too; replies are polled every frame and appear without a click.
- `RefreshServers` / `RefreshFilter`: a refresh of the current source. `UI_SERVERREFRESHDATE` (247) shows `EXE_REFRESHTIME` with the time of the last
  refresh of the source (the original stores it in `ui_lastServerRefresh_<source>`, 0x539540).
- Filter of the list build (0x5365c0, 0x536840..0x536a71): `ui_browserShowFull` 0 drops full servers, `ui_browserShowEmpty` 0 empty ones,
  `ui_browserShowPassword` 0 those with `pswrd`, `ui_browserShowNoPassword` 0 those without, `ui_browserShowPure` 1 those whose `pure` is 0,
  `ui_browserShowDedicated` 1 those whose `hw` is not 1..3; `ui_browserMod`, `ui_browserFriendlyfire`, `ui_browserKillcam`, `ui_browserShowPunkBuster` when not
  negative must equal `mod`, `ff`, `kc`, `pb`; `ui_joinGametype` above 0 (0 = "All", then the gametype list, `UI_JOINGAMETYPE` 253, wrapping) must equal `gametype`.
  Registered defaults (0x531c1c..): 1, 1, 1, 1, 1, 0, -1, -1, -1, -1. Deviation: openiw2 registers `ui_browserShowPure` as 0, because its servers do not
  implement a pure check and do not send `pure`; with the original default they would all be hidden.
- A favorite that has not answered stays listed with its stored name and ping `...`.
- `ServerSort n` (one argument): sorts by column n, the same column again reverses (0x534d60, compare 0x533e30 / 0x4154e0).
- Columns (switch at 0x537640, table 0x5379c4): 0 password, 1 platform icon from `hw` (not drawn: no matching icons drawn by openiw2's text list), 2 host name (address until a
  server answered), 3 map long name (0x532cd0), 4 "clients (max)", 5 gametype display string, 6 voice, 7 pure, 8 mod, 9 PunkBuster, 10 ping. The flag columns
  show `X` when the key is non-zero.
- `addFavorite` (from the selected row; not with source 2), `createFavorite` (`ui_favoriteName`, `ui_favoriteAddress`, 31 characters each; only with source 2,
  `createfavorite.menu`), `deleteFavorite` (source 2, the selected row): the add path 0x5348b0 writes `ui_favorite_message`, shown by `fav_message_popmenu`:
  name empty `@EXE_FAVORITENAMEEMPTY`, address empty `@EXE_FAVORITEADDRESSEMPTY`, then the list add 0x4b52f0: already listed `@EXE_FAVORITEINLIST`, full `@EXE_FAVORITELISTFULL`, an
  address that does not parse `@EXE_BADSERVERADDRESS`, else `@EXE_FAVORITEADDED`.
- openiw2 stores favorites per profile in `favorites.txt` next to `config.cfg` (the original's servercache.dat is a binary file of its own), as entered, one
  `address TAB name` per line, and looks names up when asked (below).
- Buttons with `ownerdrawFlag`: `UI_SHOW_FAVORITESERVERS` (4) needs source 2, `UI_SHOW_NOTFAVORITESERVERS` (0x1000) any other source (0x533a93).
- `UI_NETMAPCINEMATIC` (246): the selected server's `loadscreen_<map>` image; the still image only, as `UI_STARTMAPCINEMATIC` here.
- `ServerStatus` (`serverinfo.menu` onOpen, `FEEDER_SERVERSTATUS` 13): `getstatus` to the selected server; rows of four columns: each cvar (key, -, -, value),
  a blank row, the header, then the players (number, score, ping, name).

## Addresses and name lookup

The original's address parser (0x448d50) accepts a numeric address or a host name resolved by a blocking call. openiw2 (`src/net/resolve.{h,cpp}`) accepts `host[:port]`
(default port 28960, port 1..65535). Numeric addresses complete at once; a name (A record, `getaddrinfo` with AF_INET) is looked up on a thread and polled once per frame
with a limit of 4 s, so the UI does not wait. Used by favorites, the console and menu command `connect`, and any address entry; `+connect` runs before there is a window and waits with the same
limit. A failure shows `EXE_BADSERVERADDRESS` in `error_popmenu` (the same path as a refused connection, docs/spec/net.md section 14).

## Hidden entries (openiw2 decision)

The multiplayer menus of the shipped game lead to features this build has no server side for: voting, CD key, PunkBuster,
auto update, voice chat, muting players and mods. The game files stay as shipped; the engine leaves these entries out when it
parses a menu. Nothing in the menus of the single-player set is touched (only files below `ui_mp/`).
Implementation: `src/ui/menuhide.{h,cpp}`, applied by `MenuLibrary::parseSource`. Test: `tests/test_menuhide.cpp`
(ctest `menuhide`; `menuhide_world` loads the shipped menus).

### Mechanism

The original has no switch for this. Its own gates (`dvarTest` with `showDvar` / `hideDvar`, `ownerdrawFlag`) depend on dvars the
server or the engine owns (e.g. the vote button of the in-game menu is shown by the server's `ui_allowvote`), so they cannot hide an
entry on their own. A table in `menuhide.cpp` is applied to every parsed menu instead:

- **Hidden menu**: a menu name on `kHiddenMenus` is not registered. `open` of it finds nothing and does nothing, as for a menu whose
  file is missing (`ui_mp/menus.txt` names such files already). A script menu the server asks for by name (`popupopen`) that is on
  the list is ignored by iw2view. Every item of a loaded menu whose scripts `open` a hidden menu is removed too.
- **Hidden item**: `HideRule` matches items of one menu by `name`, `text` key or `dvar` (the dvar the control edits or its `dvarTest`
  reads); `row` takes the label on the same line and column with it. The removed item is gone from the menu, so it is not drawn,
  not focusable, not hit-tested by the pointer, and a `show` cannot bring it back.
- **Keys**: `kKeys` drops menu-level `execKey` shortcuts that led to a hidden entry.
- **No gap**: after a removal the rest of the list moves up (`pullY` per removed line, `span` = how far below the list reaches,
  `sameX` = the same column only), named items move or grow (`moves`), and a listbox column that must keep its index (the feeder
  fills columns by index) is marked hidden (`Item::Column::hidden`, not drawn) while its neighbours take its place.
- Rules run in table order on the positions left by the earlier rules. The parser has already added an item's `origin` to its rect.

To hide something else: add one line to `kRules` (item) or one name to `kHiddenMenus` (screen), and check it with
`IW2_HIDE_TRACE=1 test_menuhide` (lists what was removed) and `IW2_HIDE_DUMP=<menu>` (lists the items left, with positions).

### What is hidden

| Feature | Removed |
|---|---|
| Voting | the in-game menu's call-vote choice (both looks) and its key 4; menus `callvote`, `changemap`, `changegametype`, `kickplayer`; the two key-binding rows for vote yes / vote no in the multiplayer controls page; the "allow voting" row of the create-server screen and of the search-and-destroy settings page |
| Mute players | the in-game menu's mute choice and key 3; menu `muteplayer` |
| Voice chat | menu `options_voice` and the options tab opening it; the push-to-talk binding row; the voice row of the create-server screen; the voice column (header, icon, background, text column 6) of the server list |
| CD key | menu `cdkey_menu`, the enter-key button on the controls page (it opened it), the hidden message item of `multi_menu` |
| PunkBuster | menus `startpb_popmenu`, `joinpb_popmenu`; the client option row and the server option row; the filter row of the server filter popup; the PunkBuster column of the server list (text column 9) |
| Auto update | menu `auconfirm`; every item tested on `cl_updateavailable` (the main menu's update button) |
| Mods | menu `mods_menu` (nothing in the menus opened it); the mod column (header, icon, background, text column 8) of the server list; the mod row of the server filter popup |

Left in place on purpose: the "Internet" source of the server list.

### Layout facts

- In-game menu: the choices are 24 apart; with mute and vote gone, "leave" sits right under "change team". The localized labels
  carry their own digits, so the list reads 1, 2, 5; key 5 still works (the keys 3 and 4 do nothing).
- Lists of option rows keep their 15 pixel steps. The server list gives the width of a removed column to the server name column
  and moves the columns between them right, so the header row still ends at the same place.
- No code opens the CD key, update or PunkBuster screens by itself (iw2view has no call to them; the start-server and join-server
  scripts run without the PunkBuster popup, see above), so nothing needs suppressing at start or on join. A `callvote` or `cmd
  callvote ...` typed or sent anyway reaches the server as an unknown client command, which it prints and ignores; the client
  screen does not change.
