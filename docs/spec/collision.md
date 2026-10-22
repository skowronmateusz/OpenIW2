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
instead of per-thread copies. The entity box model, hit boxes and sight traces are described below.

OpenIW2 (`src/game/collision.cpp`, `src/game/hitboxes.cpp`, `src/game/entityclips.cpp`, `World::locationalTrace`):
living actors and the player are entity clips with their spawn boxes and contents, actors with hit boxes; bullets
(`fireBullet`, mask 0x280e831, priority map by `rifleBullet`) set `damagelocation`; an AI bullet that hits an actor
of its own team continues from the hit point (Bullet_Fire_Extended, "Ignoring AI shooting teammate"); AI sight
(0x54c760) and `sighttracepassed` / `bullettracepassed` use the sight trace. Not done: the player's stance-dependent
box, corpses, xmodel collision surfaces (bit 4), bullet penetration, the cached brush of the sight trace, the
player's movement mask 0x281c011 (pmove still clips only against the world mask 0x10001).

## Brush submodels and entities (implemented)

- Submodels (lump 35, 48-byte rows; `FUN_00410740`,
  `FUN_004114a0`): bounds grown by 1 and a
  bounding radius; for models ≥ 1 a leaf over AABB trees +0x20/+0x24 (first, count) and brushes +0x28/+0x2c (a
  contiguous brush range turned into leaf brushes and a leaf brush tree, `FUN_004112c0`), with brush and terrain
  contents ORed as for world leafs.
- Model trace (`FUN_00417c20` →
  `FUN_00417640`): the box centre's start and
  end relative to the entity origin, rotated into (forward, left, up) when the angles are not zero; a position test
  checks the leaf's brushes, then its triangles; a trace runs the leaf trace (`FUN_00416c20`). A hit normal is rotated
  back.
- Entities (`FUN_0044bf00` G_Trace → `FUN_0041a590` area walk → `FUN_0044b840`): after the world trace, every linked entity
  whose contents match the mask (and that is not the pass entity or its owner) whose absolute bounds the sweep touches
  is traced with its clip model; a smaller fraction replaces the result and names the entity.
- OpenIW2: `CollisionWorld::trace` clips against the entity clips the world sets each frame
  (`src/game/entityclips.cpp`): `script_brushmodel` and `script_vehicle_collmap` entities with a `*N` model, at
  their current origin and angles, contents = their brushes' and triangles' contents, skipped after `notsolid`
  (inferred: the entity contents the original stores are not read). There is no pass entity yet (no caller is a
  brush model). `traceWorld` is the world-only trace.
`src/game/pmove.cpp` implements walking, gravity, jumping, stepping (18 units) and sliding. Speed, gravity and
jump values are placeholders still to be read from the executable.

## Entity boxes: the temporary box model 0x3ff (implemented)

- Entities that are not brush models (`ent+0xa1 == 0`) are traced as a box: `FUN_0044b260` → `FUN_00415d00` copies `r.mins`/`r.maxs` (ent+0xa8/+0xb4, relative to the
  origin) into the thread's box model and `r.contents` (ent+0xc0) into its box brush, and returns model `0x3ff`.
  `FUN_00417c20` then traces it at `r.currentOrigin` (ent+0xdc) with zero angles.
- Spawn values: actors `FUN_004fc640`: mins (−15, −15, 0), maxs (15, 15, 72), contents 0x4000,
  clip mask 0x2820011, `ent+0xa2 |= 2` (hit boxes). Player (0x4e90e2): contents 0x2000000, clip mask 0x281c011,
  mins/maxs from 0x5a5bdc = (−15, −15, 0)–(15, 15, 70); the player gets no hit-box flag there.
- In `FUN_00417640` model 0x3ff runs only when the box brush contents match the mask. The box is a vertical capsule
  like the moving one: centre `c`, `r_b = min(maxs.x − c.x, maxs.z − c.z)`, `h_b = (maxs.z − c.z) − r_b`, top sphere
  `T = c + (0,0,h_b)`, bottom `B = c − (0,0,h_b)`; `R = r_b + radius` of the trace.
- Position test (`FUN_00416120`): solid (startsolid, allsolid, fraction 0, nothing else set) when
  any of the four pairs (T or B, start ± offsetZ) is closer than `R` (strictly), or when
  `|start.z − c.z| <= h_b + offsetZ` and the horizontal distance is below `R`.
- Trace (`FUN_00417060`), skipped unless the trace bounds overlap the box bounds grown by 1. With
  `S_top/S_bot = start ± offsetZ`, `E_top/E_bot = end ± offsetZ`:
  1. `S_bot.z > T.z`: sphere test (S_bot, T); stop on a hit or unless `delta.z < 0`.
     Else if `S_top.z < B.z`: sphere test (S_top, B); stop on a hit or unless `delta.z > 0`.
  2. Cylinder test; stop on a hit.
  3. If `E_bot.z > T.z`: sphere (S_bot, T) when `S_bot.z <= T.z`. Otherwise if `E_top.z < B.z` and
     `S_top.z >= B.z`: sphere (S_top, B).
- Sphere test (`FUN_00416cb0`): `d = A − P`, `c = |d|² − R²`. `c <= 0`: fraction 0, startsolid,
  normal = d normalized, contents = box contents; allsolid too when the matching end point is within `R`.
  Otherwise with `b = d·delta < 0`, `disc = b² − |delta|²·c >= 0`: `t = (−b − √disc)/|delta|² + 0.125·|d|/b`;
  when `t` < fraction: fraction = max(t, 0), normal = d normalized, contents.
- Cylinder test (`FUN_00416e70`), horizontal only: `d = start − c` with `d.z` kept apart,
  `c' = |d.xy|² − R²`, limit `L = offsetZ + h_b`. `c' <= 0`: if `|d.z| <= L` startsolid, fraction 0, normal =
  (d.x, d.y, 0) normalized, contents, allsolid when `|end.z − c.z| <= L`. Otherwise with `b = d.xy·delta.xy < 0`,
  `a = |delta.xy|²`, `disc = b² − a·c' >= 0`: `t = (−b − √disc)/a + 0.125·|d.xy|/b`; a hit needs `t` < fraction and
  `|start.z + (t − 0.125·|d.xy|/b)·delta.z − c.z| <= L`; fraction = max(t, 0), normal as above.
- `FUN_00408ad0` is `max(a, b)`. Trace result layout: +0 fraction, +4 normal, +0x10 surfaceFlags, +0x14 contents,
  +0x18 material, +0x1c entity, +0x1e partName, +0x20 partGroup, +0x22 allsolid, +0x23 startsolid.

## Entity selection (`FUN_0044b6f0` box traces, `FUN_0044b840` point traces; `FUN_0044bf00` G_Trace)

An entity is tested when its contents match the mask, it is not the pass entity, its owner (ent+0xf4) is not the
pass entity, and it is not the pass entity's owner; and when the swept trace touches its absolute bounds (ent+0xc4,
+0xd0) up to the current fraction (`FUN_00419450`). A smaller fraction replaces the result and names the entity.
For a point trace with `locational` set and a non-zero priority map, entities with a DObj and `ent+0xa2 & 2` are
traced against their hit boxes instead (below); bit 4 means xmodel collision surfaces (`FUN_00474440` →
`FUN_0047c210`; TODO in OpenIW2). With a zero priority map (`bullettrace`) every entity is a box.

## Hit boxes (`FUN_00473d20`)

- Data: per bone of each xmodel, after the LOD material lists, `float mins[3], maxs[3]` (24 bytes, bone space;
  `FUN_0047d180`); the loader adds `offset = (mins + maxs)/2` and
  `radiusSquared = |maxs − offset|²`. A bone with `radiusSquared == 0` has no box. Per bone a classification byte
  follows the bone names in xmodelparts (`FUN_0047ca50`, read by `FUN_00473d20` at parts+0x10).
- Hit locations (table 0x5a9538): 0 none, 1 helmet, 2 head, 3 neck, 4 torso_upper, 5 torso_lower,
  6 right_arm_upper, 7 left_arm_upper, 8 right_arm_lower, 9 left_arm_lower, 10 right_hand, 11 left_hand,
  12 right_leg_upper, 13 left_leg_upper, 14 right_leg_lower, 15 left_leg_lower, 16 right_foot, 17 left_foot, 18 gun.
- Priority maps (byte per location): 0x5a9584 = {1, 3 × 17, 0} and 0x5a9598 = {1, 9, 9, 9, 8, 7, 6, 6, 6, 6, 5, 5,
  4, 4, 4, 4, 3, 3, 0}. Bullet_Fire (`FUN_00531340`) uses mask 0x280e831 and 0x5a9598 when the
  weapon's `rifleBullet` (weapon def +0x31c) is set, else 0x5a9584.
- Pre-test (`FUN_0044b840`): the segment against origin + (−64, −64, −32)…(64, 64, 72) (0x5a08a0) up to the current
  fraction; start and end then go into the entity's space (origin, angles; `FUN_00424ad0`, `FUN_00423480`), and the
  hit normal is rotated back (`FUN_00423310`). The trace gets surfaceFlags 0, partName = bone name, partGroup =
  location, contents = entity contents, material none.
- Classification: walking the DObj's models in order (global bone index g), `cls` = the bone's byte, `p` =
  map[cls]. For a duplicate bone (below) with `p == 1` take the resolved class of its target; otherwise with
  `p == 1` take the resolved class of the parent (a root bone: the attach bone of its model, or class 0 when the model
  is not attached to a tag); `p` = map[that]. The resolved class of g is stored for children.
- Duplicates (`FUN_00472660`, `FUN_004725f0`): bones of a model attached without a tag whose name is
  found earlier (first model holding the name; within a model the last such bone) map to that bone.
- Test per bone with a box, model not hidden, `p >= best` (best starts at 2): bone centre = bone matrix ×
  offset; with `t` = the segment parameter of the point closest to it (clamped to [0, 1]) and `dist²` its distance,
  skip unless `radiusSquared − dist² > 0` and (p ≠ best or `t − √((radiusSquared − dist²)/|delta|²)` < fraction).
  Start and end go into bone space (`(p − trans)` against the quaternion axes, `FUN_00473c90`), then a slab test
  against mins (sign −1) and maxs (sign +1) without epsilon, the leave fraction starting at the current fraction for
  the same priority, else at the fraction the call started with. Start and end both inside: when
  `delta.xy·start.xy <= 0` (entity space) fraction 0, normal = start.xy normalized (or z = +1 moving down, −1
  otherwise when delta.xy is zero), done. A higher priority replaces any earlier hit; the same priority needs a
  smaller fraction. The final normal is ±(hit axis of the bone's rotation).

## Sight traces (`FUN_00418d10`)

Return a non-zero hit id instead of a fraction. Setup as in the trace. Model 0: an optional cached brush
(`FUN_00417f60` on brush `oldHit − 1`, skipped by OpenIW2: it only changes which id is returned), then the BSP walk
`FUN_00418a70` (node offsets as in the trace, no fraction checks, first hit returns). Leaf
(`FUN_00418500`): brushes when the leaf brush contents match (`FUN_00418470`: segment against the
leaf bounds grown by the size, then `FUN_00418190`, the leaf brush tree as in the trace), returning
brush + 1; then each AABB tree whose material matches with the normal triangle trace (`FUN_004142d0`) into a
shared result starting at fraction 1, returning `numBrushes + 1 + tree` once it is below 1.
Brush (`FUN_00417f60`): the trace's brush clip without the 0.125 epsilon and without normals:
axial sides grown by the capsule extents, other sides by `|offsetZ·n.z| + radius`; enter from 0, leave from 1, a
hit when the interval stays open. Box model 0x3ff (`FUN_004187e0`): the steps of `FUN_00417060`
with boolean tests: sphere (`FUN_00418590`) hits when `c <= 0` or
`(−b − √disc)/|delta|² + 0.125·b/|d|` < 1; cylinder (`FUN_004186a0`) uses the full `|delta|²` as `a`
and `0.125·b/|d.xy|`. Other submodels: the leaf of the model. Entities (`FUN_0044c3c0`,
`FUN_0044bba0`, `FUN_0044bc80`): the world first, then each entity as a box (no hit
boxes). `sighttracepassed` (`FUN_004f2580`): mask 0x280d803 (0x801803 without characters), then
`FUN_0048b090` below 0x592020 also blocks (TODO: not read). `bullettrace` (`FUN_004f2230`):
G_Trace with mask 0x280e831 (0x802831 without characters), locational, zero priority map.
