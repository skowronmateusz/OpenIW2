// Grenade hint node geometry (see grenadehint.h and docs/spec/grenades.md "Grenade hint nodes").

#include "game/grenadehint.h"

#include <algorithm>
#include <cmath>

namespace iw2::game {

namespace {

// 0x421160: normalises the 2D vector in place (left alone when its length is 0) and returns the length.
float normalize2(float& x, float& y) {
    const float l = std::sqrt(x * x + y * y);
    if (l != 0.0f) {
        const float inv = 1.0f / l;
        x *= inv;
        y *= inv;
    }
    return l;
}

}  // namespace

bool grenadeHintPick(const std::vector<HintVec>& hints, const HintVec& start, const HintVec& target, float arcDeg,
                     HintVec& out) {
    const float limit = std::cos((arcDeg * 0.5f + 180.0f) * 0.0174532924f);
    int best = -1;
    float bestDist = 3.4028235e+38f;
    for (size_t i = 0; i < hints.size(); ++i) {
        const HintVec& h = hints[i];
        if (h[2] - 10.0f < target[2]) continue;
        float ax = h[0] - target[0], ay = h[1] - target[1];
        const float dist = normalize2(ax, ay);
        if (dist > 192.0f) continue;
        float bx = h[0] - start[0], by = h[1] - start[1];
        normalize2(bx, by);
        if (by * ay + bx * ax > limit) continue;
        if (dist < bestDist) {
            bestDist = dist;
            best = int(i);
        }
    }
    if (best < 0) return false;
    out = hints[size_t(best)];
    return true;
}

bool grenadeHintLob(const HintVec& hint, const HintVec& start, const HintVec& target, float gravity, HintVec& vel,
                    HintVec& land) {
    const float dx = target[0] - start[0], dy = target[1] - start[1], dz = target[2] - start[2];
    const float D = std::sqrt(dy * dy + dx * dx);
    float hx = hint[0] - start[0], hy = hint[1] - start[1];
    const float hz = hint[2] - start[2];
    if (hy * dy + hx * dx + hz * dz < 0.0f) return false;
    const float L = normalize2(hx, hy);
    if (!(L < D) || L == 0.0f) return false;
    land = {hx * D + start[0], hy * D + start[1], target[2]};
    const float ex = target[0] - land[0], ey = target[1] - land[1];
    if (ey * ey + ex * ex > 36864.0f) return false;
    // The parabola z(s) = a·s + b·s² through (L, hz) and (D, dz), s the horizontal distance along the hint direction.
    const double a = double(L) * dz - double(hz) * D;
    if (!(a < 0.0)) return false;
    const double k = gravity / a * -0.5;
    const float speed = float(std::sqrt((double(D) - L) * k * L * D));
    const float up = float(k * ((double(hz) * D * D - double(L) * L * dz) / speed));
    if (double(up) * up + double(speed) * speed > 810000.0) return false;
    vel = {hx * speed, hy * speed, up};
    return true;
}

void grenadeHintSortByDistance(std::vector<HintVec>& hints, const HintVec& origin) {
    auto d2 = [&](const HintVec& p) {
        const float x = p[0] - origin[0], y = p[1] - origin[1], z = p[2] - origin[2];
        return z * z + x * x + y * y;
    };
    std::stable_sort(hints.begin(), hints.end(), [&](const HintVec& a, const HintVec& b) { return d2(a) < d2(b); });
}

}  // namespace iw2::game
