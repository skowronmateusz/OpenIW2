// Screen brightness: the r_gamma ramp (docs/spec/ui.md "Brightness"). The original loads it into the video hardware; iw2view applies the
// same curve in a final full-screen pass.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace iw2::game {

constexpr float kGammaMin = 0.5f, kGammaMax = 3.0f, kGammaDefault = 1.0f; // r_gamma registration, gfx 0x1000ab8a

inline float clampGamma(float g) { return g < kGammaMin ? kGammaMin : (g > kGammaMax ? kGammaMax : g); }

// 256-entry ramp of 16-bit values (gfx 0x10011fd0): e = 1 / r_gamma; with e == 1 the entry is i * 257, otherwise
// round(65535 * pow(i / 255, e)). The same table goes to the red, green and blue channels (0x10030c20).
inline std::array<uint16_t, 256> gammaRamp(float gamma) {
    std::array<uint16_t, 256> r{};
    const float e = 1.0f / gamma;
    for (int i = 0; i < 256; ++i)
        r[size_t(i)] = e == 1.0f ? uint16_t(i * 257) : uint16_t(std::lround(65535.0f * std::pow(float(i) / 255.0f, e)));
    return r;
}

// What a display does with one 8-bit channel value: the ramp entry scaled back to 0..1.
inline float gammaApply(float v01, float gamma) {
    const int i = int(std::lround(std::fmin(std::fmax(v01, 0.0f), 1.0f) * 255.0f));
    return float(gammaRamp(gamma)[size_t(i)]) / 65535.0f;
}

} // namespace iw2::game
