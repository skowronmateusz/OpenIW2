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
        chk(s.enabled && s.preset == 0 && s.sunShadows == 1 && !s.autoExposure && !s.probes, __LINE__);
        cv["r_enhProbes"] = "1";
        chk(enhancedSettings(get).probes, __LINE__); // low preset, probes asked for
        cv.erase("r_enhProbes");
        cv["r_enhPreset"] = "2";
        cv["r_enhSunShadows"] = "0";
        cv["r_enhBloom"] = "0";
        s = enhancedSettings(get);
        chk(s.preset == 2 && s.sunShadows == 0 && !s.bloom && s.autoExposure, __LINE__);
        cv["r_enhPreset"] = "9"; // clamped to custom (4), whose unset options are medium's
        cv["r_enhSunShadows"] = "7";
        cv["r_enhExposureBias"] = "-5";
        s = enhancedSettings(get);
        chk(s.preset == 4 && s.sunShadows == 3 && s.worldShadows == 1 && s.exposureBias == -2.0f, __LINE__);
        cv["r_enhanced"] = "0";
        chk(!enhancedSettings(get).enabled, __LINE__);
    }
    // Preset 3 (ultra) is the high preset plus ray-traced global illumination; the other presets have it off, a set cvar overrides.
    {
        std::map<std::string, std::string> cv{{"r_enhanced", "1"}, {"r_enhPreset", "3"}};
        auto get = [&](const char* n) { const auto it = cv.find(n); return it == cv.end() ? std::string() : it->second; };
        EnhancedSettings u = enhancedSettings(get);
        cv["r_enhPreset"] = "2";
        const EnhancedSettings h = enhancedSettings(get);
        chk(u.preset == 3 && u.rtGI && !h.rtGI && h.preset == 2, __LINE__);
        chk(u.sunShadows == h.sunShadows && u.worldShadows == h.worldShadows && u.volumetric == h.volumetric && u.ao == h.ao && u.taa == h.taa &&
                u.rtShadows == h.rtShadows && u.rtReflections == h.rtReflections && u.probes == h.probes, __LINE__); // ultra = high otherwise
        cv["r_enhPreset"] = "3", cv["r_enhRTGI"] = "0";
        chk(!enhancedSettings(get).rtGI, __LINE__);
        cv["r_enhRTGI"] = "-1"; // an old config's "as the preset", not resolved yet
        chk(enhancedSettings(get).rtGI, __LINE__);
        cv["r_enhPreset"] = "1", cv["r_enhRTGI"] = "1";
        chk(enhancedSettings(get).rtGI, __LINE__);
        cv["r_enhPreset"] = "7", cv.erase("r_enhRTGI");
        chk(enhancedSettings(get).preset == 4 && !enhancedSettings(get).rtGI, __LINE__); // clamped to custom: medium where unset
        cv["r_enhPreset"] = "0";
        chk(!enhancedSettings(get).rtGI, __LINE__);
    }
    // An old config's -1 ("as the preset", before docs/render-enhanced.md "Presets") reads as the preset's value.
    {
        std::map<std::string, std::string> cv{{"r_enhanced", "1"}, {"r_enhPreset", "0"}, {"r_enhSunShadows", "-1"}, {"r_enhTAA", "-1"}, {"r_enhAO", "1"}};
        auto get = [&](const char* n) { const auto it = cv.find(n); return it == cv.end() ? std::string() : it->second; };
        const EnhancedSettings s = enhancedSettings(get);
        chk(s.sunShadows == 1 && !s.taa && s.ao, __LINE__);
    }
    // Ray-traced global illumination: the sampling, the hit fade, the history check and the accumulation.
    {
        const float n[3] = {0.0f, 0.0f, 1.0f};
        double sumCos = 0.0, sumCos2 = 0.0;
        bool unit = true, hemi = true;
        const int N = 64;
        for (int i = 0; i < N; ++i)
            for (int j = 0; j < N; ++j) {
                const auto d = cosineHemisphereDir(n, (float(i) + 0.5f) / N, (float(j) + 0.5f) / N);
                unit = unit && near(d[0] * d[0] + d[1] * d[1] + d[2] * d[2], 1.0f, 1e-3f);
                hemi = hemi && d[2] >= 0.0f;
                sumCos += d[2], sumCos2 += d[2] * d[2];
            }
        chk(unit && hemi, __LINE__);
        chk(std::fabs(sumCos / (N * N) - 2.0 / 3.0) < 0.01 && std::fabs(sumCos2 / (N * N) - 0.5) < 0.01, __LINE__); // E[cos] = 2/3, E[cos^2] = 1/2
        const float tilted[3] = {0.6f, 0.0f, 0.8f}; // any normal: still unit and in its hemisphere
        bool ok = true;
        for (int i = 0; i < 16; ++i) {
            const auto d = cosineHemisphereDir(tilted, (float(i) + 0.5f) / 16, 0.37f);
            ok = ok && near(d[0] * d[0] + d[1] * d[1] + d[2] * d[2], 1.0f, 1e-3f) && d[0] * tilted[0] + d[2] * tilted[2] >= -1e-4f;
        }
        const float polar[3] = {0.0f, 0.0f, -1.0f};
        const auto dp = cosineHemisphereDir(polar, 0.3f, 0.5f);
        chk(ok && dp[2] <= 0.0f, __LINE__);
        chk(giHitFade(0.0f, kRtGiRange) == 1.0f && giHitFade(kRtGiRange * 0.5f, kRtGiRange) == 1.0f && giHitFade(kRtGiRange, kRtGiRange) == 0.0f, __LINE__);
        chk(near(giHitFade(kRtGiRange * 0.75f, kRtGiRange), 0.5f, 1e-4f) && giHitFade(kRtGiRange * 2.0f, kRtGiRange) == 0.0f, __LINE__);
        float last = 1.0f;
        bool mono = true;
        for (int i = 0; i <= 20; ++i) {
            const float f = giHitFade(kRtGiRange * float(i) / 20.0f, kRtGiRange);
            mono = mono && f <= last + 1e-6f;
            last = f;
        }
        chk(mono, __LINE__);
        chk(giHistoryAccepted(1000.0f, 1010.0f) && !giHistoryAccepted(1000.0f, 1100.0f) && giHistoryAccepted(10.0f, 14.0f) && !giHistoryAccepted(10.0f, 17.0f), __LINE__);
        chk(near(giAccumulate(1.0f, 0.0f, true, 0.25f), 0.75f) && giAccumulate(1.0f, 0.0f, false, 0.25f) == 0.0f, __LINE__);
        float v = 0.0f; // a constant input converges to it, geometrically
        for (int i = 0; i < 60; ++i) v = giAccumulate(v, 1.0f, true, kRtGiBlend);
        chk(v > 0.999f && giAccumulate(0.0f, 1.0f, true, kRtGiBlend) == kRtGiBlend, __LINE__);
        chk(giEffectiveFrames(0.5f) == 5 && giEffectiveFrames(kRtGiBlend) > 15 && giEffectiveFrames(kRtGiBlend) < 40 && giEffectiveFrames(0.0f) == 0, __LINE__);
        chk(kRtGiRange > 0.0f && kRtGiRadianceCap > 1.0f && kRtGiStrength > 0.0f && kRtGiStrength <= 1.0f && kRtGiBlend > 0.0f && kRtGiBlend < 1.0f, __LINE__);
    }
    // Shadow quality levels.
    chk(sunShadowQuality(0).resolution == 0, __LINE__);
    chk(sunShadowQuality(1).resolution == 1024 && sunShadowQuality(1).cascades == 1, __LINE__);
    chk(sunShadowQuality(3).cascades == 2 && sunShadowQuality(3).radius[0] < sunShadowQuality(3).radius[1], __LINE__);

    // World shadow cascades: quality levels, cached centre snapping, caster reach.
    chk(worldShadowQuality(0).cascades == 0 && worldShadowQuality(1).cascades == 2 && worldShadowQuality(2).cascades == 3, __LINE__);
    chk(worldShadowQuality(2).radius[0] < worldShadowQuality(2).radius[1] && worldShadowQuality(2).radius[1] < worldShadowQuality(2).radius[2], __LINE__);
    {
        const float c0[3] = {1010.0f, -490.0f, 30.0f}, c1[3] = {1040.0f, -470.0f, 30.0f}, c2[3] = {1400.0f, -490.0f, 30.0f};
        const auto a = cascadeCentre(c0, 1000.0f), b = cascadeCentre(c1, 1000.0f), d = cascadeCentre(c2, 1000.0f);
        chk(a == b && a != d && a[0] == 1000.0f && a[1] == -500.0f, __LINE__); // small moves keep the cascade, a quarter radius moves it
        const float dx = c0[0] - a[0], dy = c0[1] - a[1];
        chk(std::sqrt(dx * dx + dy * dy) <= 1000.0f * 0.25f * 0.71f, __LINE__); // the camera stays near the centre
        const float up[3] = {0.0f, 0.0f, 1.0f}, ctr[3] = {0, 0, 100}, mn[3] = {-500, -500, -200}, mx[3] = {500, 500, 3000};
        chk(casterReachForBounds(up, ctr, mn, mx, 100.0f) == 2900.0f && casterReachForBounds(up, ctr, mn, mx, 5000.0f) == 5000.0f, __LINE__);
    }
    {
        std::map<std::string, std::string> cv{{"r_enhanced", "1"}};
        auto get = [&](const char* n) { const auto it = cv.find(n); return it == cv.end() ? std::string() : it->second; };
        chk(enhancedSettings(get).worldShadows == 1, __LINE__);
        cv["r_enhPreset"] = "0";
        chk(enhancedSettings(get).worldShadows == 0, __LINE__);
        cv["r_enhPreset"] = "2";
        chk(enhancedSettings(get).worldShadows == 2, __LINE__);
        cv["r_enhWorldShadows"] = "0";
        chk(enhancedSettings(get).worldShadows == 0, __LINE__);
    }

    // GTAO slice integral: an open slice gives cos n + n sin n (1 for a normal along the view vector; the slices projected-normal weights
    // make the sum over slices 1), a slice closed at the view vector 0, horizons past the hemisphere clamp to it, monotonic in each horizon.
    for (float n : {-1.0f, -0.4f, 0.0f, 0.3f, 1.1f}) {
        chk(near(gtaoSliceVisibility(n, n - 1.5707963f, n + 1.5707963f), std::cos(n) + n * std::sin(n), 1e-4f), __LINE__);
        chk(near(gtaoSliceVisibility(n, 0.0f, 0.0f), 0.0f, 1e-4f), __LINE__); // horizons are h0 <= 0 <= h1 (view-vector relative)
        chk(near(gtaoSliceVisibility(n, -9.0f, 9.0f), gtaoSliceVisibility(n, n - 1.5707963f, n + 1.5707963f), 1e-5f), __LINE__);
    }
    chk(near(gtaoSliceVisibility(0.0f, -1.5707963f, 1.5707963f), 1.0f, 1e-5f), __LINE__);
    chk(near(gtaoSliceVisibility(0.0f, -1.5707963f, 0.0f), 0.5f, 1e-4f), __LINE__);
    {
        float prev = -1.0f;
        for (float h1 = 0.0f; h1 <= 1.5707963f; h1 += 0.05f) {
            const float v = gtaoSliceVisibility(0.0f, -1.5707963f, h1);
            chk(v >= prev - 1e-6f, __LINE__);
            prev = v;
        }
    }
    {
        std::map<std::string, std::string> cv{{"r_enhanced", "1"}, {"r_enhPreset", "0"}};
        auto get = [&](const char* n) { const auto it = cv.find(n); return it == cv.end() ? std::string() : it->second; };
        chk(!enhancedSettings(get).ao && !enhancedSettings(get).contactShadows, __LINE__);
        cv["r_enhPreset"] = "1";
        cv["r_enhAOStrength"] = "3";
        chk(enhancedSettings(get).ao && enhancedSettings(get).contactShadows && enhancedSettings(get).aoStrength == 1.0f, __LINE__);
    }

    // TAA jitter: Halton values, within half a pixel, distinct over the 8-frame cycle, zero mean-ish.
    chk(near(halton(1, 2), 0.5f) && near(halton(2, 2), 0.25f) && near(halton(3, 2), 0.75f) && near(halton(1, 3), 1.0f / 3.0f), __LINE__);
    {
        float sx = 0, sy = 0;
        for (uint32_t f = 0; f < 8; ++f) {
            const auto j = taaJitter(f, 1920, 1080);
            chk(std::fabs(j[0]) <= 1.0f / 1920.0f + 1e-7f && std::fabs(j[1]) <= 1.0f / 1080.0f + 1e-7f, __LINE__);
            for (uint32_t g = 0; g < f; ++g) chk(taaJitter(g, 1920, 1080) != j, __LINE__);
            sx += j[0] * 1920.0f, sy += j[1] * 1080.0f;
        }
        chk(std::fabs(sx / 8.0f) < 0.15f && std::fabs(sy / 8.0f) < 0.15f, __LINE__);
        chk(taaJitter(3, 100, 100) == taaJitter(11, 100, 100), __LINE__);
    }

    // Phong exponent and GGX roughness of a specular map alpha: alpha 0 is broad (45 degree half angle at 0.1, P about 6.6), alpha 1
    // tight; roughness falls monotonically and stays within its clamp.
    chk(near(phongPowerOfSpecAlpha(0.0f), std::log(0.1f) / std::log(std::cos(45.0f * 3.14159265f / 180.0f)), 1e-3f), __LINE__);
    {
        float prevP = 0.0f, prevA = 2.0f;
        for (float a = 0.0f; a <= 1.0f; a += 0.05f) {
            const float p = phongPowerOfSpecAlpha(a), r = ggxAlphaOfPower(p);
            chk(p >= prevP && r <= prevA && r >= 0.03f && r <= 1.0f, __LINE__);
            prevP = p, prevA = r;
        }
        chk(ggxAlphaOfPower(phongPowerOfSpecAlpha(0.0f)) > 0.45f && ggxAlphaOfPower(phongPowerOfSpecAlpha(1.0f)) == 0.03f, __LINE__);
        chk(near(ggxAlphaOfPower(98.0f), std::sqrt(0.02f), 1e-6f), __LINE__);
    }

    // Point light shadow faces: orthonormal right-handed bases, the face of a vector looks along it, depth 0 at near and 1 at far.
    for (int f = 0; f < 6; ++f) {
        const CubeFace c = cubeFace(f);
        auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        chk(near(dot3(c.fwd, c.fwd), 1.0f) && near(dot3(c.right, c.right), 1.0f) && near(dot3(c.up, c.up), 1.0f), __LINE__);
        chk(near(dot3(c.fwd, c.right), 0.0f) && near(dot3(c.fwd, c.up), 0.0f) && near(dot3(c.right, c.up), 0.0f), __LINE__);
        chk(cubeFaceOf(c.fwd) == f, __LINE__);
    }
    {
        const float v1[3] = {0.2f, -3.0f, 1.0f}, v2[3] = {0.0f, 0.1f, -0.5f};
        chk(cubeFaceOf(v1) == 3 && cubeFaceOf(v2) == 5, __LINE__);
        chk(near(cubeFaceDepth(2.0f, 2.0f, 500.0f), 0.0f, 1e-6f) && near(cubeFaceDepth(500.0f, 2.0f, 500.0f), 1.0f, 1e-5f), __LINE__);
        chk(cubeFaceDepth(10.0f, 2.0f, 500.0f) < cubeFaceDepth(20.0f, 2.0f, 500.0f), __LINE__);
    }

    // Atmosphere: the phase function integrates to 1 over the sphere and peaks towards the sun for g > 0; the film grade keeps black,
    // white and grey, and adds contrast around grey.
    {
        for (float g : {0.0f, 0.4f, 0.8f}) {
            double sum = 0.0;
            const int n = 4000;
            for (int i = 0; i < n; ++i) {
                const float ct = -1.0f + 2.0f * (float(i) + 0.5f) / float(n);
                sum += double(henyeyGreenstein(ct, g)) * 2.0 * 3.14159265 * (2.0 / n);
            }
            chk(std::fabs(sum - 1.0) < 2e-3, __LINE__);
        }
        chk(henyeyGreenstein(1.0f, 0.5f) > henyeyGreenstein(0.0f, 0.5f) && near(henyeyGreenstein(0.3f, 0.0f), 1.0f / (4.0f * 3.14159265f)), __LINE__);
        const auto k0 = periodFilmGrade({0, 0, 0}), k1 = periodFilmGrade({1, 1, 1}), kg = periodFilmGrade({0.5f, 0.5f, 0.5f});
        chk(near(k1[0], 1.0f) && near(k1[1], 1.0f) && near(k1[2], 1.0f) && near(kg[1], 0.5f), __LINE__);
        chk(near(k0[1], 0.0f) && k0[0] > 0.0f, __LINE__); // warm lift in black
        chk(periodFilmGrade({0.7f, 0.7f, 0.7f})[1] > 0.7f && periodFilmGrade({0.3f, 0.3f, 0.3f})[1] < 0.3f, __LINE__); // contrast
        const auto sat = periodFilmGrade({0.9f, 0.3f, 0.3f});
        chk(sat[0] - sat[1] < 0.6f, __LINE__); // desaturated
        chk(periodFilmGrade({1.5f, 1.5f, 1.5f})[1] > 1.4f, __LINE__); // HDR values kept
    }

    // Map light falloff: 1 at the light, 0 at the radius, the spot cone cuts behind and fades between the angles.
    {
        MapLight l;
        l.radius = 100.0f;
        const float at[3] = {0, 0, 0}, half[3] = {50, 0, 0}, out[3] = {150, 0, 0};
        chk(near(mapLightFalloff(l, at), 1.0f) && near(mapLightFalloff(l, half), 0.25f) && mapLightFalloff(l, out) == 0.0f, __LINE__);
        l.dir[0] = 0, l.dir[1] = 0, l.dir[2] = -1; // pointing down
        l.cosOuter = std::cos(60.0f * 3.14159265f / 180.0f), l.cosInner = std::cos(30.0f * 3.14159265f / 180.0f);
        const float below[3] = {0, 0, -50}, side[3] = {50, 0, -5}, above[3] = {0, 0, 50};
        chk(near(mapLightFalloff(l, below), 0.25f) && mapLightFalloff(l, side) == 0.0f && mapLightFalloff(l, above) == 0.0f, __LINE__);
    }

    // Baked-light shadows: local lamps near the camera only.
    {
        MapLight l;
        l.radius = 300.0f;
        const float cam[3] = {0, 0, 0};
        l.pos[0] = 800.0f; chk(bakedShadowLightNearCamera(l, cam), __LINE__);   // reaches within 500
        l.pos[0] = 1000.0f; chk(!bakedShadowLightNearCamera(l, cam), __LINE__); // reaches only within 700
        l.radius = 10000.0f, l.pos[0] = 200.0f; chk(!bakedShadowLightNearCamera(l, cam), __LINE__); // a fill light
        l.radius = 1500.0f, l.pos[0] = 1500.0f; chk(bakedShadowLightNearCamera(l, cam), __LINE__);
        l.pos[0] = 1600.0f; chk(!bakedShadowLightNearCamera(l, cam), __LINE__);  // origin too far
    }

    // Aim depth of field: sharp near, full radius far, scaled by the ADS fraction and the strength.
    chk(aimDofRadius(20.0f, 1.0f, 1.0f) == 0.0f && near(aimDofRadius(1000.0f, 1.0f, 1.0f), 2.5f) && near(aimDofRadius(240.0f, 1.0f, 1.0f), 1.25f), __LINE__);
    chk(aimDofRadius(1000.0f, 0.0f, 1.0f) == 0.0f && near(aimDofRadius(1000.0f, 0.5f, 2.0f), 2.5f), __LINE__);

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

    // Reflection probes: one per cell first (cells with more points first), then farthest-point sampling.
    {
        std::vector<std::array<float, 3>> pts;
        std::vector<int> cell;
        for (int i = 0; i < 10; ++i) pts.push_back({float(i) * 10.0f, 0, 0}), cell.push_back(0);  // cell 0: x 0..90
        for (int i = 0; i < 3; ++i) pts.push_back({1000.0f + float(i) * 10.0f, 0, 0}), cell.push_back(1); // cell 1: x 1000..1020
        pts.push_back({5000, 0, 0}), cell.push_back(-1); // outside
        const auto one = placeProbes(pts, cell, 1);
        chk(one.size() == 1 && (one[0] == 4 || one[0] == 5), __LINE__); // the biggest cell, nearest its mean (x 45)
        const auto three = placeProbes(pts, cell, 3);
        chk(three.size() == 3 && three[1] == 11 && three[2] == 13, __LINE__); // cell 1's middle, then the farthest point
        chk(placeProbes(pts, cell, 100).size() == pts.size(), __LINE__);         // never more probes than points
        chk(placeProbes({}, {}, 4).empty(), __LINE__);
    }
    // Points near geometry: a small box keeps the points around it, a huge (sky) box is ignored.
    {
        const std::vector<std::array<float, 6>> boxes = {{0, 0, 0, 100, 100, 100}, {-50000, -50000, -50000, 50000, 50000, 50000}};
        const std::vector<std::array<float, 3>> pts = {{50, 50, 50}, {50, 50, 2000}, {-3000, 0, 0}, {150, 50, 50}};
        const auto near = pointsNearBoxes(pts, boxes, 64.0f, 4096.0f);
        chk(near.size() == 2 && near[0][2] == 50.0f && near[1][0] == 150.0f, __LINE__); // the air above and far away is dropped
        chk(pointsNearBoxes(pts, {{-50000, -50000, -50000, 50000, 50000, 50000}}, 64.0f, 4096.0f).empty(), __LINE__);
    }
    // Probe index grid: bounds, cell growth, assignment by portal cell.
    {
        const float mn[3] = {0, 0, 0}, mx[3] = {1000, 500, 100};
        const ProbeGridInfo g = probeGridInfo(mn, mx, 100.0f, 1000);
        chk(g.dims[0] == 10 && g.dims[1] == 5 && g.dims[2] == 1 && g.cell == 100.0f, __LINE__);
        const ProbeGridInfo g2 = probeGridInfo(mn, mx, 100.0f, 20);
        chk(g2.count() <= 20 && g2.cell > 100.0f, __LINE__);
        // Two rooms split at x = 500 (cells 0, 1), outside beyond x = 900 (-1); one probe in each.
        const std::vector<std::array<float, 3>> probes = {{450, 250, 50}, {880, 250, 50}, {960, 250, 50}};
        const std::vector<int> pcell = {0, 1, -1};
        auto cellOf = [](const float p[3]) { return p[0] >= 900.0f ? -1 : p[0] >= 500.0f ? 1 : 0; };
        bool adjacentCalled = false;
        auto adj = [&](int a, int b) { adjacentCalled = true; return (a == 0 && b == 1) || (a == 1 && b == 0); };
        const auto idx = assignProbes(g, probes, pcell, cellOf, adj);
        chk(idx.size() == g.count(), __LINE__);
        chk(idx[g.index(5, 2, 0)] == 2, __LINE__); // x 550 (room 1): room 1's probe though room 0's is nearer
        chk(idx[g.index(4, 2, 0)] == 1, __LINE__); // x 450 (room 0)
        chk(idx[g.index(9, 2, 0)] == 3, __LINE__); // x 950 (outside): the outside probe
        chk(adjacentCalled, __LINE__);
        // A cell with no probe of its own takes one of a neighbouring cell before a nearer unconnected one.
        const std::vector<std::array<float, 3>> p2 = {{100, 250, 50}, {560, 250, 50}};
        const std::vector<int> c2 = {0, 2};
        auto cellOf2 = [](const float p[3]) { return p[0] >= 500.0f ? 1 : 0; };
        const auto idx2 = assignProbes(g, p2, c2, cellOf2, adj); // room 1 has no probe; room 0 is next to it, room 2 is not
        chk(idx2[g.index(5, 2, 0)] == 1, __LINE__);
        chk(assignProbes(g, {}, {}, cellOf, adj)[0] == 0, __LINE__); // no probes: the sky
    }

    // MetalFX conventions: a projection jitter of one pixel right and one pixel up (NDC 2 / W, 2 / H) is (+1, -1) pixels; a point
    // that moved right on screen since the last frame has a motion pointing left (to where it was).
    {
        const auto j = metalFxJitterPixels(2.0f / 1280.0f, 2.0f / 720.0f, 1280, 720);
        chk(near(j[0], 1.0f) && near(j[1], -1.0f), __LINE__);
        const auto ms = metalFxMotionScale(1280, 720);
        const float ndcMotionX = 20.0f / 1280.0f; // current - previous: 10 pixels right
        chk(near(ndcMotionX * ms[0], -10.0f) && near(0.1f * ms[1], 36.0f), __LINE__);
    }
    // Render scale: only with MetalFX, clamped to 0.5..1; sizes rounded.
    chk(effectiveRenderScale(0.67f, false) == 1.0f && effectiveRenderScale(0.67f, true) == 0.67f && effectiveRenderScale(0.2f, true) == 0.5f, __LINE__);
    chk(renderSize(1920, 1080, 0.67f)[0] == 1286 && renderSize(1920, 1080, 0.67f)[1] == 724 && renderSize(1, 1, 0.5f)[0] == 1, __LINE__);
    chk(renderSize(1920, 1080, 1.0f)[0] == 1920, __LINE__);
    chk(taaJitterPhases(1.0f) == 8 && taaJitterPhases(0.5f) == 32 && taaJitterPhases(0.67f) == 18, __LINE__);
    chk(taaJitter(8, 100, 100) == taaJitter(0, 100, 100) && taaJitter(8, 100, 100, 18) != taaJitter(0, 100, 100, 18), __LINE__);

    // Objects behind the camera join the ray tracing within the range only.
    chk(withinRtBeyondViewRange(1000, 1000, 0) && !withinRtBeyondViewRange(1500, 100, 0) && withinRtBeyondViewRange(0, 0, -1500), __LINE__);

    // Ray tracing only with hardware intersection (Apple family 9), never when forced off.
    chk(metalRayTracingAllowed(true, true, false) && !metalRayTracingAllowed(true, false, false) &&
            !metalRayTracingAllowed(false, true, false) && !metalRayTracingAllowed(true, true, true), __LINE__);

    // MetalFX holds its last output during a single-player pause only.
    chk(metalFxHold(true, false) && !metalFxHold(false, false) && !metalFxHold(true, true), __LINE__);
    // Shadow-only player body: on in every preset, off with its cvar.
    {
        auto get = [](const std::map<std::string, std::string>& m) {
            return [m](const char* n) { const auto it = m.find(n); return it == m.end() ? std::string() : it->second; };
        };
        for (const char* preset : {"0", "1", "2"})
            chk(enhancedSettings(get({{"r_enhanced", "1"}, {"r_enhPreset", preset}})).playerShadow, __LINE__);
        chk(!enhancedSettings(get({{"r_enhanced", "1"}, {"r_enhPlayerShadow", "0"}})).playerShadow, __LINE__);
        chk(enhancedSettings(get({{"r_enhanced", "1"}, {"r_enhPlayerShadow", "-1"}})).playerShadow, __LINE__);
    }
    // Its models: the multiplayer character with the same view hands.
    chk(playerBodyForHands("xmodel/viewmodel_hands_british_bare").body == "xmodel/playerbody_british_africa01", __LINE__);
    chk(playerBodyForHands("xmodel/viewmodel_hands_british").body == "xmodel/playerbody_british_normandy01", __LINE__);
    chk(playerBodyForHands("xmodel/viewmodel_hands_british_wet").hat == "xmodel/helmet_british_normandy", __LINE__);
    chk(playerBodyForHands("xmodel/viewmodel_hands_russian").head == "xmodel/head_russian_winter_jon", __LINE__);
    chk(playerBodyForHands("xmodel/viewmodel_hands_german_winter").body == "xmodel/playerbody_german_winterlight", __LINE__);
    chk(playerBodyForHands("xmodel/viewmodel_hands_cloth").body == "xmodel/playerbody_american_normandy01", __LINE__);
    chk(playerBodyForHands("").body == "xmodel/playerbody_american_normandy01", __LINE__);
    // Single player: the friendly squad's uniform.
    {
        PlayerBodyModels m;
        chk(playerBodyForSquadModel("xmodel/character_british_afrca_body", m) && m.body == "xmodel/playerbody_british_africa01", __LINE__);
        chk(playerBodyForSquadModel("xmodel/character_british_normandy_mac_body", m) && m.body == "xmodel/playerbody_british_normandy01", __LINE__);
        chk(playerBodyForSquadModel("xmodel/character_russian_winter_body", m) && m.body == "xmodel/playerbody_russian_coat01", __LINE__);
        chk(playerBodyForSquadModel("xmodel/character_us_ranger_body", m) && m.body == "xmodel/playerbody_american_normandy01", __LINE__);
        chk(!playerBodyForSquadModel("xmodel/character_german_afrca_body", m) && !playerBodyForSquadModel("", m), __LINE__);
    }
    // Its anim type: the multiplayer weapon file's, else the weapon class.
    chk(playerBodyAnimType("Thompson", "smg") == "thompson" && playerBodyAnimType("none", "Rifle") == "rifle", __LINE__);
    chk(playerBodyAnimType("", "") == "none", __LINE__);
    // Its move signs at the view yaw: forward, back-pedal, strafe right (yaw 0: forward +x, right -y), standing below 10 u/s.
    {
        float f = 0, r = 0;
        playerBodyMoves(200, 0, 0, f, r);
        chk(f == 1 && r == 0, __LINE__);
        playerBodyMoves(-200, 0, 0, f, r);
        chk(f == -1 && r == 0, __LINE__);
        playerBodyMoves(0, -200, 0, f, r);
        chk(f == 0 && r == 1, __LINE__);
        playerBodyMoves(0, 200, 90, f, r); // yaw 90: forward is +y
        chk(f == 1 && r == 0, __LINE__);
        playerBodyMoves(140, -140, 0, f, r); // diagonal: both
        chk(f == 1 && r == 1, __LINE__);
        playerBodyMoves(5, 5, 0, f, r);
        chk(f == 0 && r == 0, __LINE__);
    }

    // Water's moving-object sun shadow: none without sun; ray traced when those shadows are on (they replace the map); else the map.
    chk(waterSunShadowSource(false, true, 2) == WaterSunShadow::None && waterSunShadowSource(false, false, 2) == WaterSunShadow::None, __LINE__);
    chk(waterSunShadowSource(true, true, 0) == WaterSunShadow::RayTraced && waterSunShadowSource(true, true, 2) == WaterSunShadow::RayTraced, __LINE__);
    chk(waterSunShadowSource(true, false, 2) == WaterSunShadow::ShadowMap && waterSunShadowSource(true, false, 1) == WaterSunShadow::ShadowMap, __LINE__);
    chk(waterSunShadowSource(true, false, 0) == WaterSunShadow::None, __LINE__);
    chk(kWaterShadowDarken > 0.0f && kWaterShadowDarken < 1.0f, __LINE__);

    // The player's shadow body in shadow rays: no weight at the eye, full from 40 units, monotonic and smooth between.
    chk(playerBodyShadowWeight(0.0f) == 0.0f && playerBodyShadowWeight(kPlayerBodyShadowFadeNear) == 0.0f, __LINE__);
    chk(playerBodyShadowWeight(kPlayerBodyShadowFadeFar) == 1.0f && playerBodyShadowWeight(500.0f) == 1.0f, __LINE__);
    {
        float prev = 0.0f;
        bool mono = true;
        for (float d = 20.0f; d <= 45.0f; d += 0.5f) {
            const float wgt = playerBodyShadowWeight(d);
            mono = mono && wgt >= prev && wgt - prev < 0.12f; // no jump: a step per half unit stays small
            prev = wgt;
        }
        chk(mono, __LINE__);
        chk(std::fabs(playerBodyShadowWeight(32.0f) - 0.5f) < 1e-4f, __LINE__);
    }
    // Non-finite guard: garbage cells (NaN, infinity, negative, huge) count 0, real counts add up; any pixel resets.
    {
        const float cells[6] = {2.0f, std::nanf(""), INFINITY, -3.0f, 1e9f, 5.0f};
        chk(nanGuardSum(cells, 6) == 7.0f && nanGuardResets(1.0f) && !nanGuardResets(0.0f), __LINE__);
    }
    // Blended surfaces and the moving shadows: opaque takes the screen terms, alpha / additive blends on the opaque surface, multiply never.
    chk(blendScreenTerms(false, false) == ScreenTerms::Take && blendScreenTerms(true, false) == ScreenTerms::OnOpaque, __LINE__);
    chk(blendScreenTerms(true, true) == ScreenTerms::Never && int(ScreenTerms::OnOpaque) == 2 && int(ScreenTerms::Never) == 1, __LINE__);
    // Moving shadows in the temporal filter: no change keeps the usual 90 % history, a full change falls to the low weight,
    // monotonic in between.
    {
        chk(taaShadowHistoryWeight(0.0f) == 0.9f && taaShadowHistoryWeight(kTaaShadowChangeStart) == 0.9f, __LINE__);
        chk(std::fabs(taaShadowHistoryWeight(kTaaShadowChangeFull) - kTaaShadowHistoryWeight) < 1e-6f, __LINE__);
        chk(std::fabs(taaShadowHistoryWeight(1.0f) - kTaaShadowHistoryWeight) < 1e-6f, __LINE__);
        chk(kTaaShadowHistoryWeight < 0.9f && kTaaShadowChangeStart < kTaaShadowChangeFull, __LINE__);
        bool mono = true;
        for (float c = 0.0f; c < 0.5f; c += 0.01f) mono = mono && taaShadowHistoryWeight(c + 0.01f) <= taaShadowHistoryWeight(c);
        chk(mono, __LINE__);
    }
    // GPU errors: the driver's reason in Metal's text decides; without one the Metal code (2 timeout, 4 access revoked).
    {
        const char* pre = "Execution of the command buffer was aborted due to an error during execution. ";
        auto kind = [&](const char* reason, int code) { return gpuErrorKind(code, std::string(pre) + reason); };
        chk(kind("Caused GPU Hang Error (00000003:kIOGPUCommandBufferCallbackErrorHang)", 1) == GpuErrorKind::Stuck, __LINE__);
        chk(kind("Caused GPU Timeout Error (00000002:kIOGPUCommandBufferCallbackErrorTimeout)", 2) == GpuErrorKind::Stuck, __LINE__);
        chk(kind("Impacting Interactivity (0000000e:kIOGPUCommandBufferCallbackErrorImpactingInteractivity)", 1) == GpuErrorKind::Stuck, __LINE__);
        chk(kind("Ignored (for causing prior/excessive GPU errors) (00000004:kIOGPUCommandBufferCallbackErrorSubmissionsIgnored)", 4) == GpuErrorKind::Ignored, __LINE__);
        // a victim of another buffer's error is neither, whatever its Metal code
        chk(kind("Discarded (victim of GPU error/recovery) (00000005:kIOGPUCommandBufferCallbackErrorInnocentVictim)", 2) == GpuErrorKind::Other, __LINE__);
        chk(gpuErrorKind(2, "timed out") == GpuErrorKind::Stuck && gpuErrorKind(4, "") == GpuErrorKind::Ignored, __LINE__);
        chk(gpuErrorKind(8, "out of memory") == GpuErrorKind::Other && gpuErrorKind(0, "") == GpuErrorKind::Other, __LINE__);
        chk(!gpuTimeIsSlow(16.0) && !gpuTimeIsSlow(kGpuSlowMs) && gpuTimeIsSlow(kGpuSlowMs + 1.0), __LINE__);
    }
    // The GPU guard: an error holds the native Metal work, the errors that follow during the hold extend it and are no new
    // strike, the hold ends, and the second strike holds for good.
    {
        GpuGuard g;
        chk(gpuGuardUpdate(g, 0, 1000) == GpuGuardEvent::None && !g.holding(1000), __LINE__);
        chk(gpuGuardUpdate(g, 1, 2000) == GpuGuardEvent::Held && g.holding(2000) && g.strikes == 1, __LINE__);
        chk(gpuGuardUpdate(g, 40, 5000) == GpuGuardEvent::None && g.strikes == 1, __LINE__); // the flood behind the first error
        chk(g.holding(5000 + kGpuGuardHoldMs - 1) && !g.holding(5000 + kGpuGuardHoldMs), __LINE__); // ... and the hold starts again
        chk(gpuGuardUpdate(g, 40, 5000 + kGpuGuardHoldMs) == GpuGuardEvent::Released, __LINE__);
        chk(gpuGuardUpdate(g, 40, 5001 + kGpuGuardHoldMs) == GpuGuardEvent::None && !g.holding(5001 + kGpuGuardHoldMs), __LINE__);
        chk(gpuGuardUpdate(g, 41, 900000) == GpuGuardEvent::HeldForGood && g.holding(900000) && g.holding(~0ull), __LINE__);
        chk(gpuGuardUpdate(g, 99, 990000) == GpuGuardEvent::None && g.holding(990000) && g.strikes == kGpuGuardMaxStrikes, __LINE__);
        GpuGuard u; // a shorter hold (IW2_GPU_GUARD_MS)
        chk(gpuGuardUpdate(u, 1, 100, 30) == GpuGuardEvent::Held && u.holding(129) && gpuGuardUpdate(u, 1, 130, 30) == GpuGuardEvent::Released, __LINE__);
    }
    // An object in the ray tracing scene: a finite box and transform of a size a model can have; anything else is left out.
    {
        const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -6717, -11090, 176, 1};
        const float lo[3] = {-155, -106, -9}, hi[3] = {172, 85, 141}; // a destroyed tank
        chk(rtItemSane(lo, hi, id), __LINE__);
        const float nanHi[3] = {172, std::nanf(""), 141}, infLo[3] = {-INFINITY, -106, -9}, hugeHi[3] = {172, 85, 2.0e9f}, wideHi[3] = {kRtItemMaxExtent, 85, 141};
        chk(!rtItemSane(lo, nanHi, id) && !rtItemSane(infLo, hi, id) && !rtItemSane(lo, hugeHi, id) && !rtItemSane(lo, wideHi, id), __LINE__);
        chk(!rtItemSane(hi, lo, id), __LINE__); // an empty box (no vertex gave a bound)
        float bad1[16], bad2[16];
        std::copy(id, id + 16, bad1), std::copy(id, id + 16, bad2);
        bad1[12] = std::nanf(""), bad2[5] = 3.0e7f;
        chk(!rtItemSane(lo, hi, bad1) && !rtItemSane(lo, hi, bad2), __LINE__);
    }
    // IW2_OFFSCREEN: WxH, nothing else.
    {
        const OffscreenSize a = parseOffscreenSize("3024x1708");
        chk(a.on && a.w == 3024 && a.h == 1708, __LINE__);
        chk(!parseOffscreenSize(nullptr).on && !parseOffscreenSize("").on && !parseOffscreenSize("1").on && !parseOffscreenSize("640").on, __LINE__);
        chk(!parseOffscreenSize("640x").on && !parseOffscreenSize("640x480x2").on && !parseOffscreenSize("8x8").on && !parseOffscreenSize("640,480").on, __LINE__);
        chk(!parseOffscreenSize("99999x480").on, __LINE__);
    }
    if (bad) std::printf("%d failures\n", bad);
    else std::printf("ok\n");
    return bad ? 1 : 0;
}
