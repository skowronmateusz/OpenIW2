// Camera-relative placement of the view model (game/viewspace.h): a near-coplanar part keeps its depth order at map-sized coordinates.
#include "game/viewspace.h"
#include <array>
#include <cmath>
#include <cstdio>

using namespace iw2::game;
using V3 = std::array<float, 3>;

static float dot3(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

int main() {
    int bad = 0;
    auto chk = [&](bool c, int line) {
        if (!c) {
            std::printf("line %d failed\n", line);
            ++bad;
        }
    };
    // viewModelOffset: forward * mv0 + right * mv1 + up * mv2.
    {
        const auto o = viewModelOffset({1, 0, 0}, {0, -1, 0}, {0, 0, 1}, {2, 3, 4});
        chk(o[0] == 2 && o[1] == -3 && o[2] == 4, __LINE__);
    }
    // A button 0.002 units above a sleeve surface (towards the eye) 12 units in front of the eye. The eye is somewhere around
    // (50000, -30000, 400) and moves by a few hundredths of a unit per frame (sway, bob).
    const float yaw = 0.5236f, pitch = -0.1f; // radians
    const V3 fwd = {std::cos(yaw) * std::cos(pitch), std::sin(yaw) * std::cos(pitch), -std::sin(pitch)};
    const V3 right = {std::sin(yaw), -std::cos(yaw), 0};
    const V3 up = {fwd[1] * right[2] - fwd[2] * right[1], fwd[2] * right[0] - fwd[0] * right[2], fwd[0] * right[1] - fwd[1] * right[0]};
    // Model-local points (the view model's own axes are the view axes here): sleeve point and button point, button nearer the eye.
    const V3 sleeve = {12.0f, 1.0f, -2.0f}, button = {11.998f, 1.0f, -2.0f};
    const auto off = viewModelOffset(fwd, right, up, {2.0f, 3.0f, -4.0f});
    auto local = [&](const V3& p) {
        return V3{fwd[0] * p[0] + right[0] * p[1] + up[0] * p[2], fwd[1] * p[0] + right[1] * p[1] + up[1] * p[2],
                  fwd[2] * p[0] + right[2] * p[1] + up[2] * p[2]};
    };
    int absoluteFlips = 0, relativeFlips = 0;
    for (int i = 0; i < 400; ++i) {
        const V3 eye = {50000.0f + 0.013f * float(i), -30000.0f + 0.007f * float(i * i % 31), 400.0f + 0.011f * float(i % 17)};
        // World-space route (what the renderer did): model origin = eye + offset rounded to world size, vertices placed in world
        // space, then the view matrix subtracts the eye again (translation -f . eye).
        auto depthAbs = [&](const V3& p) {
            V3 origin, w;
            const V3 l = local(p);
            for (size_t k = 0; k < 3; ++k) origin[k] = eye[k] + off[k];
            for (size_t k = 0; k < 3; ++k) w[k] = origin[k] + l[k];
            return dot3(fwd, w) + (-dot3(fwd, eye));
        };
        // Camera-relative route: origin = offset, view matrix with the eye at the origin.
        auto depthRel = [&](const V3& p) {
            V3 w;
            const V3 l = local(p);
            for (size_t k = 0; k < 3; ++k) w[k] = off[k] + l[k];
            return dot3(fwd, w);
        };
        // The gap between the two surfaces is 0.002 in every frame; rounding at world size collapses it to 0 or widens it to a step.
        if (std::fabs((depthAbs(sleeve) - depthAbs(button)) - 0.002f) > 0.001f) ++absoluteFlips;
        if (std::fabs((depthRel(sleeve) - depthRel(button)) - 0.002f) > 0.001f) ++relativeFlips;
    }
    chk(relativeFlips == 0, __LINE__);
    chk(absoluteFlips > 0, __LINE__); // the failure the helper avoids: world-sized rounding is larger than the 0.002 gap
    std::printf("frames with a wrong depth gap out of 400: world-space %d, camera-relative %d\n", absoluteFlips, relativeFlips);
    return bad ? 1 : 0;
}
