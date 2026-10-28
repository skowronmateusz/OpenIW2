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

## Relative elements (flag 0x2) and the attachment

- A bolted play (0x4853f0 with an attachment) takes the effect's origin AND axes from the tag orientation (0x485479 →
  0x48abb0: origin, then rows forward / left / up). Relative elements get the attachment record (0x485bf0, element +0xc0 via
  0x48cb00) and are kept in the element array 0x5a39dc.
- Spawn: the spawn point is converted into tag space at the end of 0x48c290 ((p − origin) · row k, the 0x440600 pattern);
  Line B by 0x440600 (0x48d4a1), Cylinder / OrientedParticle directions by 0x440650 (0x48d786, 0x48dd53); the Emitter stores the attachment origin in +0x264
  (0x47faa0 / 0x48a870 at 0x48d964, see "Emitter physics").
  The particle velocity axes (+0xd0, 0x4803f0) are the effect's axes as given, i.e. the tag axes in world space; the
  velocity / gravity offsets are added to the tag-space position as they are.
- Frame order (view flash): the frame stamp 0x6221a8 is bumped at the client frame start (0x4afca3), the effect system gets
  cg.time 0xf708dc there (0x486b30: +0x0 = time, +0x4 advanced by the clamped delta <= 200 ms, read as "now" by plays, e.g.
  0x4855e4), then the view weapon is added (0x4b026f -> 0x4b6d40) and plays viewFlashEffect inside it after posing the gun
  (0x4b6c37 -> 0x4b6660 -> 0x4853f0, no time argument), and the effects are drawn after that. So the flash starts at the frame's
  effect time and its relative particles follow this frame's tag_flash.
- Update: 0x47fcd0 returns the attachment orientation (refreshed by 0x48abb0 when the frame stamp 0x6221a8 changed; failure
  ends the element) and 0x4404f0 maps the tag-space position back: origin + x·row0 + y·row1 + z·row2 (Particle 0x48037a,
  Line 0x482dd8 / 0x482de9, Cloud A 0x482b0f (B stays tag-space), Cylinder 0x4834a9 / 0x4834df, Tail 0x483202, Emitter
  0x483719, OrientedParticle 0x48269c + axes 0x440540).
- Relative Emitter / Tail / Cloud (0x480bd0 with the orientation): the position +0x4 and the bounce offset +0xc4 are tag space;
  velocity components without absoluteVel / absoluteAccel are used as they are (0x481004), absolute ones go through 0x440650
  (0x480fd5 / 0x4811a5), gravity becomes g · (row0.z, row1.z, row2.z) (0x481220); the trace runs between the two points
  through the bolt (0x4404f0, 0x480cd1) and the hit normal is turned into tag space for the bounce (0x440650, 0x480ab0).
  Emitter: +0x7c = bolt(position) (0x483719) and the model axis rows go through 0x440540 (x·row0 + y·row1 + z·row2, 0x483742).
  Tail (0x483010): +0x7c = bolt(position); B = bolt(position + normalize(prev − position) · length) (0x483202); with no
  movement +0x9c keeps its last (world) value (0x483233).
- OpenIW2: `FxTag` (origin, axes, hasAxes) from `World::entityTagOrientation` (bone rotation; vehicle tags give the position
  only, then the axes are the identity). Implemented for Particle, Line, Cloud, Cylinder, Emitter, Tail. TODO(spec):
  the view-weapon flash / brass bolts pass the position only.

## Emitter physics (0x480bd0) and emitFx (0x483810)

- Instance fields from the common init 0x48cb00: flags +0xa8 = def +0x90, spawn +0xb8 = now, end +0xbc = now + _ftol(life),
  box +0x14 = def +0xa0 (`min`), +0x20 = def +0xac (`max`), then `deathfx` (+0x30, list def +0x78 / count +0x7c) and `impactfx`
  (+0x2c, def +0x70 / +0x74) picked with (seed >> 17) · count >> 15, a generator draw only for a non-empty list. `min` / `max`
  (0x487be0 / 0x487c30, "%f %f %f   %f %f %f", 3 or 6 numbers, the first three kept) also set usePhysics | useBBox (0x60).
  0x48ce70 draws `bounce` (def +0x280) into +0x100 after gravity, wind and the spawn point.
- Frame time (0x486b30, before the updates): frame ms +0xc = now − last clamped to 0..200 (0xc8), 0 while paused (0x617e98+8);
  effect time +4 += frame ms. One element update per client frame.
- Step 0x480bd0 (frame ms ≠ 0, not at rest 0x1000000): u = min((time − spawn) / life, 1) in seconds (time after this frame);
  next = pos + velocity(u) · dt (0x481810: velocity + velocity2 + gravity + bounce offset +0xc4). Without usePhysics 0x20 the
  position becomes next. Otherwise 0x486f70 → CM_BoxTrace 0x417640 (model 0, contents 1; box = +0x14 / +0x20 with useBBox
  0x40, else the zero vector 0x56ce9c). Start- or all-solid (+0x22 / +0x23) or fraction 1 (0x591e20): position = next. On a hit:
  impactFx 0x800 plays +0x2c at start + (end − start) · fraction (0x47f390) with row0 = the trace normal (0x485220 → 0x4217f0);
  then impactKills 0x400 returns false (the update fails, the element is freed, the position is not moved); else 0x480a70
  with uHit = min((fraction · dt + elapsed) / life, 1) (0x4809d0): rest (0x480a50: flags &= ~0x820, |= 0x1000000) when n.z > 0
  (0x591e00) and |velocity(u)|² < 16 (0x591e84), or when the fraction is 0 (0x4809f0); else D = velocity(uHit), offset =
  b·D − (D − offset) − 2 (b·D · n) n (b = +0x100, 0x59216c = −2). The position becomes pos + (next − pos) · fraction.
- Emitter update 0x483630: alive check 0x47fd80, orientation 0x47fcd0, step; an unchanged position (0x415c50) multiplies the
  spin rates +0x284 by 0.7 (0x591f30); angles += frame ms · 0.01 (0x591ea0) · rate and axes +0x48 = forward, −right, up of
  0x421da0 (a · 0.017453292, 0x591f50) (0x483e50); size (0x4818b0); useModel: +0x7c = position, refEntity scale +0x98 = size
  +0x88; then emitFx.
- Spawn 0x48d7d0 also: +0x290 = one `emitfx` entry (def +0x80 / +0x84), +0x294 = `density` (def +0x260, 0x488a60 "%f %f"),
  +0x298 = `variance` (def +0x268, 0x488ab0), +0x24c = the spawn position, +0x258 = the start velocity, +0x264 = the attachment
  origin (0x47faa0 / 0x48a870) or 0, +0x270 = now, +0x274 = (2 · rand() / 32768 − 1) · variance + density (CRT rand 0x553d5d,
  0x591efc = 32768).
- emitFx 0x483810 (flag 0x100, frame ms ≠ 0): P = +0x24c moves with the velocity only (no collision). With t = +0x270, acc = 0,
  k = 0, while t < time: k += 12 (ka = k · 0.001); u = min((time − spawn + acc) / life, 1); V = velocity(u); Q = P + V · ka
  (+ drift · ka for relative). If |P + off − Q|² ≥ (+0x274)² (off = +0x264): play +0x290 (no attachment: at Q with row0 = V / |V|
  completed by 0x4217f0, the element axes +0x48 when |V| ≤ 0; relative: bolted to the attachment +0xc0 + 0x3c with no axes, so
  origin and axes come from the tag (0x485479) and bolt(Q) is only the spawnRange / cullrange position (0x4854ea)); a = 2 · ka ·
  |V|², when a ≠ 0: n = fistp(((ka² · |V|² − d²) / a) · 1000 (0x591e50) + 2^-30 (0x591f20)), k = max(k − n, t − +0x270), ka =
  k · 0.001, Q = P + velocity(min((ka + time − spawn + acc) / life, 1)) · ka (no drift); then +0x24c = Q, +0x258 = velocity at
  that time, off += drift · ka (relative), acc += ka, +0x270 += k, t = +0x270, k = 0, +0x274 = a new rand() distance;
  otherwise t += 12.
- Relative drift (0x4836e1..0x4837c6): zero, except for useModel when +0x270 < time: (tag origin − +0x264) · (time − +0x270) ·
  0.001 — a product, as the exe computes it (0x4837aa fmul). Shipped relative emitFx emitters: panzerfaust / panzerschreck
  smoke, fire_airplane_emitter (no useModel) and the useModel explosion debris (silo_explode, artilleryexp_*, eldaba boat, tank
  libya).
- OpenIW2: `FxSystem::stepEmitter` / `physicsStep` / `emitFx` (steps of at most 200 ms between host updates; emitted and
  impact effects play after the update loop). The emitFx distance uses the MSVC CRT rand() (0x553d5d: per-thread seed ·
  0x343fd + 0x269ec3, (seed >> 16) & 0x7fff); the exe seeds it once with timeGetTime() (srand 0x553d50 at 0x40e149) and 25
  other callers share it, so the exact sequence is not reproducible; `FxSystem::crtRand` keeps the generator and the 0..32767
  range (seed 1, the CRT default). Before, std::rand() (0..2^31 − 1 on macOS) made the distances up to 65536 · variance.
- deathFx 0x200: the death slot (vtable +4) 0x4801d0 of Particle, OrientedParticle, Emitter, Tail and Cylinder (not Line,
  Light, Cloud). The update loop 0x48be90 calls it when the system time is past the end +0xbc (after clearing impactKills
  0x400, 0x48bec9) or when the update slot returned false. With 0x200 set, 0x400 clear and an effect in +0x30: a, b, c =
  0x426250(−1, 1) in that order, dir = (c, b, a) / |·| when the length is above 1e-6 (double 0x592310), else (0, 0, 1); plays
  +0x30 at +0x4 (tag space for a relative element, passed unchanged) with row 0 = dir (0x485220). Shipped users:
  fx/misc/flare_hill400.efx (Emitter, deathfx fx/misc/flare) and fx/impacts/minefield.efx. OpenIW2: `FxSystem::deathEffect`
  (deferred like impactFx).
- Channel scales (0x4804c0, init slot vtable +0x1c of Particle / OrientedParticle / Emitter / Line): one record per group
  for the 22 groups in key order without length / lengthRand (instance +0x144, stride 0xc: curve, 0, scale = 0x48a2c0 of the
  group's `scale` range, no draw when min == max). Rand groups get a scale too but 0x47f570 multiplies only by the main
  group's. Tail / Cylinder (0x482fa0) and Cloud (0x482970) add length and lengthRand after it; Light (0x4840d0) has only rgb,
  rgbRand, size, sizeRand. So velocity / velocity2 X, Y and Z each use their own per-instance scale (+0x1c4 / +0x1d0 / +0x1dc,
  +0x20c / +0x218 / +0x224; 0x480e80 / 0x481050). The order relative to the other spawn draws (0x48cb00 / 0x48ce70) is
  still approximate.

## Particle quads (renderer type 4)

- Particle update 0x480330: Emitter step (0x480bd0), A (+0x7c) = position (through the bolt), size (0x4818b0); only when
  size ≠ 0 size2 (0x481940: the size2 channel with nonUniformScale, else = size); only when size2 ≠ 0 rgb, alpha, angle
  (0x481b90: +0x44 += frame ms · rotationDelta · 0.01, useRandomRotationDelta 0x40000) and frame (0x481c30). The start angle
  +0x44 is drawn from `rotation` by 0x48ce70 (spawn 0x48d2f0: 0x48cb00, 0x48ce70, 0x48cd20, 0x48caa0).
- refEntity (0x480010): +0x64 = size, +0x68 = size2 result, +0x6c = angle (degrees), +0x70 = 0. DLL 0x1003ff90 (entity flag
  0x2000 never set by effects) → 0x1003ff20 (+0x70 = 0, so no screen-size clamp) → 0x1003fb00: with the view's left L and up U
  axes (view +0x18 / +0x24, normal −forward +0xc) and a = angle · π/180: R = size(cos a · L − sin a · U), V = size2(cos a · U +
  sin a · L); corners C + R + V (u0, v0), C − R + V (u1, v0), C − R − V (u1, v1), C + R − V (u0, v1) (0x1003f560, indices 0 1 3,
  3 1 2), uv from the atlas rectangle 0x1003fa60, colour bytes. So `size` is the half width and `size2` the half height.
- Motion: Particle (0x480330) and OrientedParticle (0x482630) positions come from the physics step 0x480bd0 every update
  (see "Emitter physics"), so usePhysics / useBBox / impactKills / impactFx apply to them as to Emitters (95 / 150 / 76 / 9
  shipped Particle elements). The analytic position 0x4816c0 is used only by 0x48caa0 for the late part of a spawn
  (pos += 0x4816c0(ms) · ms · 0.001 for ms > 0; TODO(spec): not applied, OpenIW2 steps from the scheduled spawn time).
  Light (update 0x4841e0: +0x7c = the spawn point +0x4, through the bolt for relative) and Line / Cylinder elements do not
  move.
- Draw slot 0x4802f0: a Particle is submitted only while size (+0x88) ≠ 0 and size2 (+0x8c) ≠ 0 (it still enters the sight
  blocker list, 0x480970).
- OrientedParticle (vtable 0x57c7e4, spawn 0x48dc80): +0x24c = the instance axis row 0 (relative: into tag space, 0x440650 at
  0x48dd53). Update 0x482630: step 0x480bd0, size, size2 (0x481940), rgb, alpha, angle (0x481b90), frame, all unconditional;
  +0x7c = position and +0x48 = +0x24c, through the bolt (0x4404f0) / turned by the orientation (0x440540) for relative. Cull
  0x4825e0: sphere +0x7c, radius size. Draw 0x482620: renderer type 7 (0x480010). 1.3 DLL 0x1003ffe0: with d = refEntity +0x14
  (row 0 = +0x48), 0x10059bd0 (the 0x4217f0 construction) gives p = normalize((d.z, −d.x, d.y) − d (d · that)) and q = p × d;
  angle 0 (0x1019bd04): R = size · p, V = size2 · q; else a = angle · π/180: R = size (cos a · p − sin a · q), V = size2
  (cos a · q + sin a · p); corners by 0x1003f560 as the particle quad (0x1003f850 instead when the dvar at 0x101ce710 is 2).
- OpenIW2: `FxSprite::size2` / `rotation` / `oriented`, iw2view `spriteModel`. The per-frame angle sum is evaluated as its
  continuous limit (rot0 + 0.01 · life · ∫rotationDelta). TODO(spec): the angle sum skips frames where size or size2 is 0;
  the mode-2 corner builder 0x1003f850. The OrientedParticle cull (0x4825e0) is applied against `viewPlanes` (side planes). Check: `IW2_NO_HUD=1
  IW2_FX_TEST=fx/explosions/grenadeexp_water.efx IW2_FX_TEST_DIST=200 IW2_FX_MS=600 IW2_FX_STEP=16 iw2view <main> toujane
  --sim-seconds 1 --pos 480,-600,140 --yaw 0 --pitch 30 --shot x.png` (flat ripple rings).

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
- rgbRand (index 1) is converted with one component (0x487910 passes 3 only for index 0), but the rgb updater (0x4819b0 →
  0x47f600 / 0x47f4a0) finds the segment with the graph's own stride and then reads rows at a fixed 16-byte stride, so a
  one-component rgbRand graph is read out of bounds: undefined in the original. No shipped file has a legacy rgbRand; it is
  left as parsed.
- `random` draws from the single effect generator 0x59b2a8 (initial value 0x89abcdef in the data; its setter 0x426240 has
  no caller), so the values depend on every earlier draw. OpenIW2 shares one generator between the parser and FxSystem
  (`assets::fxGeneratorSeed`).
- OpenIW2: `src/assets/efx.cpp` (`legacyGraph`).

## Line elements (type 2)

- Spawn 0x48d3c0 (vtable 0x57c708, 0x258 bytes): A (+0x4) = the spawn shape point (0x48c290); B (+0x24c) from 0x48c850:
  origin2 ranges drawn z, y, x (+0xe0 / +0xd8 / +0xd0), world axes with cheapOrg2Calc 0x80 else x·row0 + y·row1 + z·row2
  of the instance axes (0x424a70), plus the effect origin; material from `shaders` (0x485110); random lerp draws for
  useRandomColors / Alpha / Size / Size2 (+0x118..+0x124). With org2fromTrace 0x8 B comes from a trace (not implemented).
- Update 0x482da0: +0x7c = A, +0x9c = B (through the bolt for relative), then size (0x4818b0 → +0x88), rgb, alpha. No
  frame step. Draw 0x482d90: renderer type 8, the Tail quad (half width = size).
- org2fromTrace 0x8 (0x48c850): end = A + row0 · 16384 (0x592270), plus the origin2 sample with org2isOffset 0x20 (world
  axes with cheapOrg2Calc); B = A + (end − A) · fraction of the effect trace 0x486f70 (zero box, contents 1). With
  traceImpactFx 0x10 one `impactfx` entry (list +0x70, count word +0x74; (seed >> 17) · count >> 15) plays at B along the
  trace normal (0x485220).
- Frame +0x94: no Line code writes it and the effect pool (0x47f0a0) hands out freed slots uncleared, so it is whatever the
  previous occupant left (0 in never-used pool memory 0xa32380). Frame 0 is used.
- Relative Lines: the spawn stores B in tag space (0x440600), the update maps A and B through the bolt (0x4404f0).
- OpenIW2: `FxSystem` emits a `beamTail` sprite; relative Lines map A and B through the tag orientation (see "Relative elements"). The only Line effect (minefield.efx) names materials that do not ship and is not played by
  any script.

## Cloud elements (type 12)

- Spawn 0x48de90 (vtable 0x57c78c, 0x27c bytes): random unit axis +0x24c (0x482850: up to 4 tries of x, y, z = draw·2 − 1,
  rejected when |v|² < 0.01 or > 1, else normalised; fallback (1, 0, 0)), common init, randoms (0x48ce70: start angle +0x44
  from `rotation`, nonUniformScale copied to +0x104, `wind` drawn into +0xf8), +0x260 = draw in [0, 1), +0x25c = useLength.
- Update 0x482a30: B (+0x9c) = position before the step; Emitter step 0x480bd0; size → +0x88 (0x4818b0); size2 (0x481940:
  the size2 channel with nonUniformScale, else = size); length (0x4829d0, only with useLength; useRandomLength lerps by
  +0x260); rgb / alpha bytes; angle += frame ms · rotationDelta · 0.01 (0x481b90). Axis rows (+0x48..+0x68) = rotation of
  the identity about +0x24c by the angle (0x423a50 quaternion (sin(a·π/360)·axis, cos), 0x423680 matrix). Then +0x98 = size,
  +0x88 = width (size2 result), +0x8c = height (length with useLength, else width); A (+0x7c) = position (bolt-transformed).
- Draw 0x482960: renderer type 6 → refEntity (0x480010: +0x14..+0x34 axis, +0x38 scale, +0x3c A, +0x48 B, +0x58 colour
  bytes, +0x64 width, +0x68 height). DLL 0x1002fb5f builds the world matrix (0x1005a7a0: rows axis·scale, translation A);
  0x10041370 → 0x10041230 sets particleCloudColor = bytes / 255 and particleCloudMatrix: (width, 0, 0, height) when
  width == height or A ≈ B (0x1003f260, per component d² ≤ 1e-6); else d = normalize(B − A)·height turned into view space
  (0x100410e0) and 0x10041180: (width, 0, 0, height) again when d.x < 0.001 and d.y < 0.001 (signed), else (d.y, −d.x, d.x,
  d.y) with .xy scaled to length width and .zw scaled by the same factor when |d.xy| < width. One indexed draw of 2048
  triangles / 4096 vertices of the static buffer 0x101d49b4 (0x10011ca0, 20-byte vertices): quad q = i·128 + j·16 + k
  (i, j < 8, k < 16) at ((r·(1/32767) + i)·0.25 − 1, (r·(1/32767) + j)·0.25 − 1, (r·(1/32767) + k)·0.125 − 1), three CRT
  rand() per quad (no srand anywhere in the DLL, so the sequence starts from 1), texcoords (0,0) (0,1) (1,0) (1,1), indices
  0 1 2 2 1 3.
- Shader (iw_07.iwd, materials/shaders/lib/particle_cloud.hlsl): the point is transformed to view space and offset by
  (u − 0.5)·m.xy + (v − 0.5)·m.zw; colour = colorMap · particleCloudColor; the _outdoor variant zeroes alpha where the
  point is below the outdoor map (worldOutdoorLookupMatrix, sampler.outdoor).
- OpenIW2: `FxSystem::clouds()` and iw2view `cloudModel` (CPU expansion of the same geometry, view x / y = right / up).
  The outdoor test: see "Outdoor map". TODO(spec): `wind` and
  affectedByWind (no reader in the exe's effect code).
- View cull (slot 3, 0x4827e0 → 0x486e30): the cloud is not drawn when n·A − d < −r for any plane of the view's plane set
  (view +0x20, stride 0x10, count +0x80, at most 5 with disableFarPlaneCulling; the sixth plane is the far plane, docs/spec/render-hints.md), r = max(width, height) + size. The radius is
  smaller than the cube's half diagonal, so clouds near the screen edge disappear. OpenIW2 tests the four side planes
  (`FxSystem::viewPlanes`) plus the far plane (`FxSystem::beyondFar`); the near plane is not modelled.

## CameraShake elements (type 10)

- The parser names the type `cameraShake` (0x57c920, id 10 at 0x4868b7); the spawn dispatch (0x485d10, table 0x485dec entry 9)
  calls 0x48db80, which computes the spawn point (0x48c290) and draws `life` (def +0x58), `radius` (+0xe8) and `bounce` (+0x280)
  with 0x48a2c0, discarding all three (fstp). No element instance is allocated, so a CameraShake element only consumes effect
  generator draws; it shakes nothing in SP 1.3 (the only shipped one is in fx/explosions/mortarexp_dirt.efx).
- OpenIW2: `FxSystem::playAxes` makes the same draws and spawns nothing.
- Type 11 `flash` (0x57c918) spawns through 0x48ddb0 (0xfc-byte object, vtable 0x57d428); no shipped effect uses it (not decoded).

## Outdoor map ("$outdoor", 1.3 DLL 0x10020c80)

- Built when the world loads unless the dvar at 0x101ce710 is 2: bounds 0x100208a0 over the world surface references
  (12 bytes {material, lightmap, bounds}): surfaces whose material byte +0xc has bit 8 are skipped; mins start at 131072,
  maxs at −131072 (0x48000000 / 0xc8000000); an axis still at 131072 becomes 0..0; an axis narrower than 1 grows by 0.5 per
  side. Image size 512 × 512 × 256 levels (0x10190398); per axis scale = (size − 1) / (max − min), bias = −scale · min
  (0x100209f0). worldOutdoorLookupMatrix (0x10020a50, material constants +0x1c0): identity with diagonal 1 / (max − min) and
  translation −min / (max − min), i.e. p → (p − min) / (max − min).
- Image (0x10020cd0 → 0x10020bb0, L8 format 0x32): for row r (y) and column c (x), world x = (c + 0.5 − bias.x) / scale.x,
  y likewise; 0x100207f0 traces a point from (x, y, max.z + 1) to (x, y, min.z − 1) through the exe's CM_BoxTrace import
  (refimport 0x101d4b40, model 0, contents 0x2001) and takes z = start + (end − start) · fraction; the texel is
  clamp(fistp(z · scale.z + bias.z − 0.4999999990686774), 0, 255).
- particle_cloud_outdoor.hlsl: outdoorMapPos = vertex position · worldOutdoorLookupMatrix; alpha 0 when outdoorMapPos.z is
  below the image value at outdoorMapPos.xy. The position is the flake's point (shared by its four vertices), so the test
  hides whole flakes.
- OpenIW2: `game/OutdoorMap`, built in iw2view on the first Cloud; covered flakes are skipped (`IW2_OUTDOOR_DUMP`,
  `IW2_NO_OUTDOOR`). TODO(spec): the outdoor sampler's filter / address mode (point sampling, clamped here); surface
  bounds are taken from the soup's indexed vertices. zfeather_outdoor reads the same map (see "$floatz" below).

## Cylinder elements (type 4, no shipped effect uses one)

- Spawn 0x48d6c0 (vtable 0x57c7b8, 0x278 bytes): common init, randoms 0x48ce70 (the spawn point becomes the position +0x4),
  sequence 0x48cd20, +0x48 = row 0 of the instance axes (into tag space with 0x440650 for relative).
- Update 0x483450 (no motion): size → +0x88, size2 → +0x8c (= size without nonUniformScale), length → +0x258 (0x4830d0),
  rgb, alpha; A (+0x7c) = position, B (+0x9c) = position + length · row0, both through the bolt. Draw 0x483440: renderer
  type 9 (A = ent+0x3c, B = ent+0x48, ent+0x64 = size, ent+0x68 = size2 result). Cull 0x4833f0: capsule A–B (0x486ef0).
- DLL 0x100401f0 (0x100405e0 instead when the dvar at 0x101ce710 is 2): n = clamp(round((1 − (|(A + B)/2 − view| ·
  lodScale + lodBias) / 1024) · 32 + 9.3e-10), 8, 32) (0x1019bf58, 0x1019bf20, 0x1019be00); lodScale / lodBias = r_lodScale
  (default 1) / r_lodBias (default 0) · tan(40°) / tan(fovX / 2) when fovX ≠ 80 (0x10025b76, view record +0x24 / +0x28).
  d = normalize(A − B); p1 = (d.z, −d.x, d.y) minus its d part, normalised, p2 = p1 × d (0x10059bd0). Per step k < n
  ((cos, sin) start (1, 0) and turn by 2π/n each step, t starts 0 and grows by 1/n): vertex A + (p1·sin + p2·cos) · ent+0x68
  with texcoord (t, 1) and B + (p1·sin + p2·cos) · ent+0x64 with (t, 0); triangles (b, b+1, b+3), (b+3, b+2, b); then a
  copy of the first pair with t = 1. Colour = the element bytes (D3DCOLOR order); 64-byte vertices with w = 1, nothing else.
- OpenIW2: `FxSprite::cylinder`, ring built in iw2view `spriteModel`. Check: `IW2_FX_TEST=file:<cyl.efx>` (any local efx with
  a Cylinder element). TODO(spec): r_lodScale / r_lodBias other than the defaults, the mode-2 builder, the capsule cull.

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
Exact rules (0x489c59 / 0x489c6c → 0x4886b0 → 0x488650): `flags`/`flag` search the first 24 rows (depthHack ..
disableFarPlaneCulling), `spawnFlags`/`spawnFlag` the 13 rows from org2fromTrace, which end with absoluteVel (0x200000) and
absoluteAccel (0x400000): a matched row ORs both its element and spawn bits, so these two set ELEMENT flags from spawnFlags
(grenade_smoke, large_vehicle_explosion, dust_impact_16ftladder). Names compare case-insensitively. A name outside the searched
rows fails the key, the element ("FX Error while parsing segment type") and so the whole effect (0x486917 frees it): the
shipped fx/impacts/wall_impact_small_dirt.efx uses `rgbComponentInterpolation`, which is in no row, and does not load in
the original; OpenIW2's parser rejects it the same way. An effect holds at most 24 elements (0x48693f).

Implemented: `src/assets/efx.{h,cpp}`. Not specified yet: the runtime (spawning, element update, drawing) and the
meaning of the flag bits.

## Runtime (partial, `src/game/fx.cpp`)

Particles and oriented particles are spawned by `playfx`, `playfxontag` and `playloopedfx` and drawn as
camera-facing sprites in `iw2view`. Position, gravity and channel evaluation follow the evidence in
the executable. Inferred and marked TODO: spawn shapes (sphere/cylinder, `radius`, `height`),
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

## Flash element (type 11, 0x48ddb0)

No shipped effect uses it: a census of all 375 `fx/**/*.efx` files in main finds the element types Particle 858, Tail 143,
Emitter 111, FxRunner 99, Decal 80, Light 49, OrientedParticle 20, Cloud 9, CameraShake 1, Line 1 and no Flash (nor
Cylinder). Not decoded; TODO(spec) if an effect with it ever appears.
## Sprite vertices, $floatz and the post-sun copy (1.3 DLL, heat distortion / z-feather)

Evidence: `gfx_d3d_x86_s.dll` 1.3 disassembly, exe vtables read from `CoD2SP_s.exe`, shipped `iw_07.iwd` shaders.

### Which refEntity type each element submits
Draw slot 4 of the element vtable calls 0x480010 with a type: Particle (vtable 0x57c810, slot 0x4802f0) 4, OrientedParticle
(0x57c7e4, 0x482620) 7, Cloud (0x57c78c, 0x482960) 6, Line and Tail (0x57c708 / 0x57c6b0) 8, Cylinder (0x57c7b8, 0x483440) 9.
The DLL type switch at 0x100414f0: 4 -> 0x1003ff90 (camera-facing billboard), 7 -> 0x1003ffe0 (oriented quad).

### Vertex layout (code mesh buffer, 64 bytes, vertex declaration row 0 of the table at 0x1018eac8)
Declaration builder 0x10016810 (CreateVertexDeclaration, device vtable +0x158) reads 3-byte {stream, offset, type} rows per
shader input code (stride 0x15 per layout); the semantic matcher 0x10019330 maps "binormal" to code 5, "tangent" to 6.
Row 0: code 0 position FLOAT4 @0x00, 1 normal FLOAT3 @0x10, 2 color D3DCOLOR @0x1c, 3 texcoord0 FLOAT2 @0x20, 5 **binormal
FLOAT3 @0x28**, 6 **tangent FLOAT3 @0x34**. (Row 1, 68 bytes, is another layout; row 2 is position + texcoord only.)

### Sprite tangent / binormal (the data distortion.hlsl reads as code.tangent / code.binormal)
Quad writers 0x1003f560 (normal) and 0x1003f850 (dpvs mode 2, 36-byte vertices, not decoded) get O (origin), A (width vector),
B (height vector). Vertices: O + A + B, O - A + B, O - A - B, O + A - B; w = 1.0 at +0xc; the same values go to all four
vertices: normal = -view axis row 0, **binormal = B, tangent = -A**.
- Billboard (type 4, 0x1003fb00): view = the 0x14c-byte view record behind [0x1070c3c8], whose +0xc / +0x18 / +0x24 are the three
  view axis rows (copied from the scene's view matrix by 0x100259c0; Q3 order forward / left / up assumed, TODO(spec) sign of
  row 1). Unrotated: A = width * row1, B = height * row2 (width / height = entity +0x64 / +0x68). Rotation r (entity +0x6c,
  degrees * 0x1019be0c to radians; c = cos, s = sin): A = width * (c * row1 - s * row2), B = height * (c * row2 + s * row1).
  Entity flag 0x1000 (+4) adds the height param to the origin z; entity +0x70 is a screen-size clamp (0x1003ff20 / 0x1003fdf0).
- Oriented (type 7, 0x1003ffe0): axis V = entity +0x14; P = perpendicular of V (0x10059bd0: (V.z, -V.x, V.y), projected off V,
  normalized); unrotated A = width * P, B = height * (P x V); with r != 0 the pair is rotated by sin / cos of r (fsincos at 0x1004008d), TODO(spec): the exact mixing is not implemented.
- Tail / line (type 8, 0x10041470): tangent / binormal written from the quad axes (B-A and c).
So for a distortion sprite T' / B' of distortion.hlsl are the clip-space images of the sprite's half-width and half-height
screen vectors (tangent points to the viewer's right), scaled by distortionScale.

### Frame position of the post-sun scene copy
Scene builder 0x10025490 (called from 0x100257d0 through 0x10025750): order inside one scene is: z-feather float-z pass
(0x10024ee0, below), draw lists of technique type 1 "sky" and 6 "lightmap no fog" (0x10021cf0), 0x10021d80, then, only when the
scene viewport covers the whole window (flag byte +4 of the argument built at 0x10025ce5: refdef width / height == 0x101d4ca8 /
0x101d4cac, the vid config size set by 0x10011670), command 0x19 (0x100310a0 post-sun resolve, emitted by 0x10022510), then the
effects list of technique type 0x18 (`emissive feathered no fog`) when r_zFeather and `$floatz` exist, else 0x15 (`emissive no fog`).
Technique type names (0x1001c0c0, index = type): 0 depth prepass, 1 sky, **2 build floatz**, 3..5 unlit no/linear/exp fog, 6 lightmap
no fog, ... 0x15 emissive no fog, 0x18 emissive feathered no fog.

### `$floatz` (R32F distance target) and its build pass
- Created at 0x10023240..0x10023276 when r_zFeather (0x101ce694) is on and the dvar 0x101ce710 is not 2: render target texture of
  D3D format 0x72 (D3DFMT_R32F), size = max(1, window width / height) = 0x101d4ca8 x 0x101d4cac; global 0x101d489c; render target
  array entry 6 (0x101d4820 + 6 * 0x14 = 0x101d4898).
- Build pass 0x10024ee0 (called first thing in the scene builder, 0x100254a9; skipped unless r_zFeather and $floatz exist): begin
  GPU timer slot, command 0xe (set render target) with target 6, 0x10024e90 = clear of depth + stencil (flags 6, z 1.0, stencil 0)
  and command 0x14 (viewport 0, 0, 1, 1 of the target), then the draw list of **technique type 2 "build floatz"**, end timer.
- What it writes (shipped files): build_floatz.tech (stateMap build_floatz: no blend, depth write on, colour write on, alpha test
  Always, fog off) with lib/float_z_build.hlsl: colour = clip-space z (`position.z`) of each opaque surface; the alpha-test
  variants (build_floatz_atest, _scroll, _dtl_scroll) clip texels with alpha < 128/255. Techniquesets phong_*, ambient_*,
  unlit_* map "build floatz" to these; `sky.techset` and `shadowclear.techset` use build_floatz_clear.tech with
  floatz_clear.hlsl, which writes 262144 * 2 * 1.732 (the maximum world extent) into all channels. So `$floatz` is a per-pixel
  distance (clip z) of the nearest opaque surface, "infinite" where only sky shows.
- Readers: zfeather.hlsl (`floatZSampler`; alpha *= saturate(|floatz * featherParms.x - z * featherParms.x|), `pixel.color.a *=
  saturate(z * featherParms.x)` near the camera) and distortion_floatz (`USE_FLOAT_Z`: `floatZSampler` at the offset position;
  if the offset sample is nearer than the distortion quad the unoffset scene sample is used).
- Material constants (constant table, file offset 0x40): `distortionScale` (gfx_distortion_heat 0.01 0.01, _noise / _ring_heavy /
  _ripple 0.15 0.15, _ring_light 0.05 0.05) and `featherParms` (0.025 40 for most effect_zfeather* materials, 0.2 5 for
  gfx_explosion_flash). So featherParms.x = 1 / 40: the feather and near-camera fades run over 40 units of clip z (5 for the flash).

### OpenIW2 (iw2view)
- `$floatz`: when an effect batch needs it (MatInfo::fxKind 1 = effect_zfeather*, 2 = distortion, 3 = effect_zfeather_outdoor*)
  and the scene goes to the offscreen target, `Renderer::draw` first runs the "build floatz" pass (0x10024ee0): R32_FLOAT target
  cleared to 262144 · 2 · 1.732 (sky.techset's build_floatz_clear gives the same value), depth cleared, then every
  Pass::Opaque / Pass::AlphaTest batch of the world, brush models and entity models with `vs_floatz` / `fs_floatz` (clip z;
  alpha-tested batches clip (vertex colour · colorMap).a < 128 / 255). Only the *_replace / *_alphatest technique sets (phong,
  ambient, unlit, default) map "build floatz" (census of iw_07 techniquesets); blended, effect, water and objective
  surfaces are not drawn into it, as in the DLL.
- Post-sun copy: the scene pass is split after the opaque / alpha-tested / l_point passes and the colour target is copied to
  `postSunTex` (resolvedPostSun); the pass resumes with load ops.
- Distortion: `vs_distort` / `fs_distort` (distortion_floatz.tech). Sprite tangent / binormal = r (−R) / u (V) of the billboard
  and oriented quads; Tail / Line quads (type 8, builder 0x100409c0) carry tangent = c (the unit quad normal) and binormal =
  B − A. Census: only Particle elements use distortion materials (9 efx files), and no map or xmodel references any
  `gfx_distortion*` material, so world / model distortion surfaces have no shipped user (the techset stays Skip there).
- Z-feather: `vs_feather` / `fs_feather` (lib/z_feather.hlsl). The effect list is drawn with technique type 0x18 "emissive
  feathered no fog", so only the no-fog techniques run (zfeather.tech, zfeather_add.tech, the _outdoor ones; the techsets'
  zfeather_lfog / _efog technique files are not even shipped): no fog on these sprites. zfeather_add_nofog.sm blends Add,
  SrcAlpha, One. OUTDOOR (effect_zfeather_outdoor*, e.g. gfx_rain_mist): alpha · saturate((outdoorMapPos.z − $outdoor(xy)) ·
  outdoorFeatherParms.x) with outdoorMapPos = (p − mins) / (maxs − mins) (worldOutdoorLookupMatrix) and outdoorFeatherParms =
  code constant 0xb2 (name table 0x1018f1f8, storage 0x1070c320) = r_outdoorFeather in all lanes (0x10033fd0 / 0x10034082;
  dvar 0x101ce6bc registered at 0x1000b927, default 8). "$outdoor" is built when such a sprite or a Cloud appears.
- clipSpaceLookupOffset drops D3D9's +0.5 texel (Metal samples texel centres from the same projection). `IW2_NO_POSTSUN=1`
  turns it all off. Check (feather): `IW2_NO_HUD=1 IW2_FX_TEST=file:<efx: one Particle, life 10000, size curve 1 scale 150,
  shader gfx_dark_smoke_atlas> IW2_FX_TEST_DIST=110 IW2_FX_MS=500 iw2view <main> moscow --pitch 35 --shot x.png`: the smoke
  fades into the ground and the wall; with `IW2_NO_POSTSUN=1` it is cut along straight lines.
- TODO(spec): the order of sprites inside the effect list (the DLL sorts draw records by a key with material index
  (key >> 21) & 0x3ff, layer and entity index; the key writer was not found by searching the DLL disassembly
  for the matching shifts; here the entity-item order and, inside the effect model, the shader name order); polygon offset
  in the build-floatz pass; the view model (depth-hack range) in the floatz list; the floatZ / outdoor sampler states (point,
  clamp here).
- TODO(spec): the matrix that makes clip z comparable to featherParms (material constant featherParms is not yet traced);
  mode 2 (dvar 0x101ce710 == 2) vertex format 0x1003f850; why the post-sun copy needs the full-window flag besides the
  r_distortion / glow gate in 0x100310a0.
