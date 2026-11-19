# Enhanced rendering mode

An openiw2 addition with no counterpart in the original game. The default renderer keeps the original look (docs/spec);
`r_enhanced 1` switches to a second set of pipelines with dynamic sun shadows of moving objects, cached sun shadow cascades of
the world, ambient occlusion and contact shadows, temporal anti-aliasing, GGX specular with sky and screen-space reflections, shadows of
the effect lights, sun shafts, an optional film grade and motion blur, a 16-bit float scene, a highlight tone curve, bloom of
the light above display white, mild auto exposure and extended dynamic range (EDR) output.
Everything here is our own design from public techniques (references at the end). It is not derived from the shipped game
shaders, and none of it is a specification of the original.

Where this mode depends on what the original data means (the lightmap and its sun mask, the light grid, the sun direction and
colour), it uses the meaning given in docs/spec (material.md "Lightmaps", lightgrid.md).

## Switching it on

| Cvar | Default | Meaning |
|---|---|---|
| `r_enhanced` | 0 | 1 = enhanced mode. Read when the renderer starts (like `r_aaSamples`), so it applies at the next start. |
| `r_enhPreset` | 1 | Quality preset: 0 low, 1 medium, 2 high, 3 ultra (high plus ray-traced global illumination; on GPUs without hardware ray tracing it is the high preset). Fills the per-feature settings below. |
| `r_enhSunShadows` | preset | Sun shadows of moving objects: 0 off, 1 low (1024², 4 taps), 2 medium (2048², 8 taps), 3 high (two 2048² cascades, 12 taps). |
| `r_enhGridPerPixel` | 1 | Light-grid lattice per model instead of one sample per model. |
| `r_enhTonemap` | 1 | Highlight shoulder instead of the clip at white. |
| `r_enhBloom` | 1 | Bloom of the light above display white. |
| `r_enhBloomIntensity` | 0.06 | Bloom strength (0..1). |
| `r_enhAutoExposure` | preset | Mild auto exposure (off in the low preset). |
| `r_enhExposureBias` | 0 | Exposure offset in EV (-2..2). |
| `r_enhEdr` | 1 | EDR output on displays that offer it (Liquid Retina XDR, HDR monitors). |
| `r_enhWorldShadows` | preset | Sun shadow cascades of the world and static models: 0 off, 1 two cascades (1100 / 4500 units), 2 three (700 / 2400 / 8000). |
| `r_enhAO` | preset | Ground-truth ambient occlusion on the baked and probe light. |
| `r_enhAOStrength` | 0.7 | AO strength (0..1). |
| `r_enhContactShadows` | preset | Short screen-space rays towards the sun (thin and near contact). |
| `r_enhTAA` | preset | Temporal anti-aliasing. |
| `r_enhGGX` | 1 | GGX sun specular and reflections from the spec / gloss maps (else the original Phong). |
| `r_enhSpecScale` | 1 | Sun specular scale (0..4). |
| `r_enhEnvScale` | 0.35 | Reflection strength (0..2). |
| `r_enhSSR` | preset | Screen-space reflections on water and the reflective surface classes (needs TAA). |
| `r_enhLightShadows` | preset | Shadows of the dynamic (effect) lights, up to four lights. |
| `r_enhVolumetric` | preset | Sun shafts: light scattered in the air through the sun's shadow (0 off .. 2; 0.35 in the high preset, else 0). |
| `r_enhGrade` | 0 | Colour grade: 0 neutral, 1 "period film" (contrast, desaturation, warm shadows, grain). |
| `r_enhMapLightSpec` | preset | GGX specular from the map's light entities near the camera (their diffuse stays baked). |
| `r_enhMetalFX` | 0 | MetalFX temporal anti-aliasing instead of ours (needs TAA, a MetalFX GPU and the patched SDL; see "Stage 5"). |
| `r_enhRTShadows` | 1 | Ray-traced sun and lamp shadows of the moving objects, soft (GPU family 9 and later; see "Stage 5"). |
| `r_enhRTContactAO` | 1 | Ray-traced contact occlusion of the moving objects (GPU family 9 and later). |
| `r_enhRTLightShadows` | 1 | Ray-traced shadows of the effect lights (muzzle flashes, explosions), with `r_enhLightShadows` (GPU family 9 and later). |
| `r_enhRTGI` | -1 | Ray-traced global illumination of the moving objects (GPU family 9 and later; -1 = as the preset: on in ultra only; see "Ray-traced global illumination"). |
| `r_enhRTBeyondView` | 1 | Objects outside the view but within 1500 units in the ray tracing (reflections, shadows); costs their CPU skinning. |
| `r_enhRTReflections` | 1 | Ray-traced reflections on water and the reflective surface classes (GPU family 9 and later: M3 and later; see "Stage 5"). |
| `r_enhRenderScale` | 1 | With MetalFX: the scene's resolution as a fraction of the window (0.5..1; the page offers 1, 0.77, 0.67, 0.5), upscaled by MetalFX. |
| `r_enhProbes` | preset | Reflection probes captured when the map loads: rooms reflect themselves instead of the sky (off in the low preset). |
| `r_enhDOF` | 0 | Depth of field while aiming down the sights (0 off .. 2). |
| `r_enhPlayerShadow` | -1 | The player's shadow-only body (sun shadows, ray tracing); never drawn in the view. On in every preset. |
| `r_enhMotionBlur` | 0 | Motion blur amount (0 off, 1 = one frame of motion; needs TAA). |
| `r_enhRebake` | 0 | Our own lighting re-bake instead of the shipped lightmaps (-1 = as the preset, which never turns it on; no menu row yet; needs the patched SDL and Metal ray tracing, read when a level loads; see docs/render-rebake.md). |
| `r_enhMotionDebug` | 0 | Debug views instead of the frame: 1 the motion vectors, 2 AO (red) and contact shadow (green). |

All are read once when the renderer starts, from the profile's config or `--cvar name=value` (case-insensitive). A per-feature
cvar that is set overrides the preset. Presets: low = moving-caster shadows 1, no world cascades, no auto exposure, AO, contact
shadows, TAA, screen-space reflections or light shadows; medium = shadows 2, two world cascades; high = shadows 3, three world cascades. The other features are on in
every preset. Gameplay, the server and the network are not affected; it is render-only.

## Menu

Options > OpenIW2 > Enhanced graphics (an OpenIW2 page added in memory to the game's own menus, src/ui/iw2options.cpp). The mode is
experimental and may cause visual glitches and bugs; a note at the top of the page says so.
- the master row (`r_enhanced`, Off / On, applies at the next start) is always shown; every other row only while it is On;
- Quality (`r_enhPreset`: Low / Medium / High / Ultra);
- one row per feature cvar; the on/off features offer Preset / On / Off, where Preset is the value -1 and leaves the feature to the
  quality choice (`game::enhancedSettings` ignores negative per-feature values); Sun shafts Preset / Off / On / Strong; Colour
  grade Neutral / Period film; Motion blur, Depth of field and HDR output Off / On;
- all values are saved in the profile (archived like the shipped options) and, while the mode is on, read every frame and
  applied at once (the shadow maps are made again at a new size), except the master row and HDR output, which change the
  swapchain and the scene format and so need the next start.
Labels: the game's own Low / Medium / High / On / Off strings, English OpenIW2 texts for the rest. Test: tests/test_iw2options.cpp.

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

## World sun shadows (cached cascades)

The world and the static models cast into two or three sun shadow cascades around the camera (`r_enhWorldShadows`). The world
does not move, so each cascade is rendered only when its centre, the camera position snapped to a quarter of the cascade's
radius (`game::cascadeCentre`), changes, or the map changes; otherwise the depth texture of the previous frames is reused. The
casters are the world soups and static models (LOD 0) whose box projects into the cascade, opaque and alpha-tested surfaces.
The depth range reaches the top of the world box (`game::casterReachForBounds`).

Combination with the original data:
- lightmapped world: `sun visibility = min(baked mask, cascade)`. The bake already holds every static shadow, including those of
  the invisible sun blockers (`caulk_shadow` brushes, which are not drawn and so not in the cascades); the minimum keeps them and
  adds what the coarse mask (8-16 units per texel) blurs away: sharp edges near the camera.
- static models: the cascade replaces the one baked sun bit per model (docs/spec/lightgrid.md "Implementation"), so a house or
  a crate is lit and shaded per pixel.
- entities: `min(lattice sun bit, cascade)`.
- moving casters multiply on top (above).
Beyond the last cascade the original mask alone remains (fade over its outer 15 %).

## Ambient occlusion and contact shadows

From `$floatz` (built every frame in this mode, static models included, unlike the default path where `r_depthPrepassModels 0`
keeps them out), at half size:
- ambient occlusion: ground-truth AO (Jimenez et al. 2016), two slices per pixel rotated by interleaved gradient noise, six
  horizon steps per side within 40 units, normals reconstructed from the depth; samples on or under the tangent plane do not
  occlude, so flat surfaces stay open (`game::gtaoSliceVisibility` holds the slice integral).
  Each half-size pixel reads `$floatz` at the centre of the first texel of its 2 × 2 block, never at the half-size pixel
  centre: that point lies on the edge between two texels, where point sampling picks either one by rounding. The
  reconstructed normal then tilted in regular bands, and flat ceilings and walls occluded themselves in stripes that moved
  with the camera. The ray-traced shadow pass (stage 5) samples the same way.
- contact shadows: twelve steps of a ray from the surface towards the sun over 18 units in `$floatz`; an occluder up to 10
  units thick shadows the sun term. Only within about 1200 units of the camera.
A 5 × 5 depth-aware blur follows. AO scales the baked lightmap, the light-grid probe and the static models' baked light, at
`r_enhAOStrength`; the contact shadow scales the sun term only. Blended surfaces, effect sprites and the view model take
neither (they are not in the opaque depth).

## Player shadow

Our own design (`r_enhPlayerShadow`, on in every preset; the "Player shadow" row turns it off). The first-person player has no body, so in the
original the player casts no shadow. With the switch on, iw2view builds a body that only the shadow passes see: the sun
cascades and the ray-traced scene take it, while the view, `$floatz`, the motion vectors, the probes and the lamp shadow
atlas never draw it.
- Models: the multiplayer character that uses the player's view hands. Each `character/mp_*.gsc` sets the view hands, the
  body, the head and the helmet together (for example `mp_british_africa_paul.gsc:4-8`), so the hands select the
  character (`game::playerBodyForHands`). The weapon's world model is held at `tag_weapon_right`.
- Pose: the multiplayer anim tree and `mp/playeranim.script`, stepped at render time. The inputs are the stance, the speed,
  the move direction at the view yaw (`game::playerBodyMoves`), aiming, and the weapon's anim type. Single-player weapon files
  say `none` for that type, so it comes from the multiplayer file of the same weapon (`<name>_mp`), else from the weapon
  class (`game::playerBodyAnimType`). There are no events (jump, fire, reload), no lean and no ladder poses.
- Placement: the eye's ground point (the camera minus the view height), turned to the view yaw.
- No body on a turret, in a vehicle, while linked, dead, spectating, in the killcam or prone (the prone animations lay the
  body forward of its origin, so it would cover the view's ground in shadow).
- The view weapon and hands are inside the body, so while it is on they take no shadow from the moving casters' sun map
  (the world cascades still shadow them).
- Ray tracing: the body is its own instance (mask 8). Sun and lamp shadow rays see it, except from pixels within 40 units of
  the eye (the view weapon and hands). Contact occlusion, the effect lights (a muzzle flash is inside the body) and
  reflections do not.
- Open: the character choice for each campaign and the missing event poses.

## Temporal anti-aliasing

- Jitter: every scene pass of a frame uses the same sub-pixel offset of the projection, Halton (2, 3) over 8 frames
  (`game::taaJitter`).
- Motion vectors: the `$floatz` pass writes a second target, the difference of the unjittered current and previous clip
  positions per pixel. A model's previous position uses its previous transform, and for skinned models its previous skinned
  vertices (the previous frame's skin arena is kept in a copy). Items are matched across frames by their model (skinned models
  have one per entity) and order. The view model, which moves with the camera, gets none. The sky takes the camera motion of a
  far point.
- Resolve (Karis 2014), before exposure, bloom and the tone curve: the history at the pixel's previous position, sampled with a
  5-tap Catmull-Rom filter, clipped to the variance box of the current 3 × 3 neighbourhood in YCoCg (values compressed by
  `c / (1 + max(c))`), the velocity taken at the nearest depth of that neighbourhood, blended 90 % history. The history
  restarts after a map change or a resize. A light unsharp mask in the tone pass offsets the softening.
- Limitations: blended effects have no motion vectors of their own (the clip keeps their ghosting short); MSAA stays on.

## Materials: GGX specular and reflections

- Sun specular: on surfaces whose technique set has a specular map (world lmap_s surfaces and the light-probe model techniques)
  the original's Phong lobe is replaced by GGX (Walter et al. 2007) with a height-correlated Smith visibility (Heitz 2014) and
  Schlick's Fresnel. f0 is the specular map colour. The roughness comes from the specular map alpha through the exponent the
  original's `$specularity` table assigns to it, P = ln 0.1 / ln cos((180 - 175 t) / 4 degrees), and the Blinn-Phong match
  alpha = sqrt(2 / (P + 2)) (`game::phongPowerOfSpecAlpha`, `game::ggxAlphaOfPower`). The result is scaled by 2 pi / (P + 2), the
  ratio of the Phong lobe's peak to its energy, so a highlight keeps about the original's brightness at its centre while its
  shape, tail and grazing Fresnel follow GGX.
- Reflections: the sky cube, with a box-filtered mip chain made at load for rough surfaces, times the roughness-aware Fresnel,
  times an occlusion from the surface's own light (the brightness of lightmap + sun, so dark interiors do not reflect the sky)
  and `r_enhEnvScale`.
- Reflective surface classes: world materials whose surface type (surfaceFlags bits 20-24) is water, glass, ice or metal get
  our own GGX parameters even without a specular map (water and glass f0 0.02, nearly smooth; metal f0 from the colour,
  rougher).
- Screen-space reflections, for those classes and the water technique only (the water technique's enhanced uniforms were not
  bound until stage 5, so before that the water kept its own cube): a 20-step ray in `$floatz` from the surface along
  the reflection; at a hit, the previous frame's resolved image (the TAA history) at the hit point's previous screen position
  replaces the sky, faded at the screen edges and with roughness. Needs TAA. On all specular surfaces this cost about 4.7 ms on
  the bench, hence the restriction.
- Reflection probes: see below; where they are on, the sky cube above is cube 0 of the probe array.

## Reflection probes

The maps carry none, so this is our own design (`game::pointsNearBoxes`, `game::placeProbes`, `game::probeGridInfo`,
`game::assignProbes`), made when the map's first frame is drawn:
- Placement: the light grid's points near the world's geometry, that is within 128 units of a world surface box, ignoring boxes
  over 4096 units such as the sky box (the grid also fills the empty air above a map); very dense grids are thinned to about
  40000 points. Each point gets its portal cell. One probe per cell first (the point nearest the mean of the cell's points, cells
  with more points first), then farthest-point sampling, at most 64 probes.
- Capture: six 64 x 64 faces per probe, 90 degree views along the face axes, drawn with the normal frame path from the probe's
  position into the frame's own command buffer, before the frame. No moving models, effects, view model or dynamic lights; TAA,
  auto exposure, bloom, sun shafts, AO, contact shadows, screen-space reflections, the dynamic sun shadows and the light shadows
  are off (the lightmaps carry the world's sun shadows). The linear scene image, before the tone pass, is copied into a 16-bit
  float array of cube faces: six layers per cube in the order and orientation of the sky cube's faces, cube 0 the sky, cube k
  probe k - 1. The shader picks the face itself, because SDL 3.4.20 hands Metal a cube array's layer count as its cube count.
  The orientation of each face was checked against the sky: from a probe above the map its faces equal the sky's
  (`IW2_PROBE_AT`, `IW2_PROBE_DUMP`). The mip chain (for rough surfaces) is made once before the captures, which reflect the
  sky cube, and again after them. Then the frame targets go back to the window size; TAA, the shadow cascades and the history
  start again.
- Selection: a grid of 128-unit cells over those points' box grown by 256 units (bigger cells if the grid would pass 2^20
  cells; outside it the sky), one byte per cell in a storage buffer: the nearest probe in the portal cell of the cell's centre,
  else the nearest in a cell next to it through a portal, else the nearest. The shader takes the cell of the surface point
  moved 8 units along the reflected ray. The choice is per cell, so a large surface can change probe at a cell edge.
- The reflection's occlusion from the surface's own light (above) and the screen-space rays still apply; the original water
  technique keeps its own cube.

## Map light specular

The map's light entities survive in the entity text (`light`: origin, radius, intensity, _color; spot lights a target and
fov_inner / fov_outer); their diffuse light is baked into the lightmaps and the light grid. The engine never reads these keys,
so the model is our own (`game::MapLight`, `game::mapLightFalloff`): colour times intensity, falloff (1 - d / r)^2, the spot cone
smooth between half the inner and outer angles around the direction to the target. The eight lights nearest the camera (those
reaching within 1500 units) add GGX specular on the specular surfaces and the reflective classes. They cast no shadow, so the
specular is scaled by the surface's own baked light (three times its brightness, clamped to 1): a dark wall behind a lamp does
not shine.

## Baked light shadows (moving objects)

The two strongest local map lights near the camera (power = colour x radius / distance; radius at most 1500 units, origin
within 1500 units of the camera and reach within 600 units of it, `game::bakedShadowLightNearCamera`, so large fill lights far
away cast none) that reach a moving model get a shadow map of the moving casters, in rows 4 and 5 of the light shadow atlas.
Where a world surface, static model or the water is in such a shadow, our estimate of that light's contribution (falloff x N.L x
colour, at most the baked light itself) is taken from the baked light. So soldiers cast shadows under street lamps at night. The
estimate can disagree with the bake; it never makes a surface darker than black or brighter than before.

## Depth of field while aiming

`r_enhDOF` (off by default): while the player aims down the sights, the world beyond 40 units (the weapon and its sights stay
sharp) is blurred with a 12-tap disc whose radius grows to 2.5 px at 440 units, times the aim fraction (`game::aimDofRadius`);
samples on the weapon are not taken. After TAA, before the tone curve.

## Shadowed effect lights

The dynamic lights (muzzle flashes, explosions; up to the first four of `r_dlightLimit`) get a point light shadow: six 256²
perspective faces per light in one depth atlas, with our own fixed face axes (`game::cubeFace`), casters being the light's lit
world pieces and the models it reaches. The l_point pass picks the face by the major axis of the light-to-point vector and
takes four compare taps, with a normal offset. The baked light and the light's falloff are unchanged.

## Atmosphere, grade and motion blur

- Sun shafts: per half-size pixel, 12 jittered steps along the view ray up to the opaque depth (at most 1500 units), each with
  one shadow tap of the world cascades and the moving-caster map, give the sunlit fraction of the path. It is multiplied by the
  Henyey-Greenstein phase (g 0.6, bright towards the sun) and the sun colour, and added to the scene before TAA, which smooths
  the noise. The strength is larger on maps whose level script turns fog on (their air is meant to be hazy). The original fog
  itself is unchanged.
- Grade "period film" (off by default): in the tone pass, saturation 0.8, an S-curve around mid grey (black, grey and white stay
  put, values above 1 keep their excess), warmer shadows and a fine animated grain (`game::periodFilmGrade` describes the
  curve). No look-up tables: the game ships none, and a neutral default needs none.
- Motion blur (off by default): eight taps along the TAA motion vector after the resolve, so the history stays sharp.
  The view weapon and hands move with the camera, so they are left out. The motion-vector image is RGBA16F, and its blue
  channel is 1 on view-model pixels (written by the $floatz velocity pass). A masked pixel keeps its own colour, and taps
  that land on a masked pixel are skipped, so the weapon is not smeared into the background during fast turns. Depth of
  field already keeps the weapon sharp: it reads $floatz, where the weapon is within 40 units, and never samples it.
- Depth of field and the baked light shadows: below.

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

## Motion vectors without TAA

With TAA off, the debug view (`r_enhMotionDebug 1`) shows the camera motion of the static scene reconstructed from `$floatz`
and the two frames' view-projections (`game::cameraMotion`). With TAA on it shows the motion vectors of the TAA pass.

## Stage 5: MetalFX and ray tracing

These need the native Metal objects behind SDL's handles, which SDL 3.4.20 does not give out; `tools/sdl/` has our patch that
adds them (`SDL_GPU_OPENIW2_METAL_HANDLES`). Without it the features are compiled out. `apps/iw2view/metalnative.mm` asks the
device at start what it can do and prints it (`metal: <GPU>, MetalFX yes / no, ray tracing yes / no`): MetalFX temporal
scaling where `MTLFXTemporalScalerDescriptor` supports the device, ray tracing only on Apple GPU family 9 and later (M3 and
later, hardware intersection; `game::metalRayTracingAllowed`). `IW2_NO_METALFX` and `IW2_NO_METAL_RT` force them off, to check
the fallback (the stage 1-4 paths) on any machine.

MetalFX anti-aliasing (`r_enhMetalFX 1`, the page's "MetalFX anti-aliasing" row, shown only where it is available): in place
of our TAA resolve, a small pass writes the motion of every pixel (the TAA's own: the motion vectors of the `$floatz` pass, the
camera's motion for the sky) and a device depth (clip z / w from `$floatz`, 1 for the sky); then MetalFX's temporal scaler is
encoded on SDL's command buffer between SDL passes, at the same input and output size, into the TAA history texture, which
replaces the scene image as before (so screen-space reflections still read the previous frame). It takes the jitter and motion
in input pixels with y down (`game::metalFxJitterPixels`, `game::metalFxMotionScale`); the signs were chosen by measurement:
with a static camera only the jitter's size matters, but after a 15 degree pan only these signs leave the image close to an
un-anti-aliased frame at the end angle (mean difference 3.0 against 3.2 for our TAA, 3.9 to 5.0 for the other signs). Its
history is reset where ours is invalid. Cost at 1920 x 1080 (high preset, duhoc beach): 8.25 ms against 6.63 ms with our TAA,
and from a cut it needs about a second to become as sharp as ours does in 12 frames, so it is off by default; it is the base
for rendering below the output size.

MetalFX while paused (`game::metalFxHold`): during a single-player pause (`cl_paused`, a menu over a frozen world) the
scaler is not run. The frame shows its last output again, and its history is reset on the first frame after the pause. The
frame does not change while paused, so nothing is lost. On macOS 27 the scaler runs a network on the Neural Engine, which can
fail an internal assertion and abort the process (seen once after about 12 000 frames, in the pause menu). The hold avoids that
work in the menu, and MetalFX stays off by default. It is not a complete fix: the failure is inside MetalFX, and the API has no
way to keep the scaler off the Neural Engine.

Render scale (`r_enhRenderScale`, the page's "MetalFX render resolution": Native, Quality 77 %, Balanced 67 %, Performance
50 %; only with MetalFX on, `game::effectiveRenderScale`, `game::renderSize`): the frame's scene part (depth, `$floatz` and
motion vectors, AO, the scene and post-sun images, sun shafts) runs at the render size; MetalFX writes the output size into the
TAA history, and from there on (motion blur, depth of field, the tone pass, bloom, glow, the HUD) the frame is at the window
size. The `$floatz` and post-sun images stay at the render size for the rest of the frame (read by UV); the composite gets a
depth buffer of the output size. Three things the scaling needs:
- The projection keeps the window's aspect (`frameAspect`): the render size's own ratio differs by rounding, and a matrix that
  differs from the previous frame's reads as motion (it smeared the image sideways).
- More jitter phases: 8 x (1 / scale)^2 (`game::taaJitterPhases`, 18 at 67 %, 32 at 50 %), about 8 samples per output pixel.
- A texture mip bias of log2(scale) on the material samples of the scene shaders (`IW2_MIP`, empty in the default library;
  SDL's sampler LOD bias does nothing on Metal), so textures keep the window size's detail.
The jitter signs were checked again against a supersampled reference (2560 x 1440 without TAA, downscaled): mean difference 3.6
at 67 % and 4.3 at 50 % with these signs, 6.3 and 7.7 with both flipped (which look sharper, but alias), 3.1 for MetalFX at
native size and for our TAA. Cost at 1920 x 1080, high preset, duhoc beach: native with our TAA 6.64 ms; MetalFX at 77 % 6.10,
67 % 5.35, 50 % 4.26 ms; medium preset 5.73 ms, with MetalFX at 67 % 4.86 ms. The reflection probes capture at native size.

### Ray-traced reflections

On GPUs with hardware ray tracing (`r_enhRTReflections`, the page's "RT reflections" row, shown only there):
- The scene: a two-level acceleration structure (`metal::SceneRT`): the static world and static models (the opaque surfaces of
  `Scene::batches`, built once per map) plus the frame's moving objects (models in the world: characters, their weapons,
  vehicles, script models; not the view model or effect sprites; opaque surfaces), one instance each with its model-to-world
  transform. Alpha-tested surfaces (foliage, fences) of the world are a second, non-opaque structure (its own instance): the
  queries that see the static world (reflections, the effect lights' shadows) walk their candidates with `intersection_query` and
  keep a hit only where the material's alpha passes its alpha function (`rt_alpha_pass`: the triangle's UVs from the world
  buffers, a 64 x 64 alpha mask per material in an R8 array, the geometry table; bound at fragment buffers 7-9 and texture 20). Rigid models keep their bottom level; skinned ones are built once for refitting and refit every frame, all in one
  encoder with one scratch buffer. It is bound at fragment buffer 6 of the scene passes, above SDL's buffers (the patch's render
  pass encoder getter); a placeholder that nothing hits is bound when it is off.
- The ray queries live in a second build of the enhanced library (`IW2_ENH_RT`), used only by the pipelines of the draws that
  reflect: reflective-class world surfaces and water. In every scene shader, even unused, the ray query code cost 11 ms per frame.
- The reflection (`enh_rt_reflection`), for surfaces up to GGX alpha 0.45 (`game::kRtReflectionMaxAlpha`), rays up to 6000 units:
  one mirror ray. A hit that is on screen and is the visible surface there (its clip w matches the opaque depth) takes the
  previous frame's image at its previous screen position (textured, lit, moving objects included); another hit takes the
  reflection probe of its own cell, a miss the environment as without ray tracing (sky or probe). The temporal resolve (TAA or
  MetalFX) is the only filtering; a mirror ray needs no other. Blended reflective surfaces (the sea at duhoc) get it too, since
  the ray needs no depth of the surface itself.
- Objects behind the camera: the item list is culled to the view (the original's entity cull, for the CPU skinning); with
  `r_enhRTBeyondView` the entities outside it but within 1500 units (`game::kRtBeyondViewRange`) are gathered too, into their own
  list (`Renderer::rtExtraItems`) that only the ray tracing reads (duhoc beach: 653 moving objects instead of 129; about 2.7 ms
  more CPU per frame for their skinning, 0.5 ms GPU).
- Limits: muzzle flashes and other effects appear only through the previous frame's image
  (one frame late, and not off screen). `IW2_RT_DEBUG=1` colours the reflections: red an on-screen hit, green another hit,
  blue a miss.
- Cost at 1920 x 1080, high preset: duhoc beach 6.81 ms without, 7.70 ms with (129 moving objects, 74000 triangles); rhine
  3.62 / 4.63 ms.

### Ray-traced shadows and contact occlusion of the moving objects

The baked light already carries the static world's shadows; what it lacks is the moving objects (characters, their weapons,
vehicles, script models). On GPUs with hardware ray tracing (`r_enhRTShadows`, `r_enhRTContactAO`, the page's rows shown only
there) a compute pass (`metal::SceneRT::shadowPass`, our kernel `rt_dynamic_shadow`) runs at the AO's half size over `$floatz`:
the position and a normal from the depth, offset off the surface, then rays against the moving objects only (instance mask):
- the sun: one ray a frame in a cone around the sun direction (`game::kRtSunConeTan`, a soft edge like the baked shadows');
- the two lamps the baked-light shadows choose (above): one ray each to a point jittered over the lamp's size
  (`game::kRtLampSize`); their visibility is weighted by each lamp's light at the pixel (falloff x N.L x the colour's
  luminance), so the shader's lamp loss stays the sum over the lamps when their colours are alike;
- contact occlusion: two cosine-weighted rays a frame of `game::kRtContactRange` units.
The results join the AO image (sun with the contact shadow, occlusion with the AO, the lamps in its alpha), whose depth-aware
blur and then TAA or MetalFX smooth the noise of the varying rays. With them the moving-caster sun shadow map and the lamps' rows
of the light shadow atlas are not drawn. Receivers are every pixel of `$floatz` (the world, static models and the models in it);
blended surfaces and the view model keep no screen-space terms, as before. Without TAA or MetalFX the soft edges show their noise.

The effect lights (muzzle flashes, explosions; the first four, `r_enhRTLightShadows` with `r_enhLightShadows`) get their
shadows from the same pass: one ray per light to a point jittered over `game::kRtEffectLightSize`, against the whole scene
(these lights are not baked; alpha-tested surfaces tested), into a second half-size image, one channel per light, which the
light pass samples at its pixel instead of the light shadow atlas (whose rows for these lights are then not drawn). Elsewhere
(M1, M2, or with the switch off) the atlas shadows stay. The light pass now takes its shadow parameters in its own uniform
block: in fragment slot 1 they had overwritten the frame's enhanced uniforms for the scene draws between the light draws.

The player's shadow body (its own instance mask) must not shadow the view model or what is at the eye. Its rays count with a
weight that rises smoothly with the distance of the pixel from the eye, 0 up to `game::kPlayerBodyShadowFadeNear` and 1 from
`game::kPlayerBodyShadowFadeFar` (`game::playerBodyShadowWeight`): a ray that meets only the body darkens by that weight. An
earlier cut at one view depth left a straight edge across the player's shadow on flat ground. The other moving objects always
count in full.

Ray-traced occlusion of the static world was measured and left out: against the static world as well, the contact occlusion
changes about half the pixels by at most 22 of 255, nearly all by 1 to 3, darkening corners the screen-space AO already darkens;
the lightmaps carry the indirect light.

### Ray-traced global illumination (ultra preset)

Our own design; nothing here is taken from the shipped game. The lightmaps and the light grid already hold the static world's
bounce light, so a ray-traced one-bounce term must not add that again. What the baked light cannot hold is what moves: the
objects block the sky and the bounce around them, and they send their own lit colour onto their surroundings. Two terms, both
only for the moving objects (instance mask `kMaskDynamic`), on GPUs with hardware ray tracing and with TAA on, in the ultra
preset (`r_enhPreset 3` = high plus this) or with `r_enhRTGI 1`; everywhere else ultra is the high preset, unchanged:
- Bounce (`metal::SceneRT::giPass`, our kernel `rt_gi`): at the AO's half size, from the surface point and a normal from
  `$floatz` (the same texel-centre rule as the shadow pass), one cosine-weighted ray a frame (`game::cosineHemisphereDir`)
  up to `game::kRtGiRange` (300 units) against the moving objects. A hit that is the visible surface on screen
  (the hit's clip depth matches `$floatz` there, as the reflections check it) sends back the previous frame's resolved image at
  its previous screen position (textured, lit, shadowed, with the effect lights; one frame late), capped at
  `kRtGiRadianceCap`, faded over the outer half of the range (`game::giHitFade`). A cosine-weighted ray needs no further
  weight: its radiance is the irradiance over pi. A miss, or a hit off screen, adds nothing.
- Filter and accumulation (`fs_enh_gi_resolve`): a depth-aware 5 x 5 blur of the noisy rays, then the exponential accumulation of
  the previous result reprojected by the camera's motion (`game::kRtGiBlend` = 0.12 of the new frame, about 24 frames,
  `game::giEffectiveFrames`), rejected where its depth no longer matches (`game::giHistoryAccepted`: a moving surface, a
  disocclusion) so no ghost follows an object. TAA or MetalFX smooths what is left. The image is RGBA16F (light in rgb, the view
  depth in a), ping-pong.
- The scene shaders add it to the baked light after the AO (`ENH_GI`; the surface colour multiplies it like any light), so
  the soldier sitting on a sunlit crate takes the crate's colour on his shaded side, a sunlit gun lightens the sand in its
  shadow. Not on the view model and blended surfaces (no screen-space terms there).
- Indirect occlusion: the contact occlusion's two rays a frame (rt shadow pass) reach `game::kRtGiOcclusionRange` = 200 units
  instead of 40, the occlusion fading out between 40 and 200 units, so the ambient light around a vehicle or a crowd darkens
  softly beyond the contact range. It multiplies the baked light through the same AO image.
- The radiance of a hit is the previous frame's image, which includes this term: the bounce of the bounce comes free, and
  it does not run away (the mean of a ground area next to a gun: 90.3 / 90.4 / 90.5 / 90.5 of 255 after 8 / 16 / 60 / 200
  frames, 90.0 without it).
- Cost: see "Cost". Memory: two half-size RGBA16F images and one raw one.

Measured and left out (the cuts are on `toujane_soldier`, `elalamein` flak 88, `duhoc` beach at 1280 x 720, against the high preset,
fraction of pixels that differ and their size in 8-bit levels):
- Rays against the static world too (bounce from sunlit ground and walls onto everything): 74 % of the pixels differ, a quarter
  of them by 8 levels or more, the ground and walls come out about twice as bright. It counts the lightmaps' bounce twice; subtracting a
  baked estimate is not possible, because the baked part of a pixel is not kept apart from the rest. Rays against the static world alone: 71 % of the pixels,
  the same. Left out. The moving models keep the light grid's ambient, which already holds the ground's bounce.
- Bounce of the effect lights (muzzle flash, explosions) onto the surroundings: the lit surfaces that matter are moving ones,
  which the bounce term above already carries (the previous frame's image holds the flash's light on them). A lit static surface
  would need its baked part separated from the flash's, as above, and a shadow ray per hit; a flash lasts about two frames
  of the accumulation. Left out.
- Only the bounce, or only the occlusion: the bounce alone changes 33 % of the pixels on `toujane_soldier`, by up to 51 levels (the soldier, the crate, the
  wall beside him); the occlusion alone 57 %, but by at most 11 levels and nearly all by 1 or 2. Both stay, the bounce is the
  visible part and the occlusion the soft grounding.
- More than one ray a frame: the filter and the accumulation (about 600 samples per pixel after 24 frames) already leave less
  noise than the TAA's own. Left out.

The effect is subtle by construction: the baked light is the largest part of any pixel. Next to a soldier, a vehicle or a gun it
is a few levels to about 50 of 255 on the nearby surfaces and a gentle darkening further out; across the rest of the frame it is
under 2 levels. With TAA off there is no history to take radiance from, so the term is off.

## Water shadows

The water technique is drawn after the AO image is made and has no baked sun term, so the moving objects' sun shadow did not
reach it. It now takes it per pixel, with the same strength as the opaque surfaces (`game::waterSunShadowSource` decides where
from; our own addition, the original water has no shadows):
- ray-traced shadows on (`r_enhRTShadows`, a GPU with ray tracing): the water fragment traces one ray a frame towards a point
  in the sun's cone (`game::kRtSunConeTan`) against the moving objects, the player's shadow body included with the weight above;
  the water uses the ray query pipeline variant for this even when ray-traced reflections are off;
- otherwise: the same sun shadow map cascades the opaque surfaces use (`r_enhSunShadows`), sampled with the water plane's
  up normal; the player's shadow body is one of its casters.
Where the sun is hidden the water's colour is darkened by `game::kWaterShadowDarken` (40 %) and the sun's glint is removed. Static
shadows from the world are not applied to the water. With the sun shadows off (or no sun) the water is as before. Blended
(translucent) world surfaces keep their previous handling: they have no screen-space terms.

The sun shadow map's two cascades now hand over to each other by a blend over the outer tenth of the first one, instead of a
switch.

## Cost

GPU-bound frame time with `IW2_BENCH_GPU` (frames submitted back to back), 1920 × 1080, 4x MSAA, Apple M4 Pro (20-core GPU),
the duhoc_assault beach reference spot (docs/debug-flags.md `IW2_BENCH`), yaw 180 (483 shadow casters):

| Configuration | GPU-bound ms | vs enhanced off |
|---|---|---|
| default | 2.06 | |
| enhanced, every feature off (16F scene) | 2.28 | |
| + moving-caster sun shadows low / medium / high | 2.64 / 2.99 / 3.26 | +0.36 / +0.71 / +0.98 |
| + world cascades, two / three | 2.94 / 3.16 | +0.66 / +0.88 |
| + light grid per pixel | 2.40 | +0.12 |
| + tone curve and bloom | 2.45 | +0.17 |
| + auto exposure | 2.46 | +0.18 |
| + AO / contact shadows / both | 2.79 / 2.44 / 2.70 | +0.51 / +0.16 / +0.42 |
| + TAA | 2.92 | +0.62 |
| + GGX and sky reflections | 2.68 | +0.38 |
| + GGX, TAA and screen-space reflections | 3.31 | +1.01 |
| + sun shafts (with the world cascades) | 3.14 | +0.28 over the cascades |
| + grade and motion blur (with TAA) | 3.05 | +0.17 over TAA |
| + map light specular (with GGX) | 2.81 | +0.12 over GGX |
| preset medium / high | 5.66 / 6.59 | |
| preset high without reflection probes | 6.87 | (the probes cost nothing measurable) |

Ray-traced global illumination (ultra against high, duhoc beach, interleaved runs on a machine shared with other work, so the absolute
values are higher than above; the high preset of this build against the build before it: +0.08 / +0.09 ms): yaw 180 +0.98 ms (min of 7 runs; the
median +1.1), yaw 0 +0.74 ms. Between two consecutive frames of a still view the PSNR is the same as in the high preset (52.3 and 49.8 dB), so the
term adds no flicker.

A shadowed effect light (toujane, one light of radius 350): +0.18 ms. Depth of field while aiming (toujane, high preset): +0.29 ms. The reflection probes take about
0.2 s when a map loads (64 probes) and 17 MB.

The CPU cost is the shadow pass encode (about +0.2 ms for 483 casters), the lattice lookups (under 0.1 ms with the cache) and,
in a frame where a world cascade moves, its caster list and draw (about 1 ms CPU and 0.5 ms GPU, `IW2_ENH_CASCADE_REDRAW`).

## Tests

`tests/test_renderenhanced.cpp`: settings and presets, the tone curve (identity below the knee, C1, monotonic, bounded), the
sRGB transfer, exposure target and frame-rate independent adaptation, bloom levels and threshold, the sun shadow fit (sphere
inside the map, depth order towards the sun, texel-stable snapping, a vertical sun), the cascade fade, the world cascade centre
snapping and caster reach, the GTAO slice integral, the TAA jitter sequence, the specular exponent to roughness conversion, the
point light shadow faces, the phase function and the film grade, the probe lattice, the camera motion reconstruction, the
baked-light shadow light choice and the reflection probes (points near geometry, placement, index grid and assignment). `tests/test_lightgrid.cpp`: the
grid points' positions.

## References

- L. Williams, "Casting curved shadows on curved surfaces", SIGGRAPH 1978 (shadow maps).
- W. Reeves, D. Salesin, R. Cook, "Rendering antialiased shadows with depth maps", SIGGRAPH 1987 (percentage-closer filtering).
- M. Valient, "Stable rendering of cascaded shadow maps", ShaderX6, 2008 (texel snapping).
- D. Holbert, "Normal offset shadows", GDC 2011.
- J. Jimenez, "Next generation post processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014 Advances course
  (13-tap downsample and tent upsample bloom, interleaved gradient noise).
- J. Jimenez, X. Wu, A. Pesce, A. Jarabo, "Practical real-time strategies for accurate indirect occlusion", SIGGRAPH 2016
  course (GTAO).
- B. Karis, "High quality temporal supersampling", SIGGRAPH 2014 Advances course (TAA); M. Salvi, "An excursion in temporal
  supersampling", GDC 2016 (variance clipping).
- B. Walter, S. Marschner, H. Li, K. Torrance, "Microfacet models for refraction through rough surfaces", EGSR 2007 (GGX).
- E. Heitz, "Understanding the masking-shadowing function in microfacet-based BRDFs", JCGT 2014.
- M. McGuire, M. Mara, "Efficient GPU screen-space ray tracing", JCGT 2014.
- L. G. Henyey, J. L. Greenstein, "Diffuse radiation in the galaxy", Astrophysical Journal 93, 1941 (phase function).
- B. Wronski, "Volumetric fog", SIGGRAPH 2014 Advances course; S. Hillaire, "Physically based and unified volumetric rendering
  in Frostbite", SIGGRAPH 2015 (shadowed in-scattering).
- E. Reinhard et al., "Photographic tone reproduction for digital images", SIGGRAPH 2002 (log-average luminance and key).
- IEC 61966-2-1 (sRGB transfer function).
