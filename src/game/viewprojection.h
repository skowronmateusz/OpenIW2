// Scene projection of the 1.3 renderer (docs/spec/render-hints.md "Scene projection"), column-major for a right-handed view
// space looking down -z. There is no far plane: view distance d maps to depth k - zTerm / d with k = kProjectionScale.
#pragma once
#include <array>

namespace iw2::game {

constexpr float kProjectionScale = 0.99950027f;

// cotX / cotY: cotangents of the half fields of view. Ordinary surfaces use zNear * k as the depth term (0x1005bfe0).
inline std::array<float, 16> sceneProjection(float cotX, float cotY, float zNear) {
    const float k = kProjectionScale;
    return {cotX * k, 0, 0, 0, 0, cotY * k, 0, 0, 0, 0, -k, -1, 0, 0, -zNear * k, 0};
}

// The depth-hack class replaces only the depth term by the bare near value (0x1003e6a0 stores -r_znear_depthhack, unscaled).
inline std::array<float, 16> depthHackProjection(float cotX, float cotY, float zNear) {
    auto m = sceneProjection(cotX, cotY, zNear);
    m[14] = -zNear;
    return m;
}

} // namespace iw2::game
