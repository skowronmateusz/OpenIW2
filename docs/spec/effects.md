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

Implemented: `src/assets/efx.{h,cpp}`. Not specified yet: the runtime (spawning, element update, drawing) and the
meaning of the flag bits.
