// Tests for light grid direction weights.

#include "assets/lightgrid.h"

#include <cmath>
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
    if (failures) { std::cerr << failures << " check(s) failed\n"; return 1; }
    std::cout << "test_lightgrid: all checks passed\n";
    return 0;
}
