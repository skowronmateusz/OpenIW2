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
- Not implemented: the far plane on world surfaces and entity models (needs per-surface and per-entity bounds; iw2view merges world
  geometry per material and has no CPU culling, so a far cull there would be a guess about cull-group granularity).
  TODO(spec): the r_zfar default (taken as 0), the exponential-fog contribution to FarDist, the near plane.

## setshadowhint

- Builtin 0x4ef700 stores entity-state bits 0x2000..0xe000 (names and values: `shadowHintBits`, fog.h).
- Client: 0x4c1e2a / 0x4aacb0 map the bits to refEntity render flags: never 0x200, low priority 0x400, high priority 0x600, always 0x800,
  receiver 0x100.
- Renderer: the shadow-cookie system (gated by dvar sc_enable, dll 0x101ce7cc, and r_rendererInUse != 2) selects casters in dll 0x10028150:
  entities flagged never (or flag 8) are skipped; `score = distance / largest bounds extent`; always -> 0, high priority x0.25, low
  priority x4.0; scores >= 1e8 are skipped; the 24 lowest scores are sorted (qsort) and rendered by the cookie pass 0x10028390.
  OpenIW2 has the score as `shadowCasterScore` (fog.h, tested).
- Not implemented: the shadow-cookie pass itself (no shadow rendering in iw2view; `dynamicShadow` stays 1). TODO(spec): the pass
  (shadow-cookie textures and techniques exist in the shipped assets), the sc_* dvar defaults.
