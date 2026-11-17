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

struct EnhancedSettings {
    bool enabled = false;    // r_enhanced (latched when the renderer starts)
    int preset = 1;          // r_enhPreset: 0 low, 1 medium, 2 high
    bool tonemap = true;     // r_enhTonemap: highlight shoulder instead of the clip at 1
    bool bloom = true;       // r_enhBloom: bloom of the light above display white (the original glow chain stays)
    bool autoExposure = true; // r_enhAutoExposure
    bool edr = true;         // r_enhEdr: extended dynamic range output on displays that support it
    int sunShadows = 2;      // r_enhSunShadows: 0 off, 1 low, 2 medium, 3 high (sunShadowQuality)
    bool gridPerPixel = true; // r_enhGridPerPixel: light-grid lattice per model instead of one sample
    int motionDebug = 0;     // r_enhMotionDebug: 1 shows the motion vectors instead of the frame
    float bloomIntensity = 0.06f; // r_enhBloomIntensity
    float exposureBias = 0.0f;    // r_enhExposureBias (EV)
};

// Reads the settings from a cvar lookup that returns "" for an unset cvar. The preset fills every feature; a per-feature cvar that
// is set overrides it.
inline EnhancedSettings enhancedSettings(const std::function<std::string(const char*)>& get) {
    EnhancedSettings s;
    auto num = [&](const char* n, float& out) {
        const std::string v = get(n);
        if (v.empty()) return false;
        out = float(std::atof(v.c_str()));
        return true;
    };
    float f = 0.0f;
    s.enabled = num("r_enhanced", f) && f != 0.0f;
    if (num("r_enhPreset", f)) s.preset = std::clamp(int(f), 0, 2);
    switch (s.preset) {
        case 0: s.sunShadows = 1, s.bloom = true, s.autoExposure = false, s.gridPerPixel = true; break;
        case 2: s.sunShadows = 3, s.bloom = true, s.autoExposure = true, s.gridPerPixel = true; break;
        default: break;
    }
    if (num("r_enhTonemap", f)) s.tonemap = f != 0.0f;
    if (num("r_enhBloom", f)) s.bloom = f != 0.0f;
    if (num("r_enhAutoExposure", f)) s.autoExposure = f != 0.0f;
    if (num("r_enhEdr", f)) s.edr = f != 0.0f;
    if (num("r_enhSunShadows", f)) s.sunShadows = std::clamp(int(f), 0, 3);
    if (num("r_enhGridPerPixel", f)) s.gridPerPixel = f != 0.0f;
    if (num("r_enhMotionDebug", f)) s.motionDebug = std::clamp(int(f), 0, 1);
    if (num("r_enhBloomIntensity", f)) s.bloomIntensity = std::clamp(f, 0.0f, 1.0f);
    if (num("r_enhExposureBias", f)) s.exposureBias = std::clamp(f, -2.0f, 2.0f);
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

} // namespace iw2::game
