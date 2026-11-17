# Script render hints: setculldist and setshadowhint

Addresses: exe = CoD2SP_s.exe 1.3, dll = the gfx renderer DLL (gfx13). The exe's renderer export table starts at 0x617ab0, the DLL's at
0x10748e00 (slot offset = exe address - 0x617ab0).

## setculldist

- Builtin 0x4f5700 stores configstring 8 = "%g" (docs/spec/builtins.md).
- Client: the configstring is read at 0x4a56d0 (called from the configstring-modified switch 0x4a5ac0 and at level start 0x4a5c70) and
  passed to R_SetCullDist (dll 0x1000ab00), which stores it at 0x101ce5f4.
- FarDist (dll 0x1000aaa0, exe slot 0x617af0, called each frame from 0x4afc90):
  `d = r_zfar != 0 ? r_zfar : (linear fog active ? fog far : 0); result = max(d, cullDist)`. 0 means no far plane.
- The far plane is a planar-depth cull: a point p is beyond it when `forward . (p - eye) > far` (FX add the element radius). It is
  appended to the plane lists of the world, model and effect culls; it is not the projection far clip.
- Effect view planes (exe builder 0x486dd0 / 0x486ba0): near, four sides, far (5 or 6 planes). The cloud and particle draw culls
  (0x486e30) use only the first five planes for elements with `disableFarPlaneCulling` (flag 0x2000000, so the far plane is skipped).
  The spawn gate (0x486e90) uses the previous frame's plane set (copy at 0x486b30) and has no such exception.
- OpenIW2: `farPlaneDistance` (src/game/fog.h), `World::cullDist()`, `FxSystem::beyondFar` (src/game/fx.cpp: spawn gate, clouds,
  oriented particles); iw2view feeds it each frame (`setFxFar`). Test: tests/test_farplane.cpp.
- Where the far plane is applied in the renderer (dll): scene setup 0x1000a910 builds one plane list: four side planes (0x10006c20 from
  the view-projection), then 0x100062e0 appends the near plane (pointer 0x101ce5b0) and, only when FarDist != 0, the far plane (0x101ce5b4);
  an orthographic view (0x1000a590, view +0xc4 != 0) has no far plane. Plane record = 20 bytes `{n[3], d, 3 corner-select bytes, cache byte}`.
  - Near (0x1000a400): `n = forward, d = 0.1 - forward . eye` (constant 0x1019bf48 = 0.1).
  - Far (0x1000a4e3): `n = -forward, d = far + forward . eye`.
  - Box test (0x10006420 / 0x10006340 / 0x100064f0): per plane the corner furthest along n is picked (max if n > 0 else min); the box is
    culled when `n . corner + d <= 0` (equality culls). The cache byte (+0x13) only skips planes the parent box was already fully inside;
    it does not change the result. After the planes, occluder hulls (0x101ce5c0 / 0x101ce5c4) can cull a box that lies fully inside one.
  - Granularity: the same list culls AABB-tree nodes, single world surfaces, cell boxes, static models and scene entities; see
    "World and static model culling" below for the walk. The far plane therefore also removes single world surfaces and static models.
  - OpenIW2 has the pure planes and box test as `viewcull.h` (tests/test_viewcull.cpp).
- Wired into iw2view for brush-model entities (`Scene::brushBounds` = local bounds of the model's drawn soup vertices, enclosing world box
  via `transformedBoxBounds`, then `boxCulledByDepthPlanes` with `farPlaneDistance`; `IW2_NO_CULL=1` disables). The static world and
  the static models get the whole plane list, far plane included, and xmodel entities their box test (see "Entity cull" below); the sky
  and the frame clear are described under "World and static model culling". Needs a human visual check: a brush model (door / elevator) beyond a setculldist or fog far
  must disappear, and none may pop out near the screen edge. r_zfar defaults to 0 and an exponential fog adds no distance (only record
  type 1 counts, 0x1000aacb).

## World and static model culling

Addresses are in the 1.3 renderer DLL unless marked exe. World pointer 0x101d1b9c.

- Side planes (0x10006c20, called by the scene setup 0x1000a910 with the view-projection matrix at view +0xc8): the four clip-space
  planes of the table 0x10182948 ((-1,0,0,1), (1,0,0,1), (0,-1,0,1), (0,1,0,1)) are taken back to world space through the matrix (each
  plane component is the table plane dotted with one matrix column, i.e. the classic "w +- x", "w +- y" planes), then normal and
  distance are divided by the length of the normal. The corner-select bytes are 0xc / 0x10 / 0x14 (maxs) for a positive component and
  0 / 4 / 8 (mins) otherwise, the cache byte 0xff. The near and far planes follow (0x100062e0, see setculldist above).
- Load-time data:
  - World surfaces (0x10002a00, from the triangle soups of lump 7): 12-byte entries at world +0x14 `{material, lightmap (u16), sort
    key (u16), draw surface}`; the 0x28-byte draw surface carries a box at +4 that 0x10002700 builds from the soup's indexed vertices
    (indices relative to the soup's first vertex). Sky soups also go to a separate sky surface list (0x101ae2a4); a map may have one sky
    material only.
  - AABB trees (lump 22, 12-byte rows, loader 0x10003f20): runtime nodes of 0x30 bytes `{mins, maxs, surface count (+0x18), first
    surface (+0x1c, -1 when the count is 0), static model count (+0x20), static model list (+0x24), child count (+0x28), children
    (+0x2c)}`; the file row gives first surface, surface count and child count, and children are stored after their parent in file order
    (0x10003d80 walks them, sets the children pointer and builds every node box from its children or, for a leaf, from its surfaces'
    boxes). A node's surface range covers the surfaces of all its descendants.
  - Cells (lump 23, 52-byte rows of 13 dwords, loader 0x10003fd0): `mins[3], maxs[3]` (floats), root AABB-tree node, first portal,
    portal count, first cull group index, cull group count, first occluder index, occluder count. Runtime 0x3c bytes: bounds at +4, root
    node +0x1c, portal count +0x20 and portals +0x24, cull group count +0x28 and the index list into lump 11 (0x101ae494) +0x2c,
    occluder count +0x30 and list (0x101ae490, lump 21) +0x34.
  - Cull groups (lump 10, 32 bytes: `mins[3], maxs[3], first surface, surface count`; loader 0x10004340 keeps the count at +0x18 and the
    first surface at +0x1c, -1 when the count is 0; world +0xf0, stride 0x20): surface ranges reached through a cell's indices into lump
    11. After the AABB-tree walk the cell's groups are walked (0x100096d0 -> 0x100093e0): a group outside a plane is dropped, else all its
    surfaces are added; a group stamp (0x101d7e9c) adds a group once per frame. On the shipped maps every surface of model 0 belongs to
    some cell (duhoc_assault: surfaces 179.. through the trees, 0..178 through cull groups, 60 of them in groups of two cells; toujane and
    newvillers: each surface in exactly one cell).
  - Occluders (lumps 18-21, cell +0x30): no shipped single player map has any (lump 18 is empty on every map), so 0x10007ae0 and the
    occluder tests after the planes never cull anything there.
  - Portals (lump 24, 16 bytes: `plane index, cell, first vertex, vertex count`; vertices: lump 17, 3 floats; loader 0x100041b0):
    runtime 0x44 bytes: +0 queued flag, +1 on-the-chain flag, +4 the portal that led here, +8 plane normal, +0x14 minus the plane
    distance, +0x18 corner-select bytes, +0x1c the cell behind the portal, +0x20 vertices, +0x24 vertex count, +0x25 clipped point
    count, +0x28 clipped point buffer, +0x2c / +0x38 two axes spanning the portal plane (0x1005a350 and the cross product with the normal).
    A cell's portals lead out of it.
  - BSP nodes and leaves (lumps 25 / 26, 36 bytes each, loader 0x10004770) for the camera cell: node `plane, child0, child1` (a negative
    child c is leaf -1 - c); leaf dword 6 is its cell (-1 outside every cell).
  - Static models (0x100038c0): every entity whose classname is misc_model (case-insensitive) becomes a 0x60-byte static model
    (array 0x101ae380, world +0xf8), except models whose name starts with `xmodel/shadow_` (0x10003070). A model that fails to load or
    is not rigid (a surface of some LOD with more than one bone, 0x1002ad10) is replaced by `xmodel/default_static_model`. Keys: angle
    (yaw) else angles, modelscale_vec else modelscale (default 1); the scale used for culling and LOD is the third component
    (0x100031f0, uniform for modelscale). 0x1002ad90 fills: +0 cull distance = (exe 0x47dd60, the model's last LOD distance) * scale,
    +4 origin, +0x10 model, +0x14 / +0x20 world box (a box 0x1002a730 builds from the model and the rotation, then scaled and offset by
    the origin; 0x1002a730 was not read),
    +0x50 scale. 0x10003850 then distributes the static models over the AABB trees (0x10029fc0: a leaf holding 8 or more models is
    split into up to four child nodes by the x / y midpoints of their boxes; a node without surfaces takes the models' box).
- Per frame (0x1000a6c0, called from the scene setup with the camera cell and the plane list): the camera cell comes from the BSP node
  tree at world +0xc (0x10007da0: a point exactly on a plane goes to the back child; a leaf gives the cell index). With the camera
  outside every cell (index < 0) every cell is walked with the view planes. Otherwise (r_singleCell 0) the portal flood 0x1000a060
  starts at the camera cell; with r_singleCell only the camera cell is walked. r_skipPvs skips the world walk.
  - Portal flood (0x1000a060). Visiting a cell (0x10009e30) with a plane list walks the cell (below), then looks at each of its
    portals that is not on the chain of portals leading here: with v = normal . eye - distance, the portal is skipped when v > 0 (the
    eye is in front of it) or when all its vertices lie outside one plane of the list (0x10007e50: n . p + d <= 0 for every vertex).
    - v <= -0.125: the portal polygon is clipped by the plane list (0x10009ab0 -> 0x10007f30, one plane after the other: a vertex with
      e = n . p + d - 0.001 below -0.001 is outside, above 0.001 inside, else on the plane and kept as it is; new points are made only on
      edges from an inside to an outside vertex, at the fraction e0 / (e0 - e1); no inside vertex leaves nothing). The clipped points are
      added, in the portal's 2D axes, to the portal's point buffer (0x10009950; buffers of 64 points from a pool of 255, reduced to their
      convex hull when full). Inferred from the arguments passed on: before the plane list the polygon is first clipped by the plane of
      the portal through which the cell was entered (TODO(spec); OpenIW2 leaves that clip out, which only keeps more). The first time a portal gets points this frame it is queued with the chain that reached it; reached again
      through another chain before it is taken from the queue, it only gathers those points too and forgets its chain.
    - -0.125 < v <= 0 (the eye stands in the portal plane): when the eye lies inside the portal polygon (0x1005dc00, a 2D side test on
      the polygon projected along its dominant axis) the cell behind is visited at once with the same plane list.
    - The queue (0x100097b0 / 0x10009860) is a heap of at most 256 portals keyed by a linear function over the portal vertices
      (0x100066a0; inferred: the nearest depth along the view, so the nearest portal is taken first). The loop takes portals until the
      queue is empty, or until r_portalWalkLimit portals were taken (default 0: no limit; the counter starts at 1, so 0 never stops it).
    - A taken portal: the convex hull of its points (0x10057e40 picks the two extreme x points, 0x10058310 / 0x10058100 a quick hull with
      tolerance 0.001); no hull drops it. The hull points go back to 3D on the portal plane and become a plane list (0x10007470): for each
      hull edge the plane through the eye and the edge (normal = normalised cross product of the two eye-to-point vectors) with
      d = 0.001 - n . point, i.e. pushed 0.001 outward, edges with a zero normal skipped; then the view's near plane (moved up to the
      portal by 0x10006940 when that gives a positive amount, TODO(spec): which extreme 0x10006940 returns) and the far plane when there
      is one. With r_portalBevelsOnly, with more than 10 hull points, or with r_portalBevels > 0 (default 0.7) bevel planes
      (0x10007040) come first; with more than 10 hull points or r_portalBevelsOnly the edge planes are left out. TODO(spec): 0x10007040.
    - Small portals (0x10006de0, only when bevels or r_portalMinClipArea > 0): the hull points are projected with the view-projection;
      a point with w < 0.125 makes the portal count as large. Otherwise, when a quarter of the area of the projected bounding rectangle,
      or a quarter of the projected polygon's area (shoelace), is below r_portalMinClipArea (default 0.02, a fraction of the screen),
      the portal is small: the cell behind is walked with the new plane list and then, instead of queueing its portals, every cell
      reachable from it through portals that pass the same plane list (not behind the eye, a vertex inside every plane, a non-empty
      clip; 0x10009c00, each cell once) is walked with that same list (0x10009d30).
    - The chain flags of the taken portal and its predecessors are set while its cell is visited and cleared after; a taken portal
      returns its point buffer to the pool and may be queued again later in the frame.
  - Cell walk (0x100092f0): with r_drawWorld, the cell's root node is walked. With r_portalFineCull (dvar 0x101ce808, default 1) and a
    node with children, each child goes through 0x100081a0: a node outside a plane is dropped; a node fully inside every plane
    (0x10006600) draws all its static models (0x100094c0: distance cull only) and all its surfaces without further tests; a partly
    visible leaf tests each static model (0x100095b0) and each surface (0x10008110: the surface's own box against the planes) on its
    own; a partly visible inner node recurses. A root without children (or r_portalFineCull 0) goes straight to the per-model and
    per-surface tests. Every surface and static model carries a frame stamp (0x101d7e98, 0x101d7e94), so one reached through two cells
    is added once. Scene entities linked to the cell follow (0x100083e0).
  - Static model test (0x100095b0): when the cull distance is not 0, `|lod origin - model origin| * lodScale + lodBias` (distance
    0x10059120, lod values below) greater than the cull distance drops the model for the frame (equal keeps it); then the model box
    against the planes (a box outside does not stamp the model, so another cell may still add it). A model that passes is added through
    0x10023fe0 / 0x1001f950, which picks its LOD (Model LOD below).
- Sky and the far plane: when there is a far plane, 0x1000a7c0 adds every sky surface (the sky list, world +0x18 / +0x1c) once more,
  tested against the plane list without its last (far) plane, so the sky is never removed by the far plane.
- Frame clear (0x10024e10 -> 0x10024d70, render command 0x100302b0): once a fog record has been blended (0x101d61c8, set by the blend
  0x100244d0), the clear colour is the current fog colour (bytes 0x101d61b8..ba; all zero, i.e. black, while record 0 "no fog" is
  selected), else r_clearColor (colour dvar, default 0 0 0 0); alpha 1. So beyond the far plane the screen shows the sky where sky
  surfaces lie behind, and the fog colour (black without fog) elsewhere.
- OpenIW2 (iw2view `Renderer::cullWorld`, `game/portalvis.h`): the camera cell from the BSP nodes; outside every cell (or with
  `IW2_NO_PORTALS=1`) every world surface (soup) box and every static model is tested against the view's plane list; inside a cell the
  portal flood above gives a list of (cell, plane list) visits and a surface is drawn when its box passes the plane list of a visit of
  its cell (its tree or cull group); a static model when its box passes the plane list of a visit of any cell whose bounds its box
  overlaps (TODO(spec): the original links each model to one AABB-tree node, 0x1002a620, not read). Testing each surface's own box
  gives the same set as the tree walk, which only skips tests for boxes inside a node box that is inside every plane. Plane lists are
  the four side planes, the near plane and, with a far distance (setculldist / linear fog far), the far plane; the sky is drawn by its
  own pass without any cull. Not implemented, all only ever drop more: bevel planes and bevel-only hulls (OpenIW2 always uses the
  edge planes), the near plane moved up to the portal, occluders; a flood that reaches 4096 taken portals falls back to the plane test
  for the frame (OpenIW2 safety limit; the original has none). The static model box is the box of the model's transformed vertices
  over all its LODs (flag surfaces padded by the largest cloth wave offset, 12.2 units times the scale), not the rotated model bounds.
  The frame is cleared to the current fog colour (black without fog). `IW2_NO_CULL=1` disables the surface and model culls (static
  models then draw LOD 0 at any distance). Tests: tests/test_viewcull.cpp, tests/test_modellod.cpp, tests/test_portalvis.cpp.
- Dynamic lights (0x10015210 -> 0x10014fe0): for each of at most r_dlightLimit lights, every surface of the frame's draw list (walked
  backwards) whose technique set has a point light technique and whose box (world surfaces: the draw surface box; models: the type's own
  box function, table 0x101a5a8c) satisfies `distance^2(light origin, box) <= radius^2` (0x1005a670) is added to the light's surface
  list; only those surfaces get the light pass. The box functions of table 0x101a5a8c by draw surface type: 3 and 4 (xmodel surfaces)
  the scene entity's world box (0x106570d8 + entity * 0x34, for a static model the box copied by 0x10023fe0), 5 the static model box
  (world +0xf8 record +0x14), 6 (world triangles) the draw surface box, or for a brush model entity that box taken through the entity
  transform (0x1005ccc0). OpenIW2 applies the test to world soups, static models (their instance box) and xmodel items (the box of the
  drawn vertices through the item transform, in place of the entity box of 0x10008500, which was not read); the view model and brush
  model items still get every light. Beyond the radius the l_point falloff ramp is 0, so the test drops no light.

## Model LOD

- Exe xmodel side (reached through the renderer import table, slots 0x18c, 0x190, 0x188 of the table the exe passes to GetRefAPI):
  - The LOD count (model +0x7c) counts the LOD names up to the first empty one; LOD i keeps its distance at +4 + i * 0x14.
  - LOD for a distance (0x47dd90): the first LOD i whose distance is 0 or greater than the distance; -1 when there is none (the model
    is not drawn). A test override (below) replaces a LOD's distance when enabled.
  - Last LOD distance (0x47dd60): the distance of the last LOD (or its override); the static model cull distance above.
  - Test overrides (0x47ddf0, per LOD: distance stored, enabled when >= 0): set every frame by the renderer (0x10022210) from
    r_highLodDist, r_mediumLodDist, r_lowLodDist, r_lowestLodDist (floats, default -1 = off, range -1..FLT_MAX); r_forceLod (enum,
    default 4 = none) instead sets 0 for the forced LOD and 0.001 for the others (so the forced LOD is the first to qualify).
- Renderer per scene (0x10025b10): lod origin = the view origin (refdef +0x18; r_lockPvs keeps a locked one), lodScale = r_lodScale
  (default 1, range 1..4), lodBias = r_lodBias (default 0, range -1000..0); when the refdef fov_x is not exactly 80, both are multiplied
  by tan(fov_x / 2) / tan(40 deg) (instruction bytes de f9 at 0x10025bc2: FDIVP st(1), st(0), so a narrower view lowers the distance and
  raises the detail).
- LOD distance of a model instance (0x1001dbd0): `(|lod origin - origin| * lodScale + lodBias) / scale`, the division skipped for a
  scale of 0. Static models use their origin and scale (0x10023fe0 copies them into the scene model record); scene entities their
  refEntity origin (+0x3c) and scale (+0x38). For a DObj (0x1001dc60) every attached model gets its own LOD from that one distance
  (exe 0x474520 -> 0x47dd90).
- OpenIW2: `game/modellod.h` (lodForDistance, lodOutDistance, lodView, lodDistance, staticModelDistanceCulled), tests/test_modellod.cpp.
  iw2view bakes every LOD of each static model and draws the one picked per frame; CPU-skinned entities skin the LOD picked from the
  entity origin (scale 1; TODO(spec): the refEntity scale the cgame sets). r_lodScale / r_lodBias are read from the cvar store
  (`--cvar`), the test overrides and r_forceLod are not implemented. TODO(spec): the fov the refdef carries on a non-4:3 screen;
  OpenIW2 uses the 4:3 field of view (cg_fov and the ADS zoom). Static script models drawn as entities without skinning still draw LOD 0.

## Entity cull

- Xmodel scene entities (0x100087e0): the world box (0x10008500) is the DObj box from the exe (import slot 0x1f8 -> 0x473800, the box
  stored at DObj +0x4c / +0x58), times the refEntity scale (+0x38), plus the refEntity origin (+0x3c), with no rotation. The DObj box
  (0x473130, when the DObj is built) is the sum over its models (the body and every attachment) of each model's bounds: xmodel +0x64 /
  +0x70, the mins / maxs of the xmodel file header (copied by the loader at 0x47d7df..0x47d827 from the header parse 0x47cd90). Summing
  covers attachments placed anywhere on the body. The box is tested against the scene plane list (0x10006420: side, near and far planes,
  then occluders); a box outside is not drawn. A box no larger than 1536 units in two axes is then linked to the cells it touches
  (0x10008640) and drawn by those cells' walks (0x100083e0, another box test against the cell's plane list); a larger box is drawn at once.
- Example: the duhoc_assault cliff ropes (duhoc_rope_climb_cliff_rig and others) have header bounds of +-1400 units, so they stay drawn
  while their origin is far off screen.
- OpenIW2 (iw2view gatherItems): the CPU skinning of actors and animated script models and the drawing of plain xmodel entities are
  skipped when that box (header bounds of the body and its attachments summed, scale 1) is outside the four side planes of a 32:9 view
  (wider than any window), the near plane, or the far plane. Vehicles and turrets are not culled (their drawn tags feed bolted
  effects). The cell link is not implemented (it only drops more). `game::dobjCullBox` (modellod.h), tests/test_modellod.cpp.
- OpenIW2 implementation (no behavior of its own): the drawn entities' poses are still evaluated every client frame, as 0x4afb70 advances
  every entity's anim (docs/spec/vehicles.md "Client anim time"). Their skinning runs on all cores into one shared vertex buffer uploaded
  once per frame, from per-model data built at load (`assets::SkinStream`, the same arithmetic as skinVertex / skinTangents). An
  animation's bone-to-skeleton name match is cached per skeleton (`XAnimBoneMaps`). Tests: tests/test_xmodel.cpp, tests/test_xanim.cpp.

## setshadowhint

- Builtin 0x4ef700 stores entity-state bits 0x2000..0xe000 (names and values: `shadowHintBits`, fog.h).
- Client: 0x4c1e2a / 0x4aacb0 map the bits to refEntity render flags: never 0x200, low priority 0x400, high priority 0x600, always 0x800,
  receiver 0x100.
- Renderer: the shadow-cookie system (gated by dvar sc_enable, dll 0x101ce7cc, and r_rendererInUse != 2) selects casters in dll 0x10028150:
  entities flagged never (or flag 8) are skipped; `score = distance / largest bounds extent`; always -> 0, high priority x0.25, low
  priority x4.0; scores >= 1e8 are skipped; the 24 lowest scores are sorted (qsort) and rendered by the cookie pass 0x10028390.
  OpenIW2 has the score as `shadowCasterScore` (fog.h, tested).
- Cookie budget (dll 0x10028390, called by 0x10028530 after the selection 0x10028150 and a qsort of the 24 entries ascending by score,
  comparator 0x100275c0; reset 0x10028570 sets T = 10.0 and last-time = -1). Walking the sorted list: stop at entity -1 (no budget
  change); a caster with score > T ends the walk and the count i is compared with sc_wantCount +- sc_wantCountMargin (i above the band: shrink,
  below: grow; inside: unchanged); all 24 used also shrinks. Casters with score <= T are rendered by 0x10027b20 with a fade factor: 1 up to
  (1 - sc_fadeRange) * T (and for score 0), then (T - s) / (T * sc_fadeRange). Per frame, dt = max((now - last) * 0.001, 0); shrink: `T -= dt *
  sc_shadowInRate`, grow: `T += dt * sc_shadowOutRate`; T is raised to 0.0001 if lower.
  Dvars (registered 0x1000b5ce..): sc_enable bool 1; sc_wantCount int 12 (0..24); sc_wantCountMargin int 1 (0..24); sc_fadeRange float 0.25 (0..1);
  sc_shadowInRate 2 (0..20); sc_shadowOutRate 5 (0..20); also sc_blur, sc_count (int, default 24), sc_length (400, 1..2000),
  sc_offscreenCasterLodBias, sc_showDebug, sc_showOverlay, sc_debugCasterCount, sc_debugReceiverCount.
  OpenIW2 has this as `stepShadowCookies` (src/game/shadowcookie.h, tested).
- Not implemented: the cookie render itself (0x10027b20: per-caster cookie texture render and receiver projection). TODO(spec): that
  function, the sc_blur / sc_length / sc_offscreenCasterLodBias use, and the receiver pass; no shadow rendering in iw2view (`dynamicShadow`
  stays 1), so nothing is verified visually.

## Exponential fog: density and distance

- The script's setExpFog density goes unscaled into the fog record's slot +0x18 (R_SetFog, gfx 0x1000bb90; the exponential type is chosen
  when the command's third value is below 1). The per-frame blend 0x100244d0 lerps the three floats at +0x10 / +0x14 / +0x18 (near, far,
  density) and the colour bytes with the same fraction, so density interpolation between two exponential fogs is linear in time. A change
  of fog type takes the target at once (0x100244fa).
- Constants 0x10034db0: exponent = -density; the shaders compute exp(dist * exponent) from the radial eye distance in world units
  (lib/fogcalc.hlsl, Fog_ExponentialFactor). So 50% fog lies at ln 2 / density: 0.00002 (hill400_defend start) 34657 units, 0.00035
  (moscow) 1980, 0.00075 (hill400 barrage) 924. A shipped value that small is nearly invisible by design; OpenIW2 measured the same
  blend factors (fog colour mix 1 - exp(-density * d)) in headless shots, and the state reaches the renderer on moscow, hill400_defend, beltot.
- Sky is drawn with the no-fog technique. Multiply decals: see below. Additive surfaces: see "Additive fog".

## Additive fog

- The fogged additive techniques (lmap_add_lfog / _efog, lprobe_add_*, effect_add_*) fog through the fixed-function fog stage (their
  state maps leave the fog state to the material). The stage's colour is set by one small function of the 1.3 renderer (0x10035050,
  called from 0x10034db0, 0x10039d4b and 0x1003a0dc): with argument 2 it is black, with argument 0 it looks at the blend state word 0
  of the pass (the cached word at 0x10744c00, material file +0x2c) and is black when the destination-blend field (mask 0xf0) is One
  (0x20), otherwise the scene fog colour (0x1070c4c8, alpha 0xff); it does nothing while no fog record is selected (0x101d620c is 0).
- Consequence: lerp(black, c, f) = c * f, so an additive surface fades out with distance instead of turning fog coloured (a coloured
  fog would add its own colour on top of the scene). Every other pass keeps the fog colour.
- OpenIW2: `fogColorIsBlack` (src/game/fog.h, test_fog) and `Renderer::applyFog(fu, pass)`: world and model batches of Pass::Additive
  fog towards black; effect sprites still draw with the no-fog technique. TODO(spec): the argument-2 caller 0x1003a0dc (it passes 2 when
  a byte of its pass record at +8 is nonzero; which pass kind that is was not identified), so no pass here uses it.

## Multiply decals in fog

- effect_multiply.techset maps its linear / exp fog techniques to vertcol_mul_lfog / vertcol_mul_efog, which have two passes:
  pass 1 is the ordinary multiply (state map nofog, mul.hlsl: Cm = (1 - a) + texture * rgb * a, framebuffer * Cm), pass 2 uses
  state map multiply_fog (blend Add One One, depth write off, depth test and polygon offset from the material, no fixed-function fog)
  with lib/multiply_fog.hlsl: it adds (1 - Cm) * fogColor * (1 - f), f = the fog factor of the vertex. The library's own comment
  derives it: the multiply darkened the already fogged pixel, and the added term turns the result into "multiply, then fog".
- Without pass 2 a multiply decal far away darkens a facade that should be fully fogged: the trainyard grime decals
  (stalingradwinter_wall_grime04_decal, 591 triangle soups, sort 12, polygon offset 1) showed as dark patches through the fog.
- OpenIW2: iw2view draws world Multiply batches a second time with the additive state and the fix-up shader branch while fog is active.
  The fog factor is taken per pixel (the shipped vertex shader computes it per vertex). Effects draw with the no-fog technique
  (effects.md), so effect sprites get no fix-up.

## Fog across a savegame load

- R_SetFog (gfx 0x1000bb90) writes one of five 0x20-byte fog records at 0x101d616c (index * 0x20: type, near, far, density, colour);
  R_SetFogTransition (0x1000bc20) selects a record (index at 0x101d620c) and starts the blend. The client fog command uses record 1;
  record 0 (all zero after R_ClearFog 0x1000bb70, refexport slot 0x88, called from the client init 0x401520) is "no fog", and the
  cgame init 0x4a5c10 selects it at once (R_SetFogTransition(0, time, 0)).
- Refexport slot 0x84 = DLL 0x1000bca0 passes the 0xa0 bytes of records and the 4-byte selected index to the save stream's
  read / write callback (+0x10). The exe calls it at the end of 0x402d60, which runs on save (G_WriteGame, 0x4e3804) and on load
  (0x4e4c11). So the fog survives a savegame load, while the level script's setCullFog / setExpFog (in main(), not run again on a
  load; the shared sender 0x4f5512 also skips the command while 0x1c61048 is 2, the save-loading state) does not resend it.
- The AI fog sight limit (0x1c61038) is saved too (0x4e36cd) and loaded (0x4e4aab).
- OpenIW2: the fog state lives in World::clientFog (fed by iw2view from the "fog" client commands) and the save stream (version 6)
  stores its selected record and the sight limit; a load makes that record current at once. TODO(spec): a fog transition running at
  save time (the records are archived, the running blend 0x101d61cc / 0x101d61ec / 0x101d61f0 is not, and how the client picks the
  restored record after the load was not traced).

## Decals on the triangles of their surface

- Some world decals reuse the exact triangles of the surface under them and have polygon offset 0, e.g. the trainyard pipe chalk
  (stalingradwinter_decal_gothisway / _gothisway2, technique set effect_add, state word 1 = 0x4): every triangle of both decals is also
  a triangle of stalingradwinter_pipe_b (phong_replace_spec_detail) and of stalingradwinter_wall_grime04_decal. Such a decal passes
  the LessEqual depth test only when its depth is bit-identical to the surface's, i.e. the clip position is computed the same way
  in every vertex shader.
- The same holds for every overlaid pass: the l_point light passes (with their own vertex shader, also over lmap_s
  surfaces) and the multiply fog pass.
- OpenIW2: all world / model vertex shaders of iw2view compute the clip position through one function (world_clip) written with
  explicit fma steps. A shared source expression alone did not give equal depth: fast math rounded it differently per vertex
  function, so overlays on lmap_s surfaces passed LessEqual only on part of each triangle, and which part changed with the view
  (flickering chalk, and walls flickering under the muzzle flash light). `IW2_DEPTH_PROBE=1` shows such pixels.

## Scene projection (1.3 DLL 0x1005bfe0, 0x1002fbbf)

- 0x1005bfe0 builds the projection from the two fields of view and the near value, with k = 0.99950027: _11 and _22 are the
  cotangents of the half angles times k, _33 = k, _34 = 1, _43 = -near · k. There is no far plane: a view distance d maps to
  depth k - near / d, always below 1.
- Depth classes (group flags & 0x18, table 0x1002fcd8): class 0 uses depth range 0..1, class 0x8 uses 0..0.2 and 0x1003e6a0 sets
  _43 to -r_znear_depthhack, without the factor k; classes 0x10 and 0x18 use 0..0.5.
- The build-floatz list is submitted through 0x10021cf0, the same draw-list command (0x17) as the other lists, with technique type 2,
  and the class switch is in the walker 0x1002f830. TODO(spec): inference, not traced: that command 0x17 reaches this walker (the
  dispatch was not read), hence that the view model enters the floatz list with its range and projection. OpenIW2 draws it so.
- OpenIW2: `game/viewprojection.h`, test_viewprojection.

## View model precision (OpenIW2 renderer measure)

- The view model is attached to the eye; on screen it is a function of the view-model offsets and the view angles only. iw2view
  used to place it at the eye plus the offset in world coordinates and project it with the world view matrix. On a map with
  coordinates in the tens of thousands (float step 0.004 at 50000) the model translation and the view translation are both rounded
  at that size, so the depth gap of near-coplanar parts (the button on the sleeve) changed from frame to frame as the gun swayed
  and the eye moved by hundredths of a unit; the part flickered between lit and covered.
- The view model's draw items now carry a camera-relative transform (offset from the eye) and are drawn with a view matrix whose
  eye is the origin (`applyViewModelSpace` in apps/iw2view/main.cpp, offset from `viewModelOffset`, src/game/viewspace.h). The
  eye uniform and the l_point light positions of these items are shifted the same way, so distances (fog, specular half angle,
  light falloff) are unchanged. `IW2_NO_DEPTH_HACK=1` (view model without the depth range) draws it in world space as before,
  since the camera-relative path is taken only by depth-hack items. Test: tests/test_viewspace.cpp. TODO(spec): the original's own handling of this rounding.
