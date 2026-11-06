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
  The front end stops movement, buttons and view changes meanwhile. TODO(spec): the exact pmove effect of 0x8000.
- The menu command `scriptMenuResponse "x"` (command table entry 0x4c8e20) sends `cmd mr <menu index> x`; the server command
  `mr` (0x4ea700) notifies the player `menuresponse` with (menu name, response): argc != 3 gives ("", "bad"), an index outside
  0..31 an empty name.
- The invert-mouse item toggles the bool dvar `ui_mousePitch` (exec path 0x433b80 -> 0x433a10); the uiScript update for that dvar (0x4c5118) sets
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
click use the rules above; `outOfBoundsClick` is parsed into `Menu::outOfBoundsClick`. Deliberate simplifications / TODO(spec):
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
