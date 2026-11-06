// Screen filter chain of the renderer: the Gaussian pass plan behind $blurred_screen and the glow images, and the choice of glow
// layers. Pure functions, implemented from docs/spec/effects.md "Glow and the blurred screen".
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace iw2::game {

// One symmetric tap: the pass samples uv + (x, y) and uv - (x, y), both with weight w. Offsets are in uv of the source.
struct FilterTap {
    float x = 0, y = 0, w = 0;
};
struct FilterPass {
    int count = 0;
    std::array<FilterTap, 8> taps{};
};

constexpr float kFilterMinSigma = 0.32950512f;  // below this a pass adds nothing (gfx 0x1019bf74)
constexpr float kFilter2dMaxSigma = 1.3895605f; // largest sigma of a single 2 x 2 pair pass (gfx 0x1019bf78)
constexpr float kFilter1dMaxSigma = 6.4977503f; // largest sigma of a single 8-tap pass (gfx 0x1019bf70)

// gfx 0x10035140. Half of a sampled Gaussian as bilinear pairs: pair i covers the texels at 2i + o and 2i + 1 + o, o = 0.5 when the
// rounded size ratio is even, else 0 (then the centre texel is shared by both halves, so it counts half). Weights sum to 0.5 per
// half; trailing weights under 0.01 are dropped without renormalising. Returns the tap count; pos is in uv of the source.
inline int filterHalfTaps(int maxTaps, float sigma, int src, int dst, float* pos, float* weight) {
    const float o = (std::lround(float(src) / float(dst)) & 1) ? 0.0f : 0.5f;
    const float k = -0.5f / (sigma * sigma);
    float total = 0;
    for (int i = 0; i < maxTaps; ++i) {
        const float x0 = float(2 * i) + o, x1 = float(2 * i + 1) + o;
        float w0 = std::exp(k * x0 * x0);
        const float w1 = std::exp(k * x1 * x1);
        if (i == 0 && o == 0.0f) w0 *= 0.5f;
        const float w = w0 + w1;
        weight[i] = w;
        pos[i] = w == 0.0f ? (x0 + x1) * 0.5f / float(src) : (x0 * w0 + x1 * w1) / (w * float(src));
        total += w;
    }
    if (total <= 0.001f) {
        weight[0] = 0.5f;
        return 1;
    }
    const float scale = 0.5f / total;
    int count = maxTaps;
    for (int i = maxTaps - 1; i >= 0; --i) {
        weight[i] *= scale;
        if (weight[i] < 0.01f && count == i + 1) count = i;
    }
    return count;
}

// gfx 0x10035480: both axes at once, two pairs each. Always eight taps.
inline FilterPass filterPass2d(float sigma, int srcW, int srcH, int dstW, int dstH) {
    float px[2], wx[2], py[2], wy[2];
    filterHalfTaps(2, sigma, srcW, dstW, px, wx);
    filterHalfTaps(2, sigma, srcH, dstH, py, wy);
    FilterPass p;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            p.taps[size_t(p.count++)] = {-px[i], py[j], wx[i] * wy[j]};
            p.taps[size_t(p.count++)] = {px[i], py[j], wx[i] * wy[j]};
        }
    return p;
}

// gfx 0x10035360: one axis (0 = x, 1 = y), up to eight taps.
inline FilterPass filterPass1d(float sigma, int axis, int dim) {
    float pos[8], w[8];
    FilterPass p;
    p.count = filterHalfTaps(8, sigma, dim, dim, pos, w);
    for (int i = 0; i < p.count; ++i) p.taps[size_t(i)] = {axis == 0 ? pos[i] : 0.0f, axis == 1 ? pos[i] : 0.0f, w[i]};
    return p;
}

// gfx 0x10035640: the passes that blur by (sx, sy) texels. A size change first takes one 2 x 2 pass (sigma in source texels), the
// rest is then in destination texels. Variances add, so a large sigma is worked off pass by pass. At most 16 passes.
inline std::vector<FilterPass> filterPlan(float sx, float sy, int srcW, int srcH, int dstW, int dstH) {
    std::vector<FilterPass> plan;
    if (srcW != dstW || srcH != dstH) {
        const float m = std::min(std::min(sx, sy), kFilter2dMaxSigma);
        plan.push_back(filterPass2d(m, srcW, srcH, dstW, dstH));
        sx = std::sqrt(sx * sx - m * m) * float(dstW) / float(srcW);
        sy = std::sqrt(sy * sy - m * m) * float(dstH) / float(srcH);
    }
    auto take = [](float& s) {
        if (s < kFilter1dMaxSigma) {
            const float t = s;
            s = 0;
            return t;
        }
        s = std::sqrt(s * s - kFilter1dMaxSigma * kFilter1dMaxSigma);
        return kFilter1dMaxSigma;
    };
    while (plan.size() < 16) {
        if (sx < kFilterMinSigma && sy < kFilterMinSigma) break;
        if (std::abs(sx - sy) < kFilterMinSigma) {
            const float avg = (sx + sy) * 0.5f;
            if (avg <= kFilter2dMaxSigma) {
                plan.push_back(filterPass2d(avg, dstW, dstH, dstW, dstH));
                break;
            }
        }
        if (sx > sy) plan.push_back(filterPass1d(take(sx), 0, dstW));
        else plan.push_back(filterPass1d(take(sy), 1, dstH));
    }
    return plan;
}

// Dvar defaults (gfx 0x1000b7b8..0x1000b8c3).
struct GlowDvars {
    float radius[2] = {5.0f, 12.0f};
    float skyBleed[2] = {0.5f, 0.0f};
    float bloom[2] = {1.0f, 1.0f};
    float cutoff = 0.9f, desaturation = 0.75f;
};

// gfx 0x10030a10 / 0x10035bd0. stage[] are the two blur stages in quarter-target texels at 480 lines; the second continues from the
// first, so it only adds the missing variance. Both end in $glow_0. Each image is applied once; `first` is the layer of the first
// application, the next one takes the other layer.
struct GlowChain {
    int images = 0;
    int first = 0;
    float stage[2] = {0, 0};
};
inline GlowChain glowChain(const GlowDvars& d, bool worldHasSky, float targetOverScene) {
    float r[2];
    for (int i = 0; i < 2; ++i) r[i] = d.bloom[i] > 0.0f || (worldHasSky && d.skyBleed[i] > 0.0f) ? d.radius[i] : 0.0f;
    GlowChain c;
    if (r[1] != 0.0f && (r[0] == 0.0f || r[0] > r[1])) {
        std::swap(r[0], r[1]);
        c.first = 1;
    }
    if (r[0] == 0.0f) return c;
    r[0] *= targetOverScene;
    r[1] *= targetOverScene;
    c.stage[0] = r[0];
    c.images = 1;
    if (r[1] != 0.0f) {
        c.stage[1] = std::sqrt(r[1] * r[1] - r[0] * r[0]);
        c.images = 2;
    }
    return c;
}
// gfx 0x10030910: a layer's sky bleed exists only with a world sky; at 0 the bloom-only material draws it.
inline float glowSkyBleed(const GlowDvars& d, int layer, bool worldHasSky) { return worldHasSky ? d.skyBleed[layer] : 0.0f; }

} // namespace iw2::game
