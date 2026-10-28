#pragma once
// Item slide end point (G_RunItem 0x523040, branch 0x5231b2..0x523278): when a falling item barely moved before hitting a steep
// surface, the end point of its move is pushed out along the surface normal so the item can slide along the surface:
// end' = next + n * (1 - (next - cur) . n).

#include <array>

namespace iw2::game {

inline std::array<float, 3> itemSlideEnd(const std::array<float, 3>& cur, const std::array<float, 3>& next, const std::array<float, 3>& n) {
    const float dot = (next[0] - cur[0]) * n[0] + (next[1] - cur[1]) * n[1] + (next[2] - cur[2]) * n[2];
    const float k = 1.0f - dot;
    return {next[0] + n[0] * k, next[1] + n[1] * k, next[2] + n[2] * k};
}

} // namespace iw2::game
