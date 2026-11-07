// Camera-relative placement of the depth-hack draw group (the view model).
//
// The view model is attached to the eye, so on screen its vertices are a fixed function of the view-model offsets and the
// view angles. A renderer that places it in world coordinates and then multiplies by a world-to-view matrix loses that: the
// translations of the model matrix and of the view matrix are both of the size of the map coordinates (tens of thousands of
// units), so each frame rounds the same point differently by about one float step of that size (0.004 units at 50000), and
// the rounding changes with every sway of the gun. Near-coplanar view-model parts (a button on a sleeve, a rivet on a
// receiver) then change their depth order from frame to frame. Placing the model at the offset from the eye, and drawing
// with a view matrix whose eye is the origin, keeps every intermediate value small. The result is the same scene translated
// by -eye: directions and distances are unchanged.
//
// This is a floating-point precision measure of this renderer, not behaviour of the original.
#pragma once

#include <array>

namespace iw2::game {

// Offset of the view model's origin from the eye: forward * mv[0] + right * mv[1] + up * mv[2] (mv = ViewWeaponMotion::move()).
inline std::array<float, 3> viewModelOffset(const std::array<float, 3>& forward, const std::array<float, 3>& right,
                                            const std::array<float, 3>& up, const std::array<float, 3>& mv) {
    std::array<float, 3> o{};
    for (size_t k = 0; k < 3; ++k) o[k] = forward[k] * mv[0] + right[k] * mv[1] + up[k] * mv[2];
    return o;
}

} // namespace iw2::game
