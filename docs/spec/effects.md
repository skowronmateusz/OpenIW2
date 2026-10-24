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
the per-particle velocity scale, sprite size meaning (half width), atlas sequences (the whole texture is used),
other element types (Tail, Emitter, FxRunner, Decal, Light, Cloud, CameraShake, Line), and the argument order of
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
  (surface flag 0x10), Decal elements.
