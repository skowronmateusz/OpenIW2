// Enhanced rendering mode shaders (docs/render-enhanced.md). An openiw2 addition with no counterpart in the original: our own design
// from public techniques (orthographic sun shadow maps with percentage-closer filtering, a progressive downsample / upsample bloom,
// log-average auto exposure, a highlight shoulder tone curve). Nothing here comes from the shipped game shaders.
//
// The enhanced shader library is kEnhancedPrefix + the default library (shaders.metal.inc, compiled with IW2_ENHANCED defined, which
// turns on its hooks) + kEnhancedSource. The default library without the define is unchanged.

// ---- prefix: definitions the hooks in the default library use --------------------------------------------------------------------
R"MSL(
#include <metal_stdlib>
using namespace metal;
#define IW2_ENHANCED 1

// Fragment uniform slot 1: per frame.
struct EnhFragU {
    float4x4 shadowVP[2];  // world -> shadow clip (x, y in -1..1, z in 0..1) per cascade
    float4 shadowParams;   // x: cascades (0 = off), y: 1 / map size (one cascade), z: normal offset in world units per texel, w: filter taps
    float4 shadowSphere[2]; // xyz centre, w radius per cascade
    float4 eye;            // xyz camera position
    float4 shadowBias;     // x: depth bias (0..1 depth units), y: shadow strength (1 = full)
    float4x4 worldVP[3];   // world (static) sun cascades, world -> shadow clip
    float4 worldSphere[3]; // xyz centre, w radius
    float4 worldParams;    // x: cascades (0 = off), y: 1 / map size, z: normal offset in texels, w: filter taps
    float4 worldBias;      // xyz: depth bias per cascade (0..1 depth units)
    float4 aoParams;       // xy: 1 / frame size, z: AO strength (0 = off), w: 1 = contact shadows
    float4x4 viewProj;     // this frame's (jittered) view-projection: screen-space reflections
    float4x4 prevViewProj; // the previous frame's: where a reflected point was in the history image
    float4 ssrParams;      // x, y: clip z = x * w + y; z: 1 = reflections on (history valid); w: $floatz sky threshold
    float4 specParams;     // x: 1 = GGX specular, y: specular scale, z: environment reflection scale, w: environment mip count
    float4 mlPos[8];       // map lights near the camera: xyz position, w radius
    float4 mlColor[8];     // rgb colour times intensity, w: cos of the inner spot angle
    float4 mlDir[8];       // xyz spot axis, w: cos of the outer spot angle (< -1: point light)
    float4 mlParams;       // x: count
    float4 bsPos[2];       // baked-light shadows of moving objects (b4): xyz light position, w radius
    float4 bsColor[2];     // rgb the light's colour times intensity, w: its tile row in the light shadow atlas
    float4 bsParams;       // x: count, y: near plane
    float4 probeGrid[2];   // reflection probes: xyz grid origin, w 1 / cell size; xyz cells per axis, w > 0.5: probes present
    float4 mipBias;        // x: log2 of the render scale (stage 5), the material textures' mip bias
};
#define IW2_MIP , bias(eu.mipBias.x)
// Fragment uniform slot 2: per draw (model items).
struct EnhDrawU {
    float4 lattice; // x: first point, y, z, w: points per axis (w > 0.5: lattice present)
    float4 origin;  // xyz: world position of point (0, 0, 0); w > 0.5: no screen-space AO / contact shadows (view model, blended)
    float4 step;    // xyz: world distance between points; w > 0.5: no dynamic sun shadow (effect sprites)
};
#define ENH_FS_ARGS(t) constant EnhFragU& eu [[buffer(1)]], constant EnhDrawU& du [[buffer(2)]], const device float4* lattice [[buffer(3)]], \
    depth2d<float> shadowMap [[texture(t)]], sampler shadowSampler [[sampler(t)]], \
    depth2d_array<float> worldShadow [[texture(t + 1)]], sampler worldSampler [[sampler(t + 1)]], \
    texture2d<float> aoTex [[texture(t + 2)]], sampler aoSampler [[sampler(t + 2)]], \
    texture2d_array<float> envCube [[texture(t + 3)]], sampler envSampler [[sampler(t + 3)]], \
    texture2d<float> histTex [[texture(t + 4)]], sampler histSampler [[sampler(t + 4)]], \
    texture2d<float> depthTex [[texture(t + 5)]], sampler depthSampler [[sampler(t + 5)]], \
    depth2d<float> lightAtlas [[texture(t + 6)]], sampler lightAtlasSampler [[sampler(t + 6)]], \
    const device uchar* probeIdx [[buffer(5)]]
#define ENH_BAKED_SHADOW eu, lightAtlas, lightAtlasSampler
#define ENH_WORLD_SHADOWS eu, worldShadow, worldSampler
#define ENH_REFLECT eu, envCube, envSampler, probeIdx, histTex, histSampler, depthTex, depthSampler

constant float2 kEnhPoisson[12] = {
    float2(-0.326, -0.406), float2(-0.840, -0.074), float2(-0.696, 0.457), float2(-0.203, 0.621),
    float2(0.962, -0.195), float2(0.473, -0.480), float2(0.519, 0.767), float2(0.185, -0.893),
    float2(0.507, 0.064), float2(0.896, 0.412), float2(-0.322, -0.933), float2(-0.792, -0.598)};

// Dynamic sun visibility of a world point from the shadow map of the moving casters (1 = lit). Normal offset against acne, a small
// rotated Poisson disk of hardware-filtered compare taps, the last cascade fades out towards its edge.
static float enh_sun_shadow(float3 wp, float3 n, constant EnhFragU& eu, depth2d<float> sm, sampler ss, int tapsOverride = 0) {
    const int cascades = int(eu.shadowParams.x + 0.5);
    if (cascades <= 0) return 1.0;
    int c = 0;
    float dist = distance(wp, eu.shadowSphere[0].xyz);
    if (cascades > 1 && dist > eu.shadowSphere[0].w * 0.9) {
        c = 1;
        dist = distance(wp, eu.shadowSphere[1].xyz);
    }
    const float radius = eu.shadowSphere[c].w;
    if (dist >= radius) return 1.0;
    const float texelWorld = 2.0 * radius * eu.shadowParams.y;
    const float3 p = wp + n * (eu.shadowParams.z * texelWorld);
    const float4 sc = eu.shadowVP[c] * float4(p, 1.0);
    float2 uv = sc.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0) || sc.z >= 1.0) return 1.0;
    uv.x = (uv.x + float(c)) / float(cascades);
    const float ref = sc.z - eu.shadowBias.x;
    const int taps = tapsOverride > 0 ? tapsOverride : int(eu.shadowParams.w + 0.5);
    float lit = 0.0;
    if (taps <= 1) {
        lit = sm.sample_compare(ss, uv, ref);
    } else {
        const float ang = fract(52.9829189 * fract(dot(wp.xy, float2(0.06711056, 0.00583715)))) * 6.2831853;
        const float2x2 rot = float2x2(float2(cos(ang), sin(ang)), float2(-sin(ang), cos(ang)));
        const float2 texel = float2(eu.shadowParams.y / float(cascades), eu.shadowParams.y) * 1.5;
        for (int i = 0; i < taps; ++i) lit += sm.sample_compare(ss, uv + (rot * kEnhPoisson[i]) * texel, ref);
        lit /= float(taps);
    }
    const float start = radius * 0.85;
    const float fade = (c == cascades - 1 && dist > start) ? saturate(1.0 - (dist - start) / (radius - start)) : 1.0;
    return mix(1.0, lit, fade * eu.shadowBias.y);
}

// Sun visibility from the cached world cascades (the world and static models): x = visibility, y = weight (0 outside the cascades,
// fading over the outer 15 % of the last one). The callers combine it with the baked mask: min(mask, mix(1, x, y)).
static float2 enh_world_sun(float3 wp, float3 n, constant EnhFragU& eu, depth2d_array<float> wsm, sampler ss, int tapsOverride = 0) {
    const int cascades = int(eu.worldParams.x + 0.5);
    if (cascades <= 0) return float2(1.0, 0.0);
    int c = -1;
    float dist = 0.0;
    for (int i = 0; i < cascades; ++i) {
        dist = distance(wp.xy, eu.worldSphere[i].xy);
        if (dist < eu.worldSphere[i].w * (i == cascades - 1 ? 1.0 : 0.9)) { c = i; break; }
    }
    if (c < 0) return float2(1.0, 0.0);
    const float radius = eu.worldSphere[c].w;
    const float texelWorld = 2.0 * radius * eu.worldParams.y;
    const float4 sc = eu.worldVP[c] * float4(wp + n * (eu.worldParams.z * texelWorld), 1.0);
    const float2 uv = sc.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0) || sc.z >= 1.0 || sc.z <= 0.0) return float2(1.0, 0.0);
    const float ref = sc.z - eu.worldBias[c];
    const int taps = tapsOverride > 0 ? tapsOverride : int(eu.worldParams.w + 0.5);
    const float ang = fract(52.9829189 * fract(dot(wp.xy, float2(0.06711056, 0.00583715)))) * 6.2831853;
    const float2x2 rot = float2x2(float2(cos(ang), sin(ang)), float2(-sin(ang), cos(ang)));
    const float2 texel = float2(eu.worldParams.y * 1.5);
    float lit = 0.0;
    for (int i = 0; i < taps; ++i) {
        const float2 q = uv + (rot * kEnhPoisson[i]) * texel;
        lit += wsm.sample_compare(ss, q, uint(c), ref);
    }
    lit /= float(max(taps, 1));
    const float start = radius * 0.85;
    const float weight = (c == cascades - 1 && dist > start) ? saturate(1.0 - (dist - start) / (radius - start)) : 1.0;
    return float2(lit, weight);
}

// Screen-space ambient occlusion and contact shadow at a fragment: x = factor on the baked / probe light, y = factor on the sun.
static float2 enh_screen_ao(float4 fragPos, constant EnhFragU& eu, constant EnhDrawU& du, texture2d<float> aoTex, sampler as) {
    if (du.origin.w > 0.5 || (eu.aoParams.z <= 0.0 && eu.aoParams.w < 0.5)) return float2(1.0);
    const float2 a = aoTex.sample(as, fragPos.xy * eu.aoParams.xy).rg;
    return float2(mix(1.0, a.x, eu.aoParams.z), eu.aoParams.w > 0.5 ? a.y : 1.0);
}
#define ENH_AO(pos) enh_screen_ao(pos, eu, du, aoTex, aoSampler)

// ---- specular: GGX from the spec / gloss maps, environment and screen-space reflections (docs/render-enhanced.md "Materials") ----

// Roughness (GGX alpha) of a specular map alpha: the Phong exponent of the $specularity column it selects (docs/spec/material.md
// via main.cpp specularityTable: P = ln 0.1 / ln cos((180 - 175 t) / 4 degrees)), then the Blinn-Phong to GGX match
// alpha = sqrt(2 / (P + 2)).
static float enh_phong_power(float specAlpha) {
    const float t = saturate((specAlpha * 32.0 - 0.5) / 30.0);
    return log(0.1) / log(cos((180.0 - 175.0 * t) * (3.14159265 / 720.0)));
}
static float enh_ggx_alpha(float power) { return clamp(sqrt(2.0 / (power + 2.0)), 0.03, 1.0); }

// Sun specular: GGX distribution, height-correlated Smith visibility, Schlick Fresnel from f0 = the specular map colour. Scaled by
// 2 pi / (P + 2), the ratio of the Phong lobe's peak to its energy, so a highlight keeps about the original's brightness at its
// centre while its shape, tail and grazing Fresnel follow GGX.
static float3 enh_ggx_sun(float3 n, float3 v, float3 l, float3 f0, float power) {
    const float a = enh_ggx_alpha(power), a2 = a * a;
    const float3 h = normalize(v + l);
    const float nl = saturate(dot(n, l)), nv = max(dot(n, v), 1e-3), nh = saturate(dot(n, h)), vh = saturate(dot(v, h));
    if (nl <= 0.0) return 0.0;
    const float d = a2 / (3.14159265 * pow(nh * nh * (a2 - 1.0) + 1.0, 2.0));
    const float vis = 0.5 / (nl * sqrt(nv * nv * (1.0 - a2) + a2) + nv * sqrt(nl * nl * (1.0 - a2) + a2));
    const float3 f = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
    return d * vis * f * nl * (6.2831853 / (power + 2.0));
}

// Specular of the map's baked lights near the camera (game::mapLightFalloff; diffuse stays in the lightmap). No shadows: the caller
// scales it by an occlusion from the surface's own baked light.
static float3 enh_map_light_spec(float3 wp, float3 n, float3 v, float3 f0, float power, constant EnhFragU& eu) {
    float3 sum = 0.0;
    const int count = int(eu.mlParams.x + 0.5);
    for (int i = 0; i < count; ++i) {
        const float3 d = eu.mlPos[i].xyz - wp;
        const float dist = length(d);
        if (dist >= eu.mlPos[i].w || dist < 1e-3) continue;
        float f = (1.0 - dist / eu.mlPos[i].w);
        f *= f;
        const float3 l = d / dist;
        if (eu.mlDir[i].w >= -1.0) {
            const float c = dot(-l, eu.mlDir[i].xyz);
            const float t = saturate((c - eu.mlDir[i].w) / max(eu.mlColor[i].w - eu.mlDir[i].w, 1e-4));
            f *= t * t * (3.0 - 2.0 * t);
        }
        if (f <= 0.0) continue;
        sum += enh_ggx_sun(n, v, l, f0, power) * eu.mlColor[i].rgb * f;
    }
    return sum;
}

// Reflection: the sky cube (blurred with roughness through its mips) or, where a screen-space ray hits the opaque depth, the
// previous frame's image at that point. Returns the reflected light times the roughness-aware Fresnel.
// The environment: a plain cube (the original water's), or the enhanced face array: cube 0 the sky, cube k the reflection probe
// k - 1 that the probe index grid gives the cell of the point moved 8 units along the reflected ray (off the surface).
static float3 enh_env(texturecube<float> env, sampler es, const device uchar* pidx, constant EnhFragU& eu, float3 wp, float3 r, float lod) {
    return env.sample(es, r, level(lod)).rgb;
}
// The face array: six layers per cube in the order and orientation of a Metal cube (+x, -x, +y, -y, +z, -z), so a sky cube's
// faces copy over unchanged. (SDL 3.4.20 gives Metal a cube array's layer count as its cube count, hence no real cube array.)
static float3 enh_env(texture2d_array<float> env, sampler es, const device uchar* pidx, constant EnhFragU& eu, float3 wp, float3 r, float lod) {
    uint cube = 0;
    if (eu.probeGrid[1].w > 0.5) {
        const float3 c = floor((wp + r * 8.0 - eu.probeGrid[0].xyz) * eu.probeGrid[0].w);
        const float3 d = eu.probeGrid[1].xyz;
        if (all(c >= 0.0) && all(c < d)) cube = uint(pidx[uint(c.x + d.x * (c.y + d.y * c.z))]);
    }
    const float3 a = abs(r);
    uint face;
    float2 st;
    float ma;
    if (a.x >= a.y && a.x >= a.z) ma = a.x, face = r.x >= 0.0 ? 0 : 1, st = r.x >= 0.0 ? float2(-r.z, -r.y) : float2(r.z, -r.y);
    else if (a.y >= a.z) ma = a.y, face = r.y >= 0.0 ? 2 : 3, st = r.y >= 0.0 ? float2(r.x, r.z) : float2(r.x, -r.z);
    else ma = a.z, face = r.z >= 0.0 ? 4 : 5, st = r.z >= 0.0 ? float2(r.x, -r.y) : float2(-r.x, -r.y);
    const float2 uv = st / max(ma, 1e-6) * 0.5 + 0.5;
    return env.sample(es, uv, cube * 6 + face, level(lod)).rgb;
}
template <typename E>
static float3 enh_reflection_color(float3 wp, float3 r, float a, float4 fragPos, constant EnhFragU& eu, E env, sampler es,
                                   const device uchar* pidx, texture2d<float> hist, sampler hs, texture2d<float> dep, sampler ds,
                                   bool allowSsr = true) {
    float3 refl = enh_env(env, es, pidx, eu, wp, r, a * eu.specParams.w);
    if (allowSsr && eu.ssrParams.z > 0.5 && a < 0.45) { // screen-space ray in $floatz, 20 steps up to ~1000 units
        const float noise = fract(52.9829189 * fract(dot(fragPos.xy, float2(0.06711056, 0.00583715))));
        float t = 4.0 + noise * 8.0;
        for (int i = 0; i < 20; ++i) {
            const float3 q = wp + r * t;
            const float4 c = eu.viewProj * float4(q, 1.0);
            if (c.w <= 1.0) break;
            const float2 uv = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
            if (any(uv < 0.0) || any(uv > 1.0)) break;
            const float cz = dep.sample(ds, uv).r;
            if (cz < eu.ssrParams.w) {
                const float ws = (cz - eu.ssrParams.y) / eu.ssrParams.x;
                const float diff = c.w - ws;
                if (diff > 0.0 && diff < max(t * 0.08, 6.0)) {
                    const float4 pc = eu.prevViewProj * float4(q, 1.0);
                    const float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
                    if (all(puv >= 0.0) && all(puv <= 1.0)) {
                        const float2 edge = saturate(min(puv, 1.0 - puv) * 10.0);
                        const float conf = edge.x * edge.y * saturate(1.0 - (a - 0.25) * 5.0) * saturate(1.0 - float(i) / 20.0 * 0.5);
                        refl = mix(refl, max(hist.sample(hs, puv).rgb, 0.0), conf);
                    }
                    break;
                }
            }
            t *= 1.25;
        }
    }
    return refl;
}
// Screen-space rays only for the reflective surface classes and water (allowSsr); other specular surfaces reflect the sky alone.
static float3 enh_reflection(float3 wp, float3 n, float3 v, float3 f0, float power, float4 fragPos, constant EnhFragU& eu, texture2d_array<float> env,
                             sampler es, const device uchar* pidx, texture2d<float> hist, sampler hs, texture2d<float> dep, sampler ds,
                             bool allowSsr = false) {
    const float a = enh_ggx_alpha(power);
    const float nv = saturate(dot(n, v));
    const float3 refl = enh_reflection_color(wp, reflect(-v, n), a, fragPos, eu, env, es, pidx, hist, hs, dep, ds, allowSsr);
    const float3 f = f0 + (max(float3(1.0 - a), f0) - f0) * pow(1.0 - nv, 5.0);
    return refl * f;
}

// Shadow of a dynamic point light (docs/render-enhanced.md "Shadowed effect lights"): the face of the light-to-point vector v in the
// light's 3 x 2 tile block of the atlas (game::cubeFace axes), 4 compare taps. ls: x on, y near, z far, w the light's tile row.
static float enh_light_shadow(float3 v, float3 n, float4 ls, depth2d<float> atlas, sampler ss) {
    v += n * 1.0 - normalize(v) * 0.5; // normal offset and a small pull towards the light
    const float3 a = abs(v);
    int face;
    float3 fwd, up;
    if (a.x >= a.y && a.x >= a.z) face = v.x >= 0.0 ? 0 : 1, fwd = float3(sign(v.x), 0, 0), up = float3(0, 0, 1);
    else if (a.y >= a.z) face = v.y >= 0.0 ? 2 : 3, fwd = float3(0, sign(v.y), 0), up = float3(0, 0, 1);
    else face = v.z >= 0.0 ? 4 : 5, fwd = float3(0, 0, sign(v.z)), up = float3(1, 0, 0);
    const float3 right = cross(fwd, up);
    const float d = dot(v, fwd);
    float2 uv = float2(dot(v, right), dot(v, up)) / d * float2(0.5, -0.5) + 0.5;
    const float2 tex = 1.0 / float2(atlas.get_width(), atlas.get_height());
    const float2 tileSize = float2(1.0 / 3.0, 1.0 / (2.0 * 6.0)); // 4 dynamic lights + 2 baked lights
    uv = clamp(uv, 1.5 * tex / tileSize, 1.0 - 1.5 * tex / tileSize);
    const float2 base = float2(float(face % 3), float(face / 3) + 2.0 * ls.w) * tileSize;
    const float ref = ls.z / (ls.z - ls.y) - ls.z * ls.y / ((ls.z - ls.y) * d) - 0.0005;
    float lit = 0.0;
    for (int k = 0; k < 4; ++k) {
        const float2 o = float2((k & 1) ? 0.5 : -0.5, (k & 2) ? 0.5 : -0.5) * tex;
        lit += atlas.sample_compare(ss, base + uv * tileSize + o, ref);
    }
    return lit * 0.25;
}

// Shadows of moving objects in the map's baked lights (docs/render-enhanced.md "Baked light shadows"): for the strongest lights near
// the camera the light shadow atlas holds the moving casters; where a point is in such a shadow, our estimate of that light's
// contribution (game::mapLightFalloff times N.L times its colour), at most the baked light itself, is taken away.
static float enh_light_shadow(float3 v, float3 n, float4 ls, depth2d<float> atlas, sampler ss);
static float3 enh_baked_shadow_loss(float3 wp, float3 n, float3 baked, constant EnhFragU& eu, depth2d<float> atlas, sampler ss) {
    float3 loss = 0.0;
    const int count = int(eu.bsParams.x + 0.5);
    for (int i = 0; i < count; ++i) {
        const float3 d = eu.bsPos[i].xyz - wp;
        const float dist = length(d);
        if (dist >= eu.bsPos[i].w || dist < 1e-3) continue;
        const float ndl = dot(n, d / dist);
        if (ndl <= 0.0) continue;
        const float sh = enh_light_shadow(wp - eu.bsPos[i].xyz, n, float4(1.0, eu.bsParams.y, eu.bsPos[i].w, eu.bsColor[i].w), atlas, ss);
        if (sh >= 1.0) continue;
        float f = 1.0 - dist / eu.bsPos[i].w;
        loss += (1.0 - sh) * eu.bsColor[i].rgb * (f * f * ndl);
    }
    return min(loss, baked);
}

// The model's light-grid lattice (one point = six coefficient vectors + the sun bit) interpolated at a world point.
static float3 probe_lighting_c(float3 n, thread const float4* c);
static float3 enh_lattice_probe(float3 wp, float3 n, constant EnhDrawU& du, const device float4* lat, thread float& sunOut) {
    const int3 dims = int3(du.lattice.yzw + 0.5);
    const float3 g = clamp((wp - du.origin.xyz) / du.step.xyz, float3(0.0), float3(dims - 1));
    const int3 i0 = min(int3(floor(g)), dims - 2);
    const float3 f = g - float3(i0);
    float4 c[6] = {float4(0.0), float4(0.0), float4(0.0), float4(0.0), float4(0.0), float4(0.0)};
    float sun = 0.0;
    for (int k = 0; k < 8; ++k) {
        const int3 o = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        const int3 q = i0 + o;
        const float w = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y) * (o.z ? f.z : 1.0 - f.z);
        const int base = (int(du.lattice.x + 0.5) + (q.z * dims.y + q.y) * dims.x + q.x) * 7;
        for (int j = 0; j < 6; ++j) c[j] += w * lat[base + j];
        sun += w * lat[base + 6].x;
    }
    sunOut = sun;
    return probe_lighting_c(n, c);
}
)MSL"
