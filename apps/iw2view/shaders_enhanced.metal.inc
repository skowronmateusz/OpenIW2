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
};
// Fragment uniform slot 2: per draw (model items).
struct EnhDrawU {
    float4 lattice; // x: first point, y, z, w: points per axis (w > 0.5: lattice present)
    float4 origin;  // xyz: world position of point (0, 0, 0); w > 0.5: no screen-space AO / contact shadows (view model, blended)
    float4 step;    // xyz: world distance between points; w > 0.5: no dynamic sun shadow (effect sprites)
};
#define ENH_FS_ARGS(t) constant EnhFragU& eu [[buffer(1)]], constant EnhDrawU& du [[buffer(2)]], \
    depth2d<float> shadowMap [[texture(t)]], sampler shadowSampler [[sampler(t)]], const device float4* lattice [[buffer(3)]], \
    depth2d<float> worldShadow0 [[texture(t + 1)]], sampler worldSampler [[sampler(t + 1)]], \
    depth2d<float> worldShadow1 [[texture(t + 2)]], sampler worldSampler1 [[sampler(t + 2)]], \
    depth2d<float> worldShadow2 [[texture(t + 3)]], sampler worldSampler2 [[sampler(t + 3)]], \
    texture2d<float> aoTex [[texture(t + 4)]], sampler aoSampler [[sampler(t + 4)]]
#define ENH_WORLD_SHADOWS eu, worldShadow0, worldShadow1, worldShadow2, worldSampler

constant float2 kEnhPoisson[12] = {
    float2(-0.326, -0.406), float2(-0.840, -0.074), float2(-0.696, 0.457), float2(-0.203, 0.621),
    float2(0.962, -0.195), float2(0.473, -0.480), float2(0.519, 0.767), float2(0.185, -0.893),
    float2(0.507, 0.064), float2(0.896, 0.412), float2(-0.322, -0.933), float2(-0.792, -0.598)};

// Dynamic sun visibility of a world point from the shadow map of the moving casters (1 = lit). Normal offset against acne, a small
// rotated Poisson disk of hardware-filtered compare taps, the last cascade fades out towards its edge.
static float enh_sun_shadow(float3 wp, float3 n, constant EnhFragU& eu, depth2d<float> sm, sampler ss) {
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
    const int taps = int(eu.shadowParams.w + 0.5);
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
static float2 enh_world_sun(float3 wp, float3 n, constant EnhFragU& eu, depth2d<float> w0, depth2d<float> w1, depth2d<float> w2, sampler ss) {
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
    const int taps = int(eu.worldParams.w + 0.5);
    const float ang = fract(52.9829189 * fract(dot(wp.xy, float2(0.06711056, 0.00583715)))) * 6.2831853;
    const float2x2 rot = float2x2(float2(cos(ang), sin(ang)), float2(-sin(ang), cos(ang)));
    const float2 texel = float2(eu.worldParams.y * 1.5);
    float lit = 0.0;
    for (int i = 0; i < taps; ++i) {
        const float2 q = uv + (rot * kEnhPoisson[i]) * texel;
        lit += c == 0 ? w0.sample_compare(ss, q, ref) : c == 1 ? w1.sample_compare(ss, q, ref) : w2.sample_compare(ss, q, ref);
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
