# Enhanced rendering mode

An openiw2 addition with no counterpart in the original game. The default renderer keeps the original look (docs/spec);
`r_enhanced 1` switches to a second set of pipelines with dynamic sun shadows of moving objects, a 16-bit float scene, a
highlight tone curve, bloom of the light above display white, mild auto exposure and extended dynamic range (EDR) output.
Everything here is our own design from public techniques (references at the end). It is not derived from the shipped game
shaders, and none of it is a specification of the original.

Where this mode depends on what the original data means (the lightmap and its sun mask, the light grid, the sun direction and
colour), it uses the meaning given in docs/spec (material.md "Lightmaps", lightgrid.md).

## Switching it on

| Cvar | Default | Meaning |
|---|---|---|
| `r_enhanced` | 0 | 1 = enhanced mode. Read when the renderer starts (like `r_aaSamples`), so it applies at the next start. |
| `r_enhPreset` | 1 | Quality preset: 0 low, 1 medium, 2 high. Fills the per-feature settings below. |
| `r_enhSunShadows` | preset | Sun shadows of moving objects: 0 off, 1 low (1024², 4 taps), 2 medium (2048², 8 taps), 3 high (two 2048² cascades, 12 taps). |
| `r_enhGridPerPixel` | 1 | Light-grid lattice per model instead of one sample per model. |
| `r_enhTonemap` | 1 | Highlight shoulder instead of the clip at white. |
| `r_enhBloom` | 1 | Bloom of the light above display white. |
| `r_enhBloomIntensity` | 0.06 | Bloom strength (0..1). |
| `r_enhAutoExposure` | preset | Mild auto exposure (off in the low preset). |
| `r_enhExposureBias` | 0 | Exposure offset in EV (-2..2). |
| `r_enhEdr` | 1 | EDR output on displays that offer it (Liquid Retina XDR, HDR monitors). |
| `r_enhMotionDebug` | 0 | 1 shows the motion vectors instead of the frame (debug). |

All are read once when the renderer starts, from the profile's config or `--cvar name=value` (case-insensitive). A per-feature
cvar that is set overrides the preset. Presets: low = shadows 1, no auto exposure; medium = shadows 2; high = shadows 3. The
other features are on in every preset. Gameplay, the server and the network are not affected; it is render-only.

## How it stays apart from the default look

- The scene renders into RGBA16F instead of the window format. The frame after it (composite, HUD, menus) keeps the window
  format.
- The scene shaders are compiled a second time with `IW2_ENHANCED` defined (apps/iw2view/shaders_enhanced.metal.inc +
  shaders.metal.inc + shaders_enhanced_post.metal.inc). The hooks in shaders.metal.inc sit inside `#ifdef IW2_ENHANCED`, so
  the default library is unchanged. The HUD always uses the default `fs_main`.
- With `r_enhanced 0` nothing of this runs. Each change was checked with screenshots of fixed views (several maps, 4x and 1x
  MSAA, `r_gamma` above 1) taken before and after it: the default frames are pixel-identical.
- With `r_enhanced 1` and every feature off, the frame differs from the default by rounding (16-bit instead of 8-bit
  blending: over 95 % of the differing pixels by 1 or 2 / 255, a few by up to 6) and, more, where light summed above 1
  (additive effects, fire, their glow), which the 8-bit target clipped.

## Sun shadows (moving casters)

The original bakes the sun's visibility into a 1024² mask per lightmap and adds `mask · N·L · sun` at runtime
(docs/spec/material.md "Lightmaps"). Its shaders leave room for a dynamic shadow factor that multiplies the mask. The
enhanced mode fills that factor with a real-time shadow map of the objects the bake cannot know:

- Casters: every drawn xmodel entity (actors, vehicles, script models, dropped weapons) and brush mover (doors, gates), opaque
  and alpha-tested surfaces (alpha-tested texels below half alpha do not cast). Not the first-person view model, not effect
  sprites. The world and static models are already in the baked mask.
- Projection: orthographic along the sun direction, around a sphere ahead of the camera (40 % of the radius forward, so the
  camera stays inside). The sphere centre is snapped to whole shadow texels in the light plane, so the shadow does not crawl
  as the camera moves (`game::fitSunShadow`). The depth range reaches 3000 units beyond the sphere towards the sun.
- Receivers: world surfaces (lightmapped, with and without the specular technique), static models, entity models.
  `sun visibility = baked mask × dynamic shadow`. Multiplying means a moving caster standing in baked shade adds nothing, and
  nothing is darkened twice. Static models carry their one sun visibility per vertex, so the dynamic shadow removes that
  share of their baked sun term.
- Filtering: hardware 2 × 2 percentage-closer filtering per tap, 4 / 8 / 12 taps on a rotated Poisson disk, rotation per pixel
  from interleaved gradient noise. Normal offset of 1.5 texels plus a small depth bias and the rasterizer's slope bias against
  acne. The last cascade fades out over the outer 15 % of its radius.
- High preset: two cascades, 700 and 2800 units, side by side in one depth texture.

Limitations: casters are the entities the view already gathers, so an actor behind the camera that is not gathered casts
nothing into view. A brush mover's baked shadow stays where the bake put it when it moves. The view model (arms and gun)
receives shadows but does not cast. Effect sprites neither cast nor receive.

## Per-pixel light grid

The original lights an entity from one light-grid sample at its origin (docs/spec/lightgrid.md); a whole tank is in one light.
Enhanced: every model item within 2500 units gets a lattice of the light grid's own nodes around its world box (at most 4 per
axis, `game::probeLattice`). The fragment shader interpolates the node coefficients and the node sun bits trilinearly at the
pixel, which is the grid's own interpolation, so a soldier walking from shade into sun is lit gradually across the body. Node
samples are cached per map, so the cost per frame is a lookup. Missing nodes take the item's own sample.

## Tone curve

The scene keeps the original's display encoding (the shaders light in that encoding, as the original does), so the tone curve
works on display values per channel: the identity up to a knee of 0.8, then an exponential shoulder that reaches white
asymptotically with slope 1 at the knee (`game::toneShoulder`). Everything the original could show without clipping is shown
unchanged. What the original clipped (bright snow on moscow, fire, the sun on water) rolls off instead. SDR white is 1. With
EDR, white is the display's current headroom in the same encoding.

Exposure and bloom work on linear light: the sRGB transfer function decodes the scene value, the result is encoded again
before the shoulder. With exposure 0 and no bloom the values pass through unchanged.

## Screen blur

While the screen is blurred (pain, shell shock), the blur reads the tone-mapped scene, so it has the same exposure and
highlight shoulder as the sharp frame under it.

## Bloom

The original glow chain stays as it is. It reads the scene clamped to 1, so maps keep their glow and sky bleed. On top of
that, light above display white blooms: a soft-knee threshold at linear luminance 1 (knee 0.5) on a half-size prefilter (four
taps, each damped by `1 / (1 + luma / 4)` against single-pixel flicker), a chain of 13-tap downsamples to about 8 pixels, then
tent upsamples added back up the chain. The result is added on linear light at `r_enhBloomIntensity`.

## Auto exposure

A 256² centre-weighted log2 luminance image of the scene (weight 1 in the middle, 0.3 at the edges), averaged by its mip
chain. Target exposure in EV = `clamp((key - mean) · 0.3, -0.4, +0.5)`, adapted exponentially per frame on the client render
clock (time constants 1.2 s brightening, 0.5 s darkening). Calibration: the key -3.0 is the median of that mean over ten
reference views of seven maps in the default look (daylight views -1.7 to -3.5, night and interiors about -6.3), so a
typical daylight view stays within ±0.25 EV of the original and dark interiors gain at most half a stop.

## EDR output

With `r_enhEdr 1`, where the window can use it, the swapchain is extended linear sRGB (RGBA16F). The frame is composed as
usual (HUD included) in display encoding, then a final pass applies the `r_gamma` ramp to the part up to 1 and decodes to
linear with the sRGB transfer function extended past 1. The tone curve's white follows the window's HDR headroom every frame.
Screenshots stay 8-bit SDR images with white 1.

## Motion vectors

The camera motion of the static scene: from `$floatz` (clip z of the opaque surfaces, built at the start of the frame) and this
frame's inverse view-projection, each pixel's world position is projected with the previous frame's view-projection; the
vector is the NDC difference (RG16F, `game::cameraMotion`). Only the debug view consumes it so far (`r_enhMotionDebug 1`,
which also turns on the `$floatz` build). Object motion (rigid entities' previous transforms and the previous frame's skinned
vertices) comes with temporal anti-aliasing in a later stage.

## Cost

GPU-bound frame time with `IW2_BENCH_GPU` (frames submitted back to back), 1920 × 1080, 4x MSAA, Apple M4 Pro (20-core GPU),
the duhoc_assault beach reference spot (docs/debug-flags.md `IW2_BENCH`), yaw 180 (483 shadow casters):

| Configuration | GPU-bound ms | vs default |
|---|---|---|
| default | 2.15 | |
| enhanced, every feature off (16F scene) | 2.29 | +0.14 |
| + sun shadows medium (receivers: world, models) | 2.90 | +0.61 |
| + light grid per pixel | 2.49 | +0.20 |
| + tone curve and bloom | 2.57 | +0.28 |
| + auto exposure | 2.58 | +0.29 |

The CPU cost is the shadow pass encode (about +0.2 ms for 483 casters) and the lattice lookups (under 0.1 ms with the cache).

## Tests

`tests/test_renderenhanced.cpp`: settings and presets, the tone curve (identity below the knee, C1, monotonic, bounded), the
sRGB transfer, exposure target and frame-rate independent adaptation, bloom levels and threshold, the sun shadow fit (sphere
inside the map, depth order towards the sun, texel-stable snapping, a vertical sun), the cascade fade, the probe lattice and
the camera motion reconstruction.

## References

- L. Williams, "Casting curved shadows on curved surfaces", SIGGRAPH 1978 (shadow maps).
- W. Reeves, D. Salesin, R. Cook, "Rendering antialiased shadows with depth maps", SIGGRAPH 1987 (percentage-closer filtering).
- M. Valient, "Stable rendering of cascaded shadow maps", ShaderX6, 2008 (texel snapping).
- D. Holbert, "Normal offset shadows", GDC 2011.
- J. Jimenez, "Next generation post processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014 Advances course
  (13-tap downsample and tent upsample bloom, interleaved gradient noise).
- E. Reinhard et al., "Photographic tone reproduction for digital images", SIGGRAPH 2002 (log-average luminance and key).
- IEC 61966-2-1 (sRGB transfer function).
