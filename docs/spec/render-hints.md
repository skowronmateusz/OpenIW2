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
  the fov), then 0x100062e0 appends the near plane (pointer 0x101ce5b0) and, only when FarDist != 0, the far plane (0x101ce5b4);
  an orthographic view (0x1000a590, view +0xc4 != 0) has no far plane. Plane record = 20 bytes `{n[3], d, 3 corner-select bytes, cache byte}`.
  - Near (0x1000a400): `n = forward, d = 0.1 - forward . eye` (constant 0x1019bf48 = 0.1).
  - Far (0x1000a4e3): `n = -forward, d = far + forward . eye`.
  - Box test (0x10006420 / 0x10006340 / 0x100064f0): per plane the corner furthest along n is picked (max if n > 0 else min); the box is
    culled when `n . corner + d <= 0` (equality culls). The cache byte (+0x13) only skips planes the parent box was already fully inside;
    it does not change the result. After the planes, occluder hulls (0x101ce5c0 / 0x101ce5c4) can cull a box that lies fully inside one.
  - Granularity: the same list culls world AABB-tree nodes (0x10008110, node box at +4, then the node's surface range is emitted by
    0x100247d0), cell boxes (0x100093e0 / 0x100092f0, cell stride 0x20 at world +0xf0), static models by their instance box (0x100083e0),
    xmodel scene entities (0x100087e0: world box from 0x10008500, then 0x10006420, then a small-object test) and brush-model entities
    (0x10009210: box from 0x10008900). So there is no per-surface test: a world surface is far-culled with its AABB-tree node.
  - OpenIW2 has the pure planes and box test as `viewcull.h` (tests/test_viewcull.cpp).
- Wired into iw2view only for brush-model entities (`Scene::brushBounds` = local bounds of the model's drawn soup vertices, enclosing world box
  via `transformedBoxBounds`, then `boxCulledByDepthPlanes` with `farPlaneDistance`; `IW2_NO_CULL=1` disables). Blockers for the rest: the
  static world is one merged vertex buffer per material with no AABB-tree node ranges (lumps 10 / 11 not mapped to nodes), static models
  are merged, and xmodel entity `DrawItem`s carry no local bounds (xmodel bounds were not read). Needs a human visual check: a brush
  model (door / elevator) beyond a setculldist or fog far must disappear, and none may pop out near the screen edge. TODO(spec): the r_zfar default (taken as 0), the exponential-fog contribution to FarDist, the four side planes, the loader that
  turns lump 10 / 11 (cull groups, runtime copy at 0x101ae378) into the AABB-tree nodes used above (not decoded; the runtime layout
  copied by 0x10004340 is mins[3], maxs[3], count, first index).

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
- Sky is drawn with the no-fog technique; Multiply / Additive passes remain TODO(spec).
