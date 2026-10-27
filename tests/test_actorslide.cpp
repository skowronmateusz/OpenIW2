// Tests for the actor slide move (exe 0x5475e0) and clip velocity (0x547540).

#include "game/actorslide.h"

#include <cmath>
#include <iostream>

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)
bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
using namespace iw2::game;

// A point trace against the half-space x < wallX (normal -x) and z > 0 (floor, normal +z).
SlideTrace planeTrace(const float s[3], const float e[3], float wallX, uint16_t wallEnt) {
    SlideTrace best;
    auto test = [&](float ds, float de, const float n[3], uint16_t ent) {
        if (ds >= 0 && de < 0) {
            const float f = ds / (ds - de);
            if (f < best.fraction) {
                best.fraction = f;
                for (int a = 0; a < 3; ++a) best.normal[a] = n[a];
                best.entity = ent;
            }
        }
    };
    const float nw[3] = {-1, 0, 0}, nf[3] = {0, 0, 1};
    test(wallX - s[0], wallX - e[0], nw, wallEnt);
    test(s[2], e[2], nf, 0x7fe);
    return best;
}
} // namespace

int main() {
    // clipVelocity: ramp branch on a flat floor, generic branch on a wall.
    {
        const float in[3] = {10, 0, -5}, n[3] = {0, 0, 1};
        float out[3];
        clipVelocity(in, n, out, 1.001f);
        CHECK(near(out[0], 10) && near(out[1], 0) && near(out[2], 0));
        const float w[3] = {-1, 0, 0}, inw[3] = {5, 2, 0};
        clipVelocity(inw, w, out, 1.001f); // backoff = -5 * 1.001
        CHECK(near(out[0], 5 - (-5.0f * 1.001f) * -1.0f, 1e-3f));
        CHECK(near(out[1], 2) && near(out[2], 0));
    }
    // Free move: no hit, origin advances, not blocked.
    {
        ActorSlide pm;
        pm.dt = 0.05f;
        pm.velocity[0] = 100;
        const int r = slideMove(pm, false, [&](const float* s, const float* e) { return planeTrace(s, e, 1000, 5); });
        CHECK(r == 0);
        CHECK(near(pm.origin[0], 5));
        CHECK(pm.hitEnt == 0x7ff);
    }
    // Wall: moving +x into a wall at x = 2 records the wall entity and slides along y.
    {
        ActorSlide pm;
        pm.dt = 0.1f;
        pm.velocity[0] = 100;
        pm.velocity[1] = 50;
        pm.origin[2] = 10;
        const int r = slideMove(pm, false, [&](const float* s, const float* e) { return planeTrace(s, e, 2, 9); });
        CHECK(r == 1);
        CHECK(pm.hitEnt == 9);
        CHECK(pm.blocked);
        CHECK(near(pm.hitNormal[0], -1) && near(pm.hitNormal[1], 0));
        CHECK(pm.touchCount == 1 && pm.touch[0] == 9);
        CHECK(pm.origin[0] < 2.001f && pm.origin[0] > 1.8f);
        CHECK(pm.origin[1] > 0.5f);
        CHECK(pm.velocity[0] < 1.0f);
    }
    // Floor with gravity on the ground plane: the floor is world (0x7fe), not listed, and no wall hit is recorded.
    {
        ActorSlide pm;
        pm.dt = 0.05f;
        pm.gravity = 800;
        pm.groundPlane = true;
        pm.velocity[0] = 50;
        slideMove(pm, true, [&](const float* s, const float* e) { return planeTrace(s, e, 1000, 5); });
        CHECK(pm.hitEnt == 0x7ff);
        CHECK(pm.touchCount == 0);
    }
    // All-solid start with flag87 returns 2.
    {
        ActorSlide pm;
        pm.dt = 0.05f;
        pm.velocity[0] = 10;
        pm.flag87 = true;
        const int r = slideMove(pm, false, [&](const float*, const float*) {
            SlideTrace t;
            t.fraction = 0;
            t.allSolid = true;
            return t;
        });
        CHECK(r == 2);
    }
    // Step slide: a step of height 10 at x >= 2 is climbed with stepHeight 18; with stepHeight 4 the wall blocks.
    {
        auto solid = [](float x, float, float z) { return z < 0.0f || (x >= 2.0f && z < 10.0f); };
        auto stepTrace = [&](const float* s, const float* e) {
            SlideTrace t;
            const int N = 4000;
            float px = s[0], py = s[1], pz = s[2];
            if (solid(px, py, pz)) {
                t.fraction = 0;
                t.startSolid = true;
                return t;
            }
            for (int i = 1; i <= N; ++i) {
                const float f = float(i) / N;
                const float x = s[0] + (e[0] - s[0]) * f, y = s[1] + (e[1] - s[1]) * f, z = s[2] + (e[2] - s[2]) * f;
                if (solid(x, y, z)) {
                    t.fraction = float(i - 1) / N;
                    // face: the axis that changed solidity
                    if (solid(px, py, z) && !solid(x, py, pz)) { /* z entered */ }
                    if (!solid(px, py, pz) && solid(px, py, z) && !solid(x, py, pz)) {
                        t.normal[2] = 1;
                    } else {
                        t.normal[0] = -1;
                    }
                    t.entity = 0x7fe;
                    return t;
                }
                px = x; py = y; pz = z;
            }
            return t;
        };
        ActorSlide a;
        a.dt = 0.1f;
        a.velocity[0] = 100;
        a.stepHeight = 18;
        const int r = stepSlideMove(a, false, stepTrace);
        CHECK(r == 1);
        CHECK(a.origin[0] > 2.5f);
        CHECK(a.origin[2] > 9.5f && a.origin[2] < 10.5f);
        ActorSlide b;
        b.dt = 0.1f;
        b.velocity[0] = 100;
        b.stepHeight = 4;
        stepSlideMove(b, false, stepTrace);
        CHECK(b.origin[0] < 2.01f);
        CHECK(b.origin[2] < 1.0f);
    }
    if (failures == 0) std::cout << "actorslide: all passed\n";
    return failures == 0 ? 0 : 1;
}
