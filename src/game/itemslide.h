#pragma once
// Item slide end point (G_RunItem 0x523040, branch 0x5231b2..0x523278): when a falling item barely moved before hitting a steep
// surface, the end point of its move is pushed out along the surface normal so the item can slide along the surface:
// end' = next + n * (1 - (next - cur) . n).

#include <array>
#include <cmath>

namespace iw2::game {

inline std::array<float, 3> itemSlideEnd(const std::array<float, 3>& cur, const std::array<float, 3>& next, const std::array<float, 3>& n) {
    const float dot = (next[0] - cur[0]) * n[0] + (next[1] - cur[1]) * n[1] + (next[2] - cur[2]) * n[2];
    const float k = 1.0f - dot;
    return {next[0] + n[0] * k, next[1] + n[1] * k, next[2] + n[2] * k};
}

// The angles of an item laid on a surface (0x522ea0 for a falling item that settles, and inline in FinishSpawningItem
// 0x5225e0 at 0x5228d1..0x522a35 for a map item whose 4096-unit drop trace hit something): the forward vector of the item's
// pitch and yaw is projected onto the plane of the surface normal, g = (n x f) x n; the angles become vectoangles(g) (0x424b70)
// with roll 0, and a weapon item (item type 1, table 0x5a6714) gets roll + 90 (0x5920bc), so it lies on its side.
inline std::array<float, 3> itemRestAngles(const std::array<float, 3>& angles, const std::array<float, 3>& n, bool weaponItem) {
    const float rad = 0.01745329238474369f; // 0x591f50
    const std::array<float, 3> f{std::cos(angles[0] * rad) * std::cos(angles[1] * rad), std::cos(angles[0] * rad) * std::sin(angles[1] * rad),
                                 -std::sin(angles[0] * rad)};
    const std::array<float, 3> t{n[1] * f[2] - n[2] * f[1], n[2] * f[0] - n[0] * f[2], n[0] * f[1] - n[1] * f[0]};
    const std::array<float, 3> g{t[1] * n[2] - t[2] * n[1], t[2] * n[0] - t[0] * n[2], t[0] * n[1] - t[1] * n[0]};
    float pitch = 90.0f, yaw = 0.0f;
    if (g[0] == 0.0f && g[1] == 0.0f) {
        pitch = g[2] > 0.0f ? 270.0f : 90.0f; // vectoangles of a vertical vector
    } else {
        yaw = std::atan2(g[1], g[0]) / rad;
        if (yaw < 0) yaw += 360.0f;
        pitch = -std::atan2(g[2], std::sqrt(g[0] * g[0] + g[1] * g[1])) / rad;
        if (pitch < 0) pitch += 360.0f;
    }
    return {pitch, yaw, weaponItem ? 90.0f : 0.0f};
}

} // namespace iw2::game
