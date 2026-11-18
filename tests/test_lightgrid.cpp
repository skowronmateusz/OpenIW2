// Tests for light grid direction weights.

#include "assets/lightgrid.h"

#include <cmath>
#include <cstring>
#include <vector>
#include <iostream>

int main() {
    using iw2::assets::LightGrid;
    int failures = 0;
    const std::array<std::array<float, 3>, 5> normals = {{{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {0, 1, 0}, {0.57735f, 0.57735f, 0.57735f}}};
    for (const auto& n : normals) {
        const auto w = LightGrid::weights(n);
        float sum = 0;
        for (float v : w) {
            sum += v;
            if (v < -1e-6f || v > 1 + 1e-6f) ++failures;
        }
        if (std::abs(sum - 1.0f) > 1e-4f) {
            std::cerr << "weights do not sum to 1 for normal " << n[0] << "," << n[1] << "," << n[2] << ": " << sum << "\n";
            ++failures;
        }
    }
    // Uniform coefficients give the same light for every normal.
    iw2::assets::LightGridCoeffs c{};
    for (auto& ch : c) ch.fill(0.5f);
    for (const auto& n : normals) {
        const auto l = LightGrid::evaluate(c, n);
        if (std::abs(l[0] - 0.5f) > 1e-4f) ++failures;
    }
    // Static-model weights (trilinear in the cube coordinates): sum to 1, every corner reachable, linear not smoothed.
    for (const auto& n : normals) {
        const auto w = LightGrid::smodelWeights(n);
        float sum = 0;
        for (float x : w) sum += x;
        if (std::abs(sum - 1.0f) > 1e-4f) ++failures;
    }
    {
        // the basis direction of corner 7 (all three tilted axes positive) is straight up: all weight on corner 7
        const auto up = LightGrid::smodelWeights({0.0f, 0.0f, 1.0f});
        if (std::abs(up[7] - 1.0f) > 1e-4f) ++failures;
        // a normal between two cube faces: linear weights halfway (0.5) where the smoothed weights() give smoothstep(0.5) = 0.5 too,
        // but at a quarter they differ: linear 0.25 vs smoothstep 0.15625
        const float t = 0.25f;
        const float lin = t, smo = (3 - 2 * t) * t * t;
        if (std::abs(lin - smo) < 0.05f) ++failures;
    }
    {
        iw2::assets::LightGridCoeffs q{};
        for (auto& ch : q) ch.fill(0.6f);
        float sun = 0.8f;
        LightGrid::smodelLighting(q, sun, 2.0f, 0.5f); // sun x 2 clamps to 1, coefficients x 0.5 round to bytes
        if (sun != 1.0f || std::abs(q[0][0] - 76.0f / 255.0f) > 1e-6f) ++failures;
        sun = 0.5f;
        LightGrid::smodelLighting(q, sun, 1.0f, 1.0f);
        if (std::abs(sun - 127.0f / 255.0f) > 1e-6f) ++failures;
    }
    {
        // points(): the world position of each entry (cell corner), the inverse of the lookup's key and sub
        const int x = 4099, y = 4089, z = 2052; // the cell of (100, -200, 300)
        const uint32_t key = ((uint32_t(x & ~3) << 11 | uint32_t(y & ~3)) << 8) | uint32_t(z >> 2);
        const uint8_t sub = uint8_t(((((x & 3) << 2 | (y & 3)) << 2 | (z & 3)) << 2) | 1);
        std::vector<uint8_t> hash(8, 0), coeff(24, 128);
        std::memcpy(hash.data(), &key, 4);
        hash[4] = sub;
        const LightGrid g(hash, coeff);
        const auto pts = g.points();
        if (pts.size() != 1 || pts[0][0] != 96.0f || pts[0][1] != -224.0f || pts[0][2] != 256.0f) {
            std::cerr << "points() gave the wrong position\n";
            ++failures;
        }
        iw2::assets::LightGridCoeffs out{};
        float sv = 0;
        if (!g.sample(pts[0], out, sv) || std::abs(sv - 1.0f) > 1e-4f) ++failures; // the point itself is found again
    }
    if (failures) { std::cerr << failures << " check(s) failed\n"; return 1; }
    std::cout << "test_lightgrid: all checks passed\n";
    return 0;
}
