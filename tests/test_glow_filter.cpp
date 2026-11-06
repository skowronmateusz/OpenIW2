#include "game/glow_filter.h"
#include "game/screen_blur.h"
#include <cstdio>
using namespace iw2::game;
static int fails = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++fails;
    }
}
static bool near(float a, float b, float e = 1e-4f) { return std::abs(a - b) < e; }
// Variance of the kernel of one pass along an axis, in texels of `dim`.
static float variance(const FilterPass& p, int axis, int dim) {
    float v = 0, sum = 0;
    for (int i = 0; i < p.count; ++i) {
        const float x = (axis == 0 ? p.taps[size_t(i)].x : p.taps[size_t(i)].y) * float(dim);
        v += 2 * p.taps[size_t(i)].w * x * x;
        sum += 2 * p.taps[size_t(i)].w;
    }
    return v / sum;
}
int main() {
    // Same-size taps: the first pair shares the centre texel, weights sum to 0.5 per half.
    float pos[8], w[8];
    const int n = filterHalfTaps(8, 2.0f, 100, 100, pos, w);
    float sum = 0;
    for (int i = 0; i < n; ++i) sum += w[i];
    check(n == 3 && sum > 0.49f && sum <= 0.5001f, "half weights");
    const float e1 = std::exp(-0.125f); // pair 0: texels 0 (half weight) and 1
    check(near(pos[0] * 100.0f, e1 / (0.5f + e1)), "centroid of the first pair");
    // A ratio of 4 is even: pairs are centred between texels.
    filterHalfTaps(2, 1.0f, 400, 100, pos, w);
    const float a = std::exp(-0.125f), b = std::exp(-1.125f);
    check(near(pos[0] * 400.0f, (0.5f * a + 1.5f * b) / (a + b)), "half texel offset");
    // A tiny sigma keeps one tap of the whole weight.
    check(filterHalfTaps(8, 0.2f, 64, 64, pos, w) == 1 && near(w[0], 0.5f, 1e-3f), "narrow kernel");

    // Plans: same size, equal small sigma = one 2 x 2 pass of eight taps.
    auto small = filterPlan(1.0f, 1.0f, 320, 240, 320, 240);
    check(small.size() == 1 && small[0].count == 8, "small sigma");
    // Larger: one pass per axis. The merged taps lose the spread inside each pair, so the variance is a bit under sigma^2.
    auto mid = filterPlan(3.0f, 3.0f, 320, 240, 320, 240);
    check(mid.size() == 2 && near(variance(mid[0], 1, 240), 9.0f, 1.2f) && near(variance(mid[1], 0, 320), 9.0f, 1.2f), "separable");
    // The 8-tap limit: sigma above it is split, variances add.
    auto big = filterPlan(10.0f, 10.0f, 320, 240, 320, 240);
    check(big.size() == 6, "split sigma"); // x, y, y, x, y, x: 10 -> 7.59 -> 3.94 -> 0 on each axis
    // A size change starts with a 2 x 2 pass; a sigma below the limit is used up by it.
    auto down = filterPlan(1.0f, 1.0f, 1280, 960, 320, 240);
    check(down.size() == 1 && down[0].count == 8, "downsample only");
    check(filterPlan(0.1f, 0.1f, 320, 240, 320, 240).empty(), "nothing to do");
    check(filterPlan(1000.0f, 1000.0f, 320, 240, 320, 240).size() == 16, "pass limit");

    // Glow layers with the default dvars.
    GlowDvars d;
    GlowChain c = glowChain(d, true, 0.25f);
    check(c.images == 2 && c.first == 0 && near(c.stage[0], 1.25f) && near(c.stage[1], std::sqrt(9.0f - 1.5625f)), "defaults");
    check(glowChain(d, false, 0.25f).images == 2, "no sky keeps the bloom");
    check(glowSkyBleed(d, 0, true) == 0.5f && glowSkyBleed(d, 0, false) == 0.0f && glowSkyBleed(d, 1, true) == 0.0f, "sky bleed");
    GlowDvars s = d; // radii out of order: the layers swap
    s.radius[0] = 12;
    s.radius[1] = 5;
    c = glowChain(s, true, 1.0f);
    check(c.first == 1 && near(c.stage[0], 5.0f), "sorted");
    GlowDvars o = d; // layer 0 unused
    o.bloom[0] = 0;
    c = glowChain(o, false, 1.0f);
    check(c.images == 1 && c.first == 1 && near(c.stage[0], 12.0f), "one layer");
    check(glowChain(o, true, 1.0f).images == 2, "sky bleed keeps a layer");
    o.bloom[1] = 0;
    check(glowChain(o, false, 1.0f).images == 0, "no layer");

    // $blurred_screen: below the full radius the image still has the full radius, only the alpha ramps.
    check(near(blurFullRadius(480), 3.0f) && near(blurFullRadius(960), 1.5f), "full radius");
    check(near(blurImageRadius(1.0f, 480), 3.0f) && near(blurImageRadius(32.0f, 480), 32.0f), "image radius");
    if (fails == 0) std::printf("ok\n");
    return fails != 0;
}
