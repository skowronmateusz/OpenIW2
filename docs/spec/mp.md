# Multiplayer game layer (offline, listen server)

How the multiplayer game code starts a map, runs a gametype script and drives a client through the gametype's code
callbacks. Evidence: VAs in the 1.3 Linux dedicated server (the MP server binary shares the game module with the MP
client) and the shipped scripts under `maps/mp/` (cited as `file.gsc:line`). Implementation: `src/game/mpgame.cpp`,
`src/game/mpgame.h`. Networking is not part of this page (the server here has only local clients); the network layer is
specified separately.

## Map and gametype names

- The `mapname` cvar holds the bare map name (`mp_toujane`). The BSP is `maps/mp/<mapname>.d3dbsp` and the level
  script `maps/mp/<mapname>.gsc` (path built at 0x811037a with the format `maps/mp/%s` from `mapname`).
- The gametype is the `g_gametype` cvar; its script is `maps/mp/gametypes/<g_gametype>.gsc` (path built at 0x8110286
  from the cvar's string). Shipped gametypes: dm, tdm, sd, ctf, hq; all five run ("Team gametypes" below). Every stock map lists all five in
  `mp/CoD2maps.arena`.
- Sound aliases are filtered by the bare map name: the `loadspec` column of the MP aliases lists bare names
  (`ambient_africa` is limited to `mp_matmata mp_decoy mp_toujane`, used by `maps/mp/mp_toujane.gsc:5`). Passing the
  path form (`mp/mp_toujane`) filters every MP-only alias out. Implemented: MP mode passes the bare name.

- Weapon files are read from `weapons/mp/` (the MP weapon names, `colt_mp`, `mp40_mp`, ... exist only there).
  TODO(spec): the loader path in the MP binary.
- Spawn points: `mp_dm_spawn`, `mp_tdm_spawn`, `mp_ctf_spawn_*`, `mp_sd_spawn_*`, `mp_global_intermission` stay entities
  the gametypes find by classname (`dm.gsc:107`, `dm.gsc:473`). TODO(spec): their entries in the MP spawn table; here
  every map entity whose class starts with `mp_` is kept.

## Script handles resolved at game init

The script-load step of game init (0x811041e) resolves, after the code scripts:
1. 0x8110286: `main` of the gametype script — required (inferred from the loader's third argument, 1 here and 0 for
   the level script; a missing gametype script fails the start in this port);
   `CodeCallback_StartGameType`, `CodeCallback_PlayerConnect`, `CodeCallback_PlayerDisconnect`,
   `CodeCallback_PlayerDamage`, `CodeCallback_PlayerKilled` of `maps/mp/gametypes/_callbacksetup` — all required.
   The handles live in a table at 0x884d708..0x884d71c.
2. 0x811037a: `main` of the level script — optional (the loader's "required" argument is 0; handle 0x884d700 stays
   empty when the script or function is missing).

## Game init order (0x8109096)

After the entities of the map are spawned and the per-frame systems are set up, init runs, in this order:
1. the gametype `main` (0x81182f4) with no arguments, self = level;
2. the level script `main` when present (0x81101d0);
3. `CodeCallback_StartGameType` (0x8118322).
Each call is started as a thread and its handle freed right away, so a `wait` in any of them only suspends that thread
and init continues. All three run before the first server frame.

`dm.gsc:46-61` (gametype main) only stores the callbacks in `level.callback*` and the menu hooks; `_callbacksetup.gsc:10-19`
runs `level.callbackStartGameType` once (guarded by `level.gametypestarted`). The DM start (`dm.gsc:63-150`) threads the
shared modules (`_menus`, `_serversettings`, `_clientids`, `_teams`, `_weapons`, `_scoreboard`, `_killcam`, `_shellshock`,
`_hud_playerscore`, `_deathicons`, `_damagefeedback`, `_healthoverlay`, `_grenadeindicators`, `_quickmessages`),
places the `mp_dm_spawn` entities and starts the time limit loop.

## Client lifecycle

Client entities are the first entity slots: client N uses entity N (the connect at 0x80f8e7a uses the entity at
`entity table + N * 0x230` and stores N in the client). TODO(spec): this port keeps its player entity where the map
spawn put it; scripts only see the number through `getEntityNumber`.

### Connect (0x80f8e7a)
Clears the client's game state and its entity, marks the client connected (state 1), sets the session team and
`sessionstate` to spectator,
`spectatorclient` to -1 and the follow target to -1, links the entity to the client (class "player"), reads the
userinfo and, when the server has a password that is not "none", rejects a client whose userinfo `password` differs
(reason `GAME_INVALIDPASSWORD`). Otherwise it runs `CodeCallback_PlayerConnect` with self = the client entity and no
arguments (0x8118350). A local client has no password check (no server password).

### Begin (0x80f90ae)
When the client has loaded the map: client state 2 (active), then the entity is notified `begin` with no arguments
(the string is interned at 0x81228ca into the constant 0x885328c). `dm.gsc:165` waits for it before the client joins
as a spectator (`dm.gsc:213-229`).

### Menu responses (0x8100d16)
The client command `mr <serverId> <menu index> <response>` is ignored unless the server id matches `sv_serverId`; the
menu index (0..31) selects the menu name from the precached-menu config strings. The entity is then notified
`menuresponse` with two arguments, the menu name first and the response second (a malformed command gives menu "" and
response "bad"). `_menus.gsc:54-58` loops on it; responses like `autoassign`, `allies`, `axis`, `spectator` on the
team menu and a weapon name on the weapon menu reach `level.autoassign` / `level.allies` / ... / `level.weapon`
(`dm.gsc:783-937`).
Offline the local client sends the same pair directly (no server id; the menu index lookup is replaced by the menu
name).

### Disconnect (0x80f94ce)
1. When the script system is running, the entity is notified `menuresponse` with response `disconnect`; the menu
   argument is a fixed string (0x8157ec3). TODO(spec): its text; this port passes "";
2. every active client following this one as a spectator is stopped (docs/spec/mp-spectate.md);
3. `CodeCallback_PlayerDisconnect` with self = the entity (0x8118384); `_callbacksetup.gsc:46-50` notifies
   `disconnect` in script;
4. the entity is freed and the client slot cleared (state 0).

## Script menus (in-game MP UI)

Evidence: VAs in the patch 1.3 MP executable (client) and in the 1.3 Linux dedicated server, the shipped
`ui_mp/scriptmenus/*.menu`, `default_mp.cfg` and `maps/mp/gametypes/_menus.gsc`. Implementation: `src/game/mpgame.cpp`
(server side), `apps/iw2view/main.cpp` (local client), `src/game/clientinput.cpp` (bound console commands).

### Menus the gametypes use
`_menus.gsc:1-22` names and precaches them: `ingame` (the ESC menu), `team_<allies><axis>` (team choice, e.g.
`team_britishgerman` on mp_toujane), `weapon_<allies>` / `weapon_<axis>` (weapon choice per team),
`serverinfo_<g_gametype>` (shown once after the connect), `callvote`, `muteplayer`. The client loads a precached menu
from `ui_mp/scriptmenus/<name>.menu` (path built at 0x5326d0; caller 0x4d0a40). The serverinfo menus and newer team
menus come from the patch archive `iw_15.iwd`, which overrides `iw_06.iwd` (normal archive order).
`ui_mp/ingame.txt` names menu files that are not shipped; the ESC menu is the script menu `ingame`.
What they draw over (the game view, never the main-menu backdrop `bg`): docs/spec/ui.md "Background menu".

### Server to client
- `openMenu(name)` / `openMenuNoMouse(name)` on a client send that client the menu's precache index; `closeMenu`
  closes the open script menu (SP: docs/spec/ui.md "Script menus"; the MP server builtins are in the method table,
  `openmenu` at 0x80fbc94 in the dedicated server). TODO(spec): `closeInGameMenu`'s server command; here closeMenu
  already closes the script menus the ESC key opens.
  Each openMenu / closeMenu call is its own server command (`t` / `u`, docs/spec/net.md section 10), sent in call order
  with the client's other commands; the client maps the index through the server's menu configstrings (from 1246), not
  through its own precache order.
- `setClientCvar(name, value...)` (dedicated server 0x80fcf8c; the target must be a client) sets a cvar on that client
  only. The client's handler (0x4d1af0) keeps three names out of the cvar system: `cg_objectiveText` and
  `g_scriptMainMenu` go to client buffers (1024 and 256 bytes), `cg_drawHud` to an int; every other name is set as a
  cvar. The scripts use it for the menu gates: `ui_allow_joinauto` / `ui_allow_joinallies` / `ui_allow_joinaxis`
  (`_teams.gsc:371-417`), `ui_allow_weaponchange` (`dm.gsc:192`, `dm.gsc:215`), one `ui_allow_<weapon>` per weapon from
  the matching `scr_allow_<weapon>` server cvar (`_weapons.gsc:94-204` names them, `_weapons.gsc:953` sends them), and
  `g_scriptMainMenu`, the menu ESC opens (`dm.gsc:186-231`, `dm.gsc:799-916`).
  The team menus gate their buttons with `dvartest` on the join cvars, the weapon menus with `dvartest` on
  `ui_allow_<weapon>` (`showDvar` 1 = selectable, 2 = shown as restricted), the ingame menu its weapon item on
  `ui_allow_weaponchange` (`weapon_american.menu:96-130`, `ingame.menu:68-93`).

### Client to server
- A menu item's `scriptMenuResponse "<text>"` sends `cmd mr <sv_serverId> <menu index> <text>` (format at
  0x5a32d8); the server turns it into the `menuresponse` notify (above). The team menus answer `autoassign`, `allies`,
  `axis`, `spectator`; the weapon menus the weapon name (`greasegun_mp`, ...); the ingame menu `changeweapon`,
  `changeteam`, `muteplayer`, `callvote`; serverinfo `close`. `_menus.gsc:54-195` dispatches them (team and weapon
  choices go to `level.allies` / `level.axis` / `level.autoassign` / `level.spectator` / `level.weapon`; `back` reopens
  the previous menu).
- The console command `openScriptMenu <menu> <response>` (handler 0x410920, registered in the client init) needs
  exactly two arguments (else it prints its usage text, 0x5a3328 / 0x5a32f8) and a connected client; it looks the menu
  name up case-insensitively among the 32 precached menu config strings (index -1 when absent) and sends the same
  `mr` command. `default_mp.cfg` binds x to `openscriptmenu ingame changeweapon` (the weapon menu of the client's team).
- `togglemenu` (handler 0x4107b0, bound to ESCAPE in `default_mp.cfg`): in a running game it opens the menu named by
  the client's `g_scriptMainMenu` buffer; when no menu of that name opens, it opens `main`. TODO(spec): the UI state
  calls around it (0x415830, 0x53adc0) and the branch taken while the UI is already active.

### Weapon for the next respawn
A weapon response while the player is alive only stores the choice (`self.pers["weapon"]`) and tells that client it
will respawn with it (`dm.gsc:896-937`, `menuWeapon`); the next spawn gives it. Implemented by the script; the port only has to deliver
the response.

### Chat and quick messages
See docs/spec/mp-chat.md (say / say_team, the chat lines, quick messages, name cleaning).

### Scoreboard
`+scores` (TAB and F4 in `default_mp.cfg`) shows the scoreboard while held; the MP client draws it itself (no menu
file). The server's `score` reply, the order, the layout, the team blocks and the intermission view are in
docs/spec/mp-scoreboard.md. Spectating, following and the intermission camera: docs/spec/mp-spectate.md.

## Local client view

A listen server runs the clients of the network and one local client whose screen this process shows (client 0 here,
`MpState::localClient`). A dedicated server shows nobody: it has no local client (`localClient` -1), and the
network client or bot in slot 0 is served like every other, with its own entity (`World::mpSetDedicated`). Commands the server sends to one client reach the front end only when they are for the local
client:
- `iprintln` / `iprintlnbold` exist twice in the dedicated server: the function (0x8110ad2 / 0x8110afe) sends the
  message to every client (client -1), the method (0x80fca18 / 0x80fca9e, self must be a client) only to that client.
  `dm.gsc:171` (a connect) uses the function, `dm.gsc:933` (respawn weapon) the method.
- `newClientHudElem(player)` (0x8103c9e) makes an element owned by that client (the argument must be a client); only
  the owner draws it (`_hud_playerscore.gsc` gives every client its own score element). A disconnect (0x80f94ce) frees
  every element owned by the client (0x8103414) before the disconnect callback; the scripts make these elements at each
  `connecting` and never destroy them, so without the free a rejoin into the same slot stacks a second set on the first.
  `newTeamHudElem(team)`
  (0x8103d0a) is drawn for that team. TODO(spec): the team test of the team element.
- Script menus and client cvars: above.
Implemented: `World::mpLocalClient`, `mpLocalCvar`, `mpOpenScriptMenu`, `mpHudElemVisible`; iw2view opens the local
client's menus from `ui_mp/scriptmenus/`, reads its client cvars before the server's, routes `scriptMenuResponse` to
`mpMenuResponse`, runs `togglemenu` / `openscriptmenu` from the MP bindings and draws only the local client's HUD
elements and messages. `mpAutoJoin` stays for headless runs and tests (iw2game, `IW2_MP_AUTOJOIN=1` in iw2view).

## Damage and death

Movement, fall damage and bullet differences from SP (rifle pass-through, damage flags): docs/spec/mp-pmove.md.

### Player damage (0x8101b40)
A client entity takes damage only when it can take damage, its client is not in a no-damage state and the client is
active (state 2). The damage is multiplied by the hit-location multiplier of the attacker's weapon (same tables as the
SP game) and truncated; a result above 0 starts `CodeCallback_PlayerDamage` (0x81183b8) with 10 arguments in this order:
inflictor, attacker, damage (int), damage flags (int), means of death (string), weapon name, point, direction,
hit location name, time offset (int). `_callbacksetup.gsc:56` names them.

The weapon argument is the weapon of the damage, not simply what the attacker holds: the function takes the entity weapon
index (entity +200) of the inflictor when there is one, else of the attacker, else 0 (none); the same index also selects the
hit-location multiplier table. A grenade or rocket missile carries the weapon it was thrown or fired as, so a grenade kill
names the grenade even after the thrower has switched weapons; a bullet or a melee blow has the shooter as inflictor and
names the weapon in hand. The death (below) receives that same weapon from `finishPlayerDamage` (its weapon argument).
OpenIW2: `World::damage` takes the inflictor (the radius damage of grenades and rockets and the rocket's direct hit pass the
missile) and `World::damageWeapon` picks the weapon in that order. The script's `radiusdamage` (0x8114cbe) has no inflictor and has the world
entity (1022) as attacker, always with means of death MOD_EXPLOSIVE: the callback's inflictor argument is undefined (0x8117f50 pushes
undefined for no entity), the attacker the world entity and the weapon `none` (the world's weapon index is 0). The engine does not change health here; the
gametype applies the damage with `finishPlayerDamage` (`dm.gsc:268`).

Means-of-death strings (table 0x8187c40, index 0..14, anything else gives `badMOD`): `MOD_UNKNOWN`,
`MOD_PISTOL_BULLET`, `MOD_RIFLE_BULLET`, `MOD_GRENADE`, `MOD_GRENADE_SPLASH`, `MOD_PROJECTILE`,
`MOD_PROJECTILE_SPLASH`, `MOD_MELEE`, `MOD_HEAD_SHOT`, `MOD_CRUSH`, `MOD_TELEFRAG`, `MOD_FALLING`, `MOD_SUICIDE`,
`MOD_TRIGGER_HURT`, `MOD_EXPLOSIVE`.

Damage flags are the script's `level.iDFLAGS_*` bits (`_callbacksetup.gsc:82-87`): 1 radius, 2 no armor, 4 no
knockback, 8 no team protection, 16 no protection, 32 pass-through.

### finishPlayerDamage
The method (0x80fb170) takes its arguments as the callback gave them. A damage of 0 or less does nothing. There is no armor in
the game; the steps are:
- Knockback, unless the damage flags have 4 or the entity flags 8. The knockback is the damage times 0.3 (crouched 0.15, prone
  0.02), truncated, at most 60. On a turret (eFlags 0x300) there is none. The velocity gains the normalized direction times
  knockback · `g_knockback` (default 1000) / 250 (no direction: nothing is added). When the knockback is not 0 and pm_time is 0,
  pm_time becomes 2·knockback clamped to 50..200 and pm_flags gets 0x400 (slide), also without a direction. Unlike the single
  player push it has no self-blast reduction. It runs on the damage as the script passes it, before the god check.
- Entity flag 1 (god) ends the method here. Flag 2 (demigod) reduces a damage that would kill to leave 1 health.
- For a weapon with a bullet effect: the flesh-impact events 0xb6 / 0xb7 and 0xb9 / 0xba (not implemented here).
- The accumulated damage and its direction (from the origin when there is no direction).
- The damage timer: ps+0x90 grows by damage · `player_dmgtimer_timePerPoint` (100) up to `player_dmgtimer_maxTime` (750);
  ps+0x94 copies it; ps+0x98 is the yaw of the direction (0 when none) minus the view yaw.
- Health minus damage, the notify `damage` (amount, attacker), then at 0 or below health clamps to −999 and the die handler runs,
  else the pain handler. The health copy in the player state follows.
OpenIW2: the push is queued per client and applied by the host, the bot controller and the network client's move.

### No damage debounce
Neither the engine path above nor the gametype callback (`dm.gsc:247-268`) ignores a hit that follows another one
closely: every hit that reaches the callback is applied in full. Health does come back through the regeneration loop of
`_healthoverlay.gsc:74-152` (started on spawn): every 0.05 s it compares health with the last value it saw; a drop resets
the hurt time, and health at or above that value restores full health (`setnormalhealth`) once
`level.playerHealth_RegularRegenDelay` (5000 ms) has passed since the hurt time, or regains in steps when the player is
below 35 %. A consequence: if health is raised by something the loop does not see (a direct field write, as tests may do)
and a later hit brings it down to exactly the value the loop remembered, the loop does not notice the hit and may restore
full health on its next tick. This is the shipped script's behaviour, not an engine debounce.

### Player death (0x810175a)
Only for a client whose movement type is normal (0 or 1). The entity is notified with one argument, the attacker
(the event name is the constant 0x885322a; TODO(spec): its text, this port uses `death` as the SP player death does), the
movement type becomes the matching dead type, and `CodeCallback_PlayerKilled` (0x8118492) runs with 9 arguments:
inflictor, attacker, damage, means of death, weapon, direction, hit location, time offset, death animation duration
(int, ms). Afterwards the clients following this one stop following, the entity stops taking damage and its contents
become the corpse contents. A turret's bullets have the turret entity as inflictor, so the weapon passed
on is the turret's weapon (docs/spec/turrets.md "Multiplayer"). The death animation duration comes from the player animation script's death event (0x80d96ee,
docs/spec/mp-players.md).

`dm.gsc:303-391`: the gametype sets `sessionstate` to dead, adds 1 to the victim's `deaths`, adds 1 to the attacker's
`score` (subtracts 1 for a suicide or a non-player killer), waits 2 seconds and respawns: immediately with
`scr_forcerespawn` > 0, otherwise when `useButtonPressed` is true (`dm.gsc:526-557`).

## Client fields

`sessionteam` and `sessionstate` are engine fields with validated setters: `sessionteam` (0x80f7db6) accepts
`allies`, `axis`, `none`, `spectator` (error text 0x8157ae0), `sessionstate` (0x80f7efc) accepts `playing`, `dead`,
`spectator`, `intermission` (error text 0x8157b40); anything else is a script error. Other per-client fields the DM
scripts write: `spectatorclient`, `archivetime`, `psoffsettime`, `statusicon`, `score`, `deaths`, `maxhealth`,
`name`. TODO(spec): their storage and range checks; this port keeps them as entity fields (`score` / `deaths` int 0
at connect, `name` from the client).
`self.pers` is an array on every client from the connect on: `dm.gsc:190` indexes it before any script assigns it
(and `_killtriggers.gsc:22` reads it on every player). It belongs to the client, not the entity: when client 0
(whose entity is the local player's and stays) disconnects, its `pers` goes with it, and the next client 0 starts
with an empty array. TODO(spec): its engine storage.

Implemented: the two setters reject other values with the engine's texts (VM field-store hook); the other fields are
plain entity fields.

## Client spawn and the killcam

- `self spawn(origin, angles)` on a client places it and starts it from an empty player state: `_weapons.gsc:279`
  gives the pistol only when the `primaryb` slot is `none`, so the weapons of the previous life are gone after a
  respawn.
- The client spawn itself (ClientSpawn, 0x80f910e in the 1.3 Linux dedicated server, called by the `spawn` method
  0x80fcb24): it keeps the client's persistent block (0x100 bytes at client +0x26a8), clears the whole client
  structure (0x28a4 bytes, the player state at its start included) and puts the persistent block back. Everything of
  the previous life is gone with it: stance and the stance flags, lean, aim, velocity, the shell shock fields, the
  frozen-controls flag (+0x27b4, see below). It then toggles the teleport bit (eFlags 2, ps+0xa0), sets the view height
  to 60 (target +0xf4 and current +0xf8), sets pm_flags 0x1000, stores the origin and the view angles and sets
  commandTime to the level time minus 100. The client's own stance (the one its commands request) must follow: openiw2 keeps it at standing while the
  local client is dead, so the first command after the spawn does not bring back the stance of the death
  (the client-side location of that reset is not identified; the behaviour is the one described). Implemented: the gametype-visible part (inventory, origin, angles), the shell
  shock, frozen controls, disabled weapon and unapplied pushes cleared, the accumulated damage of the hit direction icons cleared (the fatal hit, which
  is not reported while dead, would otherwise show after the respawn), and a per-client spawn count: the bot controller, the server's movement of a
  network client and the listen host's own front end restart the movement state from it (a player who died prone or
  crouched stands again). A network client's prediction starts afresh when the server's pm_type goes from a
  non-playing type (dead, spectator) back to 0, which only a spawn does. The spawn runs no kill box (0x811f0b4 has no direct
  caller); takedamage is cleared at 0x80f91e3 and set again later in the spawn (0x80f767d, reached through 0x80f7516).
  The eFlags bits 2 and 0x100000 are kept through the clearing, then bit 2 toggles. openiw2 sends this bit in the entity state
  and the player state (even spawn count 0, odd 2); as a dead player has no entity, a spawned player starts without a lerp from
  the death place.
- The client draws the weapon of the player state as it is after the spawn. Death sets the weapon to 0 and the
  spawn's gametype gives the loadout again, often the same weapon as before; the front end of this port therefore
  treats any change of the player-state weapon away from the value it last wrote itself as a new selection, also a
  return to the previous weapon (without that the respawned player had no view weapon and no ammo HUD).
- The killcam (`_killcam.gsc:37-66`) sets `archivetime` and waits one frame so that the code can trim it to the
  archived history (docs/spec/mp-hud.md "Killcam": the snapshot archive, `setarchive`, the trimming rule and the
  replay from the killer's view).
- `obituary`, `playLocalSound`, the MP `objective_*` variants, `setWayPoint`, head icons, crosshair names and the MP
  compass: docs/spec/mp-hud.md.
- `cloneplayer` returns a body entity at the player's origin and angles (`_deathicons.gsc` reads its origin) with
  the player's model, attachments and death animation (docs/spec/mp-players.md). TODO(spec): its class and lifetime.
- `getentarray()` without arguments returns every entity (`_gameobjects.gsc:3` walks them to delete game objects of
  other gametypes).

## Per-client player state

- Freeing an entity unlinks its children: G_FreeEntity (0x4d7c30 in the patch 1.3 SP executable) unlinks the entity
  from its parent and then walks its child list (ent+0x1cc), unlinking each child (0x4d6720). S&D relies on it: the
  planter is linked to the bomb zone trigger and disabled from firing for the plant (sd.gsc:1697-1698, the defuse
  likewise at sd.gsc:1911-1912); a finished plant deletes every bomb zone (sd.gsc:1727) and enables the weapon but never
  calls `unlink`, so only the free releases the player. An interrupted plant or defuse calls `unlink` itself
  (sd.gsc:1789, :1956). Implemented in `deleteEntityScripted` for `linkto` and `playerLinkTo*` children; it holds for
  the local player, bots and network clients alike (the link is per entity).

Every client has its own player state; the methods that change it work on the client `self`, and only that client's
view shows the effect. Single-player code paths that change "the player" (the one front end's view, input, sounds or
movement) must not reach the local player when `self` is another client (a bot or a network client):

- `shellshock(name, duration)` (0x81162a2): `self` must be a client, the name one of the precached shocks (the server
  compares it with the configstrings 0x48e + 1..15, case-insensitively), the duration 0..60 s in milliseconds
  (script errors otherwise). It stores the index, the level time and the duration in the client's player state
  (+0x5d8 / +0x5dc / +0x5e0, the net fields `shockType` / `shockStartTime` / `shockDuration`), so it reaches only that
  client's snapshot. With the client alive (health > 0) it also adds a predictable player-state event (0x12, parameter
  0) — TODO(spec): what the client does with that event. `stopshellshock` (0x811642a) clears the three fields.
  Implemented: the fields per `MpClient`; the local view shows the local client's; the network game publishes the
  shock names as configstrings 0x48e + index and writes the three fields into the client's own snapshot, and the
  client's view takes the start time onto its own clock like the HUD times and loads the shock record by name.
- `freezeControls` (0x80fd204) writes the client field +0x27b4 of `self`; `setPlayerAngles` (0x80fa964) the view of
  `self`. `iprintln` / `iprintlnbold` as methods (0x80fca18 / 0x80fca9e) print to `self` only, as functions to all.
- Implemented for a non-local `self`: `viewKick`, `setOrigin`, `setPlayerAngles`, `freezeControls`, `allowStand` /
  `allowCrouch` / `allowProne` / `allowLeanLeft` / `allowLeanRight`, `setReverb` / `deactivateReverb`,
  `playerLinkTo*`, `unlink` and `setBlur` keep their argument checks and entity-side effects but do not change the
  local player; frozen controls are kept per client and `setPlayerAngles` turns a bot. `getPlayerAngles` and
  `getStance` answer from that client's movement (a bot's controller, a network client's animation input),
  `playerADS` from its aim; `isFiring`, `isMeleeing`, `isThrowingGrenade` and `getNormalizedMovement` return 0.
  TODO(spec): the effect of these on a network client (the original keeps them in that client's player state; this
  port has no wire field for them yet) and the input queries of non-local clients.

## Local clients

Offline, a client is a slot in the server's client table with its entity; the local player's entity is client 0.
Further slots get a new player entity (the slot a bot or a network client would use later). A non-local client's
use button comes from the buttons set for its slot. Two clients that connect in the same server frame lose one
`connecting` notify (`_menus.gsc:49` re-arms its wait only after handling one), so the tools connect one client per
frame. TODO(spec): whether the original can connect two clients within one frame.

## Built-ins used by dm.gsc and the shared modules

Needed for DM to run (implemented): `spawn` on a client (`self spawn(origin, angles)`: places the client, sets the view
angles; `dm.gsc:419`), `finishPlayerDamage`, `suicide` (0x80fbbd2: clears the god flags and health, then the death path with the player as its own inflictor and attacker, 100000 damage, `MOD_SUICIDE`),
`isplayer`, `getEntityNumber`, `getGuid` (0 for a local client), `useButtonPressed` (existing), `setClientCvar`
(stores a per-client cvar), `openMenu` / `closeMenu` / `closeInGameMenu` (record the open script menu of the client; the local client's front end
gets popupopen / popupclose, "Script menus" above),
`logPrint`, `iprintln` / `iprintlnbold` with a player argument, `getTime`, `setSpawnWeapon`, `giveMaxAmmo`,
`exitLevel`, `isSplitScreen` (0), `newClientHudElem` (client HUD element).

`dropItem`: "Dropped weapons" below.

Stubs (cosmetic or client-only; TODO(spec)): `setWaypoint` (HUD element), `precacheRumble`, `precacheMenu`,
`precacheHeadIcon`, `precacheShader` (when absent), `setClientNameMode`, `makeCvarServerInfo`, `resettimeout`,
`playrumble`, `stoprumble`,
`setplayerteamrank`, `sendranks`, `sethudelemlist`, killcam fields. `allowSpectateTeam` and `precacheStatusIcon`:
docs/spec/mp-spectate.md, docs/spec/mp-scoreboard.md.

## Dropped weapons

VAs in the 1.3 Linux dedicated server. The gametypes drop a killed player's weapon from their killed callback
(`dm.gsc:318`, `tdm.gsc:403`, `sd.gsc:570`, `ctf.gsc:472`, `hq.gsc:494` call `_weapons.gsc` dropWeapon, lines 396-418): the
current weapon is dropped with `dropItem` when its slot still has clip or reserve ammunition.

`self dropItem(name)` (0x80fb05a): `self` must be a player ("entity %i is not a player"); a weapon name drops that weapon
(0x81064de), and the builtin returns the new item entity. 0x81064de:
- a weapon the player does not have, or a clip-only weapon (def +0x340) with an empty clip, is only taken away; no item;
- otherwise the item is launched (Drop_Item 0x81063d4, LaunchItem 0x81061cc) and the player's stock and clip of the weapon
  move onto it (item +0x1a0 / +0x1a8, -1 for none) and are cleared; the weapon is taken. With neither stock nor clip the
  item's contents lose bit 0x200000, so it cannot be touched (0x810686c..0x810688a).

Drop_Item: the item starts at the player's origin raised by half the player's box height (0x8106493..0x81064b2, the
constant 0.5 at 0x815a3d8) and flies along the player's yaw (pitch and roll 0) at `g_dropForwardSpeed`, with
`g_dropUpSpeedBase` + random() * `g_dropUpSpeedRand` added upwards (dvars registered at 0x8108861..0x81088c9: defaults 10, 10,
5, range 0..1000). LaunchItem: class `weapon_<name>` (0x8106142), the weapon's world model, a gravity trajectory from the
current time; the dropper is the item's owner (+0x90) until a think one second later (0x8106130) clears it. Until then the dropper cannot grab it; Touch_Item
(0x8105cac) also suppresses the "can't get" message for the dropper's own item.

`dropItem`'s optional second argument names a tag (default `tag_weapon_right`, 0x80fb0f8). When the dropper's model has the
tag (0x81068a1), the item is moved from the dropper's box centre to the tag by a trace with the item's box (the dropper
passes) and takes the tag's angles; without the tag it keeps the dropper's angles (0x81069bf); the roll gains 90 degrees in both
cases (0x81069d9). The item then spins: a linear angular trajectory with rates random() * 50 (pitch), 40 (yaw), 60 (roll) from the launch (0x81069fe..0x8106a79); landing clears it. The box height is the dropper's own, so a
non-local client's stance counts (30 prone, 50 crouched, 70 standing). A name that is not a weapon goes to the general item drop
(0x80df226, 0x81063d4); the shipped scripts only drop weapons (`_weapons.gsc:416`, `:428`).

Dropped-weapon slots: `g_maxDroppedWeapons` (0x8108758: default 16, 1..32) slots (0x864e0d8). A new item takes the first free
slot (0x8105ffa); with all slots in use the item whose nearest playing client is farthest away is freed (squared distances,
start value 1e12 when no client plays) and its slot reused. There is no other lifetime limit.

Messages of a pickup go to the client that took it as a print command with the letter `f` (0x8105dbd, 0x8105e19, with the
keys of the local path); openiw2 forwards them to network clients, while their pickup sound event (ps event 0x90 / 0x91) is not
yet on the wire.

Pickup: every playing client touches items (G_TouchTriggers per client, Touch_Item 0x8105cac -> Pickup_Weapon 0x8105072),
after the grab test (0x80df39e). openiw2 applies the single-player grab and pickup rules (docs/spec/grenades.md): ammunition
of an owned weapon, a weapon for a free slot; The grab test has one multiplayer rule read so far: the client number of the item (the dropper, until the one-second think clears it) equal to the client's own refuses the item (0x80df421), so the dropper does not pick its own weapon straight back; its weapon branches match the single-player shape. Pickup_Weapon 0x8105072 itself was not compared.
The use key on an item of a weapon the client lacks swaps weapons (the local player's use path, docs/spec/grenades.md); the
swapped-out weapon is dropped through 0x81064de (0x810551e / 0x81055b2).

openiw2 (src/game/mpdrop.cpp, `World::mpDropWeapon`, test `tests/test_mpdrop.cpp`): implemented as above; the item runs under
G_RunItem like a single-player item, and the network game sends it to clients as a model entity. Bots and network players
touch items through the per-client trigger pass (`World::mpClientTriggers` -> `mpClientItems`); a press of use swaps for
the item the client's use search picked (touching it or not). Their pickups raise no local HUD event, message or listener sound.
TODO(spec): a non-local client's box height (70 used), the dropItem tag argument and non-weapon items, the pickup messages and sounds those clients should get, the swap drop as a launched
item (the single-player static placement is used), the owner's message suppression.
Checked by `tests/test_mpdrop.cpp` over 100 bot deaths in play: every death that meets the dropWeapon condition drops the
item, with its world model, in the frame of the death. A bot killed in the air (jumping or falling from a roof) drops the item
from the body's height and it lands on the floor below, which can be far from where the body settles.

## Team gametypes

tdm, sd, ctf and hq use the builtins below on top of the DM set. Evidence: the builtin table of the 1.3 Linux dedicated
server (name, handler) and the handlers' VAs.

### Team scores
`getTeamScore(team)` (0x8115d5a) and `setTeamScore(team, score)` (0x8115dda) keep one integer per team (allies at
0x864c584, axis at 0x864c580). Any team string other than `allies` / `axis` is the script error "Illegal team string
'%s'. Must be allies, or axis.". Game init clears both (0x81098dc). The setter also sends the value to every client as
a server command (`H` allies, `G` axis, `%c %i`); the scoreboard and `_hud_teamscore.gsc:110-130` read it back with
the getter. Implemented: `MpState::teamScore`, `World::mpTeamScore`. The client's server-command switch (0x4d1bae, its byte map at 0x4d20d0) stores the integer argument: `G` into 0x14e5c38, `H` into 0x14e5c3c, read by the score ownerdraws by team; no other effect. openiw2's scoreboard already takes the team scores from the `b` command.

### Spawn points
`spawnpoint placeSpawnPoint()` (0x81167ae) settles a spawn entity on the floor with the standing player box
(−15, −15, 0)…(15, 15, 70) and the clip mask 0x2810011, the entity itself excluded: a trace from the origin 128 units up
(so a spawn under a low ceiling starts below it), from where it stopped a trace 262144 units down, and the entity's
origin becomes the end of the second trace. When the box is in solid at that point the text "WARNING: Spawn point
entity %i is in solid at (%i, %i, %i)" is printed (the position as integers). The spawn logic of every gametype calls
it on its spawn class at start (`_spawnlogic.gsc`). TODO(spec): the ground entity it records.

`positionWouldTelefrag(origin)` (0x811322c) lists the entities with player body contents (0x2000000) whose boxes touch
the standing box at `origin` and returns 1 when one of them is a client (the original also checks the client's state
word; here a client counts while it is playing and alive), else 0.

### Team use triggers and objectives
`trigger setTeamForTrigger(team)` (0x8117944) works on `trigger_use` and `trigger_use_touch` only (error
"setteamfortrigger: trigger entity must be of type %s or %s") and takes `allies`, `axis` or `none` (error
"setteamfortrigger: invalid team used must be %s, %s or %s"); it stores a team byte (allies 2, axis 1, none 0). The use
scan of a client (0x8121d88) skips such a trigger when the byte is not 0 and differs from the client's session team.
`sd.gsc` gives each bomb zone to the attackers and the planted bomb to the defenders this way.

`objective_team(index, team)` (0x8112f8a): index 0..15 (error "index %i is an illegal objective index. Valid indexes
are 0 to %i"), team `allies` / `axis` / `none` with the same byte encoding; it limits which team's compass shows the
objective. Stored on the objective itself (the same slot the HUD and compass read, see mp-hud.md); one registration in src/game/mphud.cpp.

`disableWeapon()` / `enableWeapon()` (0x80fd286 / 0x80fd306) on a client block and allow firing (planting and
defusing in `sd.gsc` hold the weapon down). TODO(spec): the player state bit; here a client flag the bot controller
honours.

`isOnGround()` (0x80facc4) on a client returns whether its player state has a ground entity (not 0x3ff); other
entities are the error "entity %i is not a player". Here: the local player's from the front end's pmove, a bot's from
its controller, any other client 1.

`client clientClaimTrigger(trigger)` (0x8117a72) and `clientReleaseTrigger(trigger)` (0x8117b5a): the claimer must be
a client ("clientclaimtrigger: claimer must be a client.") and the trigger a `trigger_use` / `trigger_use_touch`. The
trigger stores the claiming client (+0x1b4, 0x3ff when free); a claim of a trigger held by another client changes
nothing. The use scan skips a trigger claimed by another client (0x8121db4). `sd.gsc:1657-1658` claims both bomb zones
while one is being planted.

### Use search of every client
At the end of every playing client's frame (0x80f7516 calls 0x8121bc6) the server fills the client's player state
with what its use key would use: first hint 0 (+0x598), hint string -1 (+0x59c) and no entity (+0x5a0 = 0x3ff).
Nothing more happens while the client is dead or its weapon is in the binocular states 0x11..0x16. On a turret only
the turret weapon's drop hint is set (0x8121b36: weapon number + 4 with dropHintString, when the weapon has one).
Otherwise the scan (0x8121494, the scoring of the single-player scan in controls.md, from the client's eye along
its view) is walked in order:
- a `trigger_use` / `trigger_use_touch` whose team byte is 0 or the client's team and that is not claimed by another
  client ends the search: hint = its cursor hint, and when that hint is not 0 its hint string; a trigger with hint 0
  leaves no entity. A trigger of another team or claimed by another client is skipped;
- any other plain entity (type 0) ends the search with nothing;
- a weapon item (type 3, 0x8121aaa: an item of a weapon that is not a grenade and that the client does not own) ends
  it with hint = weapon number + 4;
- a turret (type 9) usable by the client (0x810c358: not in use, the arc test, no grenade fuse, on the ground) ends
  it with hint = weapon number + 4 and the weapon's useHintString when it has one;
- anything else is skipped.
A press of use then uses the found entity. openiw2 runs this for every non-local client (`World::mpClientUseSearch`,
bots included) and sends it in the network client's player state; a press of use (+activate or +usereload, a tap between two server frames
counting) fires the picked use trigger or takes the picked weapon item.
A press on a picked turret gets the client on it (src/game/mpturret.cpp); on the turret the search gives the drop
hint. Network clients give the search their weapon state, grenade fuse and mantle; pm_flags 4 is taken as the mantle.

### Triggers for every client
In the MP game every client entity touches triggers each server frame and can use `trigger_use` / `trigger_use_touch`
(the SP code does this for the single player only). Implemented for the non-local clients in `World::mpClientTriggers`:
the standing box (no stance yet) against trigger_multiple, trigger_radius, trigger_once, trigger_hurt (damage per client)
like the SP player, and a press of use (the use bit set this frame, clear the previous one) fires the use trigger the
client's last use search picked (above). CTF flags (`ctf.gsc:1093`) and the SD bomb zones (`sd.gsc:1638`) wait for
this "trigger".

### map_restart
`map_restart(persist)` (0x8116a3c) can be called once per level: a second call is the error "map_restart already
called", a call after `exitLevel` "exitlevel already called". It stores `persist` (0x864e0d4) and queues the console
command `fast_restart`, which restarts the level after the server frame (below, "Fast restart").

### Fast restart

openiw2: the restart rebuilds the world but keeps the function names read from all scripts (they depend on the files only), which were most of its time.
The `fast_restart` command (handler 0x808c0bc → 0x808be3e with "fast" set) restarts the level without dropping the
clients:
1. the game module is shut down and initialised again on the same map; game init (0x810954c) gives every connected
   client a new, empty `pers` array (0x808efce) unless the restart persists. The `game` array keeps its contents over
   a persistent restart: `sd.gsc:285-298` only fills the round counters and the team scores when they are undefined
   and `sd.gsc:1164` restarts with persist on. TODO(spec): where the `game` array is kept. The team scores are cleared
   by game init like at a map start; sd sets them again from `game`.
2. three server frames run, each 100 ms later (0x808bf83..0x808bfa3);
3. every client slot in use is connected again (the game's connect 0x80f8e7a, so `CodeCallback_PlayerConnect` runs),
   and the clients that were in the game (state 4) enter it again (0x808f628: the `begin` notify).
Implemented in `World::mpFastRestart` (src/game/mpgametypes.cpp): the World is rebuilt in place from the map, keeping
the cvars, the `game` array (plain values; entity references inside it become undefined), the client slots with their
names, client cvars and (persist) `pers`, the log, the script and engine random sequences and the level time.
Bots keep their controllers. Every `Entity*` taken before the restart is invalid afterwards (`World::mpRestarts()`
counts the restarts).

### sv_maxclients
An int dvar, default 20, range 1..64 (registered at 0x8092d93). It sets the number of client slots, and
`_teams.gsc:31-40` limits a team to half of it: with the cvar missing (0) nobody could join a team by name.

## End of the map

Evidence: VAs in the 1.3 Linux dedicated server; the same strings and command are in the patch 1.3 MP executable
(map_rotate at 0x451fe3, ExitLevel at 0x4fcaa4).

`exitLevel(persist)` (0x8116b6a) is a script error when `map_restart` ("map_restart already called") or `exitLevel`
("exitlevel already called") already ran this level; otherwise it stores persist (every shipped gametype passes
false, e.g. `dm.gsc:683`) and runs the game's ExitLevel (0x81098b8): it appends the console command `map_rotate` (run on
the next command-buffer pass), clears both team scores, puts every client in the game back to connecting, and logs
"ExitLevel: executed".

`map_rotate` (0x808c132) walks the string dvar `sv_mapRotationCurrent` (default "", registered with
`sv_mapRotation`):
1. it prints `map_rotate...` and both dvars; when the current list is empty it is copied from `sv_mapRotation`;
2. it reads a token (0x808c0d0: the token is cut off the front of `sv_mapRotationCurrent`, which keeps the rest); with
   no token left it copies `sv_mapRotation` again and reads once more;
3. no token: "No map specified in sv_mapRotation - forcing map_restart." and `fast_restart` of the current map;
4. `gametype` (case ignored) takes the next token as `g_gametype` ("Setting g_gametype: %s."); a different gametype
   while the server runs turns persist off (0x8108df8(0)). Without the token: the "No gametype specified after
   'gametype' keyword …" text and `fast_restart`;
5. `map` takes the next token and runs `map <name>` at once ("Setting map: %s."), which ends the command. Without the
   token: "No map specified after 'map' keyword …" and `fast_restart`;
6. any other word prints "Unknown keyword '%s' in sv_mapRotation." and the walk goes on.

So with the default empty rotation a listen server restarts the same map; it never returns to the menu by itself.
The `map` command with a missing map prints "Can't find map %s" and changes nothing.

Implemented: `game::mapRotate` (src/game/maprotation.cpp) on the cvars; iw2view runs it once after `exitLevel`, loads
the next map (`mp/<map>`) with the new `g_gametype`, the local client and the menu's bots connect again. The
`fast_restart` case reloads the map in iw2view. TODO(spec): run it as the in-place fast restart (`World::mpFastRestart`)
and the rotation's token reader's comment rules. A fast restart inside a World (sd rounds) leaves every entity
reference invalid; iw2view drops its per-entity interpolation and view-weapon state when `World::mpRestarts()` changes.

## Leaving the game

`ui_mp/scriptmenus/ingame.menu:276`–`296` (the ESC script menu's "leave game", confirmed with yes) runs `disconnect`.
On the listen server the local client's disconnect ends the server: iw2view drops the world, opens the MP `main` menu
and forgets the bots of that server; a new StartServer starts from fresh state (its weapon classes and bots are written
again).

The client's disconnect (CL_Disconnect, 0x40cd10 in the patch 1.3 MP executable) runs the same for the player's own `disconnect`,
a server drop and an error: it sends the disconnect message when connected, shuts cgame down (0x4b2490) and stops every sound
with the stop-all routine 0x44f940 called with no keep flags, clears the client state and the connection (the block at
0x609fe0, 0x40cca0 keeps only a few user settings), and, when no local server runs, shuts the server side down (0x437090).
The stop-all routine ends every 2D and 3D sample and closes every stream, the music slot (stream 0x20) and the two ambient
slots (0x21, 0x22) among them, resets the two ambient slot states and clears the channel volume slots; its keep flags (1 ambient
state, 2 music, 4 ambient streams, 8 samples, 0x10 volume slots) are all 0 here. The map load runs the same pair (cgame shutdown,
stop-all with flags 0), so nothing of the old level plays on and no cgame state (times, fades, hint hold, damage icons) survives
into the next one. Implemented as `resetClientState` in iw2view, used by the disconnect and by the level load (a connect ends in a
level load), with `Mixer::stopAll` for the sounds. A client that left a game never starts the old level's ambient or loops again.
