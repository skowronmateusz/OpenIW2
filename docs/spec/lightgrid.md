# Light grid (lumps 2 and 3 of `.d3dbsp`)

Sources: static analysis of `gfx_d3d_x86_s.dll` (v1.0) with REA 6.7.0 + Ghidra 12.1.4. The renderer's shader
`materials/shaders/lib/lightprobe.hlsl` shows how the result is used. Checked against `toujane`.

## Data

- Lump 2, the "lightgrid hash" (loaded in `FUN_10002660`): 8-byte entries
  `{ uint32 key; uint8 sub; uint8 unknown; uint16 coeffIndex }`, sorted by `(key, sub & ~1)`.
- Lump 3, the "lightgrid coefficients" (`FUN_100048d0`): 24-byte entries. Each holds three color channels
  (R, G, B) of 8 bytes; each byte is a coefficient in `[0, 1]` (×1/255 in `FUN_100360e0`).

## Grid addressing (`FUN_10036380`, `FUN_10036020`, `FUN_10035f90`)

- Cell coordinates: `x = floor((px + 131072) / 32)`, `y = floor((py + 131072) / 32)`, `z = floor((pz + 131072) / 64)`.
- `key = ((((x & ~3) << 11) | (y & ~3)) << 8) | (z >> 2)`.
- `sub = ((((x & 3) << 2) | (y & 3)) << 2 | (z & 3)) << 2`. Bit 0 of the stored `sub` is a flag; the lookup ignores it.
  It counts towards the returned sun visibility, so it is inferred to mean "sun visible".
- Lookup is a binary search on `(key, sub & ~1)`.
- Sampling is trilinear over the 8 surrounding grid points; missing points are skipped. If the total weight is
  below 0.98, the result is renormalized by the found weight. With no points at all, a fallback is used
  (not implemented here).

## Direction weights (`$lightgridweights0/1`, built by `FUN_100101c0` with callback `FUN_10010500`)

For a unit normal `n`:

1. `t0 = 0.81649655·nx + 0.57735026·nz`, `t1 = −0.40824828·nx + 0.70710677·ny + 0.57735026·nz`,
   `t2 = −0.40824828·nx − 0.70710677·ny + 0.57735026·nz`.
2. `m = max |t_i|` (the helper `FUN_10058f20` is inferred to pick the largest magnitude).
3. `u = S((t0/m + 1)/2)`, `v = S((t1/m + 1)/2)`, where `S(a) = (3 − 2a)a²`.
4. Texture 0 uses `s = S(0.5 − t2/(2m))`; texture 1 uses `s = S((t2/m + 1)/2)`.
5. Each texture stores `((1−u)(1−v)s, (1−v)s·u, (1−u)s·v, s·v·u)`.

The eight weights sum to 1. The shader computes `color.c = dot(w0, C_c0) + dot(w1, C_c1)`. This implementation
pairs coefficient byte `k` with weight `k`; that pairing is **inferred** and still has to be verified against the
shader-constant upload.

## Implementation

`src/assets/lightgrid.cpp`. `iw2view` samples the grid once at each static model's origin and bakes
`probe(n) + sunVisibility · max(0, n·L) · sunColor` into the model's vertices.
