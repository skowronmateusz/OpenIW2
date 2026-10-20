# `.iwi` texture format (version 5)

Sources: static analysis of `gfx_d3d_x86_s.dll` (v1.0, SHA-256 `cc308e22…008980`) with REA 6.7.0 + Ghidra 12.1.4,
checked against every `.iwi` in a retail install (4,892 files).

## Loading path (observed)

- The renderer builds the path as `images/<name>.iwi` and reads it through the engine filesystem (`FUN_1000faa0`).
  It prints errors for a missing file, a 0-byte file, or a path that is too long.
- Header check (`FUN_1000f300`): bytes 0–2 must be `I` `W` `i`. Byte 3 must be `5`,
  otherwise it prints "is version %i but should be version %i".
- The format byte selects one of three upload paths through a 13-entry jump table
  (`FUN_1000f960`; the table at 0x1000fa68).

## Header (28 bytes, little endian)

| Offset | Size | Field |
|---|---|---|
| 0 | 3 | magic `IWi` |
| 3 | 1 | version = 5 |
| 4 | 1 | format (table below) |
| 5 | 1 | flags |
| 6 | 2 | width (int16) |
| 8 | 2 | height (int16) |
| 10 | 2 | depth (int16) |
| 12 | 16 | four uint32: file size, then the start offsets of mip 0, 1 and 2 (observed in data; not read by the loader paths above) |

## Formats

| Value | Pixel data | Bytes/px or block | D3D format passed | Path |
|---|---|---|---|---|
| 1 | ARGB 8888 | 4 | 21 (`A8R8G8B8`) | raw |
| 2 | RGB 888 (expanded to X8R8G8B8 with alpha 0xFF) | 3 | 22 (`X8R8G8B8`) | raw |
| 3 | luminance + alpha | 2 | 51 (`A8L8`) | raw |
| 4 | luminance | 1 | 50 (`L8`) | raw |
| 5 | alpha | 1 | 28 (`A8`) | raw |
| 6–10 | same as 1–5, wavelet-compressed | – | same as 1–5 | `FUN_1000f3e0` → `FUN_10010da0` |
| 11 | DXT1 | 8 per 4×4 | `DXT1` | block |
| 12 | DXT3 | 16 per 4×4 | `DXT3` | block |
| 13 | DXT5 | 16 per 4×4 | `DXT5` | block |

Raw byte order inside a pixel follows D3D's little-endian layout (format 1 = B, G, R, A in memory). This is inferred from the D3D format codes.

## Flags

- `0x02`: no mipmaps, only level 0 is stored. Observed in the loader.
- `0x04`: cube map with 6 faces. This is inferred: every file with this bit holds exactly 6× the single-face size (31 files).
- `0x01`: meaning unknown. The loader marks the image when `flags & 3` is non-zero.

## Mip chain and layout (observed in the loader, confirmed on all files)

- Mip count is 1 when flag `0x02` is set. Otherwise it is `1 + ceil(log2(max(width, height, depth)))`.
- Data starts at byte 28. It is stored **smallest mip first**. For each mip, every face (1 or 6) is stored in turn.
- Level `l` has size `max(1, w >> l) × max(1, h >> l)`. DXT levels round up to whole 4×4 blocks.

## Not yet specified

- The wavelet formats 6–10 (22 shipped files). The decoder reads its codes through three 16 KiB lookup tables stored
  in the DLL's data (0x10182988, 0x10186988, 0x1018a988). The structure of those tables still has to be described
  as a rule, so that no table data is copied.
- Volume textures (depth > 1): none are shipped as `.iwi`.
