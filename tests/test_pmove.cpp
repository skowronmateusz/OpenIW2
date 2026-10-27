// Tests for player movement (docs/spec/pmove.md). With IW2_MAIN set to the game's main directory the player also
// walks on toujane and is put slightly into the ground.

#include "assets/d3dbsp.h"
#include "fs/vfs.h"
#include "game/pmove.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

} // namespace

int main() {
    using namespace iw2::game;
    {
        // Free fall without a map: the slide move uses the average of the start and end vertical speed (0x54eac0),
        // so the fall distance is exactly g·t²/2.
        CollisionWorld empty;
        PlayerState ps;
        for (int i = 0; i < 20; ++i) playerMove(empty, ps, {}, 0.05f);
        CHECK(near(ps.velocity[2], -800.0f, 0.01f));
        CHECK(near(ps.origin[2], -400.0f, 0.05f));
        CHECK(!ps.onGround);
    }

    {
        // PM_AddEvent (0x507ba0): event 0 is ignored, the ring keeps the newest 4.
        PlayerState ps;
        addPmEvent(ps, 0);
        CHECK(ps.eventSeq == 0);
        for (int e = 1; e <= 6; ++e) addPmEvent(ps, e);
        CHECK(ps.eventSeq == 6 && ps.events[(6 - 1) & 3] == 6 && ps.events[(6 - 4) & 3] == 3);
    }

    if (const char* main = std::getenv("IW2_MAIN")) {
        iw2::fs::Vfs vfs;
        vfs.mountDirectory(main);
        const auto bytes = vfs.read("maps/toujane.d3dbsp");
        CHECK(bytes.has_value());
        if (!bytes) return 1;
        const iw2::assets::D3dBsp bsp(*bytes);
        const CollisionWorld cw(bsp);
        Vec start{};
        for (const auto& e : bsp.entities())
            if (const std::string* c = e.get("classname"); c && *c == "info_player_start")
                if (const std::string* o = e.get("origin")) std::sscanf(o->c_str(), "%f %f %f", &start[0], &start[1], &start[2]);

        PlayerState ps;
        ps.origin = {start[0], start[1], start[2] + 8};
        for (int i = 0; i < 40; ++i) playerMove(cw, ps, {}, 0.05f);
        CHECK(ps.onGround);
        const Vec floor = ps.origin;

        // Two seconds forward at g_speed 190 (friction and acceleration settle within the first frames).
        PlayerInput in;
        in.forward = 1;
        for (int i = 0; i < 120; ++i) playerMove(cw, ps, in, 1.0f / 60.0f);
        const float moved = std::hypot(ps.origin[0] - floor[0], ps.origin[1] - floor[1]);
        std::cout << "walked " << moved << " units in 2 s, " << ps.eventSeq << " footstep events (last " << ps.events[(ps.eventSeq - 1) & 3] << ")\n";
        // Walking on a typed surface adds a run step event 1 + type (2..0x17) at each half bob cycle; the spawn floor of
        // toujane has a surface type.
        CHECK(ps.eventSeq == 0 || (ps.events[(ps.eventSeq - 1) & 3] >= 2 && ps.events[(ps.eventSeq - 1) & 3] <= 0x17));
        CHECK(moved > 330.0f && moved < 380.0f);
        CHECK(ps.onGround);

        // Half a unit into the ground: the player must not sink or fall through.
        PlayerState in0;
        in0.origin = {floor[0], floor[1], floor[2] - 0.5f};
        for (int i = 0; i < 60; ++i) playerMove(cw, in0, {}, 1.0f / 60.0f);
        std::cout << "from 0.5 below the floor: z " << in0.origin[2] - floor[2] << " relative to the floor\n";
        CHECK(in0.origin[2] > floor[2] - 1.0f);

        // PM_FoliageSounds (0x50ac30): standing still, the last foliage time is cleared after the reset interval.
        {
            PlayerState fs;
            fs.origin = floor;
            fs.lastFoliageMs = 5;
            for (int i = 0; i < 20; ++i) playerMove(cw, fs, {}, 1.0f / 60.0f); // ~330 ms: still within 500 ms
            CHECK(fs.lastFoliageMs == 5);
            for (int i = 0; i < 20; ++i) playerMove(cw, fs, {}, 1.0f / 60.0f); // ~660 ms
            CHECK(fs.lastFoliageMs == 0);
        }
        // Ladder climb (PM_CheckLadderMove 0x50bd90 + 0x50c070): a sweep of horizontal traces finds a ladder surface (flag 0x8).
        {
            float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
            for (const auto& e : bsp.entities())
                if (const std::string* o = e.get("origin")) {
                    float x = 0, y = 0, z = 0;
                    if (std::sscanf(o->c_str(), "%f %f %f", &x, &y, &z) == 3) lo[0] = std::min(lo[0], x), lo[1] = std::min(lo[1], y), hi[0] = std::max(hi[0], x), hi[1] = std::max(hi[1], y);
                }
            bool found = false;
            for (float x = lo[0]; x < hi[0] && !found; x += 24)
                for (float y = lo[1]; y < hi[1] && !found; y += 24)
                    for (float z = -400; z < 600 && !found; z += 48)
                        for (int d = 0; d < 4 && !found; ++d) {
                            const Vec dir{d == 0 ? 1.f : d == 1 ? -1.f : 0.f, d == 2 ? 1.f : d == 3 ? -1.f : 0.f, 0};
                            const TraceResult t = cw.trace({x, y, z}, {x + dir[0] * 40, y + dir[1] * 40, z}, {0, 0, 0}, {0, 0, 0}, 0xffffffffu, -1);
                            if (t.fraction < 1 && (t.surfaceFlags & 8) && !t.startSolid) {
                                found = true;
                                PlayerState cl;
                                cl.origin = {t.endPos[0] + t.normal[0] * 20, t.endPos[1] + t.normal[1] * 20, t.endPos[2]};
                                PlayerInput ci;
                                ci.forward = 1;
                                ci.yaw = std::atan2(-t.normal[1], -t.normal[0]) * 180.0f / float(M_PI);
                                ci.pitch = -45.0f; // looking up
                                const float z0 = cl.origin[2];
                                for (int i = 0; i < 20; ++i) playerMove(cw, cl, ci, 1.0f / 60.0f);
                                CHECK(cl.onLadder);
                                CHECK(cl.origin[2] > z0 + 20.0f);
                                // Jumping off the ladder gives the ladder jump event 0x5b (checkJump).
                                cl.lastJumpMs = -100000;
                                PlayerInput ji = ci;
                                ji.jump = true;
                                const int seq0 = cl.eventSeq;
                                playerMove(cw, cl, ji, 1.0f / 60.0f);
                                bool ev = false;
                                for (int k = seq0; k < cl.eventSeq; ++k) ev |= (cl.events[k & 3] & 0xff) == 0x5b;
                                CHECK(ev);
                            }
                        }
            CHECK(found);
        }
    }

    if (failures) std::cerr << failures << " failures\n";
    return failures ? 1 : 0;
}
