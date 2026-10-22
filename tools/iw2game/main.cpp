// iw2game: run a level headless (scripts, entities, no rendering).
//
//   iw2game <main-dir> <map> [seconds] [--cvar name=value ...]
//   iw2game <main-dir> <map> --walk-test     drop the player at the spawn point and walk forward
//   iw2game <main-dir> <map> --fall-test [walks] [seconds]   random player walks (tunnels, falls, solid)
//   iw2game <main-dir> <map> --replay file      re-run moves recorded by iw2view (pmove_replay.bin)
//   iw2game <main-dir> <map> --spot x,y,z yaw   replay an iw2view `P` view position: walk from it in 72 directions

#include "audio/sound.h"
#include "game/pmove.h"
#include "game/pmoverecord.h"
#include "game/world.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: iw2game <main-dir> <map> [seconds] [--cvar name=value ...]\n";
        return 2;
    }
    try {
        iw2::fs::Vfs vfs;
        vfs.mountDirectory(argv[1]);
        iw2::game::World world(vfs, argv[2]);
        if (argc > 3 && std::string(argv[3]) == "--walk-test") {
            const auto t0 = std::chrono::steady_clock::now();
            iw2::game::CollisionWorld cw(world.bsp());
            std::cout << "collision: " << cw.brushCount() << " brushes (" << cw.triangleCount() << " from triangles), built in "
                      << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s\n";
            world.start();
            iw2::game::PlayerState ps;
            const auto o = world.player()->obj->get("origin").asVector();
            const auto a = world.player()->obj->get("angles").asVector();
            ps.origin = {o[0], o[1], o[2] + 32};
            iw2::game::PlayerInput in;
            in.yaw = a[1];
            {
                const iw2::game::MoveParams mp;
                auto tr = cw.trace(ps.origin, ps.origin, mp.mins, mp.maxs, iw2::game::Contents::PlayerSolid);
                std::cout << "spawn check: startSolid " << tr.startSolid << " contents 0x" << std::hex << tr.contents << std::dec << "\n";
                auto tr2 = cw.trace(ps.origin, {ps.origin[0], ps.origin[1], ps.origin[2] - 200}, {0,0,0}, {0,0,0}, iw2::game::Contents::PlayerSolid);
                std::cout << "ray down: fraction " << tr2.fraction << " end z " << tr2.endPos[2] << " normal z " << tr2.normal[2] << " startSolid " << tr2.startSolid << "\n";
            }
            for (int f = 0; f < 40; ++f) iw2::game::playerMove(cw, ps, in, 0.05f);
            std::cout << "after drop: z=" << ps.origin[2] << " (spawn z " << o[2] << "), on ground " << ps.onGround << "\n";
            in.forward = 1;
            for (int s = 1; s <= 6; ++s) {
                for (int f = 0; f < 20; ++f) iw2::game::playerMove(cw, ps, in, 0.05f);
                std::cout << "t=" << s << "s pos (" << ps.origin[0] << ", " << ps.origin[1] << ", " << ps.origin[2] << ") on ground " << ps.onGround << "\n";
            }
            return 0;
        }
        if (argc > 4 && std::string(argv[3]) == "--replay") {
            // Each recorded move again from its recorded state with its entity clips: reports moves whose result
            // differs from the recording and the first move after which nothing is under the player.
            const auto moves = iw2::game::MoveRecorder::read(argv[4]);
            iw2::game::CollisionWorld cw(world.bsp());
            std::printf("%zu moves\n", moves.size());
            int diffs = 0;
            bool fell = false;
            for (size_t i = 0; i < moves.size(); ++i) {
                const auto& m = moves[i];
                cw.setEntityClips(m.clips);
                iw2::game::PlayerState ps = m.before;
                iw2::game::playerMove(cw, ps, m.input, m.dt, m.params);
                if (ps.origin != m.after.origin && diffs++ < 5)
                    std::printf("move %zu differs from the recording: (%g %g %g) vs (%g %g %g)\n", i, ps.origin[0], ps.origin[1], ps.origin[2],
                                m.after.origin[0], m.after.origin[1], m.after.origin[2]);
                const auto under = cw.trace(ps.origin, {ps.origin[0], ps.origin[1], ps.origin[2] - 4000}, {0, 0, 0}, {0, 0, 0},
                                            m.params.traceMask, m.params.passEntity);
                if (!fell && under.fraction == 1 && !under.startSolid) {
                    fell = true;
                    std::printf("move %zu: nothing under the player after it\n", i);
                    for (size_t k = i > 12 ? i - 12 : 0; k <= i; ++k) {
                        const auto& q = moves[k];
                        std::printf("  move %zu dt %g fwd %g right %g jump %d stance %d->%d yaw %g onGround %d vel (%g %g %g)\n"
                                    "    from (%a %a %a) = (%g %g %g)\n    to   (%g %g %g)\n",
                                    k, q.dt, q.input.forward, q.input.right, q.input.jump, int(q.before.stance), int(q.input.stance),
                                    q.input.yaw, q.before.onGround, q.before.velocity[0], q.before.velocity[1], q.before.velocity[2],
                                    q.before.origin[0], q.before.origin[1], q.before.origin[2], q.before.origin[0], q.before.origin[1],
                                    q.before.origin[2], q.after.origin[0], q.after.origin[1], q.after.origin[2]);
                    }
                }
            }
            std::printf("replay: %d moves differ, %s\n", diffs, fell ? "fell out" : "stayed in the world");
            return 0;
        }
        if (argc > 5 && std::string(argv[3]) == "--spot") {
            // The view position printed by iw2view (eye = origin + 60 standing). Walks every 5 degrees around the
            // given yaw for a few seconds in each stance, reporting tunnels and falls below the map.
            iw2::game::Vec eye{};
            std::sscanf(argv[4], "%f,%f,%f", &eye[0], &eye[1], &eye[2]);
            const float yaw0 = std::stof(argv[5]);
            float floorZ = 1e9f;
            for (const auto& v : world.bsp().vertices()) floorZ = std::min(floorZ, v.position[2]);
            world.start();
            const iw2::game::CollisionWorld& cw = world.collision();
            iw2::game::MoveParams mp;
            mp.passEntity = world.player()->number;
            const uint32_t mask = mp.traceMask;
            const iw2::game::Vec org{eye[0], eye[1], eye[2] - 60};
            const auto down = cw.trace(eye, {eye[0], eye[1], eye[2] - 4000}, {0, 0, 0}, {0, 0, 0}, mask, mp.passEntity);
            std::printf("ray down: fraction %g z %g normal (%g %g %g) startSolid %d contents 0x%x; map floor %g\n", down.fraction,
                        down.endPos[2], down.normal[0], down.normal[1], down.normal[2], down.startSolid, down.contents, floorZ);
            // Drawn triangles under or over the spot (xy inside the triangle), for comparing with collision.
            {
                const auto& bsp = world.bsp();
                float zmax = -1e9f;
                for (const auto& v : bsp.vertices()) zmax = std::max(zmax, v.position[2]);
                std::printf("map vertex z range %g..%g\n", floorZ, zmax);
                for (const auto& so : bsp.soups())
                    for (uint32_t i = 0; i + 2 < so.indexCount; i += 3) {
                        const auto& a = bsp.vertices()[so.firstVertex + bsp.indices()[so.firstIndex + i]].position;
                        const auto& b = bsp.vertices()[so.firstVertex + bsp.indices()[so.firstIndex + i + 1]].position;
                        const auto& c = bsp.vertices()[so.firstVertex + bsp.indices()[so.firstIndex + i + 2]].position;
                        auto side = [&](const auto& p, const auto& q) { return (q[0] - p[0]) * (eye[1] - p[1]) - (q[1] - p[1]) * (eye[0] - p[0]); };
                        const float s0 = side(a, b), s1 = side(b, c), s2 = side(c, a);
                        if (!((s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0))) continue;
                        const float den = s0 + s1 + s2;
                        if (std::fabs(den) < 1e-6f) continue;
                        const float z = (s1 * a[2] + s2 * b[2] + s0 * c[2]) / den;
                        std::printf("drawn tri z %g material %s (surf 0x%x contents 0x%x)\n", z, bsp.materials()[so.material].name.c_str(),
                                    bsp.materials()[so.material].surfaceFlags, bsp.materials()[so.material].contentFlags);
                    }
            }
            for (float top : {10400.0f, 10290.0f, 10280.0f}) {
                const auto r = cw.trace({eye[0], eye[1], top}, {eye[0], eye[1], eye[2]}, {0, 0, 0}, {0, 0, 0}, mask, mp.passEntity);
                std::printf("ray from z %g down: fraction %g z %g normal z %g startSolid %d\n", top, r.fraction, r.endPos[2], r.normal[2], r.startSolid);
            }
            const auto box = cw.trace(org, org, mp.mins, mp.maxs, mask, mp.passEntity);
            std::printf("standing box at origin: startSolid %d allSolid %d\n", box.startSolid, box.allSolid);
            const int seconds = argc > 6 ? std::stoi(argv[6]) : 4;
            int bad = 0;
            if (std::getenv("SPOT_CLIPS")) {
                for (int f = 0; f < 40; ++f) world.frame();
                for (const auto& c : cw.entityClips()) {
                    const float dx = c.origin[0] - eye[0], dy = c.origin[1] - eye[1];
                    if (dx * dx + dy * dy < 3000.0f * 3000.0f)
                        std::printf("CLIP id %d model %d origin (%g %g %g) mins (%g %g %g) maxs (%g %g %g) contents 0x%x\n", c.id, c.model,
                                    c.origin[0], c.origin[1], c.origin[2], c.mins[0], c.mins[1], c.mins[2], c.maxs[0], c.maxs[1], c.maxs[2], c.contents);
                }
            }
            if (const char* h = std::getenv("SPOT_SWEEP")) {
                // Standing capsules swept from 30 units above the surface to 30 below it (straight down and
                // slanted): sweeps that get through report the world surface they crossed.
                const float r = std::stof(h);
                const float step = std::getenv("SPOT_STEP") ? std::stof(std::getenv("SPOT_STEP")) : 1.0f;
                int through = 0, total = 0;
                const float dirs[5][2] = {{0, 0}, {6, 0}, {-6, 0}, {0, 6}, {0, -6}};
                for (float gx = -r; gx <= r; gx += step)
                    for (float gy = -r; gy <= r; gy += step) {
                        const iw2::game::Vec a{eye[0] + gx, eye[1] + gy, eye[2] + 1000}, b{a[0], a[1], eye[2] - 1000};
                        const auto ground = cw.trace(a, b, {0, 0, 0}, {0, 0, 0}, mask, mp.passEntity);
                        if (ground.fraction == 1 || ground.normal[2] < 0.7f) continue;
                        for (const auto& dv : dirs) {
                            const iw2::game::Vec s0{a[0] - dv[0], a[1] - dv[1], ground.endPos[2] + 30};
                            const iw2::game::Vec s1{a[0] + dv[0], a[1] + dv[1], ground.endPos[2] - 30};
                            const auto t = cw.trace(s0, s1, mp.mins, mp.maxs, mask, mp.passEntity);
                            ++total;
                            if (t.fraction == 1 && !t.startSolid && through++ < 200)
                                std::printf("THROUGH (%g %g %g) -> (%g %g %g)\n", s0[0], s0[1], s0[2], s1[0], s1[1], s1[2]);
                        }
                    }
                std::printf("%d of %d sweeps got through\n", through, total);
                return 0;
            }
            if (std::getenv("SPOT_DEBUG")) {
                // Capsule traces from the spot's origin: down by 0.25, 1, 9, 50, 400 units and a point ray.
                auto show = [&](const char* what, const iw2::game::TraceResult& t) {
                    std::printf("%s: fraction %g end z %g normal (%g %g %g) startSolid %d allSolid %d material %d\n", what, t.fraction,
                                t.endPos[2], t.normal[0], t.normal[1], t.normal[2], t.startSolid, t.allSolid, t.material);
                };
                show("position", cw.trace(org, org, mp.mins, mp.maxs, mask, mp.passEntity));
                for (float dz : {0.25f, 1.0f, 9.0f, 50.0f, 400.0f}) {
                    char name[64];
                    std::snprintf(name, sizeof name, "down %g", dz);
                    show(name, cw.trace({org[0], org[1], org[2] + 0.25f}, {org[0], org[1], org[2] - dz}, mp.mins, mp.maxs, mask, mp.passEntity));
                }
                show("point down", cw.trace(org, {org[0], org[1], org[2] - 400}, {0, 0, 0}, {0, 0, 0}, mask, mp.passEntity));
                show("point up", cw.trace(org, {org[0], org[1], org[2] + 100}, {0, 0, 0}, {0, 0, 0}, mask, mp.passEntity));
                for (float yaw : {-180.0f, 0.0f, 90.0f, -90.0f}) {
                    char name[64];
                    std::snprintf(name, sizeof name, "move yaw %g", yaw);
                    const float r = yaw * float(M_PI) / 180.0f;
                    show(name, cw.trace(org, {org[0] + 3 * std::cos(r), org[1] + 3 * std::sin(r), org[2]}, mp.mins, mp.maxs, mask, mp.passEntity));
                }
            }
            if (const char* h = std::getenv("SPOT_HOLES")) {
                // Point rays straight down on a grid: columns with no collision between eye z + 1000 and eye z − 1000.
                const float r = std::stof(h);
                int holes = 0;
                for (float gx = -r; gx <= r; gx += 4)
                    for (float gy = -r; gy <= r; gy += 4) {
                        const iw2::game::Vec a{eye[0] + gx, eye[1] + gy, eye[2] + 1000}, b{a[0], a[1], eye[2] - 1000};
                        const auto t = cw.trace(a, b, {0, 0, 0}, {0, 0, 0}, mask, mp.passEntity);
                        if (t.fraction == 1 && !t.startSolid && holes++ < 400) std::printf("HOLE %g %g\n", a[0], a[1]);
                    }
                std::printf("%d holes\n", holes);
                return 0;
            }
            // Starts: the spot itself, then the ground on a grid around it (SPOT_RADIUS, default 1024, step 128).
            std::vector<iw2::game::Vec> starts{org};
            const float radius = std::getenv("SPOT_RADIUS") ? std::stof(std::getenv("SPOT_RADIUS")) : 1024.0f;
            const float gstep = std::getenv("SPOT_STEP") ? std::stof(std::getenv("SPOT_STEP")) : 128.0f;
            const int dstep = std::getenv("SPOT_DSTEP") ? std::stoi(std::getenv("SPOT_DSTEP")) : 9;
            const float degStep = std::getenv("SPOT_DEG") ? std::stof(std::getenv("SPOT_DEG")) : 5.0f;
            const float dt = std::getenv("SPOT_DT") ? std::stof(std::getenv("SPOT_DT")) : 1.0f / 60.0f;
            for (float gx = -radius; gx <= radius; gx += gstep)
                for (float gy = -radius; gy <= radius; gy += gstep) {
                    // SPOT_TOP: start the drop from this z instead (to reach surfaces under another layer).
                    const float top = std::getenv("SPOT_TOP") ? std::stof(std::getenv("SPOT_TOP")) : eye[2] + 2000;
                    const iw2::game::Vec hi{eye[0] + gx, eye[1] + gy, top};
                    const auto g = cw.trace(hi, {hi[0], hi[1], hi[2] - 4000}, mp.mins, mp.maxs, mask, mp.passEntity);
                    if (g.fraction < 1 && !g.startSolid) starts.push_back({g.endPos[0], g.endPos[1], g.endPos[2] + 1});
                }
            std::printf("%zu starts\n", starts.size());
            for (size_t si = 0; si < starts.size(); ++si)
            for (int st = 0; st < 3; ++st)
                for (int d = -36; d < 36; d += si ? dstep : 1) {
                    iw2::game::PlayerState ps;
                    ps.origin = starts[si];
                    iw2::game::PlayerInput in;
                    in.forward = 1;
                    in.yaw = yaw0 + d * degStep;
                    in.stance = iw2::game::Stance(st);
                    // SPOT_RANDOM=1: random stance, strafe, jumps and frame times (1..16 ms) like --fall-test.
                    const bool rnd = std::getenv("SPOT_RANDOM") != nullptr;
                    const bool live = std::getenv("SPOT_LIVE") != nullptr; // run level frames (actors move)
                    std::mt19937 rng(uint32_t(si * 1000 + st * 100 + d + 50));
                    std::uniform_real_distribution<float> u(0, 1);
                    for (int f = 0; f < int(seconds / dt); ++f) {
                        in.jump = (f % 90) == 45 && st == 0;
                        float fdt = dt;
                        if (rnd) {
                            if (f % 40 == 0) {
                                in.right = u(rng) < 0.3f ? (u(rng) < 0.5f ? 1.0f : -1.0f) : 0.0f;
                                in.yaw += (u(rng) - 0.5f) * 60.0f;
                                const float sv = u(rng);
                                in.stance = sv < 0.6f ? iw2::game::Stance::Stand : sv < 0.8f ? iw2::game::Stance::Crouch : iw2::game::Stance::Prone;
                            }
                            in.jump = u(rng) < 0.03f;
                            fdt = (1 + int(u(rng) * 16)) * 0.001f;
                        }
                        const iw2::game::PlayerState before = ps;
                        iw2::game::playerMove(cw, ps, in, fdt, mp);
                        if (live && f % 3 == 0) {
                            world.setPlayerOrigin({ps.origin[0], ps.origin[1], ps.origin[2]}, {0, in.yaw, 0});
                            world.frame();
                        }
                        const iw2::game::Vec c0{before.origin[0], before.origin[1], before.origin[2] + 15};
                        const iw2::game::Vec c1{ps.origin[0], ps.origin[1], ps.origin[2] + 15};
                        const auto ray = cw.trace(c0, c1, {0, 0, 0}, {0, 0, 0}, mask, mp.passEntity);
                        const bool tunnel = ray.fraction < 1 && !ray.startSolid;
                        // Nothing under the player for 1500 units: it is below the terrain, like the reported spot.
                        bool voidBelow = false;
                        if (f % 10 == 9) {
                            const auto under = cw.trace(ps.origin, {ps.origin[0], ps.origin[1], ps.origin[2] - 1500}, {0, 0, 0}, {0, 0, 0}, mask, mp.passEntity);
                            voidBelow = under.fraction == 1 && !under.startSolid && ps.origin[2] - 1500 > floorZ;
                        }
                        if (tunnel || voidBelow || ps.origin[2] < floorZ - 64) {
                            std::printf("%s start %zu stance %d yaw %g frame %d from (%a %a %a) = (%g %g %g) to (%g %g %g) onGround %d; ray hit "
                                        "fraction %g normal (%g %g %g) material %d\n",
                                        tunnel ? "TUNNEL" : voidBelow ? "VOID" : "FELL", si, st, in.yaw, f, before.origin[0], before.origin[1],
                                        before.origin[2], before.origin[0], before.origin[1], before.origin[2], ps.origin[0], ps.origin[1],
                                        ps.origin[2], before.onGround, ray.fraction, ray.normal[0], ray.normal[1], ray.normal[2], ray.material);
                            ++bad;
                            break;
                        }
                    }
                }
            std::printf("spot test: %d bad walks\n", bad);
            return bad ? 1 : 0;
        }
        if (argc > 3 && std::string(argv[3]) == "--fall-test") {
            // Random walks from the spawn point and the path nodes. Reports a walk whose lower capsule sphere centre
            // crosses world geometry (tunnel), that ends up inside solid, or that drops below the map's lowest vertex.
            const int agents = argc > 4 ? std::stoi(argv[4]) : 50;
            const int seconds = argc > 5 ? std::stoi(argv[5]) : 60;
            iw2::game::CollisionWorld ownCw(world.bsp());
            float floorZ = 1e9f;
            for (const auto& v : world.bsp().vertices()) floorZ = std::min(floorZ, v.position[2]);
            world.start();
            // FALL_WORLD=1: trace the running level's collision (entity clips) and run its frames, like iw2view.
            const bool live = std::getenv("FALL_WORLD") != nullptr;
            const iw2::game::CollisionWorld& cw = live ? world.collision() : ownCw;
            const auto o = world.player()->obj->get("origin").asVector();
            std::vector<iw2::game::Vec> starts{{o[0], o[1], o[2]}};
            for (const auto& e : world.bsp().entities()) {
                const std::string* cls = e.get("classname");
                const std::string* org = e.get("origin");
                iw2::game::Vec v{};
                if (cls && org && cls->rfind("node_", 0) == 0 && std::sscanf(org->c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3)
                    starts.push_back(v);
            }
            iw2::game::MoveParams mp;
            if (live) mp.passEntity = world.player()->number;
            const uint32_t mask = mp.traceMask;
            int fell = 0, stuck = 0, tunnels = 0;
            auto print = [](const char* what, int a, int f, const iw2::game::Vec& p) {
                std::printf("%s agent %d frame %d at (%a %a %a) = (%g %g %g)\n", what, a, f, p[0], p[1], p[2], p[0], p[1], p[2]);
            };
            for (int a = 0; a < agents; ++a) {
                std::mt19937 rng(a + 1);
                std::uniform_real_distribution<float> u(0, 1);
                iw2::game::PlayerState ps;
                ps.origin = starts[a == 0 ? 0 : std::uniform_int_distribution<size_t>(0, starts.size() - 1)(rng)];
                ps.origin[2] += 8;
                const auto box = [&](const iw2::game::PlayerState& p) {
                    return iw2::game::Vec{mp.maxs[0], mp.maxs[1], p.stance == iw2::game::Stance::Prone ? 30.0f : p.stance == iw2::game::Stance::Crouch ? 50.0f : 70.0f};
                };
                if (cw.trace(ps.origin, ps.origin, mp.mins, box(ps), mask, mp.passEntity).startSolid) continue; // node inside solid
                iw2::game::PlayerInput in;
                bool solidSeen = false;
                for (int f = 0; f < seconds * 60; ++f) {
                    if (f % 120 == 0) {
                        in.forward = u(rng) < 0.7f ? 1.0f : u(rng) < 0.5f ? -1.0f : 0.0f;
                        in.right = u(rng) < 0.3f ? (u(rng) < 0.5f ? 1.0f : -1.0f) : 0.0f;
                        in.yaw = u(rng) * 360.0f;
                        const float s = u(rng);
                        in.stance = s < 0.8f ? iw2::game::Stance::Stand : s < 0.9f ? iw2::game::Stance::Crouch : iw2::game::Stance::Prone;
                    }
                    in.jump = u(rng) < 0.02f;
                    const iw2::game::PlayerState before = ps;
                    iw2::game::playerMove(cw, ps, in, (1 + int(u(rng) * 16)) * 0.001f, mp);
                    if (live && f % 6 == 0) {
                        world.setPlayerOrigin({ps.origin[0], ps.origin[1], ps.origin[2]}, {0, in.yaw, 0});
                        world.frame();
                    }
                    const iw2::game::Vec c0{before.origin[0], before.origin[1], before.origin[2] + 15};
                    const iw2::game::Vec c1{ps.origin[0], ps.origin[1], ps.origin[2] + 15};
                    const auto ray = cw.trace(c0, c1, {0, 0, 0}, {0, 0, 0}, mask, mp.passEntity);
                    if (ray.fraction < 1 && !ray.startSolid) {
                        print("TUNNEL", a, f, before.origin);
                        ++tunnels;
                        break;
                    }
                    if (ps.origin[2] < floorZ - 64) {
                        print("FELL", a, f, before.origin);
                        ++fell;
                        break;
                    }
                    if (!solidSeen && cw.trace(ps.origin, ps.origin, mp.mins, box(ps), mask, mp.passEntity).startSolid) {
                        print("SOLID", a, f, before.origin);
                        solidSeen = true;
                        ++stuck;
                    }
                }
            }
            std::cout << "fall test: " << agents << " agents, " << tunnels << " tunnels, " << fell << " fell, " << stuck << " in solid\n";
            return tunnels + fell ? 1 : 0;
        }
        int seconds = 10;
        bool spawnAll = false;
        int traceActor = -1;
        bool playerShoots = false;
        for (int i = 3; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--trace-actor" && i + 1 < argc) {
                traceActor = std::stoi(argv[++i]);
            } else if (a == "--player-shoots") {
                playerShoots = true; // the player fires at the nearest axis actor twice a second
            } else if (a == "--spawn-all") {
                spawnAll = true;
            } else if (a == "--cvar" && i + 1 < argc) {
                const std::string kv = argv[++i];
                const auto eq = kv.find('=');
                world.cvars[kv.substr(0, eq)] = eq == std::string::npos ? "1" : kv.substr(eq + 1);
            } else {
                seconds = std::stoi(a);
            }
        }
        world.print = [](const std::string& s) { std::cout << "[script] " << s << "\n"; };
        // Sounds are not played here; count requests and aliases that do not resolve.
        iw2::audio::SoundAliases aliases;
        aliases.load(vfs, argv[2]);
        std::mt19937 soundRng(1);
        std::map<std::string, int> soundsPlayed, soundsUnknown;
        world.playSound = [&](const std::string& alias, const iw2::game::Vec3*, float*) {
            if (aliases.pick(alias, soundRng)) ++soundsPlayed[alias];
            else ++soundsUnknown[alias];
            return 1;
        };
        const auto t0 = std::chrono::steady_clock::now();
        world.start();
        if (spawnAll) {
            std::cout << "spawned " << world.spawnAllActors() << " actors\n";
            if (traceActor >= 0) {
                int k = 0;
                for (const auto& e : world.entities())
                    if (e->isActor && k++ == traceActor) { world.vm().traceSelf = e->obj; break; }
            }
            for (const auto& e : world.entities())
                if (e->isActor) {
                    std::cout << "first actor model " << e->obj->get("model").toString() << ", attachments:";
                    for (const auto& a : e->attachments) std::cout << " " << a.model << "@" << a.tag;
                    std::cout << "\n";
                    break;
                }
        }
        int playerHits = 0;
        for (int f = 0; f < seconds * 20; ++f) {
            if (playerShoots && f % 10 == 0 && world.player()) {
                const auto eye = iw2::script::Vec3{world.player()->obj->get("origin").asVector()[0], world.player()->obj->get("origin").asVector()[1],
                                              world.player()->obj->get("origin").asVector()[2] + 60.0f};
                iw2::game::Entity* best = nullptr;
                float bestD = 1e30f;
                for (const auto& e : world.entities()) {
                    if (!e->isActor || !world.isAlive(*e) || e->obj->get("team").toString() != "axis" || !world.canSee(*world.player(), *e)) continue;
                    const auto o = e->obj->get("origin").asVector();
                    const float d = (o[0] - eye[0]) * (o[0] - eye[0]) + (o[1] - eye[1]) * (o[1] - eye[1]);
                    if (d < bestD) { bestD = d; best = e.get(); }
                }
                if (best) {
                    const auto o = best->obj->get("origin").asVector();
                    iw2::script::Vec3 dir{o[0] - eye[0], o[1] - eye[1], o[2] + 40.0f - eye[2]};
                    const float l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                    for (auto& c : dir) c /= l;
                    const int before = world.stats.hits;
                    world.playerFire(eye, dir);
                    playerHits += world.stats.hits - before;
                }
            }
            world.frame();
        }
        if (playerShoots) std::cout << "player hits " << playerHits << "\n";
        for (int i = 0; i < 16; ++i) {
            const auto& o = world.objectives()[size_t(i)];
            if (o.state) std::cout << "objective " << i << " state " << o.state << " " << o.text << "\n";
        }
        if (auto ch = world.dueLevelChange()) std::cout << "level change to " << ch->map << (ch->failed ? " (failed)" : "") << "\n";
        {
            int played = 0;
            for (const auto& [k, n] : soundsPlayed) played += n;
            std::cout << "sound aliases " << aliases.size() << "; sounds started " << played << " (" << soundsPlayed.size()
                      << " aliases)";
            for (const auto& [k, n] : soundsUnknown) std::cout << "; unknown " << k << " x" << n;
            std::cout << "\n";
        }
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        std::cout << "simulated " << seconds << " s in " << wall << " s; entities " << world.entities().size() << ", nodes "
                  << world.nodes().size() << ", live threads " << world.vm().liveThreadCount() << ", runtime errors "
                  << world.vm().errors().size() << "\n";
        {
            std::map<std::string, int> states;
            const iw2::game::Entity* mover = nullptr;
            for (const auto& e : world.entities())
                if (e->isActor && !e->obj->removed) {
                    ++states[e->animScript];
                    if (!mover && e->animScript == "move") mover = e.get();
                }
            int alive = 0, dead = 0;
            for (const auto& e : world.entities())
                if (e->isActor) (world.isAlive(*e) ? alive : dead)++;
            std::cout << "actors alive " << alive << ", dead " << dead << "; player health " << world.player()->obj->get("health").toString() << "\n";
            std::cout << "shots " << world.stats.shots << ", hits " << world.stats.hits << ", kills " << world.stats.kills << "; grenades "
                      << world.stats.grenades << ", exploded " << world.stats.grenadeExplosions << "\n";
            std::cout << "actor animscripts:";
            for (const auto& [k, v] : states) std::cout << " " << k << "=" << v;
            std::cout << "\n";
            for (const auto& e : world.entities())
                if (e->isActor && e->animScript == "combat" && e->anim) {
                    std::cout << "combat actor anim state:\n" << e->anim->debugString();
                    break;
                }
            if (mover) std::cout << "a moving actor is at " << mover->obj->get("origin").toString() << "\n";
        }
        {
            std::map<std::string, int> waits;
            for (const auto& d : world.vm().describeThreads())
                if (d.rfind("animscripts/", 0) == 0 && d.find("battlechatter") == std::string::npos && d.find("init.gsc") == std::string::npos && d.find("squadmanager") == std::string::npos) ++waits[d];
            std::vector<std::pair<int, std::string>> top;
            for (const auto& [k, v] : waits) top.push_back({v, k});
            std::sort(top.rbegin(), top.rend());
            std::cout << "top animscript waits:\n";
            for (size_t i = 0; i < top.size() && i < 15; ++i) std::cout << "  " << top[i].first << " " << top[i].second << "\n";
        }
        std::map<std::string, int> errs;
        for (const auto& e : world.vm().errors()) ++errs[e];
        int shown = 0;
        for (const auto& [e, n] : errs)
            if (shown++ < 25) std::cout << "  error x" << n << ": " << e << "\n";
        std::vector<std::pair<int, std::string>> top;
        for (const auto& [f, n] : world.missingBuiltins()) top.push_back({n, f});
        std::sort(top.rbegin(), top.rend());
        std::cout << "unimplemented built-ins called: " << top.size() << "\n";
        for (size_t i = 0; i < top.size() && i < 60; ++i) std::cout << "  " << top[i].second << " x" << top[i].first << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
