# Fonts, localized strings and HUD elements

## Fonts (`fonts/<name>`, `fonts/<language>/<name>`)

Observed layout of the shipped fonts:

| Offset | Field |
|---|---|
| 0 | uint32 offset of the font name (e.g. `fonts/normalFont`) |
| 4 | uint32 pixel height |
| 8 | uint32 glyph count |
| 12 | uint32 offset of the material name (e.g. `fonts/gamefonts`) |
| 16 | glyphs, 24 bytes each |

Glyph: `uint16 letter, int8 x0, int8 y0, uint8 dx, uint8 width, uint8 height, uint8 pad, float s0, t0, s1, t1`.
`y0` is the bitmap top relative to the baseline (negative is above), `dx` the advance. Letters are single-byte
code-page values. Languages with another code page have their own fonts: `fonts/polish/*` map the Windows-1250
bytes of the Polish strings (for example `ś` 0x9C), while `fonts/*` would show `œ` for the same byte.

## Menu text styles (`textstyle`)

The item's `textstyle` (itemDef +0x290, keyword parsed at exe 0x4d2370; values 0–6 from ui/menudefinition.h) is
passed unchanged as the last argument of the renderer's text command: the exe's text draw 0x4c3790 issues command
0x15 through refexport 0x1000c070 (style byte at command +0x24, colour at +0x20). The 1.3 renderer (0x100329a0)
tests only style 3 `ITEM_TEXTSTYLE_SHADOWED` and 6 `ITEM_TEXTSTYLE_SHADOWEDMORE` (0x10032b1f): each glyph is drawn
twice — first 1 px (3) / 2 px (6) of the render target right and down (0x10032b3d) in black keeping only the text
colour's alpha (`colour & 0xff000000`, 0x100329d7; `^digit` codes retint only the main copy, 0x10032c51), then the
glyph in full colour (0x10032bae). Styles 1 (blink) and 2 (pulse) are handled before that in the exe's item paint
(256 ms half gate 0x4cc59d–0x4cc5b7, sin colour fade 0x4cc574), not by the renderer; 4 and 5 have no code anywhere.

## Localized strings (`localizedstrings/<file>.str`)

Text with `REFERENCE <name>` followed by `LANG_<LANGUAGE> "<text>"` lines. The script key `&"FILE_NAME"` is the
upper-case file name, `_`, and the reference. `#same` means the English text. `\n` is a line break.

## HUD elements

`newhudelem()`, `newclienthudelem(player)` and `newteamhudelem(team)` return an element with script fields `x`,
`y`, `alpha`, `color`, `fontscale`, `font`, `label`, `alignx`, `aligny`, `horzalign`, `vertalign`, `sort`,
`foreground`. Methods: `setshader(material, w, h)`, `settext(string)`, `setvalue(number)`, `settimer(seconds)`,
`fadeovertime(t)`, `moveovertime(t)`, `scaleovertime(t, w, h)`, `destroy()`.

Drawing (inferred): a 640×480 virtual screen scaled by the window height. `horzalign` `fullscreen` stretches to
the window; `left`/`right` are the edges of the centred 4:3 area, `center` the middle, `subleft`/`subright` the
window edges. `alignx`/`aligny` choose the anchor point of the element. A fade, move or scale interpolates
from the values at the time of the call to the values the script assigns afterwards.

Label and value (VAs in the patch 1.3 MP executable 0x4dd430 / 0x4dd330): a number (`setvalue`), timer or text
value is joined to the localized `label` when both are non-empty: the label up to `&&1`, the value, then the rest of
the label; a label without `&&1` is followed directly by the value. The engine adds no separator, so a space comes
only from the label text (the English `MP_TIME_TILL_SPAWN` text ends with a space, the Polish one does not, so the Polish ctf
respawn timer reads without a gap in the original too). The joined text is at most 255 characters.

Missing menu files: a `loadMenu` entry whose file is not in the game data prints "WARNING: menu file not found: %s"
and is skipped (MP 0x546238 / 0x5463be); its menus do not exist and `open <name>` of one does nothing. The shipped
`ui_mp/menus.txt` names five such files (`ui/options_view.menu`, `ui/options_defaults.menu`,
`ui/options_credits.menu`, `ui/rec_restart.menu`, `ui_mp/in_rec_restart.menu`); the options menu's buttons that open
`options_view` / `options_defaults` (`ui_mp/options.menu:723`, `:810`) therefore do nothing, and the credits button is
commented out (`:768`).

Draw order (inferred): elements are sorted by `sort`. Elements with `foreground` false are drawn first, then the
engine's own HUD (crosshair, ammo counter), then elements with `foreground` true, so the intro screen's black
backdrop hides the crosshair and ammo counter.

## Text drawing (2D)

All from the 1.3 binaries:

- Glyph scale = textScale · 48 / font pixel height (gfx refexport +0x128 = DLL 0x1000bfa0; 48.0 at 0x1019bdf8).
  A fontscale-1 text is 48 virtual units (10 % of 480) tall regardless of the font's pixel height.
- The text origin is snapped to whole screen pixels before drawing: the UI text wrapper adds 2^-30 (0x591f20)
  and converts x and y with `fistp` (exe 0x4c37be–0x4c37d3 and 0x4c37df–0x4c37f1; the hudelem path repeats it
  at 0x4c3830+), then calls DrawText (refexport 0x617be4 → DLL 0x1000c070). Glyph offsets after the origin stay
  fractional; the DLL backend emits float vertices (0x1002e880).
- Hudelem text font (exe 0x4c27b0, font value 0 "default"): textScale = fontscale · 0.25 (0x591fe0),
  s = textScale · screen height / 480 (0x793944); s ≤ ui_smallFont (0.25) → $smallfont, s < ui_bigFont (0.4) →
  $font (normalFont), s < ui_extraBigFont (0.55) → $bigfont, else $extrabigfont (font handles 0x1107448
  $bigfont, 0x110744c $smallfont, 0x1107450 $consolefont, 0x1107454 $boldfont, 0x1107458 $font,
  0x110745c $extrabigfont, from the RegisterFont sequence 0x4c3487–0x4c34fe; dvars registered 0x4c312c–0x4c317e).
  Font value 1: textScale = fontscale · 0.5 (0x591e10), pick $smallfont / $font / $boldfont (< 0.55). Font
  value 2: textScale = fontscale / 3 (0x592250), always $consolefont.
- The font atlas `fonts/gamefonts` (512×1024 DXT5, 8 stored mip levels, 1 texel per font pixel in both axes)
  is sampled from mip level 0 only: its material sampler state is 0x02. The 1.3 renderer decodes a material's
  sampler-state byte (byte 4 of each texture-table entry) in its D3D9 SetSamplerState wrapper 0x1003ddc0:
  bits 0–1 → D3DSAMP_MINFILTER (6) and MAGFILTER (5) — mag gets the min value — through table 0x10190714
  {NONE, POINT, LINEAR, ANISOTROPIC}; bits 2–3 → MIPFILTER (7); bits 4/5/6 → ADDRESSU/V/W (1/2/3) with set =
  D3DTADDRESS_CLAMP (3), clear = WRAP (1). A byte with no filter bits (bits & 0xf == 0) gets the r_textureMode
  default ORed in (0x1003dec6; default byte 0x1070c4be = table 0x101906b0[r_textureMode], default index 2
  "bilinear" = 6, i.e. LINEAR min/mag, POINT mip). For state 0x02 that is LINEAR min/mag with mip NONE.

## Mission intro screen (`maps/_introscreen.gsc`)

`maps/_load.gsc` starts `maps\_introscreen::main()`, which picks the level's title/place/date/info strings by
`level.script`. `_load.gsc` sets `level.script = tolower(getcvar("mapname"))`, so the engine sets the `mapname`
cvar to the running map before the level script starts (the string `mapname` is in the executable; that the
server sets it at map load is inferred from this script use). Without it `level.script` is empty, no branch
matches and no intro appears.

`introscreen_delay(title, place, date, info, pause1, pause2, beforeFade)` (defaults 2, 2, 3 s) then:
a full-screen `black` foreground element (`horzalign`/`vertalign` `fullscreen`, 640×480), `freezecontrols(true)`,
after 0.05 s the title line, after `pause1` the place and date lines, after `pause2` the info line, each centred
30 virtual pixels apart at `fontscale` 1.75 and faded in over 1.2 s; after `beforeFade` the backdrop fades out over
1.5 s, controls are restored and the lines fade out over 1.5 s and are destroyed. Cvars `introscreen 0`, or `start` /
`jumpto` set to something other than `start`, skip it. Checked on `moscow` with the Polish strings
(`iw2view <main> moscow --sim-seconds 4`: title, place and date on black; at 8 s the fade-out is under way and the
info line is shown; at 12 s only the game HUD is left).

## Script menus (openmenu, moscow invert-axis prompt)

moscow asks "invert mouse look?" after the player shoots the teddy bear (`maps/moscow.gsc` ~1580: `openMenu("invert_axis1")`,
`freezecontrols(true)`, `setblur(5, .1)`, `player waittill("menuresponse", menu, response)`; `try_invert` repeats the
shooting and opens `invert_axis2`). Evidence:

- `precachemenu(name)` (0x4f8860) stores the name in one of 32 configstring slots 0x4ad+; an already stored name only prints.
- `openmenu` (0x539000) / `openmenunomouse` (0x539110): the entity must be the player; the server sends the client command
  `popupopen <index>` (`popupopen <index> 1` for no mouse). `closemenu` (0x539220) sends `popupclose`. The client handles both
  at 0x4a7094 and loads `ui/scriptmenus/<name>.menu` (0x4c3ad0).
- `freezecontrols(on)` (0x5392e0) stores the argument in client +0x8630; ClientThink (0x4e7bc0) turns it into ps `pm_flags` 0x8000.
  The effects of 0x8000 (view, velocity, stance, buttons) are listed in controls.md "freezecontrols".
- The menu command `scriptMenuResponse "x"` (command table entry 0x4c8e20) sends `cmd mr <menu index> x`; the server command
  `mr` (0x4ea700) notifies the player `menuresponse` with (menu name, response): argc != 3 gives ("", "bad"), an index outside
  0..31 an empty name.
- The invert-mouse item toggles the bool dvar `ui_mousePitch` (exec path 0x433b80 -> 0x433a10); the uiScript update for that dvar (0x4c5118) sets
  the cvar `m_pitch` to -0.022 (0xbcb43958) when the bool is set and +0.022 (0x3cb43958) otherwise. At UI start-up
  `ui_mousePitch = (m_pitch < 0)` (0x4c30db). The mouse look pitch change is `dy * m_pitch`, so a negative value inverts it.
- invert_axis1: "Tak" -> toggle + response `try_invert`; "Nie" and ESC -> `accept_normal`. invert_axis2 reverses the roles
  (its "Nie" toggles back and answers `try_invert`).

Implementation: `World` (src/game/menus.cpp), `ui::runScript` (src/ui/menurun.cpp), iw2view menu draw and mouse cursor
(`ui/assets/3_cursor3` registered at 0x44ccc6). Font choice: `menuFont` (0x4c38e0, see above). TODO(spec): cursor draw details, the
`openmenunomouse` cursor state (no shipped script calls it), the console commands other than `toggle`. The lookup of `openmenu` raises the script error "Menu '<name>' was not precached" (0x4f89f0, message 0x585210), `precachemenu` with a full table "Too many menus precached. Max allowed menus is 32" (0x4f89b5).
Focus sounds: item focus (Item_SetFocus 0x4c9100) plays the item's own `focusSound` (item +0x2d4), else the default `$itemfocussound` alias
(global 0x1107460, looked up at the end of the UI start 0x4c33c0, after the menu files are parsed). The menu key `itemFocusSound` (parser
0x4d3388) only registers that alias when its argument names an existing sound alias; the one shipped value is a file path ("sound/misc/…") that
is no alias name (the lookup 0x428320 only hashes and compares names, and no sound file of that name is in the shipped archives), so the lookup fails and no focus sound plays in single player. No shipped menu sets a per-item `focusSound`, `menuEnterSound`,
`menuExitSound` or `menuBuzzSound`. The click and hover sounds come only from the menus' own `play` commands (0x40fff0: the alias played without a position).
Headless check: `iw2game <main> <map> --answer-menus try_invert,accept_normal` answers the menus that open; `IW2_OVERLAY=<dir>`
mounts loose test scripts over the game files.

## Background menu (`bg`) and what script menus draw over

The main-menu backdrop (the `ui_background` image, the gradient strip, the Call of Duty 2 logo, the version text) is the ordinary
menu `bg`: SP `ui/background.menu`, MP `ui_mp/background.menu` (both non-fullscreen, items only). Nothing in the engine opens it
by name (the patch 1.3 MP executable has no `bg` menu name string); the menu `main` opens it in its own onOpen (after closing it
and `main_text`) and closes it in its onClose (`ui/main.menu:50-62`, `ui_mp/main.menu:46-58`). `bg` is listed only in the front-end
lists (`ui/menus.txt`, `ui_mp/menus.txt`), not in `ui_mp/ingame.txt`. `ui_background` itself is set by the main, options,
createserver and joinserver menus (`ui_mp/main.menu:78`, `ui_mp/options.menu:21`, ...).

So the backdrop shows exactly while `main` is open (the front end, and in SP the in-level pause menu, which is `main`). The MP
script menus never get it: serverinfo_*, team_*, weapon_* and ingame are non-fullscreen menus of style EMPTY with `blurWorld 5`
and a first item of style SHADER, material `gradient`, rect 0 0 640 480 with full-screen alignment
(`ui_mp/scriptmenus/team_americangerman.menu:17-37`, `weapon_german.menu:27-54`, `serverinfo_dm.menu:17-29`, `ingame.menu:16-29`),
plus the shared black bars (`ui/bars.menu`). They draw over the running game's view. quickcommands (and quickstatements,
quickresponses) is `fullscreen 0` with only a FILLED window box (`ui_mp/scriptmenus/quickcommands.menu:10-40`).

Implementation: iw2view draws the menu stack in stack order with no forced `bg` underneath.
### blurWorld
VAs in the patch 1.3 MP executable. The menu keyword `blurWorld` (keyword table entry 0x5dd328, handler 0x544240)
reads a float into the menu (+0x240) and warns "blur must be >= 0; %g is invalid" for a negative value. Menu_Paint
(0x542550) adds a painted menu's non-zero blurWorld to the UI context's radius (UI context +0x28, the context pointer
0x5dcb10): R = sqrt(R^2 + blurWorld^2) (0x5425c6..0x5425f4); the radius is cleared before the menus are painted
(0x53330f). The client's view setup (0x4cf900) clears the refdef and stores sqrt(ui^2 + b^2) as its blur radius
(0x4cf958), b being the scr_blur radius (0x14b5968) and ui the UI radius when the UI key catcher bit (0x96b654 & 8) is
set, 0x68a518 is non-zero and the connection state (0x609fe0) is not 1, else 0. That radius drives the same scene
blur pass as scr_blur (src/game/screen_blur.h): only the 3D view is blurred, the HUD and the menus are drawn over it.
Only MP script menus set it (`blurWorld 5.0` in 15 of `ui_mp/scriptmenus/*.menu`, e.g. `ingame.menu`,
`weapon_german.menu`); no SP menu does.

Implementation: iw2view composes the root sum of squares of the open menus' blurWorld with the scr_blur radius while a
world is running (`game::menuBlurRadius`, `game::composeSceneBlur`). TODO(spec): the meaning of 0x68a518 (the front end
without a running game is never blurred here).

## Focus scripts (onFocus / leaveFocus)

Item_SetFocus (VAs in the patch 1.3 MP executable: 0x53be40; item keyword handler for `onFocus` 0x544ee0 stores the script at
item +0x2b8). For a visible item that does not have the focus yet it sets the focus flag (window flags +0xe8 bit 0x2), runs the
item's onFocus script and plays the item's `focusSound` (+0x2d4), else the global `itemFocusSound`. An item that already has the
focus is left alone, so its script does not run again.

- Pointer hover, click and the keys (Up/Down/Tab, also out of an edit field) move the focus this way. The MP weapon menus
  (`ui_mp/scriptmenus/weapon_*.menu`, e.g. weapon_american.menu:103-114) and team menus (team_americangerman.menu:82-89)
  hide every other preview group and show the focused entry's group (weapon image, grenade icons, accuracy / damage /
  mobility bars, team image) from onFocus; the SP difficulty menu shows its description there (ui/difficulty.menu:121-128).
- `leaveFocus` of the item losing the focus runs before the new item's onFocus (difficulty.menu hides its description there).
  TODO(spec): the exact exe place of leaveFocus, whether the default focus a menu gets on open runs onFocus (the weapon menus
  show the first entry from onOpen, weapon_american.menu:35-39) and the `setfocus` command path; the focus sound is still
  the fixed `mouse_over` alias.
- The shipped team menus show a group `spectator_info` that no item has (team_americangerman.menu:248); nothing appears.

## LISTBOX (type 6) and EDITFIELD / VALIDFILEFIELD (types 4, 16)

Implemented in `src/ui/frontend.cpp` (`MenuStack`), drawn in `apps/iw2view/main.cpp`. Exe addresses: paint 0x4ce280, keys 0x4c9c60, hover
0x4c9880, mouse 0x4cb100, selection 0x4cf5b0 (`Menu_SetFeederSelection`), feeder text 0x4c5f60, feeder select 0x4c6050.

- Menu item keys: `elementwidth`, `elementheight`, `elementtype`, `feeder`, `doubleclick`, `columns n pos width maxChars ...` (the 3 * n numbers are read as tokens, across line breaks: `ui_mp/joinserver.menu` puts each of its 11 server-list columns on its own line), `maxchars`, `maxpaintchars`.
- Geometry: scrollbar column at `x + w - 17`, 16 wide: up arrow (y+1), track (y+16, h-31), down arrow (y+h-16), thumb 16x16 at
  `min(thumbY, y+h-17)`; images `ui/assets/scrollbar_arrow_up_a`, `scrollbar`, `scrollbar_arrow_dwn_a`, `scrollbar_thumb` (materials end in `.tga`).
  Rows start at `y+1`, `ftol(h/eh)` fit; text x = `x + 1 + col.pos + 4`, baseline = row top + `eh`, cut to `col.maxChars`; the selected row is a
  quad (x+3, rowTop+2, w-20, eh) in `outlinecolor`.
- Scroll: `maxScroll = max(0, count - ftol(h/eh) + 1)`, `thumbY = ftol(startPos*(h-50)/maxScroll + y + 17)`. Zones of the scrollbar column: 0x100 up,
  0x200 down, 0x400 thumb, 0x800 page up, 0x1000 page down; arrows auto-repeat (500 ms, then faster); the thumb drags.
- Mouse: hover row `ftol((mouseY-2-y)/eh) + startPos`; a second click on the selected row within 300 ms runs `doubleclick`. Keys: see "List keys" below.
- Edit fields (colour codes, PROVEN): "^" + digit 0..9 is a colour code. The renderer (0x100329a0) draws no glyph and no advance for it: 0..6 = black, red, green,
  yellow, blue, cyan, magenta (table 0x10197f9c; the item alpha is kept), 7 = the item colour again, 8/9 read run-time dwords (TODO(spec)). The edit cursor moves by visible
  characters (0x4d41c0): Left skips a code pair before the cursor for free and costs one step per other character, Right skips the codes at the cursor and then steps one;
  Backspace deletes one raw character and then steps left like Left on the new text; typing steps right like Right. The paint offset (0x4ca7e0) = max(offset, cursor - maxPaintChars
  visible characters), min(offset, cursor).
- Edit fields: editing starts on focus or click (cursor 0, overstrike on); cursor glyph `_` (overstrike) or `|`; VALIDFILEFIELD takes `[A-Za-z0-9_-]`; every
  change writes the dvar; Enter runs `accept`; Tab/Up/Down move the focus.
- Feeders: 0x18 player profiles (`ProfileList`, `players/<name>/` directories, uiScripts addPlayerProfiles, sortPlayerProfiles, selectActivePlayerProfile,
  createPlayerProfile, deletePlayerProfile, loadPlayerProfile; dvars `ui_playerProfileCount/Selected/NameNew/AlreadyChosen`, `com_playerProfile`),
  0x10 savegames (`save/*.svg`, newest first). `Menu_SetFeederSelection` visits every open menu from the top of the stack down (0x4c4c42..0x4c4c7f),
  so a popup on top does not hide the list.
- Thumb drag (0x4cb100 starts it, per tick 0x4cae10): startPos = clamp(ftol((pointerY - (y+17) - 8) * maxScroll / (h - 50)), 0, maxScroll), `maxScroll` = 0x4c9320;
  the thumb is drawn (0x4c9480) at pointerY - 8 while ftol(y+17)+8 <= pointerY <= ftol(y+h-34)+8, otherwise at the startPos position (0x4c93a0).
- List keys (handler 0x4c9c60, PROVEN, vertical lists): Up 0x9c/0xb9 and Down 0x9d/0xbb (0xb9/0xbb are the wheel codes) move one row; Home (0xa5/0xb6) sets
  cursor and startPos to 0; End (0xa6/0xbc) sets cursor = count-1 and, when it is not shown, startPos = cursor - rows + 1; PgUp (0xa4/0xb8) / PgDn (0xa3/0xbe) move
  the cursor by the visible row count (rows = 0x4c9320), clamped to [0, count-1], scrolling only as far as needed to keep it shown. Every key ends in the feeder
  selection 0x4c6050. 0xc8/0xc9 are the mouse buttons (double click), not the wheel. Left (0x9a/0xb7/0xce) and Right (0x9b/0xbd/0xcd) share the code of Up and Down.
  `notselectable` (0x4d1eb0, def +0x108 = 1) makes the keys move startPos only: Up/Left/wheel up -1 (min 0), Down/wheel down +1 (max count-1),
  Right +1 (max maxScroll), PgUp/PgDn +-rows (0 / maxScroll), Home 0, End maxScroll. `horizontalscroll` (0x4d1fb0, item flag 0x200000) runs the same
  code with rows = ftol(w / elementWidth); no shipped SP menu uses it (the only two SP listboxes are text listboxes, none sets `horizontalscroll`), so its drawing is not implemented.
- Not implemented, unused by shipped menus: image columns, horizontal listbox drawing. TODO(spec): drawing of UI_SAVEGAMENAME / UI_SAVEGAME_SHOT /
  textsavegame (see savegame.md), the 64-profile cap on the add loop, the restart of sound/video on profile load.
- No other SP listboxes exist: the mission list and controls list of the shipped menus are not listboxes (buttons / BIND items). The MP map list
  (FEEDER_ALLMAPS), the pointer routing over several open menus and NUMERICFIELD items are in docs/spec/mp-ui.md.

## Menu focus and mouse routing (open-menu stack)

VAs in the patch 1.3 SP executable. The ui state (0x10fec30) keeps an open-menu list (up to 16, newest last) and every menu has a window
flag word (+0xe8): bit 0x4 visible, bit 0x2 "has focus". The menu's own keyword flags live in +0xe4: `popup` sets 0x1000000
(0x4d16d0), `outOfBoundsClick` sets 0x2000000 (0x4d16f0); `fullScreen` is the integer at +0x214 (0x4d1240), `decoration` on an item
sets 0x100000 (0x4d1e90).

- The focused menu is not the last list entry. `Menu_GetFocused` (0x4cf4d0) returns the last list entry that has both bits visible
  and focus. Opening a menu (0x4cb760) clears the focus bit of every listed menu, moves the menu to the end of the list, sets
  visible + focus, runs `onOpen` and re-sends the pointer position (0x4d0060). Closing (0x4c7f90) runs `onClose` when the menu is
  visible, removes it, and only if it held the focus gives the focus bit to the last remaining visible menu. The key handler
  (UI_KeyEvent 0x4c63c0, ESC and every other key) hands the key to the focused menu only (0x4cbc70), not to the list end.
- Pointer move (0x4d0060, called by 0x4c6470 after it clamps the cursor to 0..640 x 0..480): if the focused menu has the popup flag,
  only that menu is offered the move (0x4cf6e0). Otherwise the list is walked from the end towards the start and the walk stops at
  the first menu whose move handler returns 1. That handler (0x4cf6e0) returns 1 only when an item under the pointer accepted the
  focus (`Item_SetFocus` 0x4c9100, which also returns 1 for an item that already has it). It runs the mouse-enter / mouse-exit
  scripts (0x4c99f0) and visits the items from the last to the first.
- Item focus rule across menus (0x4c9100, 0x4c915c..0x4c9198): an item of a menu that is not the focused one is refused when the
  pointer is inside the focused menu's rect and also inside the item's own menu rect. Outside the focused menu's rect it takes
  the focus. So the right-hand buttons of `options_menu` (rect far larger than the sub-menu on top) react as soon as the pointer
  leaves the narrower options sub-menu, and not while it is over that sub-menu.
- Pointer buttons (Menu_HandleKey 0x4cbc70, keys 0xc8..0xca = mouse 1..3, key down): when the focused menu is not a popup, its
  fullScreen value is 0, the pointer is outside its rect (0x4c7720; the right and bottom edges count as inside) and the re-entry guard
  (0x1cbaedc) is clear, the click is handled by `Menu_HandleOutOfBounds` (0x4cb8e0, entered at 0x4cbd48..0x4cbdaf):
  1. if the focused menu has the `outOfBoundsClick` flag its `onClose` runs (0x4c7f50) and its visible and focus bits are cleared
     (it stays in the list while hidden);
  2. the list is searched from the end for the first menu where `0x4d0170` holds: visible, pointer inside the menu rect, and some
     visible, non-decoration item whose rect contains the pointer (a type-0 item additionally needs its own text rect to contain it);
  3. that menu takes the focus exactly like an open does (all focus bits cleared, then visible + focus set; the list order is
     not changed and no `onOpen` runs), the pointer move is re-sent to it, and the same button event is delivered to it again
     with the guard set, so the click lands on the item (its `action`, a slider, ...);
  4. nothing found: nothing is clicked; if no menu in the whole menu table is visible any more, the key catcher is released
     (0x617f04 bit 3) and the game unpaused (cl_paused 0 through the console variable at 0x57122c).
  Because of the first condition a popup or a fullScreen menu never forwards clicks: they stay modal. A click outside the rect of a
  normal menu on empty space (no other menu has an active item there) does nothing.
- When the pointer is inside the focused menu's rect the click goes to that menu's focused item, if the pointer is inside it.

No shipped single-player menu sets `outOfBoundsClick` (the one menu that mentions it has the keyword commented out), and every dialog
(`*_popmenu`) and script menu carries `popup`, so the dialogs stay modal; the sub-menus under `options_menu` are plain menus (no popup, fullScreen 0).

Implementation: `MenuStack` (src/ui/frontend.cpp) keeps the focused menu apart from the list order (`focusedMenu()`); hover and
click use the rules above; `outOfBoundsClick` is parsed into `Menu::outOfBoundsClick`. Deliberate simplifications:
a menu closed by `outOfBoundsClick` is removed from the list at once (the original keeps it listed, hidden, which only changes
the 16-entry count and that a later `close` of it runs no `onClose`); when that leaves no menu with an active item under the
pointer the focus falls back to the last list entry (the original leaves no focused menu, so the next key releases the key
catcher through 0x4c642f); the original also clears the item focus of a menu whose focused item is no longer under the pointer
(0x4cf8c2..0x4cf8e2) and the focus bit of the item stays per menu, while `MenuStack` keeps one focused item for all menus and
keeps it until another item takes it; items are hit-tested in file order (the original visits the last item first); the text-rect
test of type-0 items in step 2 is not modelled (any visible, non-decoration item counts); the script context menu (the menu whose
script runs) is the menu a `show`/`hide`/`setfocus` command acts on (inferred from the context block filled at 0x4c7f6d and 0x4c7fef).

## Pickup HUD part (cgame 0x4ba680)

The client handler of the pickup events 0x90 / 0x91 (0x4bad52) calls 0x4ba680 with the item index only when the event's entity is the local client. It draws no pickup icon, text or flash of its own (the on-screen text is the separate `gm` message of Pickup_Weapon). It looks the index up in the item table (0x5a66f8): weapon items only. A weapon of class "item" does nothing. An offhand weapon (def +0x84 set) runs 0x4b8ba0 when no offhand is selected (0xf79700 is 0): it stores the weapon there and touches the `offhandinfo` HUD fade (0xf79a48 = level time, window flag +0xe8 |= 4, through the generic touch 0x4a6130). Any other weapon runs the weapon select 0x4b8360 when no weapon is selected (0xf796e0 is 0): the weapon becomes the selected one, the weapon-change time 0xf796e4 is stamped, `offhandinfo` (0x4a6130(4)) and the ammo display `weaponinfo` (0xf79a40) are touched, and a toggled aim ends unless the weapon is the old one's altWeapon. With a weapon already selected the pickup changes nothing on the HUD. Port: `pickupHudAction`, `World::takePickupEvents`, applied in iw2view next to the other HUD fade touches. TODO(spec): where 0xf79700 is reset (the port takes "the weapon became the player's offhand" as "none was selected").

## Damage feedback (cgame 0x4ba2a0)

VAs in the patch 1.3 SP executable; the multiplayer client's copies are in docs/spec/mp-hud.md.

Trigger (0x4ba1b7..0x4ba1e4, and the same test at 0x4ba66c): the player state's damageEvent differs from the previous player
state's and its damageCount is not 0. Nothing below happens for a changed damageEvent with a count of 0. The arguments are
damageYaw, damagePitch and damageCount.

1. Kick size: count × `cg_viewKickScale` (float, default 0.6, range 0..10, registered at 0x4a1b2f), at least 5, at most
   `cg_viewKickMax` (default 90, 0x4a1b59).
2. Damage from the world (yaw and pitch both 255): pitch kick = −size, roll kick = 0. Otherwise the direction v is the negated
   forward vector of the angles (pitch byte, yaw byte) × 360 / 255; roll kick = (v · view left) × size, pitch kick =
   −(v · view forward) × size, and the oldest of the 8 direction icon slots takes the snapshot time, `cg_hudDamageIconTime`
   and the yaw byte's angle with ±10 degrees of noise (0x4ba41b..0x4ba517).
3. In both cases (0x4ba523..0x4ba561): the flash end time 0xf79798 = client time + 500; the kick's start time 0xf79764 = the
   snapshot's server time; when the stamp 0xf79a3c is older than the client time it becomes the client time and the menu named
   "Health" gets its visible flag (window flags +0xe8 bit 0x4, see "Menu focus and mouse routing"), so the health bar comes up and
   fades by its own fade time from that stamp.

Red flash (0x4bdbe0, called from the 2D pass 0x4bfa70 at 0x4bfacd, right before the direction icons 0x4bdcb0, only with a
snapshot): while the flash end time lies ahead of the client time, v = |remaining ms × pitch kick × 0.002|, at most 5; the
whole screen (x and y from −10, the screen size + 10 wide and high) is filled through the white material 0x1107440 with the
colour (0.2, 0, 0) and alpha v × 0.2 × 0.7. Only the pitch kick counts: a hit from the side (roll kick only) gives almost no
flash, a fall or a hit from the front or behind the full one. With the minimum kick of 5 the alpha starts at 0.7 and falls to 0
over the 500 ms; a larger kick holds 0.7 until remaining × kick / 500 drops below 5.

View kick: docs/spec/weapons.md ("Damage kick on the gun") and `ViewImpulses::damageKick`. Direction icons (0x4bdcb0): the
`hit_direction` material, `cg_hudDamageIconWidth` 128 × `cg_hudDamageIconHeight` 64, `cg_hudDamageIconOffset` 128 below the
screen centre, turned by view yaw − icon yaw, alpha 2 − 2 × elapsed / duration capped at 1, not under a scope overlay unless
`cg_hudDamageIconInScope`.

The single-player client has no writer of the kicks, the kick's start time or the flash end time besides this routine
(the client state is cleared as a whole with a new level); the multiplayer client's respawn reset is in docs/spec/mp-hud.md.

Implemented: `ViewImpulses::damage` / `damageFlashAlpha` (src/game/viewweapon.h), drawn by iw2view before the direction
icons. openiw2 had no red flash at all and gave the health bar touch and an icon for a count of 0. Test:
tests/test_viewimpulses.cpp.

## Brightness (r_gamma)

- Menu: the graphics options page (`ui/options_graphics.menu`) has a slider item bound to the float dvar `r_gamma`, range 0.5 .. 3 (the item
  names a default of 1.3, but the renderer registers the dvar first with default 1, so a fresh profile shows 1). A second item at the same
  place is shown instead when `r_ignorehwgamma` is 1 (a button that needs a video restart); the slider is gated by `dvarTest r_ignorehwgamma`
  with `showdvar 0`. The in-game options page has the same kind of item (`options_graphics_set` / defaults reset it to the registered value).
- Dvars (patch 1.3 renderer DLL, registration block): `r_gamma` float, default 1, min 0.5, max 3, flags 0x2001 (archived) at 0x1000ab8a;
  `r_ignorehwgamma` bool, default 0, flags 0x2021 (archived, latched) at 0x1000abc0; `r_overbrightBits` int 0..1 default 1 (no ramp involvement).
- Ramp (0x10011fd0): 256 entries of 16 bit. With e = 1 / r_gamma: if e == 1 the entry is i * 257 (identity), otherwise
  round(65535 * pow(i / 255, e)). The same table is used for red, green and blue (0x10030c20), loaded with the Direct3D 9 device
  SetGammaRamp call (device vtable slot 0x54, flags 0 = no calibration). Because it is the display's ramp it affects everything that
  reaches the screen: world, view model, menus, HUD, movies.
- When it is loaded: the end-of-frame routine around 0x10033f65 runs when the modified flag of `r_gamma` or `r_ignorehwgamma` is set, clears
  both flags and, if `r_ignorehwgamma` is 0 and the device capability bit FULLSCREENGAMMA (caps2 bit 17, stored at 0x101d4cd0) is set, builds and loads
  the ramp. Also at device (re)creation (0x10012a2e path). With `r_ignorehwgamma` = 1 the ramp is not loaded; the renderer instead bakes the same
  curve into texture bytes at load (0x100120a0, only reachable after a restart). TODO(spec): that software path, and the exact behaviour of the
  FULLSCREENGAMMA capability on windowed displays.
- iw2view: `Renderer::gamma` is set each frame from the menu cvar store (`r_gamma`, clamped 0.5 .. 3; skipped when `r_ignorehwgamma` is non-zero).
  At 1 nothing changes. Otherwise the whole frame (scene, post effects, menus, HUD) is drawn into an offscreen texture and a final full-screen
  pass reads each 8-bit channel i and writes the ramp entry for i, i.e. the display's behaviour (`src/game/gammaramp.h`, `fs_gamma`).
  `r_gamma` and `r_ignorehwgamma` are archived: they are written to the profile config.

## Focus pulse

A focused bind (0x4cdee0) or edit field value (0x4cd0e0) pulses from the menu focus colour towards a low colour with
t = (sin(integer real ms / 75) + 1) · 0.5; the blend 0x4c6e50 is focus + (low - focus) · t, clamped, on all four channels. The low
colour is focus · 0.8, or (0.8, 0, 0, 0.8) on a bind waiting for its key, which pulses too. Ownerdraw 6 (`ammocounterback`, 0x4a7810)
draws nothing in a vehicle or without a weapon; alpha is the ammo fade, red (0.89 0.18 0.01) at low stock, else the item colour.

## Window borders (0x4c74a6..0x4c75cf)

After the background the window paint draws the border by type (+0xd4) in the border colour (+0x1ec) and thickness (+0xe0): 1 all four sides, 2 top and bottom
(0x4c35f0: the second bar at y + h - size), 3 left and right (0x4c3510: the second bar at x + w - size). The bars lie inside the rect and overlap at the corners. `setitemcolor ... bordercolor`
changes the colour at run time; the mission select uses this to mark the hovered flag. Style 4 has a fixed-tint branch (0x4c74b5) that no shipped menu reaches. Shader backgrounds use the forecolor only
with item flag 0x10000 and white otherwise (0x4c7346); the flag comes with `setitemcolor forecolor` (0x4c7c94). OpenIW2's default forecolor is white, so `MenuStack::foreColor` gives the same.

## ESC at the front end

- `ingameclose <menu>` (script handler 0x4c8320) acts only while the dvar `cl_paused` (name 0x57122c) is nonzero and a menu
  name token follows (0x4c7ef0); then UI_SetActiveMenu(0) closes the menus and unpauses (0x4c7f90). The main menu's focused
  overlay `main_text` runs `ingameclose main` on ESC, so in the pause menu ESC returns to the game, while at the front end
  (cl_paused 0) it does nothing and the main menu stays open. Tested in test_frontend.
- With no menu open, ESC in CL_KeyEvent goes by the connection state (0x5d5680): in a level (4) it opens the pause menu
  (UI_SetActiveMenu(2), 0x40c2b2); with no level (states 1, 2) 0x40e0b0 stops a skippable cinematic and, disconnected (0),
  opens the main menu (UI_SetActiveMenu(1)). ESC never quits the game; only the menu's Quit does.

### Slider value text (openiw2 placement)

A slider's number (options_look "Mouse sensitivity") is a separate decimal-field item on the same dvar and row. In the shipped layout its
rect starts inside the 96-wide track (the track begins after the label column, text rect x + width + 8), so the number was drawn over the
track's end. `MenuStack::valueTextX` places such a field 8 units after the track end when its natural start lies inside the track.
TODO(spec): the original's own placement of this field is not traced; the rule is a deliberate readability deviation. Test: tests/test_cheatkeys.cpp.
