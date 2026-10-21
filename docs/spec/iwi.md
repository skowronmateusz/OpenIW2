# `.iwi` texture format (version 5)

Sources: static analysis of `gfx_d3d_x86_s.dll` (v1.0, SHA-256 `cc308e22…008980`) with REA 6.7.0 + Ghidra 12.1.4,
checked against every `.iwi` in a retail install (4,892 files at the time; `iw2asset iwi-check` now reads 4,903 and decodes all of them).

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
| 6–10 | same as 1–5, wavelet-compressed (see below) | – | same as 1–5 | `FUN_1000f3e0` → `FUN_10010da0` |
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

## Wavelet formats 6–10

Evidence (all in `gfx_d3d_x86_s.dll` v1.0):

| ID | What |
|---|---|
| | `FUN_1000f960` pseudocode / instructions: format dispatch and the channel count passed in `EAX` |
| | `FUN_1000f3e0` pseudocode / instructions: decoder state set-up, level and face loop |
| | `FUN_10010da0` pseudocode / instructions: decodes one level of one face |
| | `FUN_10010c00`: consume N bits |
| | `FUN_10010c60`: read one delta through a 12-bit lookup table, with escape |
| | `FUN_10010d10`: refinement pass over the previous level |
| | contents of the three lookup tables (0x10182988, 0x10186988, 0x1018a988), read only to derive the codes below |
| | cross references: the tables are only read, never written (static data) |

Checked against the retail files: all 25 wavelet `.iwi` (13 of format 6, 12 of format 7; formats 8–10 are not shipped)
decode, and for every one of them the bit stream ends exactly in the last byte of the file. Decoded images
(`logo_cod2`, `hill400_map`, `crusader_vm_c`, `sun_flare`, …) look correct.

### Channels and output layout

| Format | Coded channels | Output bytes per pixel | Output layout |
|---|---|---|---|
| 6 | 4 | 4 | as format 1 (B, G, R, A) |
| 7 | 3 | 4 | B, G, R, then a constant 0xFF |
| 8 | 2 | 2 | as format 3 (L, A) |
| 9 | 1 | 1 | as format 4 |
| 10 | 1 | 1 | as format 5 |

The channel count is the value `FUN_1000f960` passes to `FUN_1000f3e0` (4, 3, 2, 1, 1). The output stride is the channel
count, except 3 becomes 4. Channels are numbered in memory order; "the last channel" is the one at offset stride − 1.

### Stream layout

The compressed stream starts right after the 28-byte header. Levels are decoded smallest first, and for each level every
face (1 or 6) in turn, all from **one continuous stream**. Each level is predicted from the previous (half-size) level
of the same face. The mip count follows the same rule as the raw formats.

A level whose width or height is 1 is stored as plain bytes: for each pixel, `channels` bytes, then 0xFF if the output
stride is larger. These levels always come first, before any bit-coded level.

At the first level with width ≥ 2 and height ≥ 2 a bit reader starts at the current byte. Bits are read LSB first:
bit *k* of the stream is bit (*k* mod 8) of byte ⌊*k*/8⌋. (The DLL keeps a 16-bit look-ahead window and refills it with
an unaligned 32-bit read; this is equivalent.) The bit reader then stays active for all larger levels and faces.

### One bit-coded level (`FUN_10010da0`)

Let the level be *w* × *h* (both even), the previous level of the same face *P* (*w*/2 × *h*/2, same layout).

1. Read 1 bit. If it is 1, refine a copy of *P* before it is used: for each pixel of *P* in row-major order and for each
   coded channel *c* in order, *P*[*c*] += delta(code C) (modulo 256); the padding byte of format 7 stays 0xFF.
   The level that was already produced for *P* is not changed by this (the DLL refines its scratch copy).
2. For every 2×2 block (row-major over *P*), with *p* the matching pixel of *P*:
   - If channels ≠ 1: channel 0 group — read 1 bit *r*, then three deltas *a*, *b*, *d* with code A.
   - If channels > 2: for channel 1 and then channel 2 — read 1 bit *r*, then three deltas with code B; each is
     **added** to channel 0's *a*, *b*, *d* in the same order to give that channel's deltas.
   - If channels = 3: write 0xFF to the padding byte of the four output pixels.
     Otherwise: last-channel group — read 1 bit *r*, then three deltas with code C (not added to anything).
   - Each group writes its channel of the four pixels, with *q* = 2·*p*[channel] and arithmetic right shift,
     results truncated to 8 bits:
     - top left = ((*q* + *a* + *b* + *d*) >> 1) + *r*
     - top right = (*q* + *a* − *b* − *d*) >> 1
     - bottom left = (*q* + *b* − *a* − *d*) >> 1
     - bottom right = (*q* + *d* − *a* − *b*) >> 1

So *p* is (up to rounding) the block average and *a*, *b*, *d* are the horizontal, vertical and diagonal differences:
a one-level Haar transform per mip, with the mip chain itself as the low-pass image.

In the DLL each face's levels share one buffer, with each level placed at the end of it so that it overlaps the
previous one; the write order never overwrites a predictor pixel before it is read, so separate buffers give the
same result.

### Delta codes (`FUN_10010c60`)

Each delta is a prefix code of at most 12 bits. The DLL looks it up in a 4096-entry table indexed by the next 12 stream
bits; an entry holds the value and the code length. The three tables are fully described by these rules
(derived from the table contents, which are complete prefix codes):

- A non-zero value *v* is coded as the prefix of |*v*| followed by one sign bit: 0 for positive, 1 for negative.
  Zero has no sign bit.
- One extra code word is an **escape**: it is followed by *n* raw bits (read LSB first) and the value is
  raw − bias. Code A: escape `001111`, 9 bits, bias 255. Code B: escape `0001011`, 10 bits, bias 510.
  Code C: escape `0000`, 9 bits, bias 255.

Prefixes, written in stream order (first bit read first):

Code A (channel 0 of colour formats):

| | | | | | | | |
|---|---|---|---|---|---|---|---|
| 0 `100` | 1 `1110` | 2 `1010` | 3 `0101` | 4 `0010` | 5 `11111` | 6 `11010` | 7 `11000` |
| 8 `01111` | 9 `01100` | 10 `01000` | 11 `00011` | 12 `00000` | 13 `111100` | 14 `110110` | 15 `101111` |
| 16 `101101` | 17 `011101` | 18 `011010` | 19 `010010` | 20 `001101` | 21 `000101` | 22 `000011` | 23 `1111010` |
| 24 `1101110` | 25 `1100110` | 26 `1100100` | 27 `1011100` | 28 `1011000` | 29 `0111000` | 30 `0100111` | 31 `0011101` |
| 32 `0011001` | 33 `0001001` | 34 `0001000` | 35 `11110111` | 36 `11011111` | 37 `11011110` | 38 `11001011` | 39 `10111011` |
| 40 `10110011` | 41 `01110011` | 42 `01101111` | 43 `01101110` | 44 `01101100` | 45 `01001100` | 46 `00111000` | 47 `00110000` |
| 48 `00001010` | 49 `00001001` | 50 `111101101` | 51 `110011111` | 52 `110011101` | 53 `110010101` | 54 `101110101` | 55 `101110100` |
| 56 `101100100` | 57 `011100100` | 58 `011011011` | 59 `010011011` | 60 `001110011` | 61 `001110010` | 62 `001100010` | 63 `000010111` |
| 64 `000010001` | 65 `000010000` | 66 `1111011000` | 67 `1100111101` | 68 `1100111001` | 69 `1100111000` | 70 `1100101000` | 71 `1011001011` |
| 72 `0111001011` | 73 `0111001010` | 74 `0110110101` | 75 `0110110100` | 76 `0100110101` | 77 `0100110100` | 78 `0011000111` | 79 `0011000110` |
| 80 `0000101101` | 81 `0000101100` | 82 `11110110011` | 83 `11110110010` | 84 `11001111001` | 85 `11001111000` | 86 `11001010011` | 87 `11001010010` |
| 88 `10110010101` | 89 `10110010100` | | | | | | |

Code B (channels 1 and 2, relative to channel 0):

| | | | | | | | |
|---|---|---|---|---|---|---|---|
| 0 `11` | 1 `01` | 2 `100` | 3 `1011` | 4 `0010` | 5 `10101` | 6 `00110` | 7 `00001` |
| 8 `101001` | 9 `001110` | 10 `000110` | 11 `000001` | 12 `1010001` | 13 `0011111` | 14 `0001110` | 15 `0001001` |
| 16 `0000001` | 17 `10100001` | 18 `00111101` | 19 `00011111` | 20 `00010101` | 21 `00010001` | 22 `00000001` | 23 `101000001` |
| 24 `001111001` | 25 `001111000` | 26 `000111100` | 27 `000101001` | 28 `000100001` | 29 `000000001` | 30 `000000000` | 31 `1010000000` |
| 32 `0001111011` | 33 `0001111010` | 34 `0001010001` | 35 `0001010000` | 36 `0001000001` | 37 `0001000000` | 38 `10100000011` | 39 `10100000010` |

Code C (last channel, single-channel formats, refinement pass):

| | | | | | | | |
|---|---|---|---|---|---|---|---|
| 0 `1` | 1 `010` | 2 `0011` | 3 `01101` | 4 `00011` | 5 `011101` | 6 `001011` | 7 `001000` |
| 8 `0111111` | 9 `0111100` | 10 `0110011` | 11 `0110000` | 12 `0010100` | 13 `0001011` | 14 `0001000` | 15 `0001001` |
| 16 `01111011` | 17 `01110001` | 18 `01100011` | 19 `01100010` | 20 `00101010` | 21 `00100111` | 22 `00100101` | 23 `00010100` |
| 24 `00010101` | 25 `011111001` | 26 `011110101` | 27 `011100111` | 28 `011100100` | 29 `011100000` | 30 `011001001` | 31 `011001000` |
| 32 `011111010` | 33 `001010110` | 34 `001001101` | 35 `001001100` | 36 `001001001` | 37 `001001000` | 38 `0111110111` | 39 `0111110110` |
| 40 `0111110001` | 41 `0111110000` | 42 `0111101001` | 43 `0111101000` | 44 `0111001101` | 45 `0111001011` | 46 `0111000011` | 47 `0110010101` |
| 48 `0111001100` | 49 `0111000010` | 50 `0110010100` | 64 `0111001010` | 127 `011001011` | 128 `001010111` | | |

The codes are complete (every 12-bit pattern decodes to exactly one entry). The engine's own lookup tables are not
copied; `src/assets/iwi.cpp` builds equivalent lookups at start-up from the prefixes above.

### Details that do not matter for the shipped files

- The DLL can start at a later level (picmip): it still decodes the smaller levels, it just does not upload them.
- A bit-coded level with no previous level (flag `0x02` with width and height ≥ 2) would read a null predictor in the
  DLL. No shipped file does this; this decoder predicts from zero.
- Odd sizes are not handled by the DLL's block loop either; the decoder rejects them.
- Flag `0x01` is set on 4 of the wavelet files (all with mips); it does not change the stream.

## Not yet specified

- Volume textures (depth > 1): none are shipped as `.iwi`.
