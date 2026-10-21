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
