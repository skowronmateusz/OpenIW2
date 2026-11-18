// Enhanced rendering mode (docs/render-enhanced.md): an openiw2 addition with no counterpart in the original. The default renderer
// keeps the original look; r_enhanced 1 selects a separate set of pipelines (16-bit float scene target, dynamic sun shadows, tone
// mapping, bloom, auto exposure, EDR output). This header holds the pure maths of that mode so it can be tested without a GPU; the
// shaders in apps/iw2view/shaders_enhanced.metal.inc implement the same formulas.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace iw2::game {

// ---- settings ----------------------------------------------------------------------------------------------------------------------

// Sun shadow quality: map size and cascade count (docs/render-enhanced.md "Sun shadows").
struct SunShadowQuality {
    int resolution = 0;  // texels per side of one cascade, 0 = off
    int cascades = 0;    // 1 or 2
    float radius[2] = {0.0f, 0.0f}; // world units covered around the camera by each cascade (sphere radius)
    int pcfTaps = 0;     // receiver filter taps
};

inline SunShadowQuality sunShadowQuality(int level) {
    switch (level) {
        case 1: return {1024, 1, {1200.0f, 0.0f}, 4};
        case 2: return {2048, 1, {1600.0f, 0.0f}, 8};
        case 3: return {2048, 2, {700.0f, 2800.0f}, 12};
        default: return {};
    }
}

// World (static) sun shadow cascades: the world and static models, cached and re-rendered only when the camera has moved a quarter
// of a cascade's radius (docs/render-enhanced.md "World sun shadows").
struct WorldShadowQuality {
    int resolution = 0;
    int cascades = 0;
    float radius[3] = {0.0f, 0.0f, 0.0f};
    int pcfTaps = 0;
};

inline WorldShadowQuality worldShadowQuality(int level) {
    switch (level) {
        case 1: return {2048, 2, {1100.0f, 4500.0f, 0.0f}, 8};
        case 2: return {2048, 3, {700.0f, 2400.0f, 8000.0f}, 12};
        default: return {};
    }
}

// The centre of a cached cascade: the camera position snapped to a world grid of a quarter radius in x and y (z keeps the camera
// height), so the cascade is re-rendered only when that snapped point changes.
inline std::array<float, 3> cascadeCentre(const float cam[3], float radius) {
    const float step = radius * 0.25f;
    return {std::round(cam[0] / step) * step, std::round(cam[1] / step) * step, std::round(cam[2] / step) * step};
}

// Depth reach towards the sun that keeps every caster of a world box in the map: the box's extent along the sun direction beyond
// the cascade centre, at least `minimum`.
inline float casterReachForBounds(const float towardsSun[3], const float centre[3], const float mins[3], const float maxs[3], float minimum) {
    float reach = minimum;
    for (int k = 0; k < 8; ++k) {
        const float p[3] = {(k & 1) ? maxs[0] : mins[0], (k & 2) ? maxs[1] : mins[1], (k & 4) ? maxs[2] : mins[2]};
        const float d = (p[0] - centre[0]) * towardsSun[0] + (p[1] - centre[1]) * towardsSun[1] + (p[2] - centre[2]) * towardsSun[2];
        reach = std::max(reach, d);
    }
    return reach;
}

struct EnhancedSettings {
    bool enabled = false;    // r_enhanced (latched when the renderer starts)
    int preset = 1;          // r_enhPreset: 0 low, 1 medium, 2 high
    bool tonemap = true;     // r_enhTonemap: highlight shoulder instead of the clip at 1
    bool bloom = true;       // r_enhBloom: bloom of the light above display white (the original glow chain stays)
    bool autoExposure = true; // r_enhAutoExposure
    bool edr = true;         // r_enhEdr: extended dynamic range output on displays that support it
    int sunShadows = 2;      // r_enhSunShadows: 0 off, 1 low, 2 medium, 3 high (sunShadowQuality)
    int worldShadows = 1;    // r_enhWorldShadows: 0 off, 1 two cascades, 2 three cascades (worldShadowQuality)
    bool ao = true;          // r_enhAO: ground-truth ambient occlusion on the baked / probe light
    float aoStrength = 0.7f; // r_enhAOStrength (0..1)
    bool contactShadows = true; // r_enhContactShadows: short screen-space rays towards the sun
    bool taa = true;         // r_enhTAA: temporal anti-aliasing (jittered projection, motion vectors, history clamp)
    bool ggx = true;         // r_enhGGX: GGX sun specular and reflections from the spec / gloss maps
    float specScale = 1.0f;  // r_enhSpecScale (0..4)
    float envScale = 0.35f;  // r_enhEnvScale: strength of the sky / screen-space reflections (0..2)
    bool ssr = true;         // r_enhSSR: screen-space reflections (needs TAA's history)
    bool lightShadows = true; // r_enhLightShadows: shadows of the dynamic (effect) lights, up to 4 lights
    bool mapLightSpec = true; // r_enhMapLightSpec: GGX specular from the map's baked light entities near the camera (diffuse stays baked)
    bool probes = true;      // r_enhProbes: local reflection cubes captured when the map loads (instead of the sky alone)
    float volumetric = 0.0f; // r_enhVolumetric: strength of the sun shafts (light scattered in the air, 0 = off, up to 2)
    int grade = 0;           // r_enhGrade: colour grade, 0 neutral, 1 period film (contrast, desaturation, warm shadows, grain)
    bool metalFx = false;    // r_enhMetalFX: MetalFX temporal anti-aliasing instead of ours (needs TAA on and a MetalFX GPU / build)
    bool rtShadows = true;   // r_enhRTShadows: ray-traced sun and lamp shadows of the moving objects (GPU family 9+)
    bool rtContactAO = true; // r_enhRTContactAO: ray-traced contact occlusion of the moving objects (GPU family 9+)
    bool rtReflections = true; // r_enhRTReflections: ray-traced reflections on water and the reflective surfaces (GPU family 9+)
    float renderScale = 1.0f; // r_enhRenderScale: scene resolution as a fraction of the window, upscaled by MetalFX (0.5..1)
    float dof = 0.0f;        // r_enhDOF: depth of field while aiming down the sights (0 = off, 1 = default strength, up to 2)
    float motionBlur = 0.0f; // r_enhMotionBlur: motion blur amount (0 = off, 1 = one frame of motion)
    bool gridPerPixel = true; // r_enhGridPerPixel: light-grid lattice per model instead of one sample
    int motionDebug = 0;     // r_enhMotionDebug: 1 shows the motion vectors, 2 the AO (red) / contact shadow (green) image
    float bloomIntensity = 0.06f; // r_enhBloomIntensity
    float exposureBias = 0.0f;    // r_enhExposureBias (EV)
};

// Reads the settings from a cvar lookup that returns "" for an unset cvar. The preset fills every feature; a per-feature cvar that
// is set overrides it.
inline EnhancedSettings enhancedSettings(const std::function<std::string(const char*)>& get) {
    EnhancedSettings s;
    // A negative value of a per-feature cvar means "as the preset" (the menu's Preset choice, docs/render-enhanced.md "Menu").
    auto num = [&](const char* n, float& out) {
        const std::string v = get(n);
        if (v.empty()) return false;
        out = float(std::atof(v.c_str()));
        return out >= 0.0f;
    };
    float f = 0.0f;
    s.enabled = num("r_enhanced", f) && f != 0.0f;
    if (num("r_enhPreset", f)) s.preset = std::clamp(int(f), 0, 2);
    switch (s.preset) {
        case 0: s.sunShadows = 1, s.worldShadows = 0, s.bloom = true, s.autoExposure = false, s.gridPerPixel = true, s.ao = false, s.contactShadows = false, s.taa = false, s.ssr = false, s.lightShadows = false, s.volumetric = 0.0f, s.mapLightSpec = false, s.probes = false; break;
        case 2: s.volumetric = 0.35f, s.sunShadows = 3, s.worldShadows = 2, s.bloom = true, s.autoExposure = true, s.gridPerPixel = true; break;
        default: break;
    }
    if (num("r_enhTonemap", f)) s.tonemap = f != 0.0f;
    if (num("r_enhBloom", f)) s.bloom = f != 0.0f;
    if (num("r_enhAutoExposure", f)) s.autoExposure = f != 0.0f;
    if (num("r_enhEdr", f)) s.edr = f != 0.0f;
    if (num("r_enhSunShadows", f)) s.sunShadows = std::clamp(int(f), 0, 3);
    if (num("r_enhGridPerPixel", f)) s.gridPerPixel = f != 0.0f;
    if (num("r_enhWorldShadows", f)) s.worldShadows = std::clamp(int(f), 0, 2);
    if (num("r_enhAO", f)) s.ao = f != 0.0f;
    if (num("r_enhAOStrength", f)) s.aoStrength = std::clamp(f, 0.0f, 1.0f);
    if (num("r_enhContactShadows", f)) s.contactShadows = f != 0.0f;
    if (num("r_enhTAA", f)) s.taa = f != 0.0f;
    if (num("r_enhGGX", f)) s.ggx = f != 0.0f;
    if (num("r_enhSpecScale", f)) s.specScale = std::clamp(f, 0.0f, 4.0f);
    if (num("r_enhEnvScale", f)) s.envScale = std::clamp(f, 0.0f, 2.0f);
    if (num("r_enhSSR", f)) s.ssr = f != 0.0f;
    if (num("r_enhLightShadows", f)) s.lightShadows = f != 0.0f;
    if (num("r_enhMapLightSpec", f)) s.mapLightSpec = f != 0.0f;
    if (num("r_enhProbes", f)) s.probes = f != 0.0f;
    if (num("r_enhVolumetric", f)) s.volumetric = std::clamp(f, 0.0f, 2.0f);
    if (num("r_enhGrade", f)) s.grade = std::clamp(int(f), 0, 1);
    if (num("r_enhMotionBlur", f)) s.motionBlur = std::clamp(f, 0.0f, 2.0f);
    if (num("r_enhDOF", f)) s.dof = std::clamp(f, 0.0f, 2.0f);
    if (num("r_enhMetalFX", f)) s.metalFx = f != 0.0f;
    if (num("r_enhRenderScale", f)) s.renderScale = std::clamp(f, 0.5f, 1.0f);
    if (num("r_enhRTReflections", f)) s.rtReflections = f != 0.0f;
    if (num("r_enhRTShadows", f)) s.rtShadows = f != 0.0f;
    if (num("r_enhRTContactAO", f)) s.rtContactAO = f != 0.0f;
    if (num("r_enhMotionDebug", f)) s.motionDebug = std::clamp(int(f), 0, 2);
    if (num("r_enhBloomIntensity", f)) s.bloomIntensity = std::clamp(f, 0.0f, 1.0f);
    if (const std::string v = get("r_enhExposureBias"); !v.empty()) s.exposureBias = std::clamp(float(std::atof(v.c_str())), -2.0f, 2.0f); // signed
    return s;
}

// ---- tone mapping ------------------------------------------------------------------------------------------------------------------

// The scene is shaded in the original's display (gamma) encoding. The tone curve works on that encoding per channel: the identity up to
// the knee, so every value the original could show is shown unchanged, then an exponential shoulder that reaches `white`
// asymptotically with slope 1 at the knee (C1). SDR: white 1. EDR: white = the display headroom in the same encoding.
constexpr float kToneKnee = 0.8f;

inline float toneShoulder(float x, float knee, float white) {
    if (x <= knee) return x;
    const float span = white - knee;
    if (span <= 0.0f) return white;
    return knee + span * (1.0f - std::exp(-(x - knee) / span));
}

// sRGB transfer function (IEC 61966-2-1), extended past 1 with the same power segment (extended linear sRGB of the EDR swapchain).
inline float srgbToLinear(float c) {
    const float a = std::fabs(c);
    const float l = a <= 0.04045f ? a / 12.92f : std::pow((a + 0.055f) / 1.055f, 2.4f);
    return c < 0.0f ? -l : l;
}
inline float linearToSrgb(float l) {
    const float a = std::fabs(l);
    const float c = a <= 0.0031308f ? a * 12.92f : 1.055f * std::pow(a, 1.0f / 2.4f) - 0.055f;
    return l < 0.0f ? -c : c;
}

// The tone curve's white for a display: SDR 1; EDR the headroom (linear multiple of SDR white) in display encoding, capped.
inline float toneWhite(float headroomLinear) {
    if (headroomLinear <= 1.0f) return 1.0f;
    return linearToSrgb(std::min(headroomLinear, 16.0f));
}

// ---- auto exposure -----------------------------------------------------------------------------------------------------------------

// Exposure in EV (a factor of 2^ev on linear light). The target moves the frame's mean log2 luminance towards `keyLog2` by `strength`
// (0 = no adaptation, 1 = full), clamped to [minEv, maxEv] so readability does not suffer.
struct ExposureParams {
    float keyLog2 = -3.0f;   // calibrated on the original look (docs/render-enhanced.md "Auto exposure")
    float strength = 0.3f;
    float minEv = -0.4f, maxEv = 0.5f;
    float tauBrighten = 1.2f, tauDarken = 0.5f; // seconds (eyes adapt to darkness slower than to light)
};

inline float exposureTargetEv(float meanLog2Lum, const ExposureParams& p, float biasEv = 0.0f) {
    return std::clamp((p.keyLog2 - meanLog2Lum) * p.strength, p.minEv, p.maxEv) + biasEv;
}

// One step of exponential adaptation over dt seconds (frame-rate independent).
inline float adaptExposureEv(float current, float target, float dt, const ExposureParams& p) {
    const float tau = target > current ? p.tauBrighten : p.tauDarken;
    if (dt <= 0.0f || tau <= 0.0f) return dt > 0.0f ? target : current;
    return current + (target - current) * (1.0f - std::exp(-dt / tau));
}

// ---- bloom -------------------------------------------------------------------------------------------------------------------------

// Mip levels of the bloom chain for a scene of w x h: the first level is half size, each next one half again, down to about 8 pixels
// on the short side, at most 7 levels.
inline int bloomLevels(int w, int h) {
    int n = 0;
    for (int s = std::min(w, h) / 2; s >= 8 && n < 7; s /= 2) ++n;
    return std::max(n, 1);
}

// Soft-knee threshold on linear luminance: 0 below threshold - knee, quadratic through the knee, linear above (continuous).
inline float bloomWeight(float lum, float threshold, float knee) {
    const float soft = std::clamp(lum - threshold + knee, 0.0f, 2.0f * knee);
    const float curve = knee > 0.0f ? soft * soft / (4.0f * knee + 1e-5f) : 0.0f;
    const float w = std::max(curve, lum - threshold);
    return lum > 0.0f ? w / std::max(lum, 1e-5f) : 0.0f;
}

// ---- sun shadows -------------------------------------------------------------------------------------------------------------------

using Mat4c = std::array<float, 16>; // column-major, m[col * 4 + row], clip = M * world (Metal depth 0..1)

struct SunShadowFit {
    Mat4c viewProj{};
    float texelWorld = 0.0f; // world units per shadow texel
    float depthRange = 0.0f; // world units mapped to depth 0..1
};

// Orthographic sun view around a sphere (center, radius). The light looks along -towardsSun. The sphere's centre is snapped to whole
// texels in the light plane so the map does not shimmer when the camera moves; casters up to `casterReach` units beyond the sphere
// towards the sun are kept in the depth range.
inline SunShadowFit fitSunShadow(const float towardsSun[3], const float center[3], float radius, float casterReach, int resolution) {
    float f[3] = {-towardsSun[0], -towardsSun[1], -towardsSun[2]};
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& c : f) c /= fl > 0.0f ? fl : 1.0f;
    const float up0[3] = {std::fabs(f[2]) > 0.99f ? 1.0f : 0.0f, 0.0f, std::fabs(f[2]) > 0.99f ? 0.0f : 1.0f};
    float r[3] = {f[1] * up0[2] - f[2] * up0[1], f[2] * up0[0] - f[0] * up0[2], f[0] * up0[1] - f[1] * up0[0]};
    const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (float& c : r) c /= rl;
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    const float texel = 2.0f * radius / float(resolution);
    auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float cx = std::floor(dot3(center, r) / texel) * texel, cy = std::floor(dot3(center, u) / texel) * texel;
    const float cz = dot3(center, f);
    const float zNear = cz - radius - casterReach, zFar = cz + radius;
    const float sx = 1.0f / radius, sz = 1.0f / (zFar - zNear);
    SunShadowFit out;
    Mat4c& m = out.viewProj;
    // row 0: (dot(p, r) - cx) / R; row 1: (dot(p, u) - cy) / R; row 2: (dot(p, f) - zNear) / (zFar - zNear); row 3: 1
    for (int k = 0; k < 3; ++k) {
        m[size_t(k * 4 + 0)] = r[k] * sx;
        m[size_t(k * 4 + 1)] = u[k] * sx;
        m[size_t(k * 4 + 2)] = f[k] * sz;
        m[size_t(k * 4 + 3)] = 0.0f;
    }
    m[12] = -cx * sx;
    m[13] = -cy * sx;
    m[14] = -zNear * sz;
    m[15] = 1.0f;
    out.texelWorld = texel;
    out.depthRange = zFar - zNear;
    return out;
}

inline std::array<float, 4> mulPoint(const Mat4c& m, const float p[3]) {
    std::array<float, 4> o{};
    for (int row = 0; row < 4; ++row) o[size_t(row)] = m[size_t(row)] * p[0] + m[size_t(4 + row)] * p[1] + m[size_t(8 + row)] * p[2] + m[size_t(12 + row)];
    return o;
}

// Fade of the dynamic shadow towards the edge of the last cascade (1 inside, 0 at the edge): the last 15 % of the radius.
inline float shadowEdgeFade(float distFromCenter, float radius) {
    const float start = radius * 0.85f;
    if (distFromCenter <= start) return 1.0f;
    return std::clamp(1.0f - (distFromCenter - start) / (radius - start), 0.0f, 1.0f);
}

// ---- per-pixel light grid ----------------------------------------------------------------------------------------------------------

// Lattice of light-grid points over a model's world box: the light grid's own nodes (32 x 32 x 64 units, world-aligned,
// docs/spec/lightgrid.md) that enclose the box, every k-th node per axis so there are at most 4 points per axis. Sampling at nodes
// lets the renderer cache the samples across frames; the fragment shader interpolates trilinearly between them, which is the grid's
// own interpolation (assets::LightGrid::sample).
struct ProbeLattice {
    int n[3] = {1, 1, 1};
    int node0[3] = {0, 0, 0}; // grid node index of point (0, 0, 0)
    int stride[3] = {1, 1, 1}; // nodes between points
    float origin[3] = {0, 0, 0};
    float step[3] = {1, 1, 1};
    int count() const { return n[0] * n[1] * n[2]; }
};

constexpr float kGridCell[3] = {32.0f, 32.0f, 64.0f};

inline ProbeLattice probeLattice(const float mins[3], const float maxs[3]) {
    ProbeLattice l;
    for (int a = 0; a < 3; ++a) {
        const int lo = int(std::floor(mins[a] / kGridCell[a]));
        int hi = int(std::ceil(maxs[a] / kGridCell[a]));
        if (hi <= lo) hi = lo + 1;
        const int cells = hi - lo;
        const int k = std::max(1, (cells + 2) / 3);
        l.n[a] = (cells + k - 1) / k + 1;
        l.node0[a] = lo;
        l.stride[a] = k;
        l.origin[a] = float(lo) * kGridCell[a];
        l.step[a] = float(k) * kGridCell[a];
    }
    return l;
}

// ---- ambient occlusion ----------------------------------------------------------------------------------------------------------------

// Ground-truth ambient occlusion (Jimenez et al. 2016), the cosine-weighted visible arc of one slice: n = the angle of the normal
// projected into the slice, from the view vector; h0 <= n <= h1 the horizon angles on both sides (radians, from the view vector).
// Returns the slice's visibility in 0..1 (1 for an unoccluded slice whose horizons lie flat at -pi/2 + n and pi/2 + n).
inline float gtaoSliceVisibility(float n, float h0, float h1) {
    const float hp = 1.5707963f;
    h0 = n + std::max(h0 - n, -hp);
    h1 = n + std::min(h1 - n, hp);
    const float cn = std::cos(n), sn = std::sin(n);
    const float a = -std::cos(2.0f * h0 - n) + cn + 2.0f * h0 * sn;
    const float b = -std::cos(2.0f * h1 - n) + cn + 2.0f * h1 * sn;
    return 0.25f * (a + b);
}

// ---- materials ------------------------------------------------------------------------------------------------------------------

// The Phong exponent of a specular map alpha (the $specularity column it selects: t = (32 a - 0.5) / 30, P = ln 0.1 / ln cos((180 - 175 t)
// / 4 degrees)) and the GGX alpha of the Blinn-Phong match alpha = sqrt(2 / (P + 2)), clamped to [0.03, 1]. The shaders use the same.
inline float phongPowerOfSpecAlpha(float a) {
    const float t = std::clamp((a * 32.0f - 0.5f) / 30.0f, 0.0f, 1.0f);
    return std::log(0.1f) / std::log(std::cos((180.0f - 175.0f * t) * 3.14159265f / 720.0f));
}
inline float ggxAlphaOfPower(float p) { return std::clamp(std::sqrt(2.0f / (p + 2.0f)), 0.03f, 1.0f); }

// ---- map lights -------------------------------------------------------------------------------------------------------------------

// A light entity of the map as the enhanced specular uses it (our own model of keys the engine never reads: origin, radius,
// intensity, _color, and for spot lights the target's position with fov_inner / fov_outer in degrees).
struct MapLight {
    float pos[3] = {0, 0, 0};
    float radius = 300.0f;
    float color[3] = {1, 1, 1}; // _color times intensity
    float dir[3] = {0, 0, -1};  // spot axis
    float cosOuter = -2.0f, cosInner = -1.0f; // spot cone (cosOuter < -1: a point light)
};
// Our falloff of a map light: (1 - d / r)^2 inside the radius, times the spot cone (smooth between the outer and inner angles).
inline float mapLightFalloff(const MapLight& l, const float p[3]) {
    const float v[3] = {l.pos[0] - p[0], l.pos[1] - p[1], l.pos[2] - p[2]};
    const float d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (d >= l.radius) return 0.0f;
    float f = (1.0f - d / l.radius) * (1.0f - d / l.radius);
    if (l.cosOuter >= -1.0f && d > 0.0f) {
        const float c = -(v[0] * l.dir[0] + v[1] * l.dir[1] + v[2] * l.dir[2]) / d;
        const float t = std::clamp((c - l.cosOuter) / std::max(l.cosInner - l.cosOuter, 1e-4f), 0.0f, 1.0f);
        f *= t * t * (3.0f - 2.0f * t);
    }
    return f;
}

// Baked-light shadows of moving objects take only local lamps: a light whose radius is at most 1500 units and whose origin is
// within 1500 units of the camera (large fill lights far away would cast a meaningless point shadow at a cost), and that reaches
// within 600 units of the camera.
inline bool bakedShadowLightNearCamera(const MapLight& l, const float cam[3]) {
    const float dx = l.pos[0] - cam[0], dy = l.pos[1] - cam[1], dz = l.pos[2] - cam[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    return l.radius <= 1500.0f && d <= 1500.0f && d - l.radius <= 600.0f;
}

// ---- atmosphere and grading ------------------------------------------------------------------------------------------------------

// Henyey-Greenstein phase function for scattering angle cos theta (between the view ray and the direction towards the light) and
// asymmetry g (> 0: forward scattering, bright towards the sun). Normalised over the sphere.
inline float henyeyGreenstein(float cosTheta, float g) {
    const float d = 1.0f + g * g - 2.0f * g * cosTheta;
    return (1.0f - g * g) / (4.0f * 3.14159265f * d * std::sqrt(d));
}

// The "period film" grade of display-encoded colour: an S-curve around mid grey (contrast k), saturation s, a warm lift of the
// shadows. Neutral grey stays grey; 0 and 1 stay put.
inline std::array<float, 3> periodFilmGrade(const std::array<float, 3>& c, float k = 1.15f, float s = 0.8f) {
    std::array<float, 3> o{};
    const float l = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
    for (int i = 0; i < 3; ++i) {
        float v = l + (c[size_t(i)] - l) * s;
        const float x = std::clamp(v, 0.0f, 1.0f);
        const float sc = x < 0.5f ? 0.5f * std::pow(2.0f * x, k) : 1.0f - 0.5f * std::pow(2.0f - 2.0f * x, k);
        v = sc + (v - x); // keep values above 1 (HDR / EDR) as they were beyond the curve
        o[size_t(i)] = v;
    }
    const float warm = 0.03f * (1.0f - std::clamp(l * 2.0f, 0.0f, 1.0f)); // shadows a little warmer
    o[0] += warm, o[2] -= warm;
    return o;
}

// Depth of field while aiming (our own simple model): the blur radius in pixels of a point at view depth w, for ADS fraction a and
// strength s: nothing up to 40 units (the weapon and the sights stay sharp), growing to 2.5 px at 440 units and beyond.
inline float aimDofRadius(float w, float ads, float strength) {
    return 2.5f * strength * std::clamp(ads, 0.0f, 1.0f) * std::clamp((w - 40.0f) / 400.0f, 0.0f, 1.0f);
}

// ---- point light shadows ---------------------------------------------------------------------------------------------------------

// The six faces of a point light's shadow: face f looks along +X, -X, +Y, -Y, +Z, -Z; its right / up axes are our own fixed choice
// (the shader picks the face by the major axis of the light-to-point vector and projects with the same axes). Tile (f % 3, f / 3).
struct CubeFace { float fwd[3], right[3], up[3]; };
inline CubeFace cubeFace(int f) {
    CubeFace c{};
    const int axis = f / 2;
    const float sgn = (f & 1) ? -1.0f : 1.0f;
    c.fwd[axis] = sgn;
    float up[3] = {0, 0, 1};
    if (axis == 2) up[0] = 1, up[2] = 0;
    // right = fwd x up
    c.right[0] = c.fwd[1] * up[2] - c.fwd[2] * up[1];
    c.right[1] = c.fwd[2] * up[0] - c.fwd[0] * up[2];
    c.right[2] = c.fwd[0] * up[1] - c.fwd[1] * up[0];
    for (int k = 0; k < 3; ++k) c.up[k] = up[k];
    return c;
}
inline int cubeFaceOf(const float v[3]) {
    const float ax = std::fabs(v[0]), ay = std::fabs(v[1]), az = std::fabs(v[2]);
    if (ax >= ay && ax >= az) return v[0] >= 0.0f ? 0 : 1;
    if (ay >= az) return v[1] >= 0.0f ? 2 : 3;
    return v[2] >= 0.0f ? 4 : 5;
}
// Depth (0..1) of a point at distance d along a face's forward axis, for near n and far f.
inline float cubeFaceDepth(float d, float n, float f) { return f / (f - n) - f * n / ((f - n) * d); }

// ---- temporal anti-aliasing ------------------------------------------------------------------------------------------------------

// Halton low-discrepancy sequence (base b) at index i >= 1, in [0, 1).
inline float halton(int i, int b) {
    float f = 1.0f, r = 0.0f;
    for (; i > 0; i /= b) {
        f /= float(b);
        r += f * float(i % b);
    }
    return r;
}

// Sub-pixel projection jitter of frame n for a w x h target, in NDC: Halton (2, 3) over 8 frames, centred, one pixel wide.
inline std::array<float, 2> taaJitter(uint32_t frame, int w, int h, uint32_t phases = 8) {
    const int i = int(frame % std::max(phases, 1u)) + 1;
    return {(halton(i, 2) - 0.5f) * 2.0f / float(w), (halton(i, 3) - 0.5f) * 2.0f / float(h)};
}

// ---- motion vectors ----------------------------------------------------------------------------------------------------------------

inline Mat4c mulMat(const Mat4c& a, const Mat4c& b) {
    Mat4c r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float v = 0.0f;
            for (int k = 0; k < 4; ++k) v += a[size_t(k * 4 + row)] * b[size_t(c * 4 + k)];
            r[size_t(c * 4 + row)] = v;
        }
    return r;
}

// General 4 x 4 inverse (cofactors); returns false for a singular matrix.
inline bool invertMat(const Mat4c& m, Mat4c& out) {
    const float* a = m.data();
    float inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (det == 0.0f || !std::isfinite(det)) return false;
    for (int i = 0; i < 16; ++i) out[size_t(i)] = inv[i] / det;
    return true;
}

// Camera motion of a pixel of the static scene (docs/render-enhanced.md "Motion vectors"): the pixel's NDC position and its clip-space
// z ($floatz, the clip z before the divide) rebuild the clip position (w from the projection's z = zA * w + zB); the inverse of this
// frame's view-projection gives the world point, the previous frame's view-projection its previous NDC. Returns current - previous NDC.
inline std::array<float, 2> cameraMotion(float ndcX, float ndcY, float clipZ, float zA, float zB, const Mat4c& invViewProj, const Mat4c& prevViewProj) {
    const float w = zA != 0.0f ? (clipZ - zB) / zA : 1.0f;
    const float clip[4] = {ndcX * w, ndcY * w, clipZ, w};
    float world[4] = {0, 0, 0, 0};
    for (int row = 0; row < 4; ++row)
        for (int k = 0; k < 4; ++k) world[row] += invViewProj[size_t(k * 4 + row)] * clip[k];
    const float p[3] = {world[0] / world[3], world[1] / world[3], world[2] / world[3]};
    const auto prev = mulPoint(prevViewProj, p);
    return {ndcX - prev[0] / prev[3], ndcY - prev[1] / prev[3]};
}

// zA, zB of a view-projection: clip z = zA * clip w + zB for every point (projection with w = view depth).
inline std::array<float, 2> clipDepthLine(const Mat4c& vp, const float eye[3], const float fwd[3]) {
    const float p1[3] = {eye[0] + fwd[0] * 100.0f, eye[1] + fwd[1] * 100.0f, eye[2] + fwd[2] * 100.0f};
    const float p2[3] = {eye[0] + fwd[0] * 1000.0f, eye[1] + fwd[1] * 1000.0f, eye[2] + fwd[2] * 1000.0f};
    const auto c1 = mulPoint(vp, p1), c2 = mulPoint(vp, p2);
    const float a = (c2[2] - c1[2]) / (c2[3] - c1[3]);
    return {a, c1[2] - a * c1[3]};
}

// ---- reflection probes (docs/render-enhanced.md "Reflection probes") -------------------------------------------------------------
// Our own design: the maps carry no reflection probes. Probe positions come from points known to be in playable space (the light
// grid's points), each with its portal cell (-1 outside every cell).

// Picks at most cap of the points: first one per cell (the point nearest the mean of the cell's points), cells with more points
// first, then farthest-point sampling (the point farthest from all picked ones) for the rest. Returns indices into pts.
inline std::vector<int> placeProbes(const std::vector<std::array<float, 3>>& pts, const std::vector<int>& ptCell, size_t cap) {
    std::vector<int> out;
    if (pts.empty() || cap == 0) return out;
    struct Acc { double s[3] = {0, 0, 0}; int n = 0; };
    std::vector<Acc> acc;
    for (size_t i = 0; i < pts.size(); ++i) {
        const int c = ptCell[i];
        if (c < 0) continue;
        if (size_t(c) >= acc.size()) acc.resize(size_t(c) + 1);
        for (int a = 0; a < 3; ++a) acc[size_t(c)].s[a] += pts[i][size_t(a)];
        ++acc[size_t(c)].n;
    }
    std::vector<int> cells;
    for (size_t c = 0; c < acc.size(); ++c) if (acc[c].n > 0) cells.push_back(int(c));
    std::stable_sort(cells.begin(), cells.end(), [&](int a, int b) { return acc[size_t(a)].n > acc[size_t(b)].n; });
    std::vector<int> best(acc.size(), -1);
    std::vector<float> bestD(acc.size(), 0.0f);
    for (size_t i = 0; i < pts.size(); ++i) {
        const int c = ptCell[i];
        if (c < 0) continue;
        const Acc& m = acc[size_t(c)];
        float d = 0;
        for (int a = 0; a < 3; ++a) { const float e = pts[i][size_t(a)] - float(m.s[a] / m.n); d += e * e; }
        if (best[size_t(c)] < 0 || d < bestD[size_t(c)]) best[size_t(c)] = int(i), bestD[size_t(c)] = d;
    }
    std::vector<float> minD(pts.size(), 3.4e38f);
    auto take = [&](int i) {
        out.push_back(i);
        for (size_t k = 0; k < pts.size(); ++k) {
            float d = 0;
            for (int a = 0; a < 3; ++a) { const float e = pts[k][size_t(a)] - pts[size_t(i)][size_t(a)]; d += e * e; }
            minD[k] = std::min(minD[k], d);
        }
    };
    for (const int c : cells) {
        if (out.size() >= cap) return out;
        take(best[size_t(c)]);
    }
    if (out.empty()) take(0);
    while (out.size() < cap) {
        size_t far = 0;
        for (size_t k = 1; k < pts.size(); ++k) if (minD[k] > minD[far]) far = k;
        if (minD[far] <= 0.0f) break; // every point is a probe
        take(int(far));
    }
    return out;
}

// Points near the world's geometry: inside a world surface box grown by margin, counting only boxes at most maxBoxSize along each
// axis (the sky box and other huge pieces would take in the empty air above a map). A 256-unit occupancy grid over the boxes.
inline std::vector<std::array<float, 3>> pointsNearBoxes(const std::vector<std::array<float, 3>>& pts,
                                                         const std::vector<std::array<float, 6>>& boxes, float margin,
                                                         float maxBoxSize) {
    constexpr float kCell = 256.0f;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    std::vector<const std::array<float, 6>*> use;
    for (const auto& b : boxes) {
        if (b[3] - b[0] > maxBoxSize || b[4] - b[1] > maxBoxSize || b[5] - b[2] > maxBoxSize || b[3] < b[0]) continue;
        use.push_back(&b);
        for (int a = 0; a < 3; ++a) mn[a] = std::min(mn[a], b[size_t(a)] - margin), mx[a] = std::max(mx[a], b[size_t(a + 3)] + margin);
    }
    std::vector<std::array<float, 3>> out;
    if (use.empty()) return out;
    int dim[3];
    for (int a = 0; a < 3; ++a) dim[a] = std::max(1, int(std::ceil((mx[a] - mn[a]) / kCell)));
    std::vector<uint8_t> occ(size_t(dim[0]) * size_t(dim[1]) * size_t(dim[2]), 0);
    auto cellOf = [&](float v, int a) { return std::clamp(int(std::floor((v - mn[a]) / kCell)), 0, dim[a] - 1); };
    for (const auto* b : use) {
        int lo[3], hi[3];
        for (int a = 0; a < 3; ++a) lo[a] = cellOf((*b)[size_t(a)] - margin, a), hi[a] = cellOf((*b)[size_t(a + 3)] + margin, a);
        for (int z = lo[2]; z <= hi[2]; ++z)
            for (int y = lo[1]; y <= hi[1]; ++y)
                for (int x = lo[0]; x <= hi[0]; ++x) occ[(size_t(z) * size_t(dim[1]) + size_t(y)) * size_t(dim[0]) + size_t(x)] = 1;
    }
    for (const auto& p : pts) {
        bool inside = true;
        for (int a = 0; a < 3; ++a) inside = inside && p[size_t(a)] >= mn[a] && p[size_t(a)] < mx[a];
        if (inside && occ[(size_t(cellOf(p[2], 2)) * size_t(dim[1]) + size_t(cellOf(p[1], 1))) * size_t(dim[0]) + size_t(cellOf(p[0], 0))])
            out.push_back(p);
    }
    return out;
}

// The probe index grid: a box of cells over the world bounds (cell size at least minCell, grown until the grid has at most
// maxCells cells), one byte per cell: 0 the sky cube, k > 0 probe k - 1.
struct ProbeGridInfo {
    float origin[3] = {0, 0, 0};
    float cell = 128.0f;
    int dims[3] = {0, 0, 0};
    size_t count() const { return size_t(dims[0]) * size_t(dims[1]) * size_t(dims[2]); }
    size_t index(int x, int y, int z) const { return (size_t(z) * size_t(dims[1]) + size_t(y)) * size_t(dims[0]) + size_t(x); }
};
inline ProbeGridInfo probeGridInfo(const float mins[3], const float maxs[3], float minCell, size_t maxCells) {
    ProbeGridInfo g;
    g.cell = minCell;
    for (;;) {
        size_t n = 1;
        for (int a = 0; a < 3; ++a) {
            g.origin[a] = mins[a];
            g.dims[a] = std::max(1, int(std::ceil((maxs[a] - mins[a]) / g.cell)));
            n *= size_t(g.dims[a]);
        }
        if (n <= maxCells) return g;
        g.cell *= 1.25f;
    }
}

// Each grid cell takes the nearest probe in the portal cell of its centre, else the nearest in a portal cell next to it (adjacent),
// else the nearest probe; a centre outside every cell takes the nearest probe outside every cell, else the nearest.
inline std::vector<uint8_t> assignProbes(const ProbeGridInfo& g, const std::vector<std::array<float, 3>>& probes,
                                         const std::vector<int>& probeCell, const std::function<int(const float[3])>& cellOf,
                                         const std::function<bool(int, int)>& adjacent) {
    std::vector<uint8_t> out(g.count(), 0);
    if (probes.empty()) return out;
    const size_t n = std::min<size_t>(probes.size(), 255);
    for (int z = 0; z < g.dims[2]; ++z)
        for (int y = 0; y < g.dims[1]; ++y)
            for (int x = 0; x < g.dims[0]; ++x) {
                const float p[3] = {g.origin[0] + (float(x) + 0.5f) * g.cell, g.origin[1] + (float(y) + 0.5f) * g.cell,
                                    g.origin[2] + (float(z) + 0.5f) * g.cell};
                const int c = cellOf(p);
                int best[3] = {-1, -1, -1}; // same cell, adjacent cell, any
                float bestD[3] = {0, 0, 0};
                for (size_t k = 0; k < n; ++k) {
                    float d = 0;
                    for (int a = 0; a < 3; ++a) { const float e = probes[k][size_t(a)] - p[a]; d += e * e; }
                    const int pc = probeCell[k];
                    const int tier = pc == c ? 0 : (c >= 0 && pc >= 0 && adjacent(c, pc)) ? 1 : 2;
                    for (int t = tier; t < 3; ++t)
                        if (best[t] < 0 || d < bestD[t]) best[t] = int(k), bestD[t] = d;
                }
                const int k = best[0] >= 0 ? best[0] : best[1] >= 0 ? best[1] : best[2];
                out[g.index(x, y, z)] = uint8_t(k + 1);
            }
    return out;
}


// ---- stage 5: MetalFX and ray tracing (docs/render-enhanced.md "Stage 5") ---------------------------------------------------------

// Ray tracing is used only on GPUs with hardware intersection (Apple GPU family 9: M3 and later); earlier Apple GPUs run it in
// software and keep the shadow maps. forcedOff: IW2_NO_METAL_RT.
// MetalFX temporal scaling takes the projection jitter and the motion in input pixels with y down. Our jitter is an NDC offset of
// the projection (x right, y up): pixels = (x * W / 2, -y * H / 2). Our motion is current minus previous unjittered NDC; MetalFX's
// points from the current pixel to the previous one: scale (-W / 2, H / 2). Checked: a pan converges with these signs only.
inline std::array<float, 2> metalFxJitterPixels(float ndcX, float ndcY, unsigned w, unsigned h) {
    return {ndcX * float(w) * 0.5f, -ndcY * float(h) * 0.5f};
}
inline std::array<float, 2> metalFxMotionScale(unsigned w, unsigned h) { return {-float(w) * 0.5f, float(h) * 0.5f}; }

// The scene's render scale: below 1 only while MetalFX does the anti-aliasing (it upscales; our TAA works at the output size).
inline float effectiveRenderScale(float scale, bool metalFxActive) { return metalFxActive ? std::clamp(scale, 0.5f, 1.0f) : 1.0f; }
// Jitter phases while upscaling: 8 per output pixel area, 8 x (1 / scale)^2 (so each output pixel gets about 8 samples).
inline uint32_t taaJitterPhases(float scale) { return uint32_t(std::ceil(8.0f / (scale * scale) - 1e-3f)); }
// The render size for an output size and scale: rounded, at least 1 x 1.
inline std::array<uint32_t, 2> renderSize(uint32_t w, uint32_t h, float scale) {
    return {std::max(1u, uint32_t(std::lround(float(w) * scale))), std::max(1u, uint32_t(std::lround(float(h) * scale)))};
}

// Ray-traced reflections: surfaces up to this GGX alpha (as the screen-space rays), rays up to this many units.
constexpr float kRtReflectionMaxAlpha = 0.45f, kRtReflectionRange = 6000.0f;
// Ray-traced shadows of the moving objects: the sun's cone (tan of the half angle; wider than the real sun's 0.27 degrees, as the
// soft edge of the baked shadows), the lamps' size for their soft edge (units) and the contact occlusion's ray length (units).
constexpr float kRtSunConeTan = 0.012f, kRtLampSize = 6.0f, kRtContactRange = 40.0f;

inline bool metalRayTracingAllowed(bool supportsRaytracing, bool apple9, bool forcedOff) {
    return supportsRaytracing && apple9 && !forcedOff;
}
} // namespace iw2::game
