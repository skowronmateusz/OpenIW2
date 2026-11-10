# Multiplayer HUD and feedback

What the multiplayer player sees about other players: the kill feed, the "you killed" messages, the compass, the icons
over heads, the name under the crosshair, the hit direction, the gametype sounds and the killcam. Server-side VAs are
VAs in the 1.3 Linux dedicated server; client-side VAs are VAs in the patch 1.3 MP executable. Script references are
`maps/mp/gametypes/*.gsc` lines. Client team numbers (client info +0x2c): 0 none (DM), 1 axis, 2 allies, 3 spectator.

## Obituary (kill feed)

### Server: `obituary(victim, attacker, weapon, meansOfDeath)` (0x8113128)
Called by every gametype from its player-killed callback (`dm.gsc:316`, `tdm.gsc:401`, `ctf.gsc:470`, ...). It makes a
temporary event entity (event 0xc6) that every client receives (server flag 8, broadcast):
- the victim's entity number (+0x74);
- the attacker's entity number when argument 1 is an entity, else 1022 (no attacker; +0x78);
- one byte: when the means of death is `MOD_MELEE` (7), `MOD_HEAD_SHOT` (8), `MOD_CRUSH` (9), `MOD_FALLING` (11) or
  `MOD_SUICIDE` (12), the means-of-death index with bit 0x80 set; otherwise the weapon's index (argument 2).
  Argument 2 is the gametype's `sWeapon` from the player-killed callback, i.e. the weapon of the damage (the inflictor's,
  docs/spec/mp.md "Player damage"): a grenade kill shows the grenade's icon.

### Client (0x4e03f0)
- Icon: with bit 0x80 the means of death picks a fixed material: melee `killiconmelee`, head shot `killiconheadshot`,
  crush `killiconcrush`, falling `killiconfalling`, suicide `killiconsuicide`, trigger hurt (13) and anything else
  `killicondied`. Without the bit the weapon's `killIcon` (weapon file) is the icon; an empty `killIcon` gives
  `killicondied`. The icon is 1.4 line heights high and 1.4 wide, 2.8 wide with the weapon's `wideKillIcon` set; the
  weapon's `flipKillIcon` mirrors it horizontally. The fixed icons are always 1.4 x 1.4.
- Victim out of 0..63 is an error ("CG_Obituary: target out of range"); a victim or attacker whose client info is not
  valid ends the handler without a line.
- Names: the victim's name (at most 31 characters) followed by `^7`; the attacker's name (31 characters) followed by
  `^7`. Each name gets its team colour (0x4d9e50): axis `g_TeamColor_Axis`, allies `g_TeamColor_Allies`, every other
  team white. The two colour dvars are server dvars (registered at 0x8108acb..0x8108b3b, flags 0x1100) with the
  defaults (1, 0.5, 0.5, 1) for axis and (0.5, 0.5, 1, 1) for allies.
- An attacker equal to the victim, or no attacker, gives a line without an attacker name (icon and victim only).
- The line goes into the game-message window (0x405530): attacker name, the icon, victim name, in that order, with the
  same lifetime, fade and scroll as the other game messages (docs/spec/ui.md). Nothing is added while the killcam is
  showing (0x151ad94).
- When the local client is the attacker of another player: a centre print of `CGAME_YOUKILLED` with the victim's
  name; when the local client is the victim of another player: `CGAME_YOUWEREKILLED` with the attacker's name. With
  both on the same (non-zero) team the text is the teammate form: the name in red after the localized
  `CGAME_TEAMMATE` (the format has the reference `&&2` coloured `^1`). No centre print while the killcam is showing.
  The centre print (0x4c8020) localizes the text, wraps it at 75 characters and keeps it for 2000 ms; a new one
  replaces the current one only with an equal or higher priority (the obituary uses 9.6). TODO(spec): the centre
  print's draw position, font and fade.
- When the local client was killed, the attacker's name is also kept (0x15195cc; TODO(spec): its reader).

Implemented: `obituary` (mphud.cpp) records the event for the local client's front end (`World::takeMpObituaries`);
`game::obituaryLine` builds the line (texts, team colours, icon and its size) and the centre print; iw2view draws the
line in the game-message window with the icon inline (one space between the parts; TODO(spec): the exact gaps) and the
centre print centred on the screen centre at `cg_centerPrintY` (default -60, registered at
0x4be55a) above it, scale ftol(9.6) / 32, shadowed, with a 100 ms fade at the end (0x4c8360).

## Game-message window position (0x4cad60)

The MP client draws the game-message window (iprintln lines and obituaries) with x 6 and character height 12, but its
y is not the SP constant 100: it is taken from the HUD menu named `Compass` (looked up after loading `ui_mp/hud.txt` by
0x4c0ab0, which keeps the menu rect's y and height) as round(y - (cg_hudCompassSize - 1) * height + 12). The stock rect
(`ui_mp/hud.menu:235`, bottom-aligned, y -140, height 160) gives -128 at size 1, i.e. the window's newest row sits just
above the compass and older rows stack upwards, clear of the clock and team scores at the top of the screen.
cg_hudCompassSize is the float dvar registered at 0x4be150 (default 1). Implemented: `game::mpGameMessageWindowY`,
measured from the bottom of the 640x480 screen. `TODO(spec)`: the alignment codes the call passes to the window draw
(0x406a20); bottom alignment is inferred from the bottom-aligned compass rect.

## Head icons (0x4dbcc0)

Per player entity, for the local client's view:
- The script head icon: `headicon` (a precached head icon, `precacheHeadIcon`) and `headiconteam` (`allies`, `axis`,
  `none`; engine fields with setters 0x80f81d4 / 0x80f82b6). It is drawn when the icon is set and the team is `none`, or
  the local client is a spectator, or the team equals the local client's team. Size `cg_scriptIconSize` (default 0,
  0..100) + 10 world units.
- After it, one of: the local client's own body during the killcam (`headiconyouinkillcam`, size
  `cg_youInKillCamSize` default 6); a client with the connection-interrupted flag (entity flag 0x80,
  `headicondisconnected`, `cg_connectionIconSize`); for a teammate (or with the local client a spectator) the voice icon
  while talking, else `headicontalkballoon` while the entity flag 0x200000 (chatting) is set, size `cg_voiceIconSize`.
- Placement (0x4dbba0): the local client's own entity is skipped in first person. The icon is a world sprite above
  the head: origin z + 82 when the client has no posed body; with a body a tag position + 21 is used (TODO(spec): the tag and that branch). Size = size dvar + 10 world units;
  with `cg_constantSizeHeadIcons` (default 0) the size × 0.0043 is a screen fraction instead. The sprite has a minimum
  screen radius `cg_headIconMinScreenRadius` (default 0.02, 0..1).

The gametypes set the icons in `_friendicons.gsc:69-133` (team games with `scr_drawfriend`) and
`_quickmessages.gsc:731-767` (the talking icon while a quick message plays).

Implemented: `World::mpHeadIcons()` lists the script head icons for the local view (position origin z + 82, size);
iw2view projects them and draws them over the scene. TODO(spec): the head tag path, the sprite's depth test, the
voice / connection / killcam icons (no voice or connection state offline).

## Crosshair names (0x4c97c0)

With `cg_drawCrosshairNames` (default 1) and not during the killcam: every frame a trace from the view origin 8192 units
along the view direction (contents mask 0x2000001, the local client ignored, 0x4c9700) that hits a client (entity
number < 64) remembers that client and the time. The name is drawn for 150 ms after the last hit, its alpha fading
out over the last 100 ms (0x4c2860). Only a teammate's name is drawn (or any name when the local client is a
spectator); with team 0 (DM) nothing is drawn, so enemies are never named. Text: the name; position x
`cg_drawCrosshairNamesPosX` (default 300, 0..640), y `cg_drawCrosshairNamesPosY` (default 180, 0..480) + 16, text scale
1/3, style 3 (shadowed), white. TODO(spec): the health tint (0x1519e5c / 0x1519e60) and the horizontal alignment of
0x5322c0.

Implemented: `World::mpCrosshairClient()` (the trace); iw2view keeps the time and draws the name.

## Compass (0x4dafe0)

- Nothing player-related is drawn for a spectator (team 3). With team 0 (DM) no friendlies are drawn.
- Friendlies: every player entity of the snapshot on the local client's team that is not dead (entity flag 0x20000)
  updates its slot (time, x, y, yaw); a slot updated in the last 800 ms is drawn except the local client: the arrow
  `objective_friendly` (or `objective_friendly_chat` blinking with a 500 ms period while the client talks), turned by
  its yaw, size `cg_hudCompassSize` × 10, placed as the SP friendly arrows (docs/spec/ui.md). The server also sends one
  teammate outside the snapshot per frame (player state +0x5b0, `iCompassFriendInfo`; TODO(spec): its selection).
- Sound pings (0x4daed0): when a player entity starts a sound with a compass ping (the weapon fire sound, 0x4d72a3;
  0x4e1300), for any client but the local one and not a spectator: the ping lasts the sound's length; the shooter is
  an enemy when the local team is 0 or differs from the shooter's team, and only an enemy ping stores the position.
  A friendly ping draws `compassping_friendlyfiring` over the arrow; an enemy ping draws `compassping_enemyfiring` at
  the stored position; both fade over `cg_hudCompassSoundPingFadeTime`. Ping drawing works also in DM (team 0).
- Objectives: as in SP (ownerdraw 86) for the objectives of the local client's player state.

Implemented: `World::compassFriendlies()` lists the local client's living teammates in MP, `compassPing` uses the MP
enemy rule for clients, and weapon fire of a non-local client pings the compass.

## Objectives (`objective_*` in MP)

- `objective_add(index, state [, origin [, icon]])` (0x811299c): at least 2 arguments, index 0..15, state `empty`,
  `invisible` or `current` (error texts at 0x815bb80, 0x815bc00, 0x815bc60). Unlike SP there is no text argument.
- `objective_onEntity(index, entity)` (0x8112e04): the objective's position follows the entity.
- `objective_team(index, team)` (0x8112f8a): `allies`, `axis` or `none`; a team objective is shown only to that
  team's clients. TODO(spec): where the filter is applied (server per client or client).
- `objective_position`, `objective_icon`, `objective_state`, `objective_delete` as in SP.
  TODO(spec): whether the MP compass pulses a new current objective (the SP ring start); the MP variants here do not stamp it.

The gametypes use them for flags and radios (`ctf.gsc:1088-1244`, `hq.gsc`), and `_objpoints.gsc` draws the same goals
as world HUD elements (`setWayPoint`).

Implemented: the MP variants above; `World::objectives()` positions follow their entity; `mpObjectiveVisible` filters
by team.

## Waypoint HUD elements

`hudelem setWayPoint(constantSize)` (0x81044c0) turns the element into type 13: the client skips it in the 2D pass
(0x4dddf0) and draws it as a world sprite (0x4ddee0 -> 0x4ddd30 -> 0x4ddcb0) at the element's (x, y, z) with its material,
radius = its width (with `scaleOverTime`, 0x4dce00), white with the element's alpha; with `constantSize` the radius × 0.0043
is a screen fraction. `_deathicons.gsc:56-68` (dead teammates) and `ctf.gsc:1365` use it.

Implemented: `setWayPoint` marks the element; iw2view projects it like the head icons.

## Hit direction and damage feedback

- `finishPlayerDamage` passes the point and direction on to the engine's player damage (0x80fb170 -> 0x8102886); the
  client shows the hit direction icons as in SP (docs/spec/ui.md, `hit_direction`, `cg_hudDamageIcon*`). Implemented:
  the local player's accumulated damage keeps the direction (as SP G_Damage), so the icons point at the attacker.
- The hit marker for the attacker is script: `_damagefeedback.gsc` shows `damage_feedback` on a client HUD element and
  plays `MP_hit_alert` with `playLocalSound`.

## Sounds from the gametype scripts

`playLocalSound(alias)` (0x80fd990) needs a client (`entity %i is not a player`) and sends only that client the sound
command (`s <index>`): announcer lines (`tdm.gsc:1150-1172`), the hit alert, the breathing of `_healthoverlay.gsc`.
`playSound` on an entity is positional for every client.

The command is reliable and has the letter `s` (0x73) with one number: the alias's sound index (the sound
configstring is 590 + index), registered on first use like the other script sounds. A sound registered after the
gamestate reaches the clients as a configstring change before the command, so the client can resolve the index (the
hit alert is usually first played during the game).

Implemented: `playLocalSound` on a client plays only when that client is the local one; on any other client the
network server sends that client `s <index>` and the client plays the alias without a position; `playSound` and weapon fire
sounds of other clients are positional (only the local client's own shots are played without position).

## HUD fade dvars

The MP client registers its HUD fade dvars at 0x4c3430 with other defaults than single player (range 0..30 s each):
`hud_fade_ammodisplay` 0, `hud_fade_healthbar` 2, `hud_fade_compass` 0, `hud_fade_stance` 1.7, `hud_fade_offhand` 0
(`hud_fadeout_speed` 0.1, range 0..1, and the bool `hud_enable` 1 between them). The fade helper (0x4c3800) returns 1
when the dvar is 0, so with the defaults the ammo counter, weapon name, firemode icon (all read `hud_fade_ammodisplay`,
0x4c3a10 / 0x4c3ae0), compass and offhand icons never fade in MP: they are visible right after a spawn, before any shot or
weapon change. The helper returns 0 when `hud_enable` is 0, when the word 0x1519e48 is 0, or when bit 8 of 0x96b654 is
set with the byte 0x96b658 clear (TODO(spec): what those two flags are), and otherwise fades over the last 700 ms of the dvar's time from the touch. The health bar (2 s) and stance (1.7 s) keep
their timed fades as in single player.

## Game messages in the log

`iprintln` / `iprintlnbold` send the message as a localized reference with its arguments (docs/spec/net.md section 10).
The client localizes it (cgame 0x496e80, src/game/textmessage.h; "&&1" takes the first argument) before it shows it;
iw2view writes that localized text to stdout (`[gm]` / `[gmb]`), never the raw `&"MP_CONNECTED"` reference.
An entity argument must be a player (else the script error "Entity is not a player") and is sent as its name followed by
`^7` (message builder at VAs in the 1.3 Linux dedicated server 0x8110720, format at 0x8110844); `dm.gsc` passes the
connecting player to `MP_CONNECTED` this way. Before, openiw2 sent the entity's debug text instead of the name.
TODO(spec): whether the original also echoes these messages to its console.

## Killcam

### The script side
- `_killcam.gsc:8-15`: `scr_killcam` (default "1") is stored in `level.killcam`; when it is above 0 the script calls
  `setarchive(true)`. `:24-35` re-reads it every second; on a change the archive is switched on when the killcam or
  `g_antilag` is above 0, else off.
- `dm.gsc:328-345`: only a kill by another player asks for a killcam (`attackerNum` = the killer's entity number); a
  suicide or a world kill does not. `dm.gsc:379-385`: after the 2 s death delay the victim runs the killcam with
  `delay` 2, the `psOffsetTime` argument of the killed callback and the respawn flag, then waits to respawn.
- `_killcam.gsc:45-48`: the victim becomes a spectator of the killer: `sessionstate` "spectator", `spectatorclient`
  the killer, `archivetime` = delay + 7 (9 s), `psoffsettime` the callback's offset. `:51-54` allows every spectate
  team. `:57` waits one server frame so that the code can shorten `archivetime`; `:59-67` when the result is not above
  `delay` the killcam is abandoned (dead, no spectator client, both times 0).
- `:69-159` the HUD: two half-transparent black bars (640 x 112, at y 0 and 368, fullscreen alignment, not in split
  screen), the title `MP_KILLCAM` (font scale 3.5, y 30, centred on the safe area, top), the skip text (respawn or skip
  variant, scale 2, y 70) and a tenths timer (scale 3.5, 32 units above the bottom) counting down
  `archivetime - delay`. All of them are created with `archived` false.
- `:161-164` / `:178-199`: the killcam ends after `archivetime - 0.05` s, on a new press of the use button (a press
  that is already held at the start does not count), or when the player spawns. `:166-175` removes the HUD, sets
  "dead", `spectatorclient` -1, both times 0, and restores the gametype's spectate permissions.

### The fields
The client field table (lnxded 0x8157d94..) gives `spectatorclient` as an int at gclient +0x26ac, `archivetime` as a
float field stored as integer milliseconds at +0x26b4 (setter 0x80f8420: seconds times 1000, truncated toward zero;
getter 0x80f8460: milliseconds times 0.001), `psoffsettime` as an int at +0x27a4 (setter 0x80f8482). The
`psoffsettime` getter (0x80f84a4) reads +0x26b4, so reading `self.psoffsettime` in a script gives `archivetime` in
milliseconds; no shipped script reads it. `killcamentity` does not exist in 1.3: the camera always follows the
killer's own player state (a grenade or artillery kill shows the thrower's view).

### The archive
- `setarchive(enable)` (builtin 0x81178ea -> 0x8093312) stores the flag and, the first time it is on, allocates the
  archive buffers; `setarchive(false)` keeps them. Without the flag nothing is archived and every archive lookup fails.
- Every server frame the server archives the frame (0x809b016, only while the server is running and the flag is on):
  the server time, every client's player state and every entity's state. Frames are numbered; the per-frame client
  index is a ring of 1200 entries (0x80990a5, 1200 x 8 bytes allocated at 0x8093358), and a frame whose data was
  overwritten in the 32 MB data ring is no longer valid (0x80990d4).
- Lookup (0x8099c04) for a time T ms back: with `sv_fps` (int dvar 0x8093ed7, default 20, 10..1000) the start frame
  is `current - T * sv_fps / 1000`. A start older than 1200 frames is clamped and T is rewritten to
  `(current - start) * 1000 / sv_fps`; a negative start becomes 0 and T is rewritten likewise. From the start frame it
  takes the first valid frame up to the current one; when none is valid T becomes 0 and the lookup fails.
- Snapshot of a client (0x809a408): its `archivetime` (0x8108e36) goes through the lookup, the trimmed value is
  written back (0x8108e52), and the entities of the snapshot come from the archived frame, with the snapshot time
  offset by `server time - archived frame time`. The client therefore receives old snapshots and draws them as live:
  the replay happens on the server, the client needs no replay code.
- The spectator's player state (0x80f69c4..0x80f6ad7, run every frame): a negative `archivetime` becomes 0. Then,
  while the followed client has no archived player state at `archivetime - psoffsettime` (0x809a0b6; a time of 0 or
  less takes the live state, 0x8099d30, of an active client) or the spectate permissions forbid its team (0x80f6dd0,
  bit per team at +0x2740): with `archivetime` 0 following stops (`spectatorclient` -1), else `archivetime` goes
  down by 50 ms and the lookup is repeated. The found player state is copied as the spectator's view: first person,
  the killer's position, view angles and weapon.

### What the killcam client draws
- The copy (0x80f6916, after the lookup above) takes the followed client's whole archived player state (0x26a8 bytes)
  over the spectator's; of the movement flags only the spectator's bit 0x100000 is kept, and the entity flags get
  0x400000 and lose 0x800000. Then 0x8104882 with flag 2 rebuilds only the current HUD-element part of the player state
  (+0x7a8, 31 slots of 0x80 bytes) from the spectator's own elements. 0x8104882 puts an element (0x8628f80, 1024 slots
  of 0x8c bytes) into a client's player state when it is in use, its team (+0x84) is 0 or the client's team (gclient
  +0x274c) and its client (+0x80) is 0x3ff (everyone) or the client; with `archived` (+0x88) set it goes into the
  archived part (+0x1728, flag 1), else into the current part (flag 2). So the killcam shows the killer's archived
  elements (the default for `archived`), and the victim's own non-archived ones (the killcam bars, title and timer).
- The client draws the copied player state as its own: the ammo counter, weapon name, firemode icon, health bar,
  compass and view weapon are the killer's.
- The snapshot entities come from the archived frame, so their events (the killer's fire event, the impact and tracer
  events of its bullets) reach the client again: it hears the shots and sees the view weapon fire, the muzzle flash
  and the impacts of the replayed frames.

### The dead view before the killcam
Player death on the server (0x810175a) calls LookAtKiller (0x810160a) after the killed callback: the player state's
dead yaw (an int) is the yaw (vectoyaw 0x80a4d5a: 0 for a zero vector, otherwise atan2 in degrees in [0, 360)) from the
victim's current origin to the attacker's, else to the inflictor's (each only when it is not the victim), else the
victim's own yaw; the float is truncated toward zero. The order differs from the SP one (inflictor first). The client
draws the dead view with that yaw (pitch -15, roll 40, as in SP). Implemented in `World::mpDeadYaw`.

### Antilag
VAs in the 1.3 Linux dedicated server.
- `g_antilag` is a bool dvar, default on (registered at 0x8108b76, flags 0x1005). It shares the archive:
  `_killcam.gsc:30` keeps the archive switched on for antilag even with the killcam off. Without the archive every
  lookup below fails and nothing is rewound.
- The time: when an active client's user command is run (0x8090de9..0x8090e10) the server keeps the command's
  `serverTime` as the client's antilag time (0x80f5614, gclient +0x289c): a time more than 1000 ms before the level time
  becomes level time - 1000; the stored value is replaced unless the new one is both older than it and older than the
  level time (so it only goes back while it is still ahead of the level time).
- The fire: the weapon fire event (0x80f546e) fires with the stored antilag time while `g_antilag` is on, else with the
  level time. The bullet fire (0x8120484) rewinds before its traces (0x811fc76) and restores after them (0x811fdec);
  both the single bullet and the spread (shotgun) pellets are inside. Projectiles, grenades and melee are not rewound.
- The rewind (0x811fc76): only with `g_antilag` on and the time more than one server frame (1000 / `sv_fps` ms) before
  the level time. For every client slot that is connected and in session state playing and has an archived origin at
  the time, the live origin is saved, the entity unlinked, its origin set to the archived one and linked again (so its
  hit boxes are there for the traces). Only the origin moves, not the angles or the animation. Every playing client is
  moved, the shooter too (its own traces skip it anyway). The restore puts the saved origins back the same way.
- The archived origin (0x8099dac): with the frame length f = 1000 / `sv_fps` and the server time rounded down to a
  frame, `back` = (rounded time - time) / f frames. The older sample is looked up `(back + 2) * f` ms back, the newer
  one `(back + 1) * f` ms back (the constants 2 and 1 at 0x817db80 / 0x817db84) through the archived player state
  lookup (0x809a0b6); a missing older one is searched further back, a missing newer one further forward, 10 tries each,
  one frame per try. The result is `(1 - k) * older + k * newer` with k = (time mod f) / (frame span between the two
  samples) (0x809bf50); only the older one: the older one; only the newer one: the newer one; none: no rewind. With
  these offsets the two samples are the frames on both sides of a time between frames; for a time exactly on a frame k
  is 0 and the frame before it is taken.

Implemented: `World::mpAntilagRewind` / `mpAntilagRestore` / `mpArchivedClientOrigin` (src/game/mpkillcam.cpp; the
archive keeps each client's origin), called around the bullet traces of `World::clientFire` when a network client fires
(src/netgame/worldgame.cpp keeps the antilag time). The port's archive stores each frame under its own server time, so
the lookups are the original's. The bots and the host fire with the level time (nothing to rewind). The openiw2 client
sends the time it draws the other players at as the command's server time (docs/spec/net.md section 10).
`TODO(spec)`: the 1.3 client's command time (the original client side is not analysed here).

### Implemented (src/game/mpkillcam.cpp)
The archive keeps the last 1200 server frames (60 s at 50 ms frames): per entity with a model its origin, angles,
model, attachments, weighted animation nodes and hidden flag, per client its session state, eye position, view angles,
weapon and hands model. `setarchive`, the millisecond rounding of `archivetime`, the lookup and trimming rule and the
spectator's 50 ms step-down run at the end of every server frame, so the killcam's one-frame wait sees the trimmed
time. The scripts store the killer's entity number in `spectatorclient`; client entities are not numbered 0..n-1 in
this port, so the followed client is the one whose entity has that number. Not modelled: the spectate permission test
(the killcam allows every team), the `psoffsettime` getter quirk, and `psOffsetTime` of the killed callback (0 offline).

The front end, while the local client is a spectator with a `spectatorclient` and a positive `archivetime`, draws the
archived frame at `now - archivetime` (interpolated between the two frames around it, as live frames are: origins and
angles between the frames, the bodies' animation advanced from the frame before by the fraction, the idle view
animation with the drawn time): the camera at the killer's archived eye and view angles, the killer's weapon view model
in its idle animation, every other entity at its archived state, entities missing from that frame hidden, and the local
client's own body in third person (the local client's body is therefore animated like the other clients, with its own
shots and aim). A bot's archived view is its controller's view angles (bots have no user command: docs/spec/mp-bots.md
keeps the view on the shot and turns it at the skill's rate on ladders), its eye at the archived origin plus its
movement view height; it is archived at the end of the same server frame as every client (after its move of that
frame). The script HUD of the killcam is drawn as usual;
the alignment `center_safearea` is taken as the centre. The HUD reads the followed client's archived state: its
weapon, clip and stock (ammo counter, weapon name, firemode icon) and its health; script elements with `archived` true
(the default) are shown by the followed client's ownership and team, those with `archived` false by the local
client's. Every archived frame also keeps the client events raised during it (the shots' fire sounds with the
shooter, the impact effects and sounds, the tracers); while the local client is in the killcam, the live ones are not
shown and the events of each newly shown archived frame are replayed (the followed client's own shots as its sound
without a position). The MP client (VAs in the patch 1.3 MP executable) runs no player-state
prediction while the player state's movement flag 0x400000 is set (the test at 0x4deb14 branches past the predictor),
so the killcam view is the received player state itself; this port draws a shot of frame n from the start of the
interval the view draws toward frame n (TODO(spec): when the original client plays the archived snapshot's events
relative to its interpolation): its sound, impacts and tracers are replayed then, and the view model plays its
fire animation (lastShotAnim for the shot that emptied the clip) from the same moment and its viewFlashEffect bolted to
the gun's tag_flash once per shot, as the live view weapon does. The view weapon's state (its animation, the aim
fraction and its zoom, reloads, the bolt cycle, raises and drops, grenade throws, the bob and the recoil) comes from
the recorded view state below. TODO(spec): HUD elements as they were at the archived time (the live elements are
used), the killer's team at that time, the shared-ammo group sum for the killer's stock, the HUD fade touches during
the killcam, the followed client's world-model muzzle flash, and the safe-area margins.

Missiles, explosions and shell shock in the killcam. The archive keeps whole entity states (0x809b016), so a missile
of an archived frame is in the replay with its trajectory even when it was freed since (a grenade that exploded), and
the explosion event of its frame is sent again. openiw2 keeps per archived missile (grenade, smoke grenade, rocket) its
position and angle trajectories, and per frame the explosions raised during it (weapon, point, direction, surface).
The local killcam draws a freed entity of the shown frame from its archived state (its removed flag is cleared only
for the drawing) and replays each newly shown frame's explosions (the surface sound, the impact effect, the weapon's
explosion effect and sound) without logging or archiving them again; live explosions are not shown while the local
client is in the killcam. The player-state copy of 0x80f6916 takes the followed client's shell-shock fields too
(ps+0x5d8 index, +0x5dc start, +0x5e0 duration): the killcam shows the killer's archived shock, not the victim's own
(the blur, the sounds and the mouse and movement effects all read it). Out of the killcam the victim's own shock is
shown again until a spawn clears it (docs/spec/mp.md "Client spawn"). The archive keeps each client's shock fields.

### Network clients (src/netgame/worldgame.cpp, src/netgame/remoteview.cpp)
As in the original the replay is built by the server: while a network client is a spectator following a client with
a positive `archivetime`, its snapshot's player state is the followed client's archived one at `now - archivetime`
(movement type 0, pm_flags only the follow bit 0x100000, eFlags 0x400000; origin, view height, view angles, health,
the current weapon with its clip and stock, the hands model), the other players are sent at their archived origin
and view angles, the followed client is not sent as an entity, and the archived HUD elements are chosen by the
followed client's ownership and team. The archived entity list skips only the entity of the sent player state's client
number (VAs in the 1.3 Linux dedicated server: 0x8098f34 compares each archived entity with it), which in the killcam
is the followed client: the spectator's own player is sent like the others, at its archived place while it was still
playing, and the client draws it as a puppet of its own (its live view slot stays the camera). The followed client's
archived shots travel in the player state's event ring (`eventSequence` ps+164, `events[4]` ps+168, `eventParms[4]`
ps+184, the field offsets of the 1.3 server's player-state field table); openiw2 sets the sequence to the archived
shot count and fills the newest four with fire (0x9e) or fire last shot (0xa0) and the weapon byte. A following
client plays each new one once as its view's shot: the fire sound without a position, the impact and tracer of a
trace from the sent eye along the sent view, and the view model's fire animation (lastShotAnim for the last shot) and
view flash from that moment, as in the local killcam. The client sees the follow bit and runs no prediction: the camera is the sent
eye and view angles, interpolated between snapshots. The snapshot time stays the live server time.
The killcam snapshot sends the missiles of the archived frame instead of the live ones (also those freed since), as
general entities with their archived trajectories; their times (and the shell shock's start in the player state) are
moved by the replay's delay (live server time - archived frame time), as the snapshot time stays live, so the
client's trajectory evaluation at its drawn time gives the archived flight. Each explosion of a frame newly shown to
the spectator becomes a new explosion event entity of that spectator's snapshots (kEventLifeMs, like the live ones);
the live explosion events are not sent to it. TODO(spec): the original's snapshot time offset and archived entity
numbers (openiw2 gives the archived missiles wire numbers of its own). Dropped weapons are sent as they are in the shown frame, and moved models at their position of that frame (the archive keeps whole
entity states). Not done: script models spawned after the frame still appear.

Bodies in the killcam. The archive keeps whole entity states (0x809b016), and a body of the body queue is an entity
(64 + slot), so the replay shows the bodies of the shown frame, not the live ones: the victim is its puppet up to the
frame of its death (the client entity leaves the snapshot in the frame `cloneplayer` runs, `dm.gsc`, same frame as the
kill), its body from that frame on, and a body that the queue of 8 reused since is still shown (with many players a
slot turns over within the 9 s of a replay; the live list then no longer has the victim's body). openiw2 archives each
body's slot, client, death animation, reuse bit and birth time and sends them with the live body's wire form in the
frame's time. The body's model is the one of its client's live client state. A client starts the death animation when
it first sees a body with eFlags 0x80000 and shows the last frame otherwise, so a body older than the replay's first
frame does not die again; openiw2 sets the bit for the first 500 ms of a body (TODO(spec): the bit's real meaning
and when the 1.3 server clears it, and how the 1.3 client poses a body it first sees late). The local killcam already
draws bodies from the archive, also those freed since.

The client draws about two snapshots behind the newest: when the newest no longer has the follow bit, the drawn time
is still in the replay. The camera, the view weapon and the puppets then stay on the older, followed snapshot until
the drawn pair has no follow bit. A followed client that was dead in the shown frame (the kill came soon after its
spawn) has no weapon in that frame: nothing is drawn in its hands.
TODO(spec): the 1.3 snapshot time offset, the archived script entities and their entity events (the impacts are
traced again by the client, not sent), the followed client's other weapons, and a differential check of the wire fields against the 1.3 server.

### The recorded view state (openiw2)
In the original the killcam client runs its ordinary first-person code on the copied player state: the weapon state
and its timers, the aim fraction, the bob cycle, the velocity and the view heights are all in the copy, and the fire
events reach the client in the copied event ring, so the view weapon animates, kicks and bobs, and the scope overlay
appears, exactly as for the client's own play. openiw2 therefore records one view state per client and frame
(`MpViewState`, src/game/mpkillcam.h), filled by whoever moves the client, bots (the bot's weapon state machine and its
movement state), network clients (the server's copy of their weapon state machine, src/netgame/netcombat.cpp, and
their movement state) and the host's own front end (its weapon and movement state, passed in with its command), and
both killcam paths draw from that record alone: the local killcam reads it from the archived frame, the network
killcam from the followed player state the server wrote from the same archived frame. The record holds the player
state's weapon and the weapon whose definition the view model uses (the offhand grenade during a throw), the weapon
state number, the view animation as its weapon-file key (`idleAnim`, `fireAnim`, `reloadEmptyAnim`, `holdFireAnim`,
...) with its elapsed time, the aim fraction and its direction, the bob cycle, the velocity, the lean and the two
view heights, plus the client's event ring (sequence and the newest four events with their weapon parameters: fire,
last shot, reload, melee swipe, offhand prepare and throw), and the client's view angles and eye of every client
kind (a network client's from its newest command).

Where it goes on the wire (the followed player state of a killcam snapshot): the followed client's number in
`clientNum` (+204, as the copy of the whole player state gives it), its stance and aim bits in `pm_flags` (+12,
with the spectator's follow bit kept), `bobCycle` (+8), `velocity` (+32), `leanFraction` (+76), `weaponstate` (+216),
`weaponPositionFraction` (+220), `targetViewHeight` (+244), `currentViewHeight` (+248), the event ring (+164..+196),
and two fields with an openiw2 meaning: `weaponAnim` (+1488) carries the index of the view animation's weapon-file key
in the table of src/game/mpkillcam.h, `aimDownSightDelay` (+224, 32 bits) the animation's elapsed time in ms (the
original's cgame derives the animation from state changes and events and its 16-bit weaponTime counts the state down;
TODO(spec): the 1.3 weapon animation numbering), and `offhandWeapon` (+208) names the view weapon only while the state
is an offhand one (12..16). The puppets of a killcam snapshot (the other players at their archived places) carry their archived stance
bits, weapon, lean and event ring instead of the live ones.

The client side is one code path: iw2view poses the killer's view weapon with the same function as its own (hands and
gun models, the action and the two aim animations with the aim cross-fade, the movement and sway offsets, the gun
bob and the recoil kick, the view flash and the brass), from a view-weapon source filled by the record; the camera
gets the recoil's view kick, the view bob and the aim zoom from the recorded aim fraction, and the scope overlay and
the crosshair read the record's weapon and aim fraction (so a scoped killer's aim shows the overlay, not the hip
crosshair). The recoil is the client's own, driven by the recorded shots, as in the original.

Divergences found and removed (the network path before this record): no aim fraction at all (a scoped kill replayed
from the hip, unzoomed), the receiver's own number left in `clientNum`, a network killer archived with pitch 0 and a
standing eye, the puppets with their live stance and live shots during the replay, and a bot's grenade throw outside
its weapon state machine (no pull-back and throw animation, locally either). Bots now throw through the offhand
states of the shared weapon state machine (the solved arc is kept until the throw event).
