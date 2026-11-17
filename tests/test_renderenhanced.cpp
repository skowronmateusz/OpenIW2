// Enhanced rendering mode maths (src/game/renderenhanced.h, docs/render-enhanced.md).
#include "game/renderenhanced.h"
#include <cmath>
#include <cstdio>
#include <map>

using namespace iw2::game;

int main() {
    int bad = 0;
    auto chk = [&](bool c, int line) {
        if (!c) {
            std::printf("line %d failed\n", line);
            ++bad;
        }
    };
    auto near = [](float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; };

    // Settings: off unless r_enhanced is set; the preset fills the features, a set per-feature cvar overrides it.
    {
        std::map<std::string, std::string> cv;
        auto get = [&](const char* n) { const auto it = cv.find(n); return it == cv.end() ? std::string() : it->second; };
        EnhancedSettings s = enhancedSettings(get);
        chk(!s.enabled && s.preset == 1 && s.sunShadows == 2 && s.tonemap && s.bloom && s.autoExposure && s.edr, __LINE__);
        cv["r_enhanced"] = "1";
        cv["r_enhPreset"] = "0";
        s = enhancedSettings(get);
        chk(s.enabled && s.preset == 0 && s.sunShadows == 1 && !s.autoExposure, __LINE__);
        cv["r_enhPreset"] = "2";
        cv["r_enhSunShadows"] = "0";
        cv["r_enhBloom"] = "0";
        s = enhancedSettings(get);
        chk(s.preset == 2 && s.sunShadows == 0 && !s.bloom && s.autoExposure, __LINE__);
        cv["r_enhPreset"] = "9";
        cv["r_enhSunShadows"] = "7";
        cv["r_enhExposureBias"] = "-5";
        s = enhancedSettings(get);
        chk(s.preset == 2 && s.sunShadows == 3 && s.exposureBias == -2.0f, __LINE__);
        cv["r_enhanced"] = "0";
        chk(!enhancedSettings(get).enabled, __LINE__);
    }
    // Shadow quality levels.
    chk(sunShadowQuality(0).resolution == 0, __LINE__);
    chk(sunShadowQuality(1).resolution == 1024 && sunShadowQuality(1).cascades == 1, __LINE__);
    chk(sunShadowQuality(3).cascades == 2 && sunShadowQuality(3).radius[0] < sunShadowQuality(3).radius[1], __LINE__);

    // Tone curve: identity up to the knee, continuous with slope 1 there, monotonic, below white, approaching white.
    for (float x = 0.0f; x <= kToneKnee; x += 0.01f) chk(toneShoulder(x, kToneKnee, 1.0f) == x, __LINE__);
    {
        const float e = 1e-3f;
        const float slope = (toneShoulder(kToneKnee + e, kToneKnee, 1.0f) - toneShoulder(kToneKnee, kToneKnee, 1.0f)) / e;
        chk(near(slope, 1.0f, 5e-3f), __LINE__);
        float prev = 0.0f;
        for (float x = 0.0f; x < 20.0f; x += 0.05f) {
            const float y = toneShoulder(x, kToneKnee, 1.0f);
            chk(y >= prev && y < 1.0f + 1e-6f, __LINE__);
            prev = y;
        }
        chk(toneShoulder(1.0f, kToneKnee, 1.0f) > 0.9f, __LINE__); // a value of 1 stays bright
        chk(toneShoulder(20.0f, kToneKnee, 1.0f) > 0.999f, __LINE__);
        chk(toneShoulder(3.0f, kToneKnee, 2.0f) > toneShoulder(3.0f, kToneKnee, 1.0f), __LINE__); // EDR white keeps more
    }
    // sRGB transfer: round trip, the 0.5 point, extension past 1 is monotonic.
    for (float c = -1.0f; c <= 3.0f; c += 0.05f) chk(near(linearToSrgb(srgbToLinear(c)), c, 2e-4f), __LINE__);
    chk(near(srgbToLinear(0.5f), 0.214041f, 1e-5f), __LINE__);
    chk(srgbToLinear(1.5f) > srgbToLinear(1.0f) && near(srgbToLinear(1.0f), 1.0f, 1e-6f), __LINE__);
    chk(toneWhite(1.0f) == 1.0f && toneWhite(0.5f) == 1.0f && toneWhite(4.0f) > 1.0f && near(srgbToLinear(toneWhite(4.0f)), 4.0f, 1e-3f), __LINE__);

    // Exposure: at the key no change; darker frame brightens (positive EV), brighter darkens; clamped.
    {
        ExposureParams p;
        chk(near(exposureTargetEv(p.keyLog2, p), 0.0f), __LINE__);
        chk(exposureTargetEv(p.keyLog2 - 1.0f, p) > 0.0f && exposureTargetEv(p.keyLog2 + 1.0f, p) < 0.0f, __LINE__);
        chk(exposureTargetEv(-40.0f, p) == p.maxEv && exposureTargetEv(40.0f, p) == p.minEv, __LINE__);
        chk(near(exposureTargetEv(p.keyLog2, p, 0.5f), 0.5f), __LINE__);
        // Adaptation is frame-rate independent: two half steps equal one full step.
        const float one = adaptExposureEv(0.0f, 1.0f, 0.2f, p);
        const float two = adaptExposureEv(adaptExposureEv(0.0f, 1.0f, 0.1f, p), 1.0f, 0.1f, p);
        chk(near(one, two, 1e-5f) && one > 0.0f && one < 1.0f, __LINE__);
        chk(adaptExposureEv(0.0f, -1.0f, 0.2f, p) < -(1.0f - std::exp(-0.2f / p.tauBrighten)), __LINE__); // darkening is faster
        chk(adaptExposureEv(0.3f, 1.0f, 0.0f, p) == 0.3f, __LINE__);
    }
    // Bloom chain levels and threshold weight.
    chk(bloomLevels(1920, 1080) == 7 && bloomLevels(1280, 720) == 6 && bloomLevels(64, 32) == 2 && bloomLevels(16, 16) == 1 && bloomLevels(4096, 4096) == 7, __LINE__);
    chk(bloomWeight(0.1f, 1.0f, 0.5f) == 0.0f && bloomWeight(3.0f, 1.0f, 0.5f) > 0.6f && bloomWeight(0.0f, 1.0f, 0.5f) == 0.0f, __LINE__);
    {
        float prev = 0.0f;
        for (float l = 0.0f; l < 4.0f; l += 0.01f) { // the bloomed amount (weight times luminance) never decreases
            const float v = bloomWeight(l, 1.0f, 0.5f) * l;
            chk(v + 1e-5f >= prev, __LINE__);
            prev = v;
        }
    }

    // Sun shadow fit: the sphere maps inside the map, the sun direction maps to depth, texel snapping keeps texel centres fixed.
    {
        const float sun[3] = {0.3f, 0.2f, 0.93f};
        const float c[3] = {1000.0f, -2000.0f, 50.0f};
        const SunShadowFit fit = fitSunShadow(sun, c, 1000.0f, 3000.0f, 2048);
        chk(near(fit.texelWorld, 2000.0f / 2048.0f), __LINE__);
        const auto cc = mulPoint(fit.viewProj, c);
        chk(std::fabs(cc[0]) < 0.01f && std::fabs(cc[1]) < 0.01f && cc[2] > 0.0f && cc[2] < 1.0f && cc[3] == 1.0f, __LINE__);
        // A point towards the sun is nearer (smaller depth) than the centre.
        const float l = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
        const float up[3] = {c[0] + 500 * sun[0] / l, c[1] + 500 * sun[1] / l, c[2] + 500 * sun[2] / l};
        const auto uc = mulPoint(fit.viewProj, up);
        chk(uc[2] < cc[2] && std::fabs(uc[0] - cc[0]) < 1e-3f && std::fabs(uc[1] - cc[1]) < 1e-3f, __LINE__);
        // Points on the sphere's light-plane rim map within [-1, 1] (allowing one texel of snap).
        for (int k = 0; k < 6; ++k) {
            const float p[3] = {c[0] + (k == 0 ? 990.f : k == 1 ? -990.f : 0.f), c[1] + (k == 2 ? 990.f : k == 3 ? -990.f : 0.f), c[2] + (k == 4 ? 990.f : k == 5 ? -990.f : 0.f)};
            const auto pc = mulPoint(fit.viewProj, p);
            chk(std::fabs(pc[0]) <= 1.0f + 2.0f / 2048 && std::fabs(pc[1]) <= 1.0f + 2.0f / 2048 && pc[2] >= 0.0f && pc[2] <= 1.0f, __LINE__);
        }
        // A world point keeps its texel position (fractional part) when the camera moves by less than a texel or many texels.
        const float probe[3] = {1234.5f, -1987.25f, 40.0f};
        auto frac = [&](const SunShadowFit& f) {
            const auto pc = mulPoint(f.viewProj, probe);
            const float tx = (pc[0] * 0.5f + 0.5f) * 2048.0f, ty = (pc[1] * 0.5f + 0.5f) * 2048.0f;
            return std::array<float, 2>{tx - std::floor(tx), ty - std::floor(ty)};
        };
        const auto f0 = frac(fit);
        for (float d : {0.3f, 7.7f, 133.0f}) {
            const float c2[3] = {c[0] + d, c[1] - d * 0.5f, c[2]};
            const auto f1 = frac(fitSunShadow(sun, c2, 1000.0f, 3000.0f, 2048));
            chk(std::fabs(f1[0] - f0[0]) < 0.02f || std::fabs(std::fabs(f1[0] - f0[0]) - 1.0f) < 0.02f, __LINE__);
            chk(std::fabs(f1[1] - f0[1]) < 0.02f || std::fabs(std::fabs(f1[1] - f0[1]) - 1.0f) < 0.02f, __LINE__);
        }
        // Straight-down sun works (no degenerate basis).
        const float down[3] = {0.0f, 0.0f, 1.0f};
        const auto fd = fitSunShadow(down, c, 500.0f, 100.0f, 1024);
        const auto dc = mulPoint(fd.viewProj, c);
        chk(std::isfinite(dc[0]) && std::fabs(dc[0]) < 0.01f && std::fabs(dc[1]) < 0.01f, __LINE__);
    }
    chk(shadowEdgeFade(0.0f, 100.0f) == 1.0f && shadowEdgeFade(85.0f, 100.0f) == 1.0f && shadowEdgeFade(100.0f, 100.0f) == 0.0f, __LINE__);
    chk(shadowEdgeFade(92.5f, 100.0f) > 0.4f && shadowEdgeFade(92.5f, 100.0f) < 0.6f && shadowEdgeFade(150.0f, 100.0f) == 0.0f, __LINE__);

    // Probe lattice: grid-node aligned; a standing soldier spans 3 x 3 x 3 nodes; a point gets 2 per axis; big boxes stop at 4 points
    // per axis with a coarser stride, and the box always lies inside the lattice.
    {
        const float mn[3] = {-16, -16, 0}, mx[3] = {16, 16, 72};
        const ProbeLattice l = probeLattice(mn, mx);
        chk(l.n[0] == 3 && l.n[1] == 3 && l.n[2] == 3 && l.count() == 27, __LINE__);
        chk(l.origin[0] == -32.0f && l.origin[2] == 0.0f && l.step[2] == 64.0f && l.node0[0] == -1, __LINE__);
        const float p[3] = {5, 5, 5};
        const ProbeLattice z = probeLattice(p, p);
        chk(z.n[0] == 2 && z.n[2] == 2 && z.step[0] == 32.0f && z.origin[0] == 0.0f, __LINE__);
        for (float sz : {40.0f, 100.0f, 400.0f, 1000.0f}) {
            const float bmn[3] = {-7, 3, -50}, bmx[3] = {-7 + sz, 3 + sz, -50 + sz};
            const ProbeLattice b = probeLattice(bmn, bmx);
            for (int a = 0; a < 3; ++a) {
                chk(b.n[a] >= 2 && b.n[a] <= 4, __LINE__);
                chk(b.origin[a] <= bmn[a] && b.origin[a] + b.step[a] * float(b.n[a] - 1) >= bmx[a], __LINE__);
                chk(std::fmod(b.origin[a], kGridCell[a]) == 0.0f && b.step[a] == float(b.stride[a]) * kGridCell[a], __LINE__);
            }
        }
    }

    // Camera motion vectors: a perspective view-projection like the renderer's (w = view depth).
    {
        auto viewProj = [](const float eye[3], float yawDeg) {
            const float y = yawDeg * 3.14159265f / 180.0f;
            const float f[3] = {std::cos(y), std::sin(y), 0.0f}, r[3] = {std::sin(y), -std::cos(y), 0.0f}, u[3] = {0.0f, 0.0f, 1.0f};
            Mat4c view = {r[0], u[0], -f[0], 0, r[1], u[1], -f[1], 0, r[2], u[2], -f[2], 0,
                          -(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2]), -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]),
                          f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2], 1};
            const float t = 1.5f, n = 4.0f; // infinite far plane like game::sceneProjection (clip z = w - n)
            Mat4c proj = {t / 1.777f, 0, 0, 0, 0, t, 0, 0, 0, 0, -1, -1, 0, 0, -n, 0};
            return mulMat(proj, view);
        };
        const float eye[3] = {100, 200, 50};
        const Mat4c vp = viewProj(eye, 30.0f);
        Mat4c inv{};
        chk(invertMat(vp, inv), __LINE__);
        const Mat4c id = mulMat(vp, inv);
        for (int i = 0; i < 16; ++i) chk(near(id[size_t(i)], (i % 5 == 0) ? 1.0f : 0.0f, 1e-4f), __LINE__);
        const float fwd[3] = {std::cos(0.5235988f), std::sin(0.5235988f), 0.0f};
        const auto zl = clipDepthLine(vp, eye, fwd);
        // A world point seen this frame: its NDC and clip z.
        const float wp[3] = {500, 350, 80};
        const auto c = mulPoint(vp, wp);
        chk(near(c[2], zl[0] * c[3] + zl[1], 1e-3f), __LINE__);
        const float nx = c[0] / c[3], ny = c[1] / c[3];
        // Same camera: no motion.
        const auto m0 = cameraMotion(nx, ny, c[2], zl[0], zl[1], inv, vp);
        chk(near(m0[0], 0.0f, 1e-4f) && near(m0[1], 0.0f, 1e-4f), __LINE__);
        // Previous camera: the point's previous NDC is what the motion vector points back to.
        const float eye0[3] = {90, 205, 50};
        const Mat4c prev = viewProj(eye0, 28.0f);
        const auto pc = mulPoint(prev, wp);
        const auto m1 = cameraMotion(nx, ny, c[2], zl[0], zl[1], inv, prev);
        chk(near(m1[0], nx - pc[0] / pc[3], 1e-3f) && near(m1[1], ny - pc[1] / pc[3], 1e-3f), __LINE__);
        chk(std::fabs(m1[0]) > 1e-3f, __LINE__);
        Mat4c sing{};
        chk(!invertMat(sing, inv), __LINE__);
    }

    if (bad) std::printf("%d failures\n", bad);
    else std::printf("ok\n");
    return bad ? 1 : 0;
}
