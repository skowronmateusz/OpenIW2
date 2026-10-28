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

Draw order (inferred): elements are sorted by `sort`. Elements with `foreground` false are drawn first, then the
engine's own HUD (crosshair, ammo counter), then elements with `foreground` true, so the intro screen's black
backdrop hides the crosshair and ammo counter.

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
  The front end stops movement, buttons and view changes meanwhile. TODO(spec): the exact pmove effect of 0x8000.
- The menu command `scriptMenuResponse "x"` (command table entry 0x4c8e20) sends `cmd mr <menu index> x`; the server command
  `mr` (0x4ea700) notifies the player `menuresponse` with (menu name, response): argc != 3 gives ("", "bad"), an index outside
  0..31 an empty name.
- `execnow "toggle ui_mousePitch"` (0x433b80 -> 0x433a10) flips the bool dvar; `uiScript update ui_mousePitch` (0x4c5118) sets
  the cvar `m_pitch` to -0.022 (0xbcb43958) when the bool is set and +0.022 (0x3cb43958) otherwise. At UI start-up
  `ui_mousePitch = (m_pitch < 0)` (0x4c30db). The mouse look pitch change is `dy * m_pitch`, so a negative value inverts it.
- invert_axis1: "Tak" -> toggle + response `try_invert`; "Nie" and ESC -> `accept_normal`. invert_axis2 reverses the roles
  (its "Nie" toggles back and answers `try_invert`).

Implementation: `World` (src/game/menus.cpp), `ui::runScript` (src/ui/menurun.cpp), iw2view menu draw and mouse cursor
(`ui/assets/3_cursor3` registered at 0x44ccc6). TODO(spec): font choice by textscale, cursor draw details and focus sounds, the
`openmenunomouse` cursor state, the console commands other than `toggle`, the error text for a menu that was not precached.
Headless check: `iw2game <main> <map> --answer-menus try_invert,accept_normal` answers the menus that open; `IW2_OVERLAY=<dir>`
mounts loose test scripts over the game files.

## LISTBOX (type 6) and EDITFIELD / VALIDFILEFIELD (types 4, 16)

Implemented in `src/ui/frontend.cpp` (`MenuStack`), drawn in `apps/iw2view/main.cpp`. Exe addresses: paint 0x4ce280, keys 0x4c9c60, hover
0x4c9880, mouse 0x4cb100, selection 0x4cf5b0 (`Menu_SetFeederSelection`), feeder text 0x4c5f60, feeder select 0x4c6050.

- Menu item keys: `elementwidth`, `elementheight`, `elementtype`, `feeder`, `doubleclick`, `columns n pos width maxChars ...`, `maxchars`, `maxpaintchars`.
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
  code with rows = ftol(w / elementWidth); no shipped menu uses it, so its drawing stays TODO(spec).
- TODO(spec): image columns (unused by shipped menus), drawing of UI_SAVEGAMENAME / UI_SAVEGAME_SHOT /
  textsavegame, horizontal listbox drawing, 64-profile cap on the add loop, the restart of sound/video on profile load.
- No other SP listboxes exist: the mission list and controls list of the shipped menus are not listboxes (buttons / BIND items); MP server/map lists are
  out of scope.
