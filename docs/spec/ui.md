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
