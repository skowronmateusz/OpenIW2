# World collision

Sources: the collision loaders in `CoD2SP_s.exe` (1.3): `FUN_00412770` and the per-lump
functions in `docs/spec/d3dbsp.md`. Data layouts are checked on `toujane`.

## Data (observed)

- Brushes (lump 6, 4 bytes): `int16 sideCount; int16 material`. `sideCount` is at least 6
  ("brush has fewer than 6 sides" otherwise) (`FUN_004116c0`).
- Brush sides (lump 5, 8 bytes). The first 6 sides of each brush are the axial bounds in the order min x, max x,
  min y, max y, min z, max z, stored as `{ float value; int32 material }`. The remaining `sideCount − 6` sides are
  `{ int32 planeIndex; int32 material }` into lump 4.
- The contents of a brush come from its material's content flags (lump 0, offset 68), masked with `0xdffffffb`.
- Models (lump 35, 48 bytes): `float mins[3], maxs[3]; int32 firstTriSoup, numTriSoups, …, firstBrush, numBrushes`
  (`FUN_00410740`). Model 0 is the world. Brushes after its range belong to brush entities
  (triggers, `script_brushmodel`) and are not static world collision.
- Collision vertices (lump 29, 16 bytes): `{ float unused; float xyz[3] }` (`FUN_00411d10`).
- Collision triangles (lump 31, 72 bytes): `{ float plane[4], svec[4], tvec[4]; int32 verts[3]; int32 edges[3] }`
  (copied as-is in `FUN_004120f0`; the field split is inferred from the data).
- Partitions (lump 33, 12 bytes): `{ uint16 unused; uint8 triCount; uint8 borderCount; int32 firstTri; int32 firstBorder }`
  (`FUN_00412340`).
- Collision AABB trees (lump 34, 32 bytes): `{ float origin[3], halfSize[3]; int16 material; int16 childCount;
  int32 firstChildOrPartition }` (field split inferred from the data).

## Content flags (inferred from material names)

`0x1` solid, `0x10000` player clip (`clip_player`), `0x20000` AI clip (`clip_ai`). The player is blocked by `0x10001`.

## OpenIW2 implementation (simplified)

`src/game/collision.cpp` builds world brushes from model 0's range. It turns each collision triangle into a thin
solid brush with bevels, and sweeps an axis-aligned box against them, using a uniform 2D grid for broad-phase
culling. The original's partition, border and edge logic is **not** reproduced yet, and triangle materials are
ignored (every triangle blocks). `src/game/pmove.cpp` implements walking, gravity, jumping, stepping (18 units)
and sliding. Speed, gravity and jump values are placeholders still to be read from the executable.
