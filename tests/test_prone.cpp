// Prone fit test (exe 0x4fb530) against hand-made worlds of axis planes.
#include "game/prone.h"

#include <cmath>
#include <iostream>
#include <optional>

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using iw2::game::TraceResult;
using iw2::game::Vec;

// Solid below z = floorZ (only where x > floorFromX) and, optionally, solid for x < wallX.
struct World {
    float floorZ = 0.0f;
    float floorFromX = -1e9f;
    std::optional<float> wallX;
    TraceResult trace(const Vec& s, const Vec& e, const Vec& mn, const Vec& mx) const {
        TraceResult r;
        r.endPos = e;
        auto solidAt = [&](const Vec& p) {
            const bool floor = p[2] + mn[2] < floorZ && p[0] + mx[0] > floorFromX;
            const bool wall = wallX && p[0] + mn[0] < *wallX;
            return floor || wall;
        };
        if (solidAt(s)) {
            r.startSolid = true;
            if (solidAt(e)) r.allSolid = true;
            r.fraction = 0.0f;
            return r;
        }
        // March in small steps; enough for these checks.
        const int n = 4000;
        for (int i = 1; i <= n; ++i) {
            const float t = float(i) / float(n);
            const Vec p{s[0] + (e[0] - s[0]) * t, s[1] + (e[1] - s[1]) * t, s[2] + (e[2] - s[2]) * t};
            if (solidAt(p)) {
                r.fraction = float(i - 1) / float(n);
                const bool floorHit = p[2] + mn[2] < floorZ && p[0] + mx[0] > floorFromX;
                r.normal = floorHit ? Vec{0, 0, 1} : Vec{1, 0, 0};
                return r;
            }
        }
        return r;
    }
};

iw2::game::ProneFitResult fit(const World& w, iw2::game::ProneFitArgs a) {
    return iw2::game::proneFit(a, [&](const Vec& s, const Vec& e, const Vec& mn, const Vec& mx) { return w.trace(s, e, mn, mx); });
}
} // namespace

int main() {
    using namespace iw2::game;
    CHECK(std::fabs(pronePitchOf({1, 0, 1}) - 315.0f) < 0.01f);
    CHECK(pronePitchOf({0, 0, 1}) == 270.0f);
    CHECK(pronePitchOf({0, 0, -1}) == 90.0f);
    CHECK(std::fabs(proneAngleDelta(350.0f, 10.0f) + 20.0f) < 0.001f);
    CHECK(std::fabs(proneAngleNormalize180(270.0f) + 90.0f) < 0.01f);

    ProneFitArgs a; // checkprone's arguments: 15, 48, length 66, strict
    // Open flat floor: the body fits lying flat.
    World flat;
    ProneFitResult r = fit(flat, a);
    CHECK(r.fits);
    CHECK(std::fabs(r.pitchBody) < 0.1f && std::fabs(r.pitchLegs) < 0.1f); // the marching trace above is coarse
    // A wall 20 units behind (yaw 0: the body lies towards -x): too short even with the raised retry.
    World wall;
    wall.wallX = -20.0f;
    CHECK(!fit(wall, a).fits);
    // Facing the wall instead: the body lies away from it.
    a.yaw = 180.0f;
    CHECK(fit(wall, a).fits);
    a.yaw = 0.0f;
    // No floor under the hip (48 behind): strict says no, non-strict answers "fits, flat".
    World ledge;
    ledge.floorFromX = -30.0f;
    CHECK(!fit(ledge, a).fits);
    a.strict = false;
    r = fit(ledge, a);
    CHECK(r.fits && r.pitchBody == 0.0f && r.pitchLegs == 0.0f);
    a.strict = true;
    // Standing box buried in the floor: rejected unless already prone skips that test.
    a.origin = {0, 0, -20};
    CHECK(!fit(flat, a).fits);
    // A strict caller's ground normal steeper than 0.7 rejects at once.
    a.origin = {0, 0, 0};
    const Vec steep{0.8f, 0.0f, 0.6f};
    a.groundNormal = &steep;
    CHECK(!fit(flat, a).fits);

    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
