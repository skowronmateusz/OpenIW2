# Multiplayer TODO(spec) index

Every `TODO(spec)` marker in the multiplayer specs and code marks a behaviour that is **not** proven from the
original (a VA not read, a default not checked, an exact text or coding unknown); the port's own choice is stated
at the marker. This file lists all of them that were still open on 2026-11-05, grouped by area, so a spec pass can
pick a group and close items one by one. Each entry is one sentence with its source `file:line` (several files
state the same open point; all of them are cited). When a point is resolved, remove the entry here and the marker
at the source. The convention itself (docs/spec/net.md section 1) is not an entry.

## Network: transport, connection and wire protocol

- Whether the 1.3 client uses the server-side routine that has no direct callers in the server (`docs/spec/net.md:89`).
- The download server message (type 5) (`docs/spec/net.md:166`).
- The client side of the `rcon` command (E2+) and the `v` command (`docs/spec/net.md:239`, `docs/spec/net.md:240`).
- The names of the two byte fields of the user command (inferred) (`docs/spec/net.md:281`).
- The names of the two address fields (inferred; the wire behaviour does not depend on them) (`docs/spec/net.md:356`).
- The exact use of the entity state's head-icon fields (`docs/spec/net.md:364`).
- The exact semantics of the client state's bit words and hint fields (`docs/spec/net.md:379`).
- What the table reader does with elements at or after the count (`docs/spec/net.md:408`).
- The order of the connection-refused texts in the 1.3 server (`docs/spec/net.md:498`).
- Which print letter is the console line, the HUD line and the bold centre line, and the close-menu letter
  (`docs/spec/net.md:513`, `src/game/mpgame.cpp:485`, `src/netgame/worldgame.cpp:1472`).
- The exact chat command names on the wire (`docs/spec/net.md:521`).
- Which dvar the client's time-scale value is (`docs/spec/net.md:539`).
- The MP cgame's entity placement without a next snapshot (a trajectory evaluation stays at its base)
  (`docs/spec/net.md:543`).
- The 1.3 client's state machine during a map load (the exe's client states 5..7) (`docs/spec/net.md:597`).
- What the table of 256 configstrings at 910 holds (`docs/spec/net.md:622`).
- What the table of 32 configstrings at 110 holds (`docs/spec/net.md:626`).
- Configstring 22's winner values later in a game (`docs/spec/net.md:652`, `src/netgame/worldgame.cpp:526`).
- What holds the freed entity numbers the 1.3 server reuses (`docs/spec/net.md:671`).
- The server's free-slot delay before it reuses an entity number (wire entities and event entities)
  (`docs/spec/net.md:675`, `docs/spec/net.md:693`, `src/netgame/worldgame.cpp:877`, `src/netgame/worldgame.cpp:881`).
- The 1.3 coding of the client state's team field (1 axis / 2 allies is this port's coding)
  (`docs/spec/net.md:681`, `src/netgame/worldgame.cpp:1111`).
- The linking rule that keeps an out-of-bounds brush entity out of snapshots (`docs/spec/net.md:685`,
  `src/netgame/worldgame.cpp:1167`).
- The 1.3 animation numbering of the corpse's legsAnim / torsoAnim and its toggle bit (`docs/spec/net.md:705`,
  `src/netgame/worldgame.cpp:1209`).
- The further check 0x80ed5fc behind the firing eFlags bit 0x40 (`docs/spec/net.md:710`, `src/netgame/worldgame.cpp:783`).
- `pm_flags` 0x80000, which appears some time after going prone (`docs/spec/net.md:714`, `src/netgame/worldgame.cpp:1031`).
- The ammo / clip slot assignment routine (near the "BG_FillInAmmoItems" error text) (`docs/spec/net.md:737`,
  `src/netgame/worldgame.cpp:858`).
- The give's clip fill (the 1.3 pistol has a loaded clip, openiw2 an empty one) (`docs/spec/net.md:738`).
- Confirm the +0x1728 placement of non-archived HUD elements with a 1.3 server (`docs/spec/net.md:742`).
- The 1.3 missile entity type (`docs/spec/net.md:750`).
- The VAs of the 1.3 trajectory evaluator and the MP cgame's use of it (`docs/spec/net.md:753`).
- The MP cgame's handling of a re-based trajectory at a bounce (the closest-point hold is openiw2's own rule)
  (`docs/spec/net.md:763`).
- A grenade's new spin at a bounce before its snapshot time (`src/netgame/remoteview.cpp:482`).
- The 1.3 server turns the missile entity itself into the explosion event and sends the direction as a byte in
  eventParm (`docs/spec/net.md:770`, `src/netgame/worldgame.cpp:914`).
- The 1.3 client routine that applies the synced dvars (`docs/spec/net.md:793`).
- The user command's weapon byte and the offhand byte against the 1.3 client (`docs/spec/net.md:827`).
- The original's entity event parameters for fire, melee, throw and suicide (`docs/spec/net.md:845`).
- The original sends the bullet impacts as their own events (0xb5..0xba) and the shooter's spread
  (`docs/spec/net.md:848`, `src/game/mpremote.cpp:124`).
- The 1.3 reason for a host disconnect (`docs/spec/net.md:880`).
- The original's text for a lost connection and whether it shows a dialog (`docs/spec/net.md:881`,
  `src/net/gameclient.cpp:41`).
- `clearError`'s exact effect (`docs/spec/net.md:882`).
- The source of `serverId` in the system info (`src/net/client.cpp:242`).
- The meaning of the snapshot flags byte (`src/net/gameclient.cpp:420`).
- The dead eFlags bit of a player entity (a dead player normally has no entity at all)
  (`src/netgame/worldgame.cpp:787`).
- The offhand field's wire name (inferred) (`src/net/delta.h:14`).
- Sign extension of negative-coded integer fields on read (the E2 player-state reader) (`src/net/delta.cpp:243`).
- The reader's handling of a field count above the table (`src/net/delta.cpp:301`).
- Whether the player-state reader clears elements at and after the count (`src/net/playerstate.cpp:122`).
- The player-state reader's behaviour for a count above the table size (105) (`src/net/playerstate.cpp:249`).
- The flip of the server-count bit and the other bits of that word (`src/net/server.h:88`).
- The game's rejection text for a join (error / EXE_CANNOTJOIN used) (`src/net/server.cpp:187`).
- The MP executable's check that ignores the view input at the intermission and the dead types (SP 0x50b12b)
  (`src/netgame/netmove.h:67`, `docs/spec/mp-spectate.md:126`).
- The count of the status-icon configstrings (base 22 observed) (`src/netgame/worldgame.h:54`).
- Whether the 1.3 map's own entities also use numbers 64..71 (72 is the first free here)
  (`src/netgame/worldgame.h:74`).
- The `pm_flags` stance bits' coding (1 prone, 2 crouch inferred) (`src/netgame/worldgame.h:92`).
- The 1.3 eFlags bits of another player's stance (`src/netgame/worldgame.h:95`).
- Whether the 1.3 server sends hidden script models (the 0x20 bit is openiw2's own) (`src/netgame/worldgame.h:99`).
- The original's overflow error when a configstring name table is full (`src/netgame/worldgame.cpp:94`).
- Which game global configstring 13 ("%i" at map start) prints (`src/netgame/worldgame.cpp:505`).
- The setter of the talking-icon configstring 31 (`src/netgame/worldgame.cpp:527`).
- The original's entity-number overflow error (`src/netgame/worldgame.cpp:762`).
- The 1.3 stat layout (health in stats[0] as in Quake III) (`src/netgame/worldgame.cpp:844`).
- Standing on another entity is not tracked in groundEntityNum (1022 / 1023 only) (`src/netgame/worldgame.cpp:1059`).
- The order of the `C` offhand-weapon command against the give's other commands (`src/netgame/worldgame.cpp:1545`).
- Whether the server reads the second token of `getchallenge` (the 1.3 client always sends `0`) (`docs/spec/net.md` section 5).
- Which dvar 0x84a8780 is in the LAN rate rule (`docs/spec/net.md` section 4a).

## Killcam and spectating

- The 1.3 client's command time for the antilag rewind (the original client side is not analysed)
  (`docs/spec/mp-hud.md:283`).
- When the original client plays an archived snapshot's events relative to its interpolation
  (`docs/spec/mp-hud.md:313`).
- The killcam HUD as it was at the archived time (live elements are used), the killer's team then, the shared-ammo
  stock, the HUD fade touches, the aim transition and zoom blend, the reload of a network or the local client, the
  rifle's bolt cycle, the world-model muzzle flash and the safe-area margins (`docs/spec/mp-hud.md:318`,
  `src/game/mpkillcam.cpp:217`, `src/netgame/worldgame.cpp:1443`).
- The original's snapshot time offset and archived entity numbers, and script models spawned after the shown frame (`docs/spec/mp-hud.md:357`, `src/netgame/worldgame.cpp:1321`).
- The archived script entities and their entity events, the victim's death animation in the replay, the followed
  client's other weapons, a network killer's archived view angles, and a differential check of the killcam wire
  fields against a 1.3 server (`docs/spec/mp-hud.md:360`).
- The followed client's other weapons and offhand in the killcam player state (`src/netgame/worldgame.cpp:978`).
- The other archived player-state events (reload, raise, melee) and the original's event parameters
  (`src/netgame/worldgame.cpp:988`).
- A network client's archived view angles and view height (its player state; 0 / the entity angles used)
  (`src/game/mpkillcam.cpp:203`).
- The archive's validity test for a follow target (dead clients, archive time; 0x809a0b6)
  (`docs/spec/mp-spectate.md:46`, `src/game/mpspectate.cpp:54`).
- The free look while following (pm_flags 0x400000 is not predicted here) (`src/netgame/netmove.cpp:57`).
- The exact vertical term of the spectator fly move (0x515f20..0x515f4b) (`docs/spec/mp-spectate.md:75`).
- Which three HUD draws pm_type 4 skips in the 2D pass (`docs/spec/mp-spectate.md:82`).
- The server side of the `follow` / `follownext` / `followprev` commands (`docs/spec/mp-spectate.md:85`).
- The colour of the following lines (0x59d038) and the second hide flag (0x151ad94) (`docs/spec/mp-spectate.md:87`).

## HUD and scoreboard

- The centre print's draw position, font and fade (`docs/spec/mp-hud.md:41`).
- The reader of the attacker name kept at 0x15195cc (`docs/spec/mp-hud.md:43`).
- The exact gaps of the obituary line around its icon (`docs/spec/mp-hud.md:47`).
- The alignment codes the game-message window draw call passes (0x406a20) (`docs/spec/mp-hud.md:59`).
- The body tag and its +21 branch of the head icons (`docs/spec/mp-hud.md:74`).
- The head tag path, the sprite's depth test and the voice / connection / killcam head icons
  (`docs/spec/mp-hud.md:82`, `src/game/mphud.cpp:148`).
- The crosshair names' health tint (0x1519e5c / 0x1519e60) and horizontal alignment (`docs/spec/mp-hud.md:93`).
- The selection of the one teammate outside the snapshot (iCompassFriendInfo, ps+0x5b0) (`docs/spec/mp-hud.md:105`).
- Where the team-objective filter is applied (server per client or client) (`docs/spec/mp-hud.md:122`).
- Whether the MP compass pulses a new current objective as the SP ring start does (`docs/spec/mp-hud.md:124`).
- What the two words 0x96b654 bit 8 / 0x96b658 of the HUD fade helper are (`docs/spec/mp-hud.md:166`).
- Whether the original echoes the connect / disconnect messages to its console (`docs/spec/mp-hud.md:177`).
- The 1.3 codes of the HUD alignment enumerations on the wire (`src/netgame/worldgame.cpp:46`).
- The 1.3 codes of the HUD element types on the wire (`src/netgame/worldgame.cpp:81`).
- The entity numbers the objective entries carry on the wire (`src/netgame/worldgame.cpp:1463`).
- Where the client game clears its scoreboard request time at a level start (`docs/spec/mp-scoreboard.md:16`).
- The scoreboard scroll keys in this port's key names (scrolling is not bound) (`docs/spec/mp-scoreboard.md:48`).
- The map display name lookup (0x532cd0; `mapname` shown) (`docs/spec/mp-scoreboard.md:66`).
- Voice chat icons (voice_on / voice_off; no voice chat in this port) (`docs/spec/mp-scoreboard.md:103`,
  `src/game/mpscoreboard.cpp:176`).
- The renderer's font height query for the header baselines, the font choice by scale and the backdrop material's
  colour (`docs/spec/mp-scoreboard.md:113`).
- The status-icon configstring range and the error past 8 (`src/game/mpspectate.cpp:209`).

## Players, movement, animation and items

- Which of the two model sites is the third-person player body, and the left-hand tag's users
  (`docs/spec/mp-players.md:31`).
- How the original weights the torso against the legs (the per-bone mask, if any) and the unused turning node
  (`docs/spec/mp-players.md:45`).
- The playerAnimType value with no weapon ("none" used) (`docs/spec/mp-players.md:86`).
- The `strafing` animtype value (computed in the script player 0x4f9190, not decoded) (`docs/spec/mp-players.md:90`).
- The script player call's first argument 3 (consistent with the COMBAT state) (`docs/spec/mp-players.md:98`).
- The turn anim types 14-17 (`docs/spec/mp-players.md:100`).
- The death event's duration returned by 0x80d92e8 (the animation length in ms used) (`docs/spec/mp-players.md:120`).
- The server site that marks a client entity for hit boxes (`docs/spec/mp-players.md:139`).
- The not-covered animation list: torso pitch to the view, leg yaw against the torso, the turning node, mantling,
  ladder climbing, MG42 mounting, flinches, stumbles, the transition events and the sounds of commands
  (`docs/spec/mp-players.md:142`).
- The blend time of a command without "blendtime" (0.2 s used) (`src/game/mpplayers.cpp:16`).
- The cgame's pellet impact effects are those of a single bullet (`docs/spec/mp-pmove.md:120`).
- The sessionteam string of value 0 (taken as "none" from the setter's order) (`docs/spec/mp-pmove.md:122`).
- What the client does with the predictable shell-shock event 0x12 (`docs/spec/mp.md:294`).
- The wire effect of viewKick, setBlur and the other view methods on a network client (no wire field yet) and the
  input queries of non-local clients (`docs/spec/mp.md:306`, `src/game/mpgame.cpp:599`).
- Compare the MP Pickup_Weapon (0x8105072) with the single-player one (`docs/spec/mp.md`).
- The use scan's look-at choice for remote clients and the pickup sound event of those clients (`docs/spec/mp.md`).
- The weapon states and pm_flags 4 (assumed the mantle) of network clients are not tracked, nor a grenade fuse
  (`docs/spec/mp.md:444`, `src/game/mpgametypes.cpp:247`, `src/netgame/worldgame.cpp:683`).
- The aim slow of another player (not in the snapshot) (`src/game/mpremote.cpp:95`).

## Bots

Bots are openiw2's own design; the points raised here are decisions, stated at their sites in `src/game/mpbots.cpp` and
`src/game/mpgame.cpp`.

## UI and menus

- The MP client's own `cl_ingame` rules (`docs/spec/mp-ui.md:24`).
- The sort order of the engine's gametype file list (`docs/spec/mp-ui.md:66`).
- The ownerdraw's paint handler of the gametype row (`docs/spec/mp-ui.md:96`).
- The original console window's size, font, colours and title, and whether it echoes a typed command
  (`docs/spec/mp-ui.md:141`).
- The LOADBAR fill (style 7), `com_expectedhunkusage`, the connecting texts and the font choice of 0x538a70
  (`docs/spec/mp-ui.md:172`).
- The menu argument of 0x4c77d0 / 0x4c78a0 behind show / hide / fade (`docs/spec/mp-ui.md:188`).
- 0x4cc170 adds the value's width to the centred width for some edit items (`docs/spec/mp-ui.md:194`).
- The original's YESNO read of a dvar that does not exist (`docs/spec/mp-ui.md:215`).
- The exact use of the two chat field draw widths (`docs/spec/mp-chat.md:14`).
- The chat field's colours and cursor (`docs/spec/mp-chat.md:21`).
- The time prefix of the log print 0x4fcb50 (`docs/spec/mp-chat.md:39`).
- The other writers of the message-key bit and how the UI call 0x4024a0 treats menus already open
  (`docs/spec/mp-chat.md:82`).
- The UI flag 0x19a1c94 of the quick-message menus (`docs/spec/mp-chat.md:90`).
- `pingPlayer` (0x52add0), the compass ping of a player for its team; a no-op here (`docs/spec/mp-chat.md:98`,
  `src/game/mpchat.cpp:207`).
- `closeInGameMenu`'s server command (`docs/spec/mp.md:107`, `src/game/mpgame.cpp:489`).
- `togglemenu`'s UI state calls (0x415830, 0x53adc0) and the branch taken while the UI is already active
  (`docs/spec/mp.md:135`).
- The error text of the name lookup for a menu that was not precached (`src/game/mpgame.cpp:469`).
- The team element's exact team test (the team string is mapped to a number at 0x8103d0a)
  (`docs/spec/mp.md:161`, `src/game/mpgame.cpp:514`).
- The 1.3 client's LAN port range and the platform icon of column 1 of the server list (`docs/spec/net.md`, `docs/spec/dedicated.md:124`).

## Server and game logic

- The exitLevel argument (keep / reset persistent data) and the intermission delay (`docs/spec/dedicated.md:70`).
- `fast_restart` keeps the loaded level in the original; openiw2 reloads in both the console and iw2view's map
  rotation (`docs/spec/dedicated.md:99` / `src/netgame/dedicated.cpp:99`, `docs/spec/mp.md:510`).
- How the original selects the serverinfo cvars (`src/net/svconsole.cpp:81`).
- The MP weapon loader path in the MP binary (`docs/spec/mp.md:21`).
- The mp_* spawn classes' entries in the MP spawn table (`docs/spec/mp.md:23`).
- Client N uses entity N in the original; this port keeps its player entity where the map spawn put it
  (`docs/spec/mp.md:55`).
- The fixed `menuresponse` menu argument at a disconnect (0x8157ec3; "" passed) (`docs/spec/mp.md:83`).
- The death event name's text (the constant 0x885322a; "death" used) (`docs/spec/mp.md:213`).
- The storage and range checks of the client entity fields (`docs/spec/mp.md:231`).
- The engine storage of `self.pers` (`docs/spec/mp.md:236`).
- Whether the original can connect two clients within one server frame (`docs/spec/mp.md:315`).
- The stubs: `setWaypoint` (HUD element), `precacheRumble`, `precacheMenu`, `precacheHeadIcon`, `precacheShader`
  (when absent), `setClientNameMode`, `makeCvarServerInfo`, `resettimeout`, `playrumble`, `stoprumble` and the rest
  of that list (`docs/spec/mp.md:329`).
- The ground entity `placespawnpoint` records (+0x7c) (`docs/spec/mp.md:394`, `src/game/mpgametypes.cpp:60`).
- The player state bit `disableWeapon` sets (a client flag the bot controller honours is used)
  (`docs/spec/mp.md:412`, `src/game/mpgametypes.cpp:112`).
- Where the `game` array is kept across a fast restart (`docs/spec/mp.md:466`).
- Further arguments after a plain first argument of a server command (joined with spaces here)
  (`src/game/mpgame.cpp:33`).
- The MP init time (the frame start +50 ms before init, as in runLevelMain) (`src/game/mpgame.cpp:82`).
- The corpse contents and the dead pm_type (`src/game/mpgame.cpp:287`).
- The snapshot / configstring side and the exact level time after a fast restart
  (`src/game/mpgametypes.cpp:357`).
- What routine 0x8060eb8 does for `killserver` (`docs/spec/dedicated.md` section 1).

## Turrets

- The AI-turret fields 0x4e7150(+0x294, +0x298), the bumped-sentient team branch 0x54aafa..0x54ab45, the
  actor-turret link itself, 0x54e3f0 / 0x503e10 with the start argument, and the debug string at actor +0x21a0
  (`docs/spec/turrets.md:148`).
- The other writers of ps+0x564 (1 getting on, 0 getting off; 2 is only read as "fired this think")
  (`docs/spec/turrets.md:167`).
- openiw2 does not register weapons, so the weapons' useHintString / dropHintString do not reserve hint
  configstring slots (`docs/spec/turrets.md:198`).
- The &&N parameter substitution of the hint reader 0x496e80 past 0x49704d, and where plain pieces are localized
  (`docs/spec/turrets.md:201`).
- HINT_INHERIT is not drawn here (only HINT_NONE occurs in the shipped scripts) (`docs/spec/turrets.md:204`).
- In a vehicle the scope overlay draws only for a value of 0xf7099c in 17..22 (`docs/spec/turrets.md:235`).
- The own-turret flag's effect inside 0x4d5c50, the original's bullet hit events, and the fire event's parm
  without a gunner (1022 sent) (`docs/spec/turrets.md:273`, `src/netgame/worldgame.cpp:1510`).
- Whether the client ammo ownerdraw hides itself on a turret (`docs/spec/turrets.md:276`).
- The client's own movement code for the 0x300 turret bits (the move is skipped) and the third-person animation of
  another client on a turret (`docs/spec/turrets.md:288`).
- The meaning of the turret entity's observed angles2[0] and apos.trType 3 (`docs/spec/net.md:686`).
