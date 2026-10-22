# Models: `xmodel`, `xmodelparts`, `xmodelsurfs` (version 20)

Sources: static analysis of `CoD2SP_s.exe` (1.3) and `gfx_d3d_x86_s.dll` (1.0) with REA 6.7.0 + Ghidra 12.1.4.
Checked on every shipped model: all 1,533 `xmodel` files and their LODs parse, and every `xmodelsurfs` file is
consumed to its last byte.

A model is three files. `xmodel/<model>` lists the LODs and materials. `xmodelparts/<lod0 name>` holds the
skeleton, shared by all LODs. `xmodelsurfs/<lod name>` holds the geometry of each LOD.
Loader functions: `FUN_0047d180`, `FUN_0047cd90`, `FUN_0047ceb0`, `FUN_0047ca50`, `FUN_0047d8c0`. The surfaces are parsed by
the renderer export `FUN_1002d480`.

## `xmodel/<name>`

| Field | Type | Notes |
|---|---|---|
| version | int16 | must be 20 ("out of date (version %d, expecting %d)") |
| flags | uint8 | |
| mins, maxs | float[3] × 2 | bounds |
| 4 × LOD | `float distance; char name[]` | an empty name ends the useful LODs |
| collision LOD | int32 | inferred meaning (−1 in samples) |
| collision surface count | uint32 | then per surface: `uint32 triCount; triCount × 48 bytes; float mins[3], maxs[3]; int32 bone; uint32 contents; int32 surfaceFlags` |
| per non-empty LOD | `int16 count; count × char material[]` | one material per surface, in surface order |

Then one hit box per skeleton bone: `float mins[3], maxs[3]` in bone space (`FUN_0047d180`; see
`docs/spec/collision.md`, "Hit boxes").

## `xmodelparts/<lod0 name>` (skeleton)

| Field | Type |
|---|---|
| version | int16 = 20 |
| child bone count | int16 |
| root bone count | int16 (total must be < 128: "has more than %d bones") |
| per child bone | `uint8 parent; float offset[3]; int16 qx, qy, qz` |
| per bone (roots first) | `char name[]` |
| per bone | `uint8` hit location (read by the hit-box trace `FUN_00473d20` at parts+0x10) |

The parent is an absolute bone index (the loader stores `index − parent` internally).
The rotation is a quaternion with components scaled by 32767; `w` is rebuilt as `round(sqrt(32767² − x² − y² − z²))`
(`FUN_0047c6f0`). Root bones are at the origin with identity rotation. The base pose is composed
parent-first (`FUN_0047c780`): `rotation = parent.rotation × local`, and
`position = parent.position + rotate(parent.rotation, offset)`.

## `xmodelsurfs/<lod name>` (geometry)

`int16 version = 20; int16 surfaceCount` (must match the material count, "File conflict" otherwise), then each surface:

| Field | Type |
|---|---|
| tile mode | uint8 (inferred) |
| vertex count | int16 |
| triangle count | int16 |
| rigid bone | int16; `−1` means a skinned surface, followed by one more int16 (size of the weight table) |
| vertices | see below |
| indices | `triangleCount × 3 × uint16` |

Vertex: `float normal[3]; uint8 color[4]; float uv[2]; float tangent[3]; float binormal[3]` (the tangent and binormal
names are inferred), then:

- rigid surface: `float position[3]` in the space of the rigid bone;
- skinned surface: `uint8 extraWeights; int16 bone; float offset[3]`. If `extraWeights > 0`, an unknown `uint8`
  follows, then `extraWeights × { int16 bone; float offset[3]; uint16 weight }`.

Each offset is relative to its bone. The first bone's weight is inferred as `1 − Σ(extra weights / 65535)`.
The rendered bind pose of `character_german_afrca_body` and `vehicle_panzer_ii` looks correct.
