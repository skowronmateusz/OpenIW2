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
            t.startSolid = true;
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
                t.allSolid = true;
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
    { // ground probe
        auto floorAt = [](float zf, float nz) {
            return [zf, nz](const float st[3], const float en[3]) {
                SlideTrace t;
                if (st[2] >= zf && en[2] < zf) {
                    t.fraction = (st[2] - zf) / (st[2] - en[2]);
                    t.normal[2] = nz;
                    t.normal[0] = nz < 0.7f ? 0.9f : 0.0f;
                    t.entity = 0x7fe;
                }
                return t;
            };
        };
        ActorSlide a;
        a.origin[2] = 0.1f;
        groundProbe(a, floorAt(0.0f, 1.0f));
        CHECK(a.groundPlane && a.groundEntValid && a.groundEnt == 0x7fe);
        CHECK(a.origin[2] >= -0.001f && a.origin[2] <= 0.1f);
        ActorSlide b;
        b.origin[2] = 50;
        groundProbe(b, floorAt(0.0f, 1.0f));
        CHECK(!b.groundPlane && b.groundEnt == 0x7ff);
        ActorSlide c;
        c.origin[2] = 0.1f;
        groundProbe(c, floorAt(0.0f, 0.5f));
        CHECK(c.groundPlane && !c.groundEntValid && c.groundEnt == 0x7ff && c.hitEnt == 0x7fe);
        ActorSlide d;
        d.origin[2] = 0.1f;
        d.velocity[2] = 50;
        groundProbe(d, floorAt(0.0f, 1.0f));
        CHECK(!d.groundPlane); // moving up away from the floor: dot > 10 -> no ground
    }
    { // move block: walking on a floor
        auto floor0 = [](const float st[3], const float en[3]) {
            SlideTrace t;
            if (en[2] < 0.0f && st[2] >= 0.0f) {
                t.fraction = st[2] / (st[2] - en[2]);
                t.normal[2] = 1;
                t.entity = 0x7fe;
            }
            return t;
        };
        ActorSlide a;
        a.dt = 0.05f;
        a.timeMs = 50;
        a.origin[2] = 0.0f;
        a.groundEnt = 0x7fe;
        a.moveDelta[0] = 5.0f;
        const int r = moveBlock(a, floor0);
        CHECK(r == 1);
        CHECK(a.origin[0] > 4.9f && a.origin[0] < 5.1f);
        CHECK(a.groundPlane && a.groundEntValid && a.contact);
        ActorSlide b;
        b.dt = 0.05f;
        b.groundEnt = 0x7fe;
        b.velocity[0] = 9;
        moveBlock(b, floor0);
        CHECK(b.velocity[0] == 0.0f && b.origin[0] == 0.0f);
    }
    {
        CHECK(nearestReplaces(10.0f, 9.0f) && nearestReplaces(10.0f, 10.0f) && !nearestReplaces(9.0f, 10.0f)); // ties: the later node
    }
    {
        // The path walk's direct move after a successful block (0x501e53..0x501e8c): only a steep lookahead (slope > 4)
        // with an upward delta that the block did not carry out.
        CHECK(pathSteepUnlifted(5.0f, 1.0f, 10.0f, 10.0f));   // block z == entity z: not lifted
        CHECK(pathSteepUnlifted(5.0f, 1.0f, 9.0f, 10.0f));    // block z below
        CHECK(!pathSteepUnlifted(5.0f, 1.0f, 10.5f, 10.0f));  // the block raised the actor
        CHECK(!pathSteepUnlifted(4.0f, 1.0f, 10.0f, 10.0f));  // slope 4 is not above 4
        CHECK(!pathSteepUnlifted(5.0f, 0.0f, 10.0f, 10.0f));  // delta z 0 is not up
        CHECK(!pathSteepUnlifted(5.0f, -1.0f, 9.0f, 10.0f));
        // Budget floor of the direct move (0x56f6e0 = 64).
        CHECK(pathDirectBudget(10.0f) == 64.0f && pathDirectBudget(64.0f) == 64.0f && pathDirectBudget(100.0f) == 100.0f);
    }
    {
        CHECK(stuckNudge(20) == 8 && stuckNudge(3) == 3 && stuckNudge(0) == 0 && stuckNudge(-10) == 0 && stuckNudge(-30) == -8);
        // trigger threshold at walking speed 160: 160^2 * 0.005 = 128 -> within ~11.3 units, not 13
        CHECK(negotiationInRange(127.0f, 25600.0f) && !negotiationInRange(129.0f, 25600.0f) && negotiationInRange(0.0f, 0.0f));
    }
    {
        // stop soon: r = 50 / 500 = 0.1 -> k = 0.19; at 50 ms k = 1; with a contact the factor is divided by the normal z
        CHECK(near(stopSoonScale(500, 0.0f), 0.19f, 1e-5f) && near(stopSoonScale(50, 0.0f), 1.0f, 1e-5f));
        CHECK(near(stopSoonScale(500, 0.5f), 0.38f, 1e-5f));
    }
    {
        // gethityaw: the yaw of the reversed hit normal, 0..360; zero vector gives 0
        CHECK(near(actorHitYaw(-1, 0), 0.0f) && near(actorHitYaw(0, -1), 90.0f) && near(actorHitYaw(1, 0), 180.0f) && near(actorHitYaw(0, 1), 270.0f));
        CHECK(actorHitYaw(0, 0) == 0.0f);
    }
    {
        // 0x500de0 sidestep delta: s = 4, unit normal (-1, 0) (a wall ahead along +x), first segment direction (1, 0).
        const float n[2] = {-1, 0}, seg[2] = {1, 0};
        float d1[3] = {8, 0, 1};
        const float left[2] = {-50, 0}; // d.y*P.x (0) <= d.x*P.y (0): side 0x10, delta (-s*n.y, s*n.x)
        CHECK(sidestepDelta(d1, left, n, seg, 4.0f, 0) == 0x10 && near(d1[0], 0.0f) && near(d1[1], -4.0f) && d1[2] == 1.0f);
        float d2[3] = {8, 0, 0};
        const float right[2] = {0, 5}; // 5 > 0: side 8, delta (s*n.y, -s*n.x)
        CHECK(sidestepDelta(d2, right, n, seg, 4.0f, 0) == 8 && near(d2[0], 0.0f) && near(d2[1], 4.0f));
        float d3[3] = {8, 0, 0}; // stepped the other way last time: 0.33 of the side vector is added
        CHECK(sidestepDelta(d3, left, n, seg, 4.0f, 8) == 0x10 && near(d3[0], 8.0f) && near(d3[1], -1.32f));
        float d4[3] = {8, 0, 0};
        CHECK(sidestepDelta(d4, right, n, seg, 4.0f, 0x10) == 8 && near(d4[0], 8.0f) && near(d4[1], 1.32f));
    }
    {
        // 0x5011d7: side bits replaced; a flip only when an old side differs from a new non-zero one
        int f = 0x100;
        CHECK(!sidestepFlip(f, 8) && f == 0x108);
        CHECK(!sidestepFlip(f, 8) && f == 0x108);
        CHECK(sidestepFlip(f, 0x10) && f == 0x110);
        CHECK(!sidestepFlip(f, 0) && f == 0x100);
        // 0x501248: progress along the delta below a quarter of its length
        const float eo[3] = {0, 0, 0}, delta[3] = {8, 0, 0};
        const float a[3] = {1.9f, 5, 0}, b[3] = {2.1f, 0, 0};
        CHECK(sidestepShort(a, eo, delta) && !sidestepShort(b, eo, delta));
        // 0x5442a0
        CHECK(pathSidestepOk(4, 4, 0, 3, 2, 0, 100.0f, false));
        CHECK(!pathSidestepOk(4, 4, 0, 3, 2, 0, 100.0f, true)); // still exactly on the planned start
        CHECK(pathSidestepOk(5, 4, 0, 3, 2, 0, 100.0f, true));  // re-extended path
        CHECK(!pathSidestepOk(4, 4, 3, 3, 2, 0, 100.0f, false)); // first segment is a traversal
        CHECK(!pathSidestepOk(4, 4, 0, -1, 2, 0, 100.0f, false));
        CHECK(!pathSidestepOk(4, 4, 0, 3, 2, 0x20, 100.0f, false));
        CHECK(!pathSidestepOk(4, 4, 0, 3, 2, 0, 0.001f, false));
    }
    {
        // sidestepAttempts against a wall (entity 5) at x = 10 on a floor: the first move stops at the wall; with the
        // sidestep allowed the delta turns to -y (side 0x10, s = 0.75 * 20); the second move hits nothing, and since
        // 0x500d80 also passes for the empty slot 0x7ff the third move runs with s * 0.75 from the stale hit record.
        auto run = [&](bool avoid, ActorSlide& pm) {
            pm.dt = 0.05f;
            pm.timeMs = 50;
            pm.mode = 1;
            pm.mask = 1;
            pm.groundEnt = 0x7fe;
            pm.moveDelta[0] = 20;
            SidestepInput in;
            in.start[0] = -100;
            in.segDir[0] = 1;
            in.pathOk = true;
            in.avoid = [avoid](int) { return avoid; };
            const float eo[3] = {0, 0, 0};
            return sidestepAttempts(pm, eo, in, [&](const float* s, const float* e) { return planeTrace(s, e, 10, 5); });
        };
        ActorSlide a;
        const SidestepResult ra = run(false, a);
        CHECK(ra.outcome == SidestepOutcome::Tail && ra.side == 0 && ra.firstHit == 5);
        CHECK(near(a.origin[0], 10.0f, 0.2f) && near(a.origin[1], 0.0f));
        ActorSlide b;
        const SidestepResult rb = run(true, b);
        CHECK(rb.outcome == SidestepOutcome::Tail && rb.side == 0x10 && rb.firstHit == 5);
        CHECK(near(b.moveDelta[1], -11.25f) && near(b.origin[1], -11.25f, 0.01f) && near(b.origin[0], 0.0f, 0.01f));
        CHECK(b.hitEnt == 0x7ff);
    }
    if (failures == 0) std::cout << "actorslide: all passed\n";
    {
        // 0x5487c0: with a non-walkable ground plane (normal z < 0.7) the block takes the airborne branch (0x5480d0), which steps
        // the velocity only. The desired delta (+0x2c) is converted to a velocity only on walkable ground (0x548110), so a turret
        // gunner whose velocity the pose step zeroes does not move towards the pose (docs/spec/turrets.md "Gunner on steep ground").
        const float nx = -0.88f, nz = 0.47f;
        const SlideTraceFn steep = [=](const float* s, const float* e) {
            SlideTrace t;
            t.entity = 0x7ff;
            const float dz = s[2] - e[2];
            if (s[0] == e[0] && s[1] == e[1] && dz > 0.0f && dz <= 20.0f) { // the ground probe only
                t.fraction = 0.5f;
                t.normal[0] = nx;
                t.normal[2] = nz;
                t.entity = 0x7fe;
            }
            return t;
        };
        const auto run = [&](const SlideTraceFn& fn) {
            ActorSlide pm;
            pm.dt = 0.05f;
            pm.timeMs = 50;
            pm.mode = 1;
            pm.mask = 1;
            pm.groundEnt = 0x7fe;
            pm.moveDelta[0] = -17.0f;
            moveBlock(pm, fn);
            return pm;
        };
        const ActorSlide on = run(steep);
        CHECK(on.groundEnt == 0x7ff && !on.groundEntValid && on.groundPlane);
        CHECK(near(on.origin[0], 0.0f, 0.01f));
        const SlideTraceFn flat = [](const float* s, const float* e) {
            SlideTrace t;
            t.entity = 0x7ff;
            const float dz = s[2] - e[2];
            if (s[0] == e[0] && s[1] == e[1] && dz > 0.0f && dz <= 20.0f) {
                t.fraction = 0.5f;
                t.normal[2] = 1.0f;
                t.entity = 0x7fe;
            }
            return t;
        };
        const ActorSlide walk = run(flat);
        CHECK(walk.groundEntValid && near(walk.origin[0], -17.0f, 0.01f));
    }
    return failures == 0 ? 0 : 1;
}
