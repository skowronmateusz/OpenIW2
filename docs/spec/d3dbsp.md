# `.d3dbsp` map format (IBSP version 4)

Sources: static analysis of `CoD2SP_s.exe` (1.3) and `gfx_d3d_x86_s.dll` (1.0) with REA 6.7.0 + Ghidra 12.1.4,
checked against the 52 shipped maps.

## Header

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `IBSP` |
| 4 | 4 | version: 4. The exe also accepts `0x3d` (61) in one path (`FUN_00410260`, "bad bsp version %d"); no shipped map uses it |
| 8 | 39 × 8 | lump directory: `{ uint32 length; uint32 offset; }` |

Total header size is 0x140 = 320 bytes. The writer pads every lump to 4 bytes (observed in `FUN_00410260`).

The renderer validates each lump with `FUN_10001980`: offset + length must fit in the file,
the offset must be ≥ 4, and the length must be a multiple of the element size.
It reports "LoadMap: funny lump size / funny lump offset / lump extends past end of file".

## Lumps

The index and element size come from the call sites. The collision loader (`FUN_00412770`)
passes `header + 8 + 8 × index`. Renderer loaders pass `EDX = index, EBX = element size` to `FUN_10001980`
Names in *italics* are inferred.

| # | Element size | Name | Used by | Evidence |
|---|---|---|---|---|
| 0 | 72 | materials: `char name[64]; uint32 surfaceFlags; uint32 contentFlags` | both | `FUN_00410680`; `FUN_100048d0` |
| 1 | 4,194,304 | lightmaps (one 4 MiB block per lightmap) | renderer | `FUN_10001a60` |
| 2 | 8 | *light grid hash/index* | renderer | `FUN_10002660` |
| 3 | 24 | *light grid points* | renderer | `FUN_100048d0` |
| 4 | 16 | planes: `float normal[3]; float dist` | both | `FUN_00411b10` |
| 5 | 8 | brush sides | collision | `FUN_004116c0` |
| 6 | 4 | brushes (`int16 sideCount; int16 material`), see note | collision | `FUN_004116c0` |
| 7 | 16 | triangle soups (below) | renderer | `FUN_10002a00` |
| 8 | 68 | draw vertices (below) | renderer | `FUN_10002a00` |
| 9 | 2 | draw indices (`uint16`) | renderer | `FUN_10002a00` (inline size check) |
| 10 | 32 | *cull groups* | renderer | `FUN_10004340` |
| 11 | 4 | *cull group indices* | renderer | `FUN_10004410` |
| 12–16 | – | unused (always empty) | – | observed in data |
| 17 | 12 | *portal vertices* | renderer | `FUN_100040c0` |
| 18 | 20 | *occluders* | renderer | `FUN_10004470` |
| 19 | 4 | *occluder planes* | renderer | `FUN_10004470` |
| 20 | ? | *occluder edges* (size not decoded yet) | renderer | `FUN_10004470` |
| 21 | 2 | *occluder indices* | renderer | `FUN_100046b0` |
| 22 | 12 | *AABB trees* | renderer | `FUN_10003f20` |
| 23 | 52 | *cells* | renderer | `FUN_10003fd0` |
| 24 | 16 | *portals* | renderer | `FUN_100041b0` |
| 25 | 36 | nodes: plane index + two child indices + … | both | `FUN_004115d0`; `FUN_10004770` |
| 26 | 36 | leafs | both | `FUN_00411910`; `FUN_10004770` |
| 27 | 4 | leaf brushes | collision | `FUN_00411c30` |
| 28 | 4 | leaf surfaces | collision | `FUN_00411cb0` |
| 29 | 16 | collision vertices | collision | `FUN_00411d10` |
| 30 | 56 | collision edges | collision | `FUN_00411df0` |
| 31 | 72 | collision triangles | collision | `FUN_004120f0` |
| 32 | 28 | collision borders | collision | `FUN_004121d0` |
| 33 | 12 | collision partitions | collision | `FUN_00412340` |
| 34 | 32 | collision AABB trees | collision | `FUN_004123e0` |
| 35 | 48 | models (brush submodels, see below) | both | `FUN_00410740`; `FUN_10002df0` |
| 36 | – | visibility; must be empty in single player ("do not compile the bsp with visibility") | collision | `FUN_00412770` |
| 37 | 1 | entity string (text, `{ "key" "value" … }`) | collision | `FUN_00412770` |
| 38 | – | *path nodes / AI data* (binary, not decoded yet) | game | observed in data |

Note on lumps 5/6: the brush loader reads the side lump as 8-byte entries and the brush lump as 4-byte entries
(`(sides >> 3) - brushes × 6` extra sides beyond the six axial ones per brush).

## Triangle soup (lump 7, 16 bytes), observed in `FUN_10002a00`

| Offset | Type | Field |
|---|---|---|
| 0 | uint16 | material index (lump 0) |
| 2 | uint16 | lightmap index (`31` = none, observed in `FUN_10001a60`) |
| 4 | uint32 | first vertex (lump 8) |
| 8 | uint16 | vertex count |
| 10 | uint16 | index count |
| 12 | uint32 | first index (lump 9) |

Indices are relative to `first vertex`. Several soups share one vertex range, so `vertex count` covers the whole
shared range (all 12,043 soups of `toujane` satisfy `max(index) < vertex count`).

## Draw vertex (lump 8, 68 bytes)

The renderer copies each vertex into a 68-byte runtime vertex and swaps the two 3-float groups at
bytes 44–55 and 56–67 (`FUN_10002a00`). The field meaning is inferred from the data
(every sampled normal has unit length):

| Offset | Type | Field |
|---|---|---|
| 0 | float[3] | position |
| 12 | float[3] | normal |
| 24 | uint8[4] | color RGBA |
| 28 | float[2] | texture coordinates |
| 36 | float[2] | lightmap coordinates |
| 44 | float[3] | *tangent* |
| 56 | float[3] | *binormal* |

## Brush models (lump 35, 48 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | float[3] | mins |
| 12 | float[3] | maxs |
| 24 | int32 | first triangle soup (lump 7) |
| 28 | int32 | triangle soup count |
| 32 | int32[2] | first / count of collision AABB trees (lump 34) |
| 40 | int32[2] | first / count of brushes (lump 6) |

The 1.3 renderer loader `0x10002df0` (lump 0x23, element 0x30) copies each row into a 32-byte runtime model: bounds,
soup count at +0x18 and first soup at +0x1c (−1 when the count is 0). Model 0 is the static world (on every shipped
map it owns the soups `[0, n)`); each submodel `*N` owns a contiguous soup range after it, in model order (checked on all 54 shipped maps). Submodel vertices and
bounds are relative to the entity origin (demolition `*73`, origin 6488 480 272: bounds −137..188, soup vertices
−136..160), so a brush model without an `origin` key is in world space.

OpenIW2 (iw2view): with a running level the soups of `script_brushmodel` submodels are not baked into the world; they
are drawn by the world passes (same shaders, lightmaps, `lmap_s` path) at the entity's interpolated origin/angles,
and not while the entity is hidden (`hide` 0x4efd10 sets bit 0x80 of ent+0x128, `show` 0x4efcb0 clears it) or
deleted. Marks and the outdoor bounds use model 0's soups only. TODO(spec): the client draw path of brush entities
(cgame → renderer) was not traced; marks on brush models; sky/water soups in brush models (skipped).

## Not yet specified

Lightmap texel layout (4 MiB per lightmap), light grid, cells/portals/occluders, and lump 38.
