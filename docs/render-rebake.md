# Enhanced FX stage 6: our own lighting re-bake (plan)

An openiw2 addition with no counterpart in the original game, and none of it is a specification of the original. Where it
depends on what the shipped data means, it uses docs/spec: the lightmap layout and its directional basis (material.md
"Lightmaps", "`$lightmapweights`"), the light grid (lightgrid.md), the BSP lumps (d3dbsp.md) and the sun direction
(material.md). Status: stages 1 to 4 are implemented (pure parts in `src/game/rebake.h` / `.cpp`, the Metal ray tracing in
`apps/iw2view/rebake.mm`; tests in `tests/test_rebake.cpp`); the rest is planned. The design is our own: it uses the compiler's
output (the shipped lightmaps) as a reference to calibrate against, never as a source of the algorithm.

## Why

The shipped lighting is baked at 8 bits per channel, 512 × 512 texels per lightmap (the sun mask 1024²), and it mixes every
non-sun light into one set of coefficients. That sets three limits that no runtime effect can lift:
- banding and crushed shadows in dark interiors (8-bit, low range);
- blurry light at 16–32 world units per texel;
- no way to change one light (switchable lamps, a darker night, time of day) without the others.

The re-bake computes the same quantities again on the GPU from the map's own geometry, materials and lights. It is in HDR
and at a higher resolution, with lights kept apart, and it is calibrated against the shipped lightmaps so that the art
direction stays the same.

## What the shipped data gives (from docs/spec)

- Per lightmap, part 0..2: four coefficients per colour channel. Their weights come from `$lightmapweights` at the normal-map
  sample, giving one weight along the surface normal and three around it. A flat normal map uses only the first one.
- Part 3: the sun's visibility (0..1). The sun term `visibility × sun colour × max(0, N·L)` is added at runtime, so the shipped
  coefficients hold everything except direct sun: bounced sun, sky and lamps.
- The sun: direction from worldspawn `sundirection`, colour from `suncolor` / `sunlight` / `ambient` / `diffuseFraction`
  (main.cpp `worldSunColor`, with its TODO(spec)).
- Map lights: the `light` entities in the entity text (origin, radius, intensity, `_color`; spot lights add a target and
  `fov_inner` / `fov_outer`). The engine never reads them, so their meaning comes from our own calibration, not from the
  original compiler (which we do not have). TODO(spec): the original compiler's falloff.
- Triangle soups: material, lightmap index (31 = none), per-vertex lightmap coordinates, normal and tangent frame
  (d3dbsp.md lumps 6–8). Static models (lump 38 / misc_model) are lit by the light grid, not by lightmaps.

## Pipeline

1. **Inputs** (CPU, at map load). *Stage 1: the triangles and the sun are done; albedo, sky colour and the light entities are not used yet (see
   "Stage 1 and 2: what was built").* The world triangles with their lightmap index and coordinates; per material an albedo
   (the mean of the colour map's smallest mip, times the material's colour); the sun; the sky colour (the mean of the sky
   cube's lowest mip); the map lights. All of these are pure functions with tests.
2. **Texel set-up** (CPU). Each lightmap's triangles are rasterised in lightmap space at the bake resolution: 1× (512²) or 2×
   (1024², bilinearly upsampled coordinates). Each texel gets a world position, a normal, a tangent frame and a validity
   flag, with conservative coverage and a 1-texel dilation so that bilinear filtering never reads unset texels. This step is
   tested on synthetic triangles.
3. **Direct light** (GPU compute, Metal ray tracing against the static world plus the alpha-tested surfaces).
   - Sun: a cone of rays (soft) gives the visibility. It is stored as a new sun mask at the bake resolution, so the runtime
     sun term stays as it is, only sharper.
   - Lamps: one light at a time, inverse-square falloff ended at the radius (our own model; calibration below), and the spot
     cone where one is set. Each lamp keeps its own visibility for the "separable" output.
   - Sky: cosine rays that escape to the sky, times the sky colour.
4. **Bounces** (GPU). The light at the hit point is taken from the previous pass's result at the hit's lightmap coordinates
   (an irradiance cache in lightmap space), times the hit material's albedo. Two or three passes; each pass is one dispatch
   over all texels.
5. **Projection onto the shipped basis**. The incoming radiance is gathered per texel in the tangent frame and fitted, by
   least squares, onto the four `$lightmapweights` directions. The runtime shader then needs no change in how the
   coefficients are used: only the textures and their scale change (RGBA16F instead of 8-bit).
6. **Calibration**. Per map and per colour channel, a gain g minimises the difference between the re-baked non-sun
   coefficient 0 and the shipped one over valid texels (a robust fit: median of ratios over texels that are not near black).
   The lamp falloff exponent and the sky weight are fitted the same way on a few reference maps and then fixed. The tool
   reports the residual. A debug view shows the ratio as a colour, to find where the re-bake disagrees with the shipped art.
7. **Light grid**. The model lighting points (lightgrid.md) are re-baked with the same kernel: rays from each point, the
   same calibration. Moving objects then match the re-baked world.
8. **Cache**. The result goes into the user's cache folder (not the game folder, never the repo), keyed by the map file's
   hash, the bake settings and a format version. The first load bakes (with progress shown on the load screen, or in the
   background with the shipped lightmaps until it is done). Later loads read the cache.

## Runtime

- `r_enhRebake` (0 off by default, and no preset turns it on; to become an opt-in "Remastered lighting" choice once
  it is accepted, no menu row yet). With it on, the world shader samples the re-baked RGBA16F coefficient arrays and the re-baked sun
  mask instead of the shipped ones. The light grid will use the re-baked points (stage 4). Implemented: needs `r_enhanced 1`, is read
  when a level loads (the textures are replaced then), and has no effect in a build without the Metal handles patch.
- Separable lights (later): one coefficient set per group of lamps, so a script or a setting can dim one group. This also
  opens a night variant and time of day (the sun is already separate).
- M1/M2: Metal ray tracing runs there without hardware acceleration. The bake is slower but possible, because it runs once
  per map and is then cached. The runtime cost is the same on every Mac (texture sampling only).

## Stages of the work (each one its own branch and squash merge, each with tests and A/B)

1. Inputs and texel set-up (steps 1–2), plus a debug export of the texel positions and normals. No visible change. **Done.**
2. Direct sun and sky (step 3 without lamps), projection (step 5), calibration of the sky (step 6), and the runtime switch.
   Check: with the sun term only, the re-baked sun mask matches the shipped one, as an IoU per map over sunlit texels. **Done**
   (numbers below); the minimal cache of stage 4 came with it.
3. Lamps and bounces (steps 3–4), with the full calibration. **Done** ("Stage 3" below); what is left is listed there.
4. Light grid (step 7) and the rest of the cache (step 8: the bake no longer blocks the load). **Done** ("Stage 4" below).
5. Separable lights (optional, after a decision).

## Stage 1 and 2: what was built

Everything below is measured on an M4 Pro (Apple GPU family 9, hardware ray tracing); the numbers come from the line `rebake: ...` that
iw2view prints when it bakes (or loads) a map.

**Texel set-up** (`rasterizeTexels`). Two grids per lightmap, because the shipped data has two: 512² for the coefficients and 1024² for
the sun mask (both over the same lightmap coordinates; texel (x, y) has its centre at ((x + .5) / res, (y + .5) / res), row 0 at v = 0).
Coverage is conservative: a texel whose centre is outside every triangle but whose square a triangle touches takes the closest point on
that triangle (state Edge); a texel whose centre is inside takes that point (Inside), and a surface that claims it wins over a
conservative one. One ring of Dilated texels (copies of a neighbour, the four direct neighbours first) follows. A texel whose centre two
triangles claim with different positions (more than 1 unit) or normals (dot below 0.9) is Shared (overlapping or mirrored chart): it
is never traced and the shipped data stays there. Tangent frames are interpolated from the vertices and orthonormalised. Only model 0 (the
static world) is baked: brush submodels move, so their soups keep the shipped data. `IW2_REBAKE_DUMP` writes the position and normal
images (docs/debug-flags.md); on toujane they show the town's ground chart as a plan view, as expected.

**The basis** (our reading of docs/spec/material.md). Coefficient k is the light arriving at a surface whose normal points along basis
direction k: direction 0 is the normal, directions 1–3 are tilted acos(1/√3) from it, at azimuths 90°, 210° and 330° in the (tangent,
binormal) plane (`lightmapBasis`, checked against the weight function in the test). Evidence: over the sky-lit texels of toujane and
moscow the median shipped c_k / c_0 is 0.71–0.77, and the same ratio from the re-baked projections is 0.69–0.79, which is what an
irradiance for tilted normals gives on open ground (an unoccluded tilted normal over a hemisphere gets 0.79); on rhine and
duhoc_assault the pairs are 0.84 / 0.73–0.77 and 0.71–0.75 / 0.75. TODO(spec): this fixes the reading only loosely (it does not say how
the compiler weights the lower hemisphere); the fit is on the sky-lit part only.

**Tracing** (`rebake.mm`, own kernels, own acceleration structure and queue: the bake does not need SDL's command buffers, the runtime
ray tracing features or GPU family 9; M1 and M2 run the same ray queries in software). Occluders are the opaque triangles of the world
and of the static models (the same selection as the runtime scene, without the moving objects). Rays start 1 unit above the texel along
its normal and end at 20000 units.
- Sun: 16 rays in a cone of 0.5° half angle (spiral over the disc, a per-texel rotation), visibility = the fraction that is not blocked.
  A texel that faces away from the sun gets 0. The result is the new sun mask, 1024² bytes, so the runtime sun term is unchanged and
  only its input is sharper and independent of the shipped mask.
- The kernels are checked against a brute-force CPU tracer with the same sampling (`makeCpuTracer`, used by the unit tests for open ground,
  a roof, a wall that blocks half the sky and a wall's shadow, and by `IW2_REBAKE_VERIFY`): on 12 texels of moscow's first lightmap the largest
  difference is 0 for the sun and 1.2e-7 for the sky projections.
- Sky: 128 rays, uniform over the hemisphere (equal steps in height, a spiral in azimuth, a per-texel rotation); each unblocked ray adds
  max(0, d · basis_k) for the four basis directions, times 2 / N, so that open flat ground gives 1 for basis 0. The sky colour is
  white: the calibration below absorbs any constant colour per channel, so a measured one changes nothing until lamps and bounces
  need an absolute scale. TODO(spec): the sky's real directional colour.

**Calibration (stage 2; stage 3 replaces it, see below)**. Per colour channel the gain is the median of shipped c_0 / re-baked c_0 over a fit set: Inside texels where the
shipped c_0 is at least 3/255 in all channels, the re-baked projection is at least 0.05 and the texel is not within 1.25 × radius of a
map light (the re-bake has no lamps yet, so the lit surroundings of lamps would pull the gain up). The median comes from a logarithmic
histogram (3 % bins), so no per-texel arrays are kept.

**Composition**. Texels that were traced (and their dilated copies) get gain × projections; every other texel (unset, shared, lightmaps
without triangles) keeps the shipped value, decoded to float. The coefficient textures are RGBA16F in the *sampled* channel order
(c2, c1, c0, c3: the file's B, G, R, A bytes), so the shader's dot products with `$lightmapweights` are unchanged: with `r_enhRebake` the
only differences are the two textures per lightmap. Shaders are not edited.

**Cache** (stage 4's minimal form, done now because the bake time and size were asked for). One file per map in
`~/Library/Caches/OpenIW2/rebake/` (`$IW2_REBAKE_CACHE` overrides), named `<map>-<hash of the map file>-<hash of the settings>-v<format>.bin`:
the settings are the cone, the ray counts, the calibration thresholds, the sun direction and a hash of the map lights; the format version
(`kFormatVersion`) is bumped with any change of the numbers. Contents: the statistics and, per baked lightmap, the RGBA16F
coefficients and the sun mask, each zlib-compressed. A file that fails to parse (or has another version) is ignored and baked again.
Not done: progress on the load screen or a background bake (the load waits for the bake, 0.3–1.4 s on these maps).

### Measured (M4 Pro, hardware ray tracing; one bake, no cache)

IoU is over texels that face the sun (N·L above 0.05) and whose centre a triangle covers, a texel counting as sunlit above 127 / 255 in
both masks. The luminance ratio is the mean of the calibrated re-baked c_0 luminance over the shipped one (Rec. 709 weights): over the fit
set, and over every covered texel.

| map | lightmaps | covered coef. texels | sun IoU (facing / all) | gain r g b | fit texels | lum. ratio fit / all | bake | cache |
|---|---|---|---|---|---|---|---|---|
| toujane | 13 | 1 296 030 | 0.972 / 0.969 | 0.454 0.441 0.418 | 365 420 | 0.907 / 0.564 | 1.34 s | 27.6 MB |
| moscow | 5 | 274 024 | 0.878 / 0.870 | 0.651 0.669 0.727 | 165 868 | 0.975 / 0.840 | 0.49 s | 9.0 MB |
| decoytown | 3 | 329 929 | 0.979 / 0.974 | 0.096 0.117 0.227 | 10 741 | 0.968 / 0.326 | 0.34 s | 6.2 MB |
| rhine | 4 | 186 417 | 0.935 / 0.929 | 0.551 0.551 0.616 | 64 895 | 0.913 / 0.669 | 0.40 s | 7.2 MB |
| duhoc_assault | 5 | 252 241 | 0.917 / 0.910 | 0.454 0.467 0.521 | 77 558 | 0.982 / 0.789 | 0.47 s | 6.1 MB |
| elalamein | 9 | 587 914 | 0.929 / 0.926 | 0.551 0.633 0.768 | 241 750 | 0.955 / 0.652 | 0.66 s | 10.7 MB |

Reading the numbers:
- The sun mask is close to the shipped one where the casters are right: 0.92–0.98 on five of the six maps. The disagreement is mostly a
  texel lit in the re-bake and shadowed in the shipped mask (moscow 44 871 baked-only against 15 504 shipped-only of 496 794 facing
  texels; duhoc_assault 43 811 against 9 538; elalamein 58 303 against 11 538): something casts in the shipped bake that is not an opaque
  triangle of the world or of the static models. TODO(spec): candidates are alpha-tested surfaces tested properly (stage 3 reuses the
  runtime ray tracing's alpha masks; counting them as solid made moscow worse, 0.823), script models and entities placed at load.
- The sun cone does not matter at this texel size: toujane 0.970 / 0.971 / 0.972 / 0.972 and moscow 0.878 / 0.878 / 0.878 / 0.875 for half
  angles 0.1 / 0.25 / 1 / 2 degrees (0.5 is the default). The shipped mask's softness is therefore not recoverable from this.
- Static models as casters raise the IoU (toujane 0.961 → 0.972, moscow 0.849 → 0.878, decoytown 0.967 → 0.979), so they stay in.
- The luminance ratio over the fit set is 0.91–0.98 (within the 10 % of the acceptance, by construction of a median fit), but over every
  covered texel it is 0.33–0.84: this stage has the sky only. Interiors and everything that the shipped bake lit with lamps or bounced light
  stays darker. That is the known gap stage 3 closes; the acceptance (within 10 % over all texels) is not met yet. decoytown is the
  extreme (0.33): its shipped light is mostly lamps and its sky gain is 0.1.
- The bake is 0.3–1.4 s, dominated by the rasterisation and the sun grid (1024², 4 M texels on toujane); the cache is 6–28 MB per map
  (RGBA16F is twice the shipped 8 bits, and zlib hardly helps floats).

Runtime cost (duhoc_assault beach, 1080p, GPU-bound ms per frame, minimum of three runs of 120 frames, `r_enhRebake 0` / `1`): high preset
9.27 / 9.23 looking at the sea and 10.99 / 11.03 inland, medium preset 9.22 / 9.22 and 11.05 / 11.64 (that last pair is a single disturbed run, the machine was shared: its median was 14.86 ms against
11.10; the other three pairs agree within 0.05 ms). The shader and its texture reads are
unchanged, so the cost is the same as with the shipped textures, apart from RGBA16F reading twice the bytes of 8-bit data. Memory: the
coefficient array grows from 3 MB to 6 MB per lightmap (toujane: 13 lightmaps, +39 MB).

A/B (the base build against this branch, ten fixed views each): the default look, and the low, medium and high enhanced presets with
`r_enhRebake 0`, are identical on both SDL builds (two toujane shots, one in the low and one in the medium preset, differed slightly on the first pass: 0.4 % and 0.6 % of the pixels; the cause
was not traced, and repeating both with the base build twice and this branch once gave three identical images each).

### Decisions and deviations from the plan above (stage 2; the alpha, lamp, sky-gain and ratio-view points are superseded by stage 3 below)
- The coefficient grid stays at 512² (1×) and only the sun mask is baked at 1024²: the plan's 2× option (1024² coefficients, 24 MB
  per lightmap in RGBA16F) is not implemented; every texel and layout stays as shipped, so the runtime needs no change.
- Alpha-tested surfaces, glass and water do not cast in this stage (see the IoU discussion); the runtime scene's alpha machinery is
  for stage 3.
- The minimal cache is part of stage 2 (the plan had it in stage 4).
- The sky colour input is a constant white (the per-channel calibration absorbs it); albedo is not needed before bounces.
- The ratio debug view of step 6 is not built; the statistics line stands in for it.
- Only model 0 is baked. Texels of Shared charts and of brush submodels keep the shipped data.

## Stage 3: lamps, bounces, alpha casters and the full calibration

**Lamps** (`Tracer::lamps`). For every traced texel and every `light` entity (`scene.mapLights`): the falloff is `game::mapLightFalloff`
(our own (1 - d / r)² with the spot cone), the light must be in front of the surface, and four rays go to points spread over a 6-unit
box around the lamp (jittered per texel) for a soft visibility. The contribution to coefficient k of colour channel c is
colour_c × falloff × visibility × max(0, n_k · l), n_k the basis direction k in the world, l the direction to the lamp. TODO(spec): the
original compiler's falloff and the lamps' real size; the gain absorbs the scale only, not the shape.

**Alpha-tested casters**. The alpha-tested batches are separate geometry groups of the acceleration structure, non-opaque, and every ray
candidate on them is tested in the kernel against a 64 × 64 alpha mask of its colour map (our own compute pass reads the map's mip
level through the Metal handle of the SDL texture; the shipped alpha function GE128 / GT0 / LT128 as in docs/spec/material.md) with
`intersection_query`, so it works on every Metal ray tracing GPU, without the runtime scene's masks. `IW2_REBAKE_ALPHA` 0 / 1 / 2: they do
not cast / cast where the test passes (default) / cast as solid. Sun IoU over facing texels (bounces off):

| map | off | tested | solid |
|---|---|---|---|
| toujane | 0.972 | 0.971 | 0.961 |
| moscow | 0.878 | 0.924 | 0.823 |
| decoytown | 0.979 | 0.975 | 0.962 |
| rhine | 0.935 | 0.922 | 0.865 |
| duhoc_assault | 0.917 | 0.913 | 0.824 |
| elalamein | 0.929 | 0.957 | 0.942 |

Tested always beats solid. It beats off on moscow and elalamein (where the shipped masks have the shadows of foliage and fences) and loses
0.1 to 1.3 points on the other four: on those the kernel casts more shadow than the shipped bake does (moscow: 32 619 shipped-only against
1 631 baked-only texels now). TODO(spec): the shipped compiler's rule for which alpha-tested surfaces cast, and the alpha level it tests.
The default stays "tested".

**Bounces** (`Tracer::bounce`). A closest-hit hemisphere pass (64 rays, the same alpha test). Each triangle of the scene carries its
lightmap index, its three lightmap coordinates, its texture slot and, for surfaces without a lightmap (static models), the light baked into
its vertices (`GpuVertex::light`, displayed units). A hit reads the irradiance cache (RGB per texel of every lightmap's 512² grid, in
displayed units: sun × N·L × sun visibility + sky + lamps, plus the previous bounce) at the hit's lightmap coordinates, multiplies by the
albedo, and the result is projected onto the four basis directions like the sky. The cache holds the shipped coefficient 0 where nothing
was traced (shared and unset texels). Two passes. Albedo is the mean of an 8 × 8 grid of the colour map at a small mip level, weighted by
alpha (`meanAlbedo`; TODO(spec): the stored encoding is used as it is, and the material colour is not multiplied in).

**Calibration, replaced**. A single gain fitted over everything would make the all-texel ratio 1 by construction and prove nothing, so the terms are
fitted separately, from the texels where each one dominates, and iterated (`Config::fitRounds`, 3 rounds):
1. sky gain per channel: median of shipped / sky over texels no lamp reaches and with sky visibility above 0.05;
2. lamp gain (one): the ratio of the sums of (shipped - sky - bounce) over the lamp-lit texels (luminance, at least 0.02) to the sum of the lamp
   term, not the median, because the lamp term has a heavy tail near the lamp and a median fit left the lamp-lit class 1.4 to 2.9 times too
   bright;
3. bounce gain per channel: median of (shipped - sky) / bounce over texels with no lamp and a sky visibility below 0.5 (indoor);
4. an overall correction per channel (the median of shipped / total over texels where both are measurable), folded into the gains, then the next
   round builds the cache and bounces again with the new gains. The correction of the last round (how far the terms still were from the
   shipped light) is in the statistics line: 0.93 to 1.01.

### Measured, stage 3 (M4 Pro, hardware ray tracing; sun IoU unchanged by lamps and bounces, with alpha tested)

Luminance ratio = sum of the re-baked c₀ luminance over the shipped one, over all covered Inside texels and by class: **sky-lit** (sky
visibility at least 0.5, no lamp), **lamp-lit** (a lamp's c₀ luminance at least 0.02) and **other** (indoor and bounced light); *within 25 %*
is the share of texels (shipped luminance at least 3/255) whose re-baked luminance is within ±25 % of the shipped one.

| map | all | sky-lit | lamp-lit | other | within 25 % | sky and lamps only | sun IoU facing | bake | cache |
|---|---|---|---|---|---|---|---|---|---|
| toujane | 1.009 | 1.020 | 1.064 | 0.987 | 0.728 | 0.716 | 0.971 | 5.5 s | 29.6 MB |
| moscow | 1.069 | 1.056 | 1.046 | 1.093 | 0.661 | 0.870 | 0.924 | 2.3 s | 9.9 MB |
| decoytown | 1.006 | 0.981 | 1.100 | 0.799 | 0.415 | 0.670 | 0.975 | 1.3 s | 8.1 MB |
| rhine | 1.019 | 1.022 | 1.052 | 1.003 | 0.798 | 0.858 | 0.922 | 1.7 s | 8.0 MB |
| duhoc_assault | 1.039 | 1.031 | 1.046 | 1.063 | 0.840 | 0.845 | 0.913 | 2.6 s | 7.5 MB |
| elalamein | 1.045 | 1.013 | 1.041 | 1.118 | 0.728 | 0.788 | 0.957 | 1.8 s | 14.0 MB |

The all-texel acceptance (within 10 %) is met on all six maps (1.01 to 1.07), where stage 2 had 0.33 to 0.84. Without the bounces the ratio is
0.67 to 0.87 so the bounces carry the indoor light as the plan expected. What stays
open, measured:
- The per-texel spread is wide: 41 % (decoytown) to 84 % of the texels are within ±25 %. The mean is right and the texel is often not.
  decoytown is the worst: its lamp-lit class is 1.10 and its indoor class 0.80 (lamps whose radius and falloff differ from our model, a
  shipped bake lit by more than the `light` entities).
- The lamp gain is near 1 (0.6 to 1.35), i.e. our falloff and the entities' `intensity` times `_color` are about the shipped lamp scale. TODO(spec):
  unmeasured whether that is by luck.
- The bounce gain is 1.2 to 2.6: our bounce is weaker than the shipped one. Candidates: the albedo (stored encoding, no material colour), only
  two bounces, the static models' vertex light used for their hits. Not separated.
- The "other" class is 0.80 on decoytown and 1.12 on elalamein, which the global gains cannot both fix.
- Colour cast indoors: in the elalamein house (-13690,-7060,50) the re-bake is greener and more yellow than the shipped light. Measured cause: with
  `IW2_REBAKE_BOUNCES=0` the room is neutral grey (and too dark); the cast is the bounce, whose albedo is the saturated sand colour map used as stored
  and compounded over two passes, then raised by a bounce gain of 2.6. The per-channel bounce gain reduced it but did not remove it. Candidate
  fixes (not done): desaturate the albedo towards its luminance, undo the colour map's gamma, fit the bounce gain per lightmap.
- The sky is white and uniform; the sun-facing side of the sky is brighter in the shipped bake.
- `IW2_REBAKE_RESIDUAL=<dir>` writes the per-lightmap ratio images (below); on toujane the errors are local patches, red around some house walls
  and blue in a few courtyards, not a global tilt.

**Residual view** (step 6's debug view). `IW2_REBAKE_RESIDUAL=<dir>` bakes without reading or writing the cache and writes
`<dir>/<map>/lmNN_ratio.ppm` (512 × 512): the re-baked over the shipped c₀ luminance as log₂, blue below (down to a quarter), white equal,
red above (up to four times), black not traced.

The Metal kernels match the CPU reference tracer (sun, sky and lamps: differences of 1e-7 on texels of elalamein's lightmap 1, with
`IW2_REBAKE_ALPHA=0`, since the CPU tracer has no textures). The bounce kernel is covered by the CPU tracer's analytic test (a closed box)
and by the images.

Runtime cost with the stage 3 bake (duhoc beach, 1080p, GPU-bound ms per frame, minimum of three runs, `r_enhRebake` 0 / 1): high 7.60 / 7.48
looking at the sea, medium 7.44 / 7.55; inland the shared machine disturbed the runs (high 8.21 / 9.86, medium 9.80 / 8.29: the same
configuration differs more between runs than the two settings do), so no cost of `r_enhRebake 1` shows; the shader is unchanged. A/B of the
base build against this branch with `r_enhRebake 0`: the default look (both SDL builds) and the low, medium and high presets are identical in
all ten views each. (In an earlier pass one rhine view of the medium preset differed by 99.8 % of its pixels; repeating it with the base build
twice and this branch once gave three identical images, so it was a one-off; the cause was not traced.)

## Stage 3 follow-up: the bounce colour cast

The elalamein house (-13690,-7060,50) came out greener and more yellow than the shipped light. Candidates, measured on toujane, moscow and elalamein
(all-texel ratio stays within 10 % in every row; *colour error* is the mean of |shipped - re-baked| of the red and blue shares of the luminance over
the covered texels, *within 25 %* the share of texels within ±25 % of the shipped luminance):

| candidate | toujane colour error / within 25 % | moscow | elalamein |
|---|---|---|---|
| before (albedo as stored, no lightmap gain) | 0.072 / 0.728 | 0.048 / 0.661 | 0.105 / 0.728 |
| albedo gamma 2.2 (stored encoding undone) | 0.207 / 0.774 | not run | 0.289 / 0.645 |
| albedo desaturated to 0.5, then 0 | 0.033 (scalar bounce gain) | 0.030 | 0.067 |
| per-lightmap channel gain only | 0.053 / 0.802 | 0.043 / 0.673 | 0.107 / 0.731 |
| **grey albedo + per-lightmap gain (now the default)** | **0.029 / 0.796** | **0.019 / 0.672** | **0.069 / 0.732** |

- Undoing the gamma is wrong for the colour error (it raises it three times) and it lowers the within-25 % share on elalamein by 8 points, so it
  is not used (`IW2_REBAKE_ALBEDO_GAMMA` keeps it for experiments).
- Desaturating the albedo to its luminance (`Config::albedoSat` 0) is what removes the cast: the coloured sand albedo was compounded over two
  passes and then multiplied by a bounce gain above 2. A per-channel or a single bounce gain gives the same result with a grey albedo.
- The per-lightmap channel correction (median shipped / re-baked per lightmap, clamped to 0.6 .. 1.6) adds 7 points of within-25 % on toujane and
  nothing on the others; together they halve the colour error on the three maps and on rhine and duhoc_assault (0.052 to 0.025 and 0.054 to 0.018).
- In the house the cast is gone but the room is still warmer than the shipped one (colour error on elalamein stays 0.069). decoytown's colour error
  (0.24) does not move with any of these: its shipped light is a night scene whose sky tint our white sky cannot follow. TODO(spec).

Final stage-3 numbers with the defaults (M4 Pro; the bake time includes the light grid of stage 4; cache with the grid):

| map | all | colour error | within 25 % | sun IoU | bake | cache |
|---|---|---|---|---|---|---|
| toujane | 0.993 | 0.029 | 0.796 | 0.971 | 5.7 s | 37.7 MB |
| moscow | 1.041 | 0.019 | 0.672 | 0.924 | 2.6 s | 55.3 MB |
| decoytown | 0.982 | 0.240 | 0.426 | 0.975 | 1.3 s | 23.0 MB |
| rhine | 1.016 | 0.029 | 0.801 | 0.922 | 1.8 s | 16.4 MB |
| duhoc_assault | 1.015 | 0.022 | 0.847 | 0.913 | 3.8 s | 43.0 MB |
| elalamein | 1.053 | 0.069 | 0.732 | 0.957 | 2.0 s | 48.0 MB |

## Stage 4: light grid and the background bake

**Light grid** (step 7). The grid's 8 coefficients per colour channel are read as the light arriving at a normal pointing along one of the 8 corners
of the cube in the grid's t space (`game::rebake::gridBasis()`: the three axes 54.7° from vertical, 120° apart, coefficient k at the corner whose
bits 0, 1, 2 are the signs of t₀, t₁, t₂; checked in the test against `LightGrid::weights`, which is 1 at its own corner). Every grid point (entry of
lump 2, `LightGrid::points()`) is traced over the whole sphere with the same terms and the same kernels' structure as a lightmap texel
(`Tracer::gridSky`, `gridLamps`, `gridBounce`): the sky as a dome (rays with d.z ≤ 0 never see it), the lamps with the same falloff and soft
visibility, the bounce from the irradiance cache of the lightmap bake. The terms are combined with the lightmap bake's gains
(correction × (sky gain × sky + lamp gain × lamps + bounce gain × bounce)), then one more correction per channel is fitted against the shipped
grid (median of shipped / re-baked where the shipped coefficient is at least 3/255) and applied. The sun is not re-baked: the grid keeps the
shipped one-bit sun visibility, and models still get `sunVisibility × N·L × sun` on top. The result replaces the shipped bytes
(`LightGrid::setEntryCoefficients`, floats, per entry), so entities, the per-pixel grid lattice and the static-model lighting block use it at once;
the static-model lighting block is made again from the new grid, and the grid's sample cache is keyed on the grid's epoch. The light baked into
the vertices of static models that do not use the block (loaded before the bake in a background bake) follows at the next load from the cache.

| map | points | shipped mean | ratio with the lightmaps' gains (within 25 %) | grid correction | ratio after (within 25 %) |
|---|---|---|---|---|---|
| toujane | 96 888 | 0.34 | 1.019 (0.818) | 0.99 1.01 0.99 | 1.014 (0.816) |
| rhine | 102 142 | 0.40 | 1.093 (0.720) | 0.93 0.93 0.93 | 1.020 (0.768) |
| duhoc_assault | 440 551 | 0.38 | 1.077 (0.628) | 0.93 0.93 0.93 | 1.004 (0.690) |
| elalamein | 414 002 | 0.41 | 1.470 (0.091) | 0.65 0.69 0.69 | 0.995 (0.968) |
| decoytown | 176 823 | 0.14 | 1.573 (0.044) | 0.57 0.60 0.73 | 1.013 (0.740) |
| moscow | 670 660 | 0.10 | 2.909 (0.030) | 0.47 0.45 0.45 | 1.331 (0.268) |

On toujane, rhine and duhoc_assault the lightmaps' calibration carries over to the grid within 10 % without a grid-specific correction, which is what
"same calibration" was meant to give. On elalamein, decoytown and moscow it does not: the re-baked light is 1.5 to 2.9 times the shipped grid's, and a
median correction brings the mean to 1.0 (elalamein, decoytown) or 1.33 (moscow, whose grid points are mostly open air with a shipped mean of 0.10
against a lightmap-calibrated sky of about 0.5: the shipped grid there is much darker than its lightmaps, so a single sky term cannot fit both).
TODO(spec): whether the shipped grid carries an `indirectLightScale`-like factor or its own sky term; unmeasured. Without the grid-specific correction
the models on those three maps would be too bright, which is why it is always applied.

Bake cost and cache: the grid adds 0.1 to 1.2 s and 8 to 45 MB per map (96 bytes per point, zlib hardly helps floats; moscow has 670 660 points); the cache files above include it.
`IW2_REBAKE_NO_GRID=1` leaves it out (and out of the cache key).

**A bake that does not block the load**: background. A window starts the level with the shipped lightmaps and the shipped grid, and the bake runs
on a worker thread; the main loop installs the result in the frame after the thread has finished (new RGBA16F textures, the new grid, the new
static-model lighting block). Reasons: the first bake is 1 to 6 s on the reference maps, which is a pause the player would see on every first load
of a map on a slow Mac (M1 without hardware ray tracing will be several times slower), but the shipped lightmaps are always a valid picture, so
there is no need to hide the wait behind a progress bar; the cache makes every later load instant (it is read on the loading thread, then installed
at once, no flash); and a load-screen progress bar would need a progress channel through the tracer's chunks that the single-shot tools and tests
do not use. Costs of the choice: the picture changes once (lighting pops to the re-baked one) a few seconds into the level on a first load, and
while the bake runs the GPU is shared with the frame (the tracer works in chunks of at most 32 768 texels per command buffer, so frames are
delayed, not stalled; not measured on an M1). A single `--shot` (and every headless tool) waits for the bake, so its picture is always the re-baked
one. The worker owns copies of everything it reads (triangles, textures' handles, the shipped lightmaps, the grid's points and values); the
scene's destructor path joins it before textures are released (`RebakeJob`, `releaseScene`), so changing level while it runs waits for it to
finish. Tested by running a window on toujane with an empty cache (bake, install, screenshot, exit), then again with the cache.

### What stage 4 and later need
- (done in stage 4: the light grid and the background bake)
- Cancelling a background bake when the level changes instead of waiting for it; a progress note for slow Macs; the vertex-baked static-model light.
- Separable lamps (stage 5): the lamp term is already separate in the tracer's output and in `bakeMap`.
- To tighten the spread: the sky's direction-dependent colour, the lamp falloff exponent (our (1 - d / r)²) fitted per map class, the albedo (material
  colour, linearisation) and a third bounce.

#### What stage 3 needed (done)
- Lamps: the `light` entities (`scene.mapLights`) with our falloff (`game::mapLightFalloff`), one visibility ray per lamp and texel
  (soft), added to the projection per basis direction through max(0, n_k · l); the calibration of the exponent on the reference maps
  (the fit set excludes texels near lamps now, which stays right for the sky gain). TODO(spec): the original compiler's falloff.
- Bounces: an irradiance cache in lightmap space (the previous pass's RGBA16F result sampled at the hit's lightmap coordinates, times the
  hit material's albedo): the tracer must return the hit triangle and barycentrics, so it needs the per-triangle lightmap coordinates and
  a per-triangle material albedo (the mean of the colour map's smallest mip times the material colour; step 1's albedo input).
- Alpha-tested casters with per-candidate alpha tests (the runtime scene has the masks), and a closer look at the casters that the shipped
  sun mask has and ours lacks (moscow, duhoc_assault, elalamein).
- A calibration that also looks at the lit and indoor texels (the all-texel ratio is the acceptance), with a residual map for the debug view.
- The tracer interface stays (`game::rebake::Tracer`); `bakeMap` gets the lamps and the passes.

## Acceptance

- Default look and every existing enhanced preset: identical (A/B) while `r_enhRebake` is 0.
- With it on: on the reference maps (toujane, moscow, decoytown, rhine, duhoc_assault, elalamein) the calibrated mean
  luminance of the non-sun light is within 10 % of the shipped value per map. No visible seams at lightmap chart borders.
  No light leaks under walls (checked in bunkers and houses).
- Bake time and cache size are reported per map, M4 Pro (and M1 when one is available).

## Risks

- Art direction: the shipped bake has hand tuning (lamp intensities, `ambient`) that a physical re-bake will not reproduce.
  Calibration per map limits this, but the result may still look different. That is why it is opt-in.
- Lightmap charts with overlapping or mirrored coordinates (shared texels). Detect them in step 2 and keep the shipped data
  there.
- Static models are not in the lightmaps. They cast shadows in the bake (their triangles join the scene) but receive light
  from the grid.
