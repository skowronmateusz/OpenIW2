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

## Further data (observed in the loaders)

- Planes (lump 4) get a type: 0/1/2 when the normal is exactly +x/+y/+z (compared with 1.0), otherwise 3
  (so −x is type 3) (`FUN_00411b10`).
- Nodes (lump 25, 36 bytes): `int32 plane; int32 children[2]` (16-bit range checked); a negative child is leaf
  `−1 − child` (`FUN_004115d0`). The other 24 bytes are not used by collision.
- Leafs (lump 26, 36 bytes): `+8 firstCollAabb, +12 collAabbCount, +16 firstLeafBrush, +20 leafBrushCount`
  (`FUN_00411910`). At load each leaf gets `brushContents` (OR of its brushes' contents),
  `terrainContents` (OR of the contents of its AABB trees' materials, unmasked), bounds of its brushes grown by
  0.125, and a leaf brush tree (`FUN_00411a10`; `FUN_004112c0`).
- Collision edges (lump 30, 56 bytes): `float unused; float origin[3], axis0[3], axis1[3], dir[3], dirScale`;
  `dir` is divided by `dirScale` at load (`FUN_00411df0`).
- Triangles: `plane`, `svec`, `tvec` (barycentric planes), `verts[3]`, `edges[3]` (−1 = none) — confirmed by use.
- Borders (lump 32, 28 bytes): `float nx, ny, dist, zBase, zSlope, start, length` (vertical wall edges).
- Partitions point at `triCount` triangles and `borderCount` borders. AABB tree `material` is a lump 0 index,
  `childCount` 0 means `firstChild` is a partition index.

## Leaf brush tree (`FUN_00410bd0`, `FUN_00410930`)

Built per leaf over its brush list (reordered in place), with the brushes' unexpanded bounds. Every brush's min
and max on every axis is scored: brushes with `mins < v` and `maxs <= v` are "below", brushes with `mins >= v`
"above"; `n = min(below, above) − 1`; the candidate moves to `(lowest above min + highest below max) / 2`; the
score is `n × min(region max − highest below, lowest above − region min)` (0 when `n < 1`). The best score above
0 wins (first found on ties, axis order x, y, z). No split → leaf node with the brush list. Otherwise brushes
straddling the split form a subtree in the next node (`count = −1`), brushes with `split <= mins` go to child 0
and with `maxs <= split` to child 1; `range` is the smallest gap between the split and those brushes, and the
child regions are cut at `split + range` / `split − range`. Node: `axis, count, contents, dist | brushes, range,
children[2]` (relative). Node 0 is an empty node used by leafs without brushes.

## Trace (`FUN_00417640`)

The box is traced as a vertical capsule around its centre: `size = maxs − (mins + maxs)/2`,
`radius = min(size.x, size.z)`, `offsetZ = size.z − radius`, start/end shifted by the centre. Trace work also
holds `delta`, `|delta|`, `1/(start − end)` (`FUN_004193d0`), capsule bounds (`±radius` in x/y,
`±(offsetZ + radius)` in z), `isPoint` (size sum 0) and `axialCullOnly` (`bounds volume < size product × |delta| × 16`).
`start == end` runs the position test; otherwise the world BSP walk:

- Nodes (`FUN_00417320`): offset `size[type] + 0.125` on axial planes, `0.125` (point) or `2048`
  on other planes; Quake-style split with `frac = (d1 ± offset)/|d2 − d1|`, `|d2 − d1| <= 2^−21` treated as
  parallel, stop when the trace fraction is at or below the segment start.
- Leaf (`FUN_00416c20`): brushes first; the triangles are skipped when the brushes left fraction 0.
- Brushes (`FUN_00416b30`): segment against leaf bounds grown by `size` (`FUN_00419450`), then the
  leaf brush tree (`FUN_00416820`, offset `size[axis] + 0.125 − range`), then each brush
  (`FUN_00416480`): axial sides expanded by `(radius, radius, offsetZ + radius)`, other sides by
  `|offsetZ·n.z| + radius`; enter fraction starts at 0 with epsilon 0.125; no clip side → `startsolid`, and
  `allsolid` + fraction 0 when the end is inside too. Contents are brush contents, surface flags from the side's
  material.
- Triangles (`FUN_004142d0`, `FUN_00414040`): AABB tree cull by separating axes
  (`FUN_00412d10`), partitions visited once per trace (16-bit stamp, see the sign quirk in the
  code). Points use a ray test with barycentric tolerance 0.001 (`FUN_00412e40`). Capsules sweep
  the lower sphere (and the upper one for triangles facing down) against the triangle, then vertices and edges
  outside it, each once per trace (`FUN_004133c0`); hits on a floor (`n.z >= 0.7`) from an
  edge/vertex with a steep normal while moving down take the triangle normal. Moving horizontally with
  `offsetZ != 0`, the capsule side is swept against the borders (`FUN_00413cb0`).
- Position test (`FUN_00416350`): up to 1024 leafs under `start ± (size + 1)` (`FUN_004158e0`,
  `FUN_00425a50` = BoxOnPlaneSide), brush tree (`FUN_00415f80`) and brushes (`FUN_00415ec0`, non-axial sides
  only after a bounds check), triangles (`FUN_00414200`, `FUN_00412fa0`); any hit is `allsolid`.

Constants (`DAT_…`): 0.125 (`591f80`), 0.7 (`591f30`), 16 (`591e84`), 1e-4 (`592170`), 2^−21 (`5921dc`),
±0.001 / 1.001 (`5920c8`/`5920c4`).

## OpenIW2 implementation

`src/game/collision.cpp` follows the functions above for world model 0. Differences: arithmetic is single
precision (the original uses x87 extended precision in places, the split score in double); one set of visit stamps
instead of per-thread copies. TODO: submodel traces (`FUN_004114a0`), the temporary box/capsule model
`0x3ff` (`FUN_00416120`, `FUN_00417060`), transformed traces (`FUN_00417c20`), sight traces (`FUN_00418d10`).
`src/game/pmove.cpp` implements walking, gravity, jumping, stepping (18 units) and sliding. Speed, gravity and
jump values are placeholders still to be read from the executable.
