// Focused value items (bind 0x4cdee0, edit field 0x4cd0e0): the colour pulses between the menu's focus colour and a low colour,
// focus * 0.8, or (0.8, 0, 0, 0.8) on a bind waiting for its key. docs/spec/ui.md "Focus pulse".
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace iw2::ui {

inline std::array<float, 4> focusPulseColor(const std::array<float, 4>& focus, bool capturing, uint64_t realMs) {
    const float t = (std::sin(float(int64_t(realMs) / 75)) + 1.0f) * 0.5f;
    std::array<float, 4> out{};
    for (size_t i = 0; i < 4; ++i) {
        const float low = capturing ? (i == 1 || i == 2 ? 0.0f : 0.8f) : focus[i] * 0.8f;
        out[i] = std::clamp(focus[i] + (low - focus[i]) * t, 0.0f, 1.0f); // 0x4c6e50: a + (b - a) * t, clamped
    }
    return out;
}

} // namespace iw2::ui
