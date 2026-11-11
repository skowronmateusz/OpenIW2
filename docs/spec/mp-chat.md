# Multiplayer chat and quick messages

Evidence: VAs in the patch 1.3 MP executable (it contains both the client and the game module), the shipped
`default_mp.cfg`, `ui_mp/ingame.txt`, `ui_mp/wm_quickmessage.menu`, `ui_mp/scriptmenus/quick*.menu`,
`maps/mp/gametypes/_quickmessages.gsc` and `maps/mp/gametypes/_menus.gsc`. Implementation: `src/game/mpchat.{h,cpp}`
(server side and the client chat buffer), `apps/iw2view/main.cpp` (input field, drawing), `src/ui/frontend.cpp` (execKey).

## Key bindings
`default_mp.cfg:54-56` binds t to `chatmodepublic`, y to `chatmodeteam` and v to `mp_QuickMessage`.

## Message field (client)
- `chatmodepublic` (0x4039d0) and `chatmodeteam` (0x403a20) clear the 256-byte message field, remember whether it is
  a team message (0 / 1) and toggle the message key catcher (bit 0x10). The two set different field draw widths
  (0x24c / 0x21f; TODO(spec): their exact use, the port draws the field after the label).
- Characters (field handler 0x40a1b0, char branch at 0x40a25e): any character code from 0x20 up is accepted (signed
  compare, the code comes from the Win32 char message as one ANSI code-page byte) and stored as one byte, up to 255
  bytes; there is no 0x7e limit. The localized strings (.str files) and the fonts use the same single-byte code page:
  Windows-1250 for Polish (e.g. a with ogonek 0xb9, l with stroke 0xb3, s acute 0x9c, z dot 0xbf, z acute 0x9f, o acute
  0xf3, c acute 0xe6, e ogonek 0xea, n acute 0xf1; capitals 0xa5 0xa3 0x8c 0xaf 0x8f 0xd3 0xc6 0xca 0xd1), and the
  glyph tables of `fonts/polish/*` are indexed by those bytes. The text travels as raw bytes: the network string
  writers only map 0x92 to an apostrophe, the server's say function does not clean the text, the client only removes
  0x19. The "outside 0x20..0x7e is dropped" filter (0x44a8d0, signed byte compares) is applied to the speaker's NAME
  in the chat line and the log (0x502883), never to the text, so Polish letters in a name vanish from chat lines.
  openiw2 converts the keyboard's UTF-8 text to the language's code page (`src/util/codepage.h`: 1250 Polish, Czech and
  Hungarian, 1251 Russian, 1252 otherwise; characters the page lacks are dropped) in the message field and in the menu
  text fields (name, address, password; a file-name field keeps its letters-and-digits filter).
- Field keys (0x40ade0): Escape closes the field. Enter (and keypad Enter) sends, when the field is not empty and the
  client is active (state 8), the console text `say "\x15<text>"` or `say_team "\x15<text>"` (formats 0x5a240c /
  0x5a2418); the 0x15 marker keeps the typed text from being localized. Both keys then clear the field and the key
  catcher. Every other key edits the field.
- Drawing (0x406d2d): the label is the localized `EXE_SAY` or `EXE_SAYTEAM` (0x5a1cf4 / 0x5a1d04) formatted `%s: `,
  then the field, at the vec2 dvar `cg_hudSayPosition` (registered at 0x4be4fc: default 5 126, range 0..640), text
  scale 0.2083 (5/24) with the usual small-font choice. TODO(spec): colours and the cursor.
- Field position (verified): the dvar registered at 0x4be4d4 (`cg_hudChatPosition`) is the one the chat lines read
  (0x4c7760 reads the pointer stored at 0x14c362c) and `cg_hudSayPosition` (0x4be4fc) the one the field reads (pointer
  0x14b593c). The field caller (0x4caf70) passes the rounded say y + 24 to 0x406d00, which adds the font height times
  the text scale (about 10 at 0.2083) to get the baseline: 126 + 24 + 10 = 160 with the defaults, below the history, whose
  newest line ends at 150. The history is therefore never drawn over the field. The field is not an item of the history.
- While the field is open the history is drawn exactly as before: 0x4c7760 does not look at the field state, so
  `cg_chatTime` expiry and the fade go on and `cg_chatHeight` still limits the lines (callers 0x4cbd3b / 0x4cbd70,
  in the HUD draw for the playing states). TODO(spec): the exact gating of those two calls per client state.

## Server: say / say_team (game module)
- ClientCommand (0x503ad0) handles `say` and `say_team` with at least one argument: the arguments from 1 on are joined
  (0x501600) and passed to the say function (0x502820) with mode 0 (all) or 1 (team). No script callback or notify
  is involved; the only script entry points are the `sayAll` / `sayTeam` methods below.
- Say (0x502820), arguments speaker, target (none for both commands), mode, text:
  1. Team mode falls back to all when the speaker's team is neither 1 (axis) nor 2 (allies).
  2. The speaker's name (client name, at most 63 bytes) is cleaned (0x44a8d0): every `^` followed by a digit is
     dropped together with the digit, and characters outside 0x20..0x7e are dropped. Chat lines therefore never show
     the player's own name colours.
  3. Team colour: axis `^9`, allies `^8`, others none (0x502888). The marker before the name is
     `\x15(\x14GAME_SPECTATOR\x15)` for team 3 (spectator, no colour); for a speaker that is not playing
     (session state other than 0) `\x15<colour>(\x14GAME_DEAD\x15)`; otherwise `\x15<colour>`.
  4. The name part: all `<marker><name>^7: ` (format 0x5b89fc), team `<marker>(\x14GAME_AXIS|GAME_ALLIES\x15)<name>^7: `
     (0x5b89d4, team 1 axis); the private form `<marker>[<name>]^7: ` (0x5b89c8) exists for mode 2. Text colours: all
     `7`, team `5`, mode 2 `3`.
  5. Log lines (G_LogPrintf 0x4fcb50): `say;<guid>;<client>;<clean name>;<text>` for all, `sayteam;...` for team
     (0x5b8a08, 0x5b89e4). The port's log keeps no time prefix (as for `logPrint`); TODO(spec) the prefix of 0x4fcb50.
  6. The text is cut to 149 bytes (0x95). A dedicated server echoes `<name part><text>` to its console.
  7. With a target only that client is told, otherwise every client slot in order (0x502ab1 loop).
- Delivery to one client (0x502750): only in-use clients that are connected and active (state 2); in team mode only
  clients on the same team (0x520b10: both teams equal and non-zero). When `g_deadChat` is 0 (bool, default 0,
  registered at 0x4fbb77) a speaker that is not playing is not heard by clients that are playing. The client gets the
  server command `h "\x15<name part>^<colour><text>"` (team: `i`; format 0x5b8a4c).
- `sayAll(text)` / `sayTeam(text)` (methods 0x52dcc0 / 0x52dd90; self must be a client, else a script error) build the
  message from their arguments (0x504700, as `iprintln` does), prefix it with 0x14 (so the client localizes it) and call
  the say function with mode 0 / 1.

## Client: chat lines
- Server commands (switch 0x4d1bb8): `h` is shown unless the bool dvar `cg_teamChatsOnly` (default 0, registered at
  0x4beb28) is set; `i` is always shown. The text is localized (0x4ab700, the 0x14/0x15 segment rules of
  `textmessage.h`), cut to 149 bytes, 0x19 bytes are removed (0x4d1550), the line is added to the chat buffer
  (0x4d0e10) and printed to the console.
- Chat buffer (0x4d0e10): a ring of `cg_chatHeight` lines (int, default 8, range 0..8, registered at 0x4bea74) of 271
  bytes, each with the cg time it was added. With `cg_chatHeight` 0 or `cg_chatTime` <= 0 the buffer is emptied and
  nothing is added. A line holds 90 visible characters (`^<digit>` pairs do not count); a longer message is broken at
  the last space of the line, which is dropped (at 90 characters when the line has no space), and the next line starts
  with `^<last colour digit seen>` (initially `7`). When more than `cg_chatHeight` lines are pending, the oldest are dropped.
- Drawing (0x4c7760): `cg_chatTime` is an int in ms (default 12000, range 0..60000, registered at 0x4bea31). Each frame
  the oldest line older than `cg_chatTime` is dropped (one per frame). Lines are drawn from the newest up: line `i`
  (newest = 0) at y = `cg_hudChatPosition`.y − 10·(i+1) (vec2 dvar, default 5 150, range 0..640, registered at
  0x4be4d4), x = `cg_hudChatPosition`.x. Alpha is 1 while more than 200 ms of `cg_chatTime` remain, then
  remaining·0.005, a line at alpha 0 is skipped. Behind each line a `hudcolorbar` strip 10 high and (text width + 24)
  wide in the colour of the line's first `^N` code (white without one) with rgb·0.25 and alpha·0.6; the text at
  baseline +9 with scale 0.2083, style 3 (shadowed).
- Colour codes: `^0`..`^7` are the fixed table; `^8` is `g_TeamColor_Allies` and `^9` `g_TeamColor_Axis` (0x4032e0,
  set from the dvars at 0x4033c0); the dvars are colours registered by the game module (0x4fbd64), defaults allies
  0.5 0.5 1 1, axis 1 0.5 0.5 1.

## Player names
Network name (openiw2 choice, TODO(spec): where the original takes the default name): the userinfo name is the `name` dvar
of the menu / profile (`seta name` in the profile's config), else the active profile's own name (`default` gives
`player`), read from the profile files so a `+connect` start, which joins before the menus load, uses it too.
ClientUserinfoChanged (0x4fe1ea) cleans the userinfo `name` into a 32-byte buffer (0x4fe010): leading spaces are
dropped, `^0` is dropped, other `^x` pairs are kept while 2 more bytes fit, more than 3 spaces in a row are dropped,
other characters are kept while 1 more byte fits (at most 31 bytes). A name with nothing left, or with no character
that is not a colour code, becomes `UnnamedPlayer` (0x5b8544).

## Quick messages
- `mp_QuickMessage` (cgame command table 0x59e060, handler 0x4c1680) opens the UI menu `quickmessage`
  (`UIMENU_WM_QUICKMESSAGE`, `ui_mp/wm_quickmessage.menu`, loaded by `ui_mp/ingame.txt`) only when there is a snapshot
  whose player state has pm_flags bit 0x800000. The game module sets that bit in the end-of-frame client update
  (0x501068..0x501098) for a client in session state playing or dead that is not following another client.
  TODO(spec): the other writers of the bit and how the UI call (0x4024a0) treats menus already open. The port opens
  the menu while the local player is playing or dead and no other menu is open.
- `wm_quickmessage.menu:124-154`: keys 1, 2, 3 close it and open `quickcommands`, `quickstatements`,
  `quickresponses`; the script menus (`quickcommands.menu:124-236` etc.) answer the digit keys with
  `scriptMenuResponse "<digit>"` and close themselves. All four menus set `cl_bypassMouseInput` 1 while open: they are keyboard-only. Port rule (`menuBypassesGameInput`,
src/ui/menuinput.h): while the top menu is not full-screen and the cvar is 1, no cursor is shown, the mouse keeps turning the view,
movement and buttons keep working, and the menu takes only its execKey keys and Escape.
- execKey (menu parse 0x544150: menu list +0x250 of {key, script}; the key is the first byte of the string): the menu
  key handler (0x53e900) runs the script of a matching entry before anything else. 0x5345b0 decides whether the
  menu takes a key: always while the UI flag 0x19a1c94 is set, otherwise only keys up to 0x100 with an execKey
  entry (the team, weapon, ingame and callvote menus use execKey too). TODO(spec): the flag 0x19a1c94. The port lets the keys of a non-fullscreen menu without a matching
  execKey reach the game bindings.
- The responses reach the gametype as `menuresponse` (`_menus.gsc:179-186`) and go to `_quickmessages.gsc`:
  the message is ignored for a client without a team, a spectator, or one that sent a quick message less than 2 s
  ago (`self.spamdelay`, `_quickmessages.gsc:15-18`, `:251-254`). The response selects a team- and nation-specific
  sound alias and a `QUICKMESSAGE_*` string; `doQuickMessage` (`_quickmessages.gsc:724-750`) does nothing unless the
  player is playing, sets the head icon `talkingicon` (team-only unless `level.QuickMessageToAll`), plays the alias
  on the player and calls `sayTeam` (or `sayAll`), then `pingPlayer`.
- TODO(spec): `pingPlayer` (method 0x52add0, compass ping of the player for its team); a no-op here.

## Bots
openiw2's bots (docs/spec/mp-bots.md) neither chat nor react to chat or quick messages. A possible extension (not
from the original): a bot that hears a team quick command such as "follow me" or "regroup" could move toward the
speaker for a while.
