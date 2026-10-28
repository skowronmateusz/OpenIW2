# Effects (`fx/*.efx`)

Source: the effect parser `FUN_00489650` in CoD2SP_s.exe 1.3, channel parser
`FUN_00489450`, curve readers `FUN_00489270` and `FUN_00489340`, graph builder `FUN_0047ed80` and graph integral `FUN_0047ecf0`. All 375 shipped files (1,376 elements) parse with `iw2asset <main> efx-check`.

## Text layout

A file is a list of elements `Type { … }`. Types seen: Particle, Tail, Emitter, FxRunner, Decal, Light,
OrientedParticle, Cloud, CameraShake, Line. Inside an element each line is `key value…`, a name list
`key [ name … ]` (`shaders`, `models`, effect references), or a channel group `key { curve [ rows ] scale a [b] }`.

Element keys handled by the parser: `count` (one or two numbers), `shader(s)`, `model(s)`, `impactfx`, `deathfx`,
`emitfx`, `playfx`, `cullrange`, `spawnRange`, `delay`, `bounce`/`intensity`, `life`, `angle(s)`, `angleDelta`,
`velocity`, `acceleration`/`accel`, `gravity`, `density`, `variance`, `origin`, `origin2`, `radius`, `height`,
`rotation`, `flags` (table at 0x56d528, starting `depthHack`), `spawnFlags` (table at 0x56d648, starting
`org2fromTrace`), `nonUniformScale`, `useLength`, `name`, `shaderImpact`, `sequenceStartFrameMode`,
`sequenceFixedFrameValue`, `sequencePlayRateMode`, `sequenceFixedFpsValue`, `sequenceLoopMode`,
`sequenceLoopTimes`, `spawnFrustumCullRadius`; other keys are a parse error ("Unknown key parsing an effect").
Channel groups: `rgb`, `rgbRand`, `alpha`, `alphaRand`, `size`/`width`, `sizeRand`, `size2`/`width2`,
`size2Rand`, `length`/`height`, `lengthRand`, `rotationDelta(Rand)`, `velocity{X,Y,Z}(Rand)`,
`velocity2{X,Y,Z}(Rand)`. A channel group accepts `curve`, `scale`, `start`, `end`, `parms`, `flags`.
The element's bounding box (mins/maxs at +0xa0…+0xb4) must be ordered and not wider than tall.

## Curves

`rgb` and `rgbRand` rows are read as `%f %f %f %f` (time, r, g, b); every other channel as `%f %f` (time, value),
extra numbers on the row are ignored. `scale` reads one or two numbers (one means min = max). The graph builder
adds a point at time 0 when the first row's time is not 0 and a point at time 1 when the last row's time is not 1;
both new points copy the values of the file's first row. `FUN_0047ecf0` integrates a graph up to t (area under
the piecewise-linear curve), used for velocities.

## Legacy channel syntax (start / end / parm / flags)

Seven shipped files (minefield, elalamein_trenchdust, large_smoke_plume, fireheavysmoke, flare, grenade_flash,
coolaidmanbrick) describe a channel with keys instead of a curve. The channel parser (0x489450) writes them into a
24 × 0x40-byte scratch record per element (start 3 × {min, max} at +0, end at +0x18, parm at +0x30, flags at +0x38, "set"
byte +0x3c), initialised to start = end = parm = 1 (0x4878c0). `start` / `end` of rgb (channel index 0) read 3 numbers
(fixed) or 6 (min xyz, max xyz) unsorted (0x488c30 / 0x488ca0); other channels read "%f %f" sorted (0x488d10 / 0x488d90);
`parm`/`parms` one or two numbers (0x488e10); `flags`/`flag` up to four names (0x487a30, names at 0x57d1d8): linear 0x1,
random 0x2, nonlinear 0x4, wave 0x8, clamp 0xc. Channel indices follow the group key order (0x489d92..): rgb 0, rgbRand 1,
alpha 2, alphaRand 3, size 4, …, velocity2ZRand 23.

When the element is complete, 0x487910 converts every touched record (overriding a `curve` of that channel) with 0x47eb10,
mid(r) = (max − min) · 0.5 + min:
- M = 1 for alpha; otherwise M = max(1, larger of mid(start), mid(end) · 4/3) (component 0, 0x59212c). Scale range =
  M ∓ (start.max − start.min) / 2. Start / end / parm values are divided by M; p = 0.01 · parm/M, w = parm/M · mid(life) ·
  0.0031415929 (0x592158).
- linear only (0x47e780): rows {0, start}, {1, end}. Nonlinear only (0x47e7f0): {0, start}, {p, start}, {1, end}.
- anything else, including no flags (0x47e870): 16 rows at t = k/15 (0x5920a4, clamped to 1); f = 1 (1 − t with linear);
  nonlinear g = t > p ? 1 − (t − p)/(1 − p) : 1, clamp g = t < p ? (p − t)/p : 0, f = linear ? (f + g)/2 : g; wave
  f ·= cos(t · w); value = (1 − f) · end + f · start, 0 when negative, 1 when f > 1; random multiplies by a generator draw.
- OpenIW2: `src/assets/efx.cpp` (`legacyGraph`). TODO(spec): rgbRand converts with one component (left as parsed), the
  generator state for `random` at load time.

## Line elements (type 2)

- Spawn 0x48d3c0 (vtable 0x57c708, 0x258 bytes): A (+0x4) = the spawn shape point (0x48c290); B (+0x24c) from 0x48c850:
  origin2 ranges drawn z, y, x (+0xe0 / +0xd8 / +0xd0), world axes with cheapOrg2Calc 0x80 else x·row0 + y·row1 + z·row2
  of the instance axes (0x424a70), plus the effect origin; material from `shaders` (0x485110); random lerp draws for
  useRandomColors / Alpha / Size / Size2 (+0x118..+0x124). With org2fromTrace 0x8 B comes from a trace (not implemented).
- Update 0x482da0: +0x7c = A, +0x9c = B (through the bolt for relative), then size (0x4818b0 → +0x88), rgb, alpha. No
  frame step. Draw 0x482d90: renderer type 8, the Tail quad (half width = size).
- OpenIW2: `FxSystem` emits a `beamTail` sprite. TODO(spec): the frame value (+0x94) of a Line (never written by its spawn
  or update), org2fromTrace / traceImpactFx, the bolt transform of B. The only Line effect (minefield.efx) names materials
  that do not ship and is not played by any script.

## Cylinder elements (type 4, no shipped effect uses one)

- Update 0x483450: size → +0x88, size2 → +0x8c (= size without nonUniformScale), length → +0x258 (0x4830d0), rgb, alpha;
  A (+0x7c) = position, B (+0x9c) = position + length · axis row 0 (+0x48), both through the bolt. Draw 0x483440: renderer
  type 9. Cull 0x4833f0: capsule A–B (0x486ef0) with the two radii.
- DLL 0x100401f0 (0x100405e0 when the dvar at 0x101ce710 is 2): n = clamp(round((1 − (|mid(A, B) − view| · [0x1070c3d8] +
  [0x1070c3dc]) / 1024) · 32), 8, 32) segments, angle step 2π / n, ring vertices around A and B from a perpendicular basis
  (0x10059bd0) scaled by ent+0x68 … TODO(spec): the rest of the ring / texcoord build and the spawn 0x48d6c0; not implemented.

## Flags

`flags` and `spawnFlags` names map to bits through one table of `{name, element flag, spawn flag}` (0x56d528):
depthHack 0x1, relative 0x2, setShaderTime 0x4, useModel 0x10, usePhysics 0x20, useBBox 0x40, useAlpha 0x80,
emitFx 0x100, deathFx 0x200, impactKills 0x400, impactFx 0x800, blocksSight 0x1000, useRandomColors 0x2000,
useRandomAlpha 0x4000, useRandomSize 0x8000, useRandomSize2 0x10000, useRandomLength 0x20000,
useRandomRotationDelta 0x40000, useRandomVelocity 0x80000, useRandomVelocity2 0x100000, absoluteVel 0x200000,
absoluteVel2 / absoluteAccel 0x400000, affectedByWind 0x800000, disableFarPlaneCulling 0x2000000; spawn flags
orgOnSphere 0x1, axisFromSphere 0x2, orgOnCylinder 0x4, org2fromTrace 0x8, traceImpactFx 0x10, org2isOffset
0x20, cheapOrgCalc 0x40, cheapOrg2Calc 0x80, randrotaroundfwd 0x100, evenDistribution 0x200, frustumCull 0x400.
The spawnFlags parser uses the same table from its `org2fromTrace` row (0x56d648).

Implemented: `src/assets/efx.{h,cpp}`. Not specified yet: the runtime (spawning, element update, drawing) and the
meaning of the flag bits.

## Runtime (partial, `src/game/fx.cpp`)

Particles and oriented particles are spawned by `playfx`, `playfxontag` and `playloopedfx` and drawn as
camera-facing sprites in `iw2view`. Position, gravity and channel evaluation follow the evidence in
the executable. Inferred and marked TODO: spawn shapes (sphere/cylinder, `radius`, `height`),
the per-particle velocity scale, sprite size meaning (half width),
other element types (CameraShake, Cylinder; see the sections below for the decoded ones), and the argument order of
the script built-ins (taken from the shipped scripts' calls).

## Visibility through effects

`getfxvisibility(start, end)` (0x4f53b0 → 0x48b090) returns the visibility (0..1) through sight-blocking effects:
- The blocker list (0xc3ed80, 20-byte entries, count 0xc47a20) is rebuilt after every effects update (0x48b000,
  called at 0x48c1ec): emptied, then every live element whose flags (+0xa8) have `blocksSight` 0x1000 adds itself
  through its vtable slot +0x18 (0x480970, shared by all element vtables): position (+0x7c), size² (+0x88, the size
  channel, 0x4818b0) and 1 − alpha byte (+0x93) · (1/255). The alpha byte (0x481a40) is the clamped alpha channel ·
  255 truncated (`_ftol`) with `useAlpha` 0x80, else 255 (rgb is scaled by alpha instead), so a blocker without
  `useAlpha` blocks fully.
- With no blockers, or when |end − start| is below the dvar `fx_visMinTraceDist` (0x617eb0; float, default 80,
  range 0..1000, registered at 0x40e544), the result is 1.
- Otherwise, with d the unit direction and t = (blocker − start)·d, every blocker with |t − len/2| ≤ len/2 and
  squared distance from start + d·t below its size² multiplies the result by its 1 − alpha/255.

`sighttracepassed` (0x4f2580) also fails a clear trace when this visibility is below 0.2 (0x592020).

In this engine `FxSystem::sprites` rebuilds the list from its live particles and `iw2view` hands it to the world
(`World::setFxSightBlockers`); `iw2game` runs no effects, so its list stays empty. TODO(spec): particles still in
their spawn delay are left out (the original lists every element in its two element arrays, 0x5a39e0 / 0x5a39dc).

## Bullet impacts (server 0x531340, client 0x4b81d0 / 0x4b82f0)

- Bullet_Fire_Extended sends an impact event at the trace end when the trace hit something (fraction < 1) whose surface
  has no surface flag 4, except an AI hitting a sentient of its own team. Surface type = surface flags >> 20 & 0x1f,
  7 (flesh) for a sentient hit with type 0.
- Hit on a client: event 0xbb (0xbc with `rifleBullet`, weapon +0x31c); the client plays the sound
  `bullet_small_<surface>` / `bullet_large_<surface>` at the point.
- Otherwise event 0xb8 / 0xb9 (`rifleBullet`) / 0xba (`armorPiercing` +0x320, 0x531310) with the trace normal (+4)
  and the normalized bullet direction reflected off it, `d − 2(d·n)n` (+3, 0x59216c = −2). The client plays the sound
  of `bullet_small` / `bullet_large` / `bullet_ap` for the surface (tables 0xf5b85c / 0xf5b8b8 / 0xf5b914 built by
  0x4a27b0 as "%s_%s") and the impact table effects `<type>_normal` along the normal and `<type>_reflect` along the
  reflection (FUN_00485220). Flesh with `cg_blood` (0x1040c58, bool, default 1): the table effects at most once per
  330 ms (0x14a, last time 0xf7e8c0), otherwise `fx/impacts/flesh_hit_noblood.efx` (0xf5bcc4) along the normal only.
- Then the tracer (0x4b8140, `cg_tracerChance`). The impact effects carry the bullet marks as Decal elements.
- OpenIW2: `World::bulletImpact` (fx events + sounds); TODO(spec): dir-byte quantization, tracers, penetration
  (surface flag 0x10).

## Marks (Decal elements)

- Element type names (0x48683e): `decal` = type 6. Spawn of one decal (0x48da90 → 0x485b60): position from 0x48c290
  (not read), rotation random in the `rotation` range (+0x220, parser 0x488020), colour (0x4857d0: rgb at graph time 0
  times a random scale, lerped to rgbRand with useRandomColors), alpha (0x4858c0, useRandomAlpha, clamped 0..1), size
  (0x485a30, useRandomSize), material random from `shaders` ((seed >> 17)·count >> 15), then CG_ImpactMark (0x4ad430)
  with the effect's axis as direction (EDX = the effect, 0x485bd9).
- CG_ImpactMark (0x4ad430), only with `cg_marks` (0xf48b40, bool, default 1) and 0xf79778 clear:
  a0 = dir normalized (0x421270), a1 = PerpendicularVector(a0) (0x421f70: basis vector of the smallest |component|
  minus its projection, normalized) turned around a0 by the rotation (0x421550), a2 = a0 × a1; the square
  o − r·a2 − r·a1, o + r·a2 − r·a1, o + r·(a1 + a2), o − r·a2 + r·a1 goes to the renderer's MarkFragments (refexport
  0x617ab0 + 0xe8; 1.3 DLL FUN_10015df0) with at most 0x400 points and 0x180 fragments; each fragment
  becomes one pool mark, its vertices coloured with the rgba bytes (0x4ad1b0, round(c·255) clamped).
- MarkFragments: box = corners ± r·a0 (FUN_10015c70); world surfaces touching it (cells FUN_100157f0, AABB trees
  FUN_10015640; skipped when the material byte +0xc has bit 4, set on skies, water and clip materials; deduplicated
  FUN_100157c0); per surface with no 0x30 bits in material byte +0x30 (mtlPolygonOffset, i.e. no decal layers) each triangle
  (i0, i1, i2) with n = (v0 − v1) × (v2 − v1), n·a0 ≥ 0 and |n|²/4 ≤ (n·a0)² is clipped (FUN_10015a80 →
  FUN_100154b0: ±0.5 on-plane band, FUN_10015430 lerps position, lightmap uv, normal) by the four edge planes
  ((c_i+1 − c_i) × a0 normalized, through c_i) and a0·p ≥ a0·c0 − r, −a0·p ≥ −a0·c0 − r. A fragment that does not
  fit the remaining points is dropped; the walk stops at 0x180 fragments or when fewer than 3 points remain. UV:
  s = (p − o)·a2·0.5/r + 0.5, t = (p − o)·a1·0.5/r + 0.5. The fragment keeps the surface's lightmap (surface ref +4,
  set from the soup's lightmap in FUN_10002a00).
- Pool (0x4ad2d0 / 0x4ad3a0): `cg_marksLimit` (0x1040c48, default 1024, 0..1024) entries of 0x288 bytes (≤ 9
  vertices); a new mark takes a free entry, else the active mark with the smallest last-visible time (+8) is
  reused. The draw (0x4ad710) culls each mark by its sphere (0x486e30), stamps visible ones with the time and hands
  them to the renderer (refexport +0x68). Marks never fade.
- OpenIW2: `src/game/marks.{h,cpp}`, `FxSystem::spawnDecals`, iw2view draws marks in the blend pass with the
  surface lightmap (`iw2view … --shot x.png --shoot N`, `IW2_MARK_DEBUG=1`). TODO(spec): 0x48c290 spawn position,
  frustumCull spawn flag, the per-mark cull, the renderer's mark draw (0x10024320, depth offset), 0xf79778, BSP walk
  order (only matters at the limits).

## Atlas sequences

- Frame count of a material = byte 0xe · byte 0xf of materials/<name> (refexport 0x617ae8 → 1.3 DLL 0x10017580); e.g.
  gfx_fire_atlas 4 · 4.
- Spawn (0x48cd20): material at random ((seed >> 17)·count >> 15); with n ≠ 1 frames:
  start frame by sequenceStartFrameMode (+0x288, 0..2; 0 = sequenceFixedFrameValue (+0x28c) − 1, 1 = random
  (seed >> 17)·n >> 15, 2 = a caller value), rate by sequencePlayRateMode (+0x290, 0..1; 0 = sequenceFixedFpsValue
  (+0x294) / 1000 per ms, 1 = n / life), sequenceLoopMode (+0x298, 0..1) and sequenceLoopTimes (+0x29c) copied. The
  element is zeroed before parsing (0x4878c0), so all default to 0.
- Frame (0x481c30): start + _ftol((now − spawn)·rate); loop mode 0: mod n once past the end; mode 1 with times > 0: mod
  n below (times + 1)·n, else the last frame and times = 0; mode 1 with times ≤ 0: the last frame.
- Rectangle (1.3 DLL 0x1003fa60): c = byte 0xf, r = byte 0xe, u0 = (frame % c)/c, v0 = (frame / c)/r, u1 = u0 + 1/c,
  v1 = v0 + 1/r; a single frame uses 0..1.
- OpenIW2: `FxSystem::atlas` (host reads the material bytes), `FxSprite::uv`. TODO(spec): start mode 2's value.
