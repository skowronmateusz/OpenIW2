#include <array>
// iw2game: run a level headless (scripts, entities, no rendering).
//
//   iw2game <main-dir> <map> [seconds] [--cvar name=value ...] [--spawn-all] [--move-stats] …
//   iw2game <main-dir> <map> --walk-test     drop the player at the spawn point and walk forward
//   iw2game <main-dir> <map> --fall-test [walks] [seconds]   random player walks (tunnels, falls, solid)
//   iw2game <main-dir> <map> --replay file      re-run moves recorded by iw2view (pmove_replay.bin)
//   iw2game <main-dir> <map> --mantle-test [runs]   mantle from every path node in 16 directions
//   iw2game <main-dir> <map> --spot x,y,z yaw   replay an iw2view `P` view position: walk from it in 72 directions

#include "assets/xanim.h"
#include "audio/sound.h"
#include "game/pmove.h"
#include "game/pmoverecord.h"
#include "game/clientinput.h"
#include "game/playerweapon.h"
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
        if (const char* overlay = std::getenv("IW2_OVERLAY")) vfs.mountDirectory(overlay); // debug: loose test scripts override the game files
        iw2::game::World world(vfs, argv[2]);
        if (argc > 3 && std::string(argv[3]) == "--item-test") {
            // Touch pickup of weapon items: items are spawned at the player's origin one after another.
            world.start();
            for (int i = 4; i < argc; ++i) {
                std::string w = argv[i];
                if (const size_t eq = w.find('='); eq != std::string::npos) { // name=N: set the clip first
                    world.debugSetClip(w.substr(0, eq), std::stoi(w.substr(eq + 1)));
                    w.resize(eq);
                    std::cout << w << " clip set to " << world.debugAmmo(w).second << "\n";
                    continue;
                }
                world.debugSpawnItem(w);
                world.frame();
                int left = 0;
                for (const auto& e : world.entities())
                    if (!e->obj->removed && e->classname == "weapon_" + w) ++left;
                const auto a = world.debugAmmo(w);
                std::cout << w << ": stock " << a.first << " clip " << a.second << ", items left " << left << "\n";
            }
            return 0;
        }
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
            if (const char* h = std::getenv("SPOT_STAND")) {
                // The player standing still on the topmost surface (0.125 above it) on a fine grid: positions the
                // position test calls all solid, and positions after which nothing is under the player.
                const float r = std::stof(h);
                const float step = std::getenv("SPOT_STEP") ? std::stof(std::getenv("SPOT_STEP")) : 1.0f;
                int solid = 0, lost = 0, total = 0;
                for (float gx = -r; gx <= r; gx += step)
                    for (float gy = -r; gy <= r; gy += step) {
                        const iw2::game::Vec a{eye[0] + gx, eye[1] + gy, eye[2] + 1000}, b{a[0], a[1], eye[2] - 1000};
                        const auto ground = cw.trace(a, b, {0, 0, 0}, {0, 0, 0}, mask, mp.passEntity);
                        if (ground.fraction == 1 || ground.normal[2] < 0.7f) continue;
                        iw2::game::PlayerState ps;
                        ps.origin = {a[0], a[1], ground.endPos[2] + 0.125f};
                        ++total;
                        if (cw.trace(ps.origin, ps.origin, mp.mins, mp.maxs, mask, mp.passEntity).allSolid && solid++ < 50)
                            std::printf("ALLSOLID (%g %g %g)\n", ps.origin[0], ps.origin[1], ps.origin[2]);
                        iw2::game::PlayerInput in;
                        for (int f = 0; f < 30; ++f) iw2::game::playerMove(cw, ps, in, 1.0f / 60.0f, mp);
                        const auto under = cw.trace(ps.origin, {ps.origin[0], ps.origin[1], ps.origin[2] - 1500}, {0, 0, 0}, {0, 0, 0}, mask, mp.passEntity);
                        if (under.fraction == 1 && !under.startSolid && lost++ < 50)
                            std::printf("LOST from (%g %g %g) to (%g %g %g)\n", a[0], a[1], ground.endPos[2] + 0.125f, ps.origin[0], ps.origin[1], ps.origin[2]);
                    }
                std::printf("%d positions: %d all solid, %d lost\n", total, solid, lost);
                return 0;
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
                            world.playerPmEvents(ps.eventSeq, ps.events, ps.stance == iw2::game::Stance::Crouch);
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
        if (argc > 3 && std::string(argv[3]) == "--mantle-test") {
            // From every path node in 16 view directions: one standing step with the mantle button. Counts where a
            // mantle is offered (hint) and starts, runs the first `runs` to the end and checks the end position.
            const int runs = argc > 4 ? std::stoi(argv[4]) : 10;
            std::vector<iw2::assets::XAnim> owned;
            owned.reserve(11);
            iw2::game::MantleAnims anims;
            for (int i = 1; i < 11; ++i)
                if (const auto d = vfs.read(std::string("xanim/") + iw2::game::mantleAnimName(i))) {
                    owned.push_back(iw2::assets::parseXAnim(*d));
                    anims.anims[size_t(i)] = &owned.back();
                }
            if (!anims.loaded()) { std::cout << "mantle test: animations missing\n"; return 1; }
            iw2::game::CollisionWorld cw(world.bsp());
            iw2::game::MoveParams mp;
            mp.mantle = &anims;
            int hints = 0, starts = 0, ran = 0, bad = 0;
            for (const auto& e : world.bsp().entities()) {
                const std::string* cls = e.get("classname");
                const std::string* org = e.get("origin");
                iw2::game::Vec v{};
                if (!cls || !org || cls->rfind("node_", 0) != 0 || std::sscanf(org->c_str(), "%f %f %f", &v[0], &v[1], &v[2]) != 3) continue;
                for (int d = 0; d < 16; ++d) {
                    iw2::game::PlayerState ps;
                    ps.origin = {v[0], v[1], v[2] + 1};
                    if (cw.trace(ps.origin, ps.origin, mp.mins, mp.maxs, mp.traceMask).startSolid) break;
                    iw2::game::PlayerInput in;
                    in.yaw = float(d) * 22.5f;
                    iw2::game::playerMove(cw, ps, in, 0.016f, mp); // settle
                    in.jump = true;
                    const iw2::game::Vec from = ps.origin;
                    iw2::game::playerMove(cw, ps, in, 0.016f, mp);
                    if (ps.mantleFlags & 8) ++hints;
                    if (!ps.mantling) continue;
                    ++starts;
                    if (ran >= runs) continue;
                    ++ran;
                    in.jump = false;
                    int ms = 16;
                    iw2::game::Stance stance = ps.stance;
                    for (; ps.mantling && ms < 5000; ms += 16) {
                        if (ps.stanceEvent == 1) in.stance = iw2::game::Stance::Crouch;
                        if (ps.stanceEvent == 0) in.stance = iw2::game::Stance::Stand;
                        iw2::game::playerMove(cw, ps, in, 0.016f, mp);
                        if (std::getenv("MANTLE_TRACE")) std::printf("  t %d (%g %g %g) mantling %d\n", ps.mantleTimeMs, ps.origin[0], ps.origin[1], ps.origin[2], ps.mantling);
                        stance = ps.stance;
                    }
                    const iw2::game::Vec box{mp.maxs[0], mp.maxs[1], stance == iw2::game::Stance::Crouch ? 50.0f : 70.0f};
                    const bool solid = cw.trace(ps.origin, ps.origin, mp.mins, box, mp.traceMask).startSolid;
                    if (solid || ps.mantling) ++bad;
                    std::printf("mantle from (%g %g %g) yaw %g: trans %d flags %u, %d ms, up %g, to (%g %g %g)%s%s\n", from[0], from[1], from[2],
                                in.yaw, ps.mantleTrans, ps.mantleFlags, ms, ps.origin[2] - from[2], ps.origin[0], ps.origin[1], ps.origin[2],
                                solid ? (cw.trace(ps.origin, ps.origin, mp.mins, {15, 15, 50}, mp.traceMask).startSolid ? " IN SOLID (also 50)" : " IN SOLID (70 only)") : "", ps.mantling ? " NOT DONE" : "");
                }
            }
            std::printf("mantle test: %d hints, %d starts, %d run, %d bad\n", hints, starts, ran, bad);
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
        if (argc > 3 && std::string(argv[3]) == "--lookat-test") {
            // For each trigger_lookat: search a view direction from the player's eye whose look trace (mask 0x2280e801)
            // first hits it, then run frames looking there (expect touches) and in the opposite direction (expect none).
            world.start();
            world.frame();
            int fail = 0, tested = 0;
            for (const auto& ep : world.entities()) {
                if (ep->classname != "trigger_lookat" || ep->obj->removed) continue;
                const auto o = world.player()->obj->get("origin").asVector();
                iw2::game::World::PlayerCommand cmd;
                const iw2::game::Vec3 eye{o[0], o[1], o[2] + cmd.viewHeight};
                iw2::game::Vec3 standAt{};
                bool found = false;
                float fp = 0, fy = 0;
                {
                    std::printf("trigger_lookat #%d: model %s brush contents 0x%x\n", ep->number, ep->obj->get("model").toString().c_str(),
                                world.collision().modelContents(std::atoi(ep->obj->get("model").toString().c_str() + 1)));
                    // Where is the brush? Ray search with only the lookat contents; stand 150 units in front of it.
                    for (float pitch = -80; pitch <= 80 && !found; pitch += 2)
                        for (float yaw = 0; yaw < 360 && !found; yaw += 2) {
                            const float cp = std::cos(pitch * 3.14159265f / 180), sp = std::sin(pitch * 3.14159265f / 180);
                            const float cy = std::cos(yaw * 3.14159265f / 180), sy = std::sin(yaw * 3.14159265f / 180);
                            const iw2::game::Vec3 end{eye[0] + 15000 * cp * cy, eye[1] + 15000 * cp * sy, eye[2] - 15000 * sp};
                            const iw2::game::Vec3 mo = ep->obj->get("origin").isVector() ? ep->obj->get("origin").asVector() : iw2::game::Vec3{0, 0, 0};
                            const iw2::game::Vec3 ma = ep->obj->get("angles").isVector() ? ep->obj->get("angles").asVector() : iw2::game::Vec3{0, 0, 0};
                            const auto tr = world.collision().traceModel(std::atoi(ep->obj->get("model").toString().c_str() + 1), mo, ma, eye, end, {0, 0, 0}, {0, 0, 0}, 1u);
                            if (tr.fraction < 1.0f) {
                                found = true;
                                standAt = {tr.endPos[0] - cp * cy * 120.0f, tr.endPos[1] - cp * sy * 120.0f, tr.endPos[2] + sp * 120.0f - cmd.viewHeight};
                                std::printf("trigger_lookat #%d: lookat-only ray from the spawn eye hits at (%.0f %.0f %.0f)\n", ep->number, tr.endPos[0], tr.endPos[1], tr.endPos[2]);
                            }
                        }
                    if (!found) std::printf("trigger_lookat #%d: not hit by a lookat-contents ray at all\n", ep->number);
                    if (found) world.setPlayerOrigin(standAt, world.player()->obj->get("angles").asVector());
                    found = false;
                }
                const auto o2 = world.player()->obj->get("origin").asVector();
                const iw2::game::Vec3 eye2{o2[0], o2[1], o2[2] + cmd.viewHeight};
                for (float pitch = -60; pitch <= 60 && !found; pitch += 2)
                    for (float yaw = 0; yaw < 360 && !found; yaw += 2) {
                        const float cp = std::cos(pitch * 3.14159265f / 180), sp = std::sin(pitch * 3.14159265f / 180);
                        const float cy = std::cos(yaw * 3.14159265f / 180), sy = std::sin(yaw * 3.14159265f / 180);
                        const iw2::game::Vec3 end{eye2[0] + 15000 * cp * cy, eye2[1] + 15000 * cp * sy, eye2[2] - 15000 * sp};
                        const auto tr = world.collision().trace(eye2, end, {0, 0, 0}, {0, 0, 0}, 0x2280e801u, world.player()->number);
                        if (tr.entity == ep->number) { found = true; fp = pitch; fy = yaw; }
                    }
                ++tested;
                if (!found) {
                    std::printf("trigger_lookat #%d: no direction from the spawn eye hits it (blocked or out of view)\n", ep->number);
                    continue;
                }
                const int b0 = world.stats.lookatTouches;
                cmd.viewAngles = {fp, fy, 0};
                world.setPlayerCommand(cmd);
                for (int f = 0; f < 10; ++f) world.frame();
                const int looked = world.stats.lookatTouches - b0;
                cmd.viewAngles = {fp, fy + 180.0f, 0};
                world.setPlayerCommand(cmd);
                const int b1 = world.stats.lookatTouches;
                for (int f = 0; f < 10; ++f) world.frame();
                const int away = world.stats.lookatTouches - b1;
                std::printf("trigger_lookat #%d: look (pitch %.0f yaw %.0f) touches %d, away touches %d\n", ep->number, fp, fy, looked, away);
                if (looked == 0 || away != 0) ++fail;
            }
            std::printf("lookat test: %d trigger_lookat entities, %d failed\n", tested, fail);
            return fail ? 1 : 0;
        }
        int seconds = 10;
        bool spawnAll = false;
        int traceActor = -1;
        std::string throwClass;
        bool playerShoots = false;
        bool shootBarrels = false; // --shoot-barrels: from 1 s the player's bullet hits each explodable barrel from 60 units, twice a second
        bool drive = false;
        bool controls = false;
        int turretTest = -1; // --turret N: stand behind the N-th usable turret, F at 1 s, aim + fire 2–3 s, F at 4 s
        iw2::game::Entity* testTurret = nullptr;
        bool pmove = false;
        iw2::game::PlayerState pmoveState;
        bool pmoveInit = false;
        std::vector<std::pair<float, iw2::game::Vec3>> playerPath; // --player-at T x,y,z: player origin from time T (linear between)
        bool moveStats = false;
        std::vector<std::string> menuAnswers; // --answer-menus a,b: respond to the script menus that open, in order (headless test)
        size_t menuAnswered = 0;
        float fireVehiclesAt = std::getenv("IW2_FIRE_VEHICLES") ? float(std::atof(std::getenv("IW2_FIRE_VEHICLES"))) : -1.0f;
        float touchTestAt = std::getenv("IW2_TOUCH_TEST") ? float(std::atof(std::getenv("IW2_TOUCH_TEST"))) : -1.0f, touchReportAt = -1.0f;
        bool touchTestIsRocket = false;
        float combatSceneAt = std::getenv("IW2_COMBAT_SCENE") ? float(std::atof(std::getenv("IW2_COMBAT_SCENE"))) : -1.0f;
        decltype(world.player()) sceneA = nullptr, sceneB = nullptr;
        int touchBefore = 0;
        decltype(world.player()) touchTarget = nullptr;
        if (std::getenv("IW2_ROCKET_TEST")) {
            touchTestAt = float(std::atof(std::getenv("IW2_ROCKET_TEST")));
            touchTestIsRocket = true;
        }
        float killReportAt = -1.0f, fireVehiclesReportAt = -1.0f;
        float killVehiclesAt = std::getenv("IW2_KILL_VEHICLES") ? float(std::atof(std::getenv("IW2_KILL_VEHICLES"))) : -1.0f;
        std::vector<std::pair<float, std::array<float, 2>>> playerLooks, playerFires; // --player-look / --player-fire T yaw,pitch
        std::vector<std::pair<float, iw2::game::Vec3>> playerFireAts; // --player-fire-at T x,y,z: one bullet from the eye at the point
        std::vector<std::pair<float, iw2::game::Vec3>> playerMelees;  // --player-melee T x,y,z: one melee blow looking at the point
        std::vector<std::pair<float, float>> playerAds;               // --player-ads T frac: the aim fraction from T
        std::vector<std::pair<float, std::string>> playerWeapons;     // --player-weapon T name: the player's current weapon from T
        std::vector<std::pair<float, std::array<float, 2>>> playerUses; // --player-use T yaw,pitch: press the use key once at T looking that way (headless test)
        std::vector<std::pair<float, std::string>> levelNotifies; // --level-notify T name: level notify at T seconds (headless test)
        for (int i = 3; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--trace-actor" && i + 1 < argc) {
                traceActor = std::stoi(argv[++i]);
            } else if (a == "--move-stats") {
                moveStats = true; // speed of actors moving along a path (anim mode 1, movemode walk/run)
            } else if (a == "--shoot-barrels") {
                shootBarrels = true;
            } else if (a == "--player-shoots") {
                playerShoots = true; // the player fires at the nearest axis actor twice a second
            } else if (a == "--answer-menus" && i + 1 < argc) {
                std::string list = argv[++i];
                for (size_t p = 0; p <= list.size();) {
                    const size_t q = std::min(list.find(',', p), list.size());
                    menuAnswers.push_back(list.substr(p, q - p));
                    p = q + 1;
                }
            } else if (a == "--spawn-all") {
                spawnAll = true;
            } else if (a == "--player-throw" && i + 1 < argc) {
                throwClass = argv[++i]; // frag | smoke: +frag / +smoke held 1 s .. 2 s through PM_Weapon (implies --controls)
                controls = true;
            } else if (a == "--controls") {
                controls = true; // default.cfg keys through the client commands and PM_Weapon: 1, SHIFT, G, C, SPACE
            } else if (a == "--turret" && i + 1 < argc) {
                turretTest = std::stoi(argv[++i]);
            } else if (a == "--drive") {
                drive = true; // in a vehicle: forward, turn right, turn to the view (jump), fire; prints the vehicle each second
            } else if (a == "--level-notify" && i + 2 < argc) {
                const float t = std::stof(argv[++i]);
                levelNotifies.push_back({t, argv[++i]});
            } else if ((a == "--player-look" || a == "--player-fire") && i + 2 < argc) {
                const float t = std::stof(argv[++i]);
                std::array<float, 2> ang{};
                std::sscanf(argv[++i], "%f,%f", &ang[0], &ang[1]);
                (a == "--player-look" ? playerLooks : playerFires).push_back({t, ang});
            } else if (a == "--player-fire-at" && i + 2 < argc) {
                const float t = std::stof(argv[++i]);
                iw2::game::Vec3 p{};
                std::sscanf(argv[++i], "%f,%f,%f", &p[0], &p[1], &p[2]);
                playerFireAts.push_back({t, p});
            } else if (a == "--player-melee" && i + 2 < argc) {
                const float t = std::stof(argv[++i]);
                iw2::game::Vec3 p{};
                std::sscanf(argv[++i], "%f,%f,%f", &p[0], &p[1], &p[2]);
                playerMelees.push_back({t, p});
            } else if (a == "--player-ads" && i + 2 < argc) {
                const float t = std::stof(argv[++i]);
                playerAds.push_back({t, std::stof(argv[++i])});
            } else if (a == "--player-weapon" && i + 2 < argc) {
                const float t = std::stof(argv[++i]);
                playerWeapons.push_back({t, argv[++i]});
            } else if (a == "--player-use" && i + 2 < argc) {
                const float t = std::stof(argv[++i]);
                std::array<float, 2> ang{};
                std::sscanf(argv[++i], "%f,%f", &ang[0], &ang[1]);
                playerUses.push_back({t, ang});
            } else if (a == "--player-at" && i + 2 < argc) {
                iw2::game::Vec3 p{};
                const float t = std::stof(argv[++i]);
                std::sscanf(argv[++i], "%f,%f,%f", &p[0], &p[1], &p[2]);
                playerPath.push_back({t, p});
            } else if (a == "--pmove") {
                pmove = true; // the player stands still under pmove (pushed by entities, falls) as in iw2view
            } else if (a == "--mixer") {
            } else if (a == "--cvar" && i + 1 < argc) {
                const std::string kv = argv[++i];
                const auto eq = kv.find('=');
                world.cvars[kv.substr(0, eq)] = eq == std::string::npos ? "1" : kv.substr(eq + 1);
            } else {
                seconds = std::stoi(a);
            }
        }
        // The menu default of the saved cvar (50 digits); maps/_endmission.gsc indexes it at the end of a mission.
        world.cvars.emplace("mis_difficulty", std::string(50, '0'));
        world.print = [](const std::string& s) { std::cout << "[script] " << s << "\n"; };
        // Sounds are not played here; count requests and aliases that do not resolve.
        iw2::audio::SoundAliases aliases;
        aliases.load(vfs, argv[2]);
        std::mt19937 soundRng(1);
        std::map<std::string, int> soundsPlayed, soundsUnknown;
        // --mixer: start sounds in an (unheard) mixer as iw2view does, so scripts get the real sound lengths.
        std::unique_ptr<iw2::audio::Mixer> mixer;
        for (int i = 3; i < argc; ++i)
            if (std::string(argv[i]) == "--mixer") mixer = std::make_unique<iw2::audio::Mixer>(vfs, aliases, 44100);
        const bool soundLog = std::getenv("IW2_SOUND_LOG") != nullptr; // debug: every sound start with its sim time
        world.soundExists = [&](const std::string& n) { return aliases.exists(n); };
        world.playSound = [&](const std::string& alias, const iw2::game::Vec3* origin, float* duration) {
            if (soundLog) std::cout << "[sound " << world.timeMs() << "ms] " << alias << "\n";
            if (mixer) return mixer->play(alias, origin ? std::optional<iw2::audio::Vec3>(*origin) : std::nullopt, duration);
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
        struct MoveStat { int frames = 0, stalls = 0; double sum = 0; float max = 0; };
        std::map<int, MoveStat> moveStatsBy;
        iw2::game::ClientInput input;
        iw2::game::PlayerWeapon pweapon;
        std::string weaponSelect;
        std::map<int, int> weaponEvents;
        if (controls) {
            if (const auto cfg = vfs.read("default.cfg")) input.loadConfig(std::string(cfg->begin(), cfg->end()));
            pweapon.lookup = [&](const std::string& n) { return world.weapon(n); };
            pweapon.findOffhand = [&](const std::string& cls) { return world.playerOffhandFor(cls); };
        }
        iw2::game::Vec3 playerLookAngles{};
        for (int f = 0; f < seconds * 20; ++f) {
            // IW2_HEALTH_LOG=1: the player's health and the low-health overlay hud element's alpha every 0.5 s.
            if (std::getenv("IW2_HEALTH_LOG") && world.player() && f % 10 == 0) {
                float overlayAlpha = -1;
                for (const auto& h : world.hudElems())
                    if (h->get("$shader").toString() == "overlay_low_health") overlayAlpha = h->get("alpha").isNumber() ? h->get("alpha").asFloat() : -2;
                const auto lvl = world.vm().level();
                const auto fl = lvl->get("flag");
                std::printf("t=%.1f dmgEvent %d yaw %d health %s overlay alpha %g cutoff %s gameskill %s flag %s\n", f * 0.05f,
                            world.playerDamageFeedback().event, world.playerDamageFeedback().yaw, world.player()->obj->get("health").toString().c_str(), overlayAlpha,
                            lvl->get("healthoverlaycutoff").toString().c_str(), lvl->get("gameskill").toString().c_str(),
                            fl.isObject() ? fl.asObject()->get("player_has_red_flashing_overlay").toString().c_str() : "?");
            }
            if (controls && world.player()) {
                // A key script: weapnext at 1 s, melee at 3 s, frag held 5–6 s, crouch at 8 s, stand up at 9 s.
                const std::map<int, std::pair<const char*, bool>> keys{
                    {20, {"1", true}},     {21, {"1", false}},    {60, {"SHIFT", true}}, {62, {"SHIFT", false}},
                    {100, {"G", true}},    {120, {"G", false}},   {160, {"C", true}},    {161, {"C", false}},
                    {180, {"SPACE", true}}, {181, {"SPACE", false}}};
                if (!throwClass.empty()) {
                    const char* key = throwClass == "smoke" ? "4" : "G";
                    if (f == 20) input.key(key, true);
                    if (f == 40) input.key(key, false);
                } else if (auto k = keys.find(f); k != keys.end()) input.key(k->second.first, k->second.second);
                const uint32_t b = input.buttons(false);
                const auto w = world.player()->obj->get("$weapon");
                if (weaponSelect.empty() || (w.isString() && w.asString() != pweapon.name() && pweapon.equipped() && f % 20 == 0))
                    weaponSelect = w.isString() && !w.asString().empty() ? w.asString() : "none";
                if (!pweapon.equipped())
                    if (const auto* d = world.weapon(weaponSelect)) pweapon.equip(weaponSelect, *d);
                if (input.takeWeaponCycle()) {
                    const auto& inv = world.inventory(*world.player());
                    const std::string& other = inv.slot[inv.slot[2] == weaponSelect ? 1 : 2];
                    if (!other.empty()) weaponSelect = other;
                }
                iw2::game::PlayerWeapon::Input wi;
                wi.fire = b & iw2::game::Button::Attack;
                wi.melee = b & iw2::game::Button::Melee;
                wi.frag = b & iw2::game::Button::Frag;
                wi.smoke = b & iw2::game::Button::Smoke;
                wi.weapon = weaponSelect;
                // CONTROLS_SUBSTEPS=n: n weapon updates per 50 ms server frame, like iw2view's per-frame updates.
                {
                    static const int sub = std::getenv("CONTROLS_SUBSTEPS") ? std::max(1, std::atoi(std::getenv("CONTROLS_SUBSTEPS"))) : 1;
                    for (int k = 0; k < sub; ++k) pweapon.update(0.05f / float(sub), wi);
                }
                if (pweapon.equipped()) world.player()->obj->set("$weapon", iw2::script::Value(pweapon.name()));
                const auto o = world.player()->obj->get("origin").asVector();
                const iw2::game::Vec3 eye{o[0], o[1], o[2] + 60.0f};
                for (const auto& e : pweapon.takeEvents()) {
                    ++weaponEvents[int(e.kind)];
                    if (e.kind == iw2::game::PlayerWeapon::Event::MeleeHit) world.playerMelee(eye, {0, 0, 0});
                    if (e.kind == iw2::game::PlayerWeapon::Event::OffhandThrow) world.playerThrowGrenade(e.weapon, eye, {-20, 0, 0}, {0, 0, 0});
                    if (e.kind == iw2::game::PlayerWeapon::Event::OffhandExplode) world.playerGrenadeInHand(eye);
                    std::printf("controls t=%.2f event %d %s (state %d, weapon %s, stance %d)\n", f * 0.05f, int(e.kind),
                                e.weapon.c_str(), pweapon.stateNumber(), pweapon.name().c_str(), input.stance());
                }
                if (!throwClass.empty()) {
                    static int seenG = 0, seenX = 0;
                    if (world.stats.grenades != seenG || world.stats.grenadeExplosions != seenX) {
                        seenG = world.stats.grenades;
                        seenX = world.stats.grenadeExplosions;
                        std::printf("throw t=%.2f grenades thrown %d exploded %d\n", f * 0.05f, seenG, seenX);
                    }
                }
                if (f == 170 || f == 190) std::printf("controls t=%.2f buttons %x stance %d\n", f * 0.05f, b, input.stance());
            }
            if (shootBarrels && f >= 20 && f % 10 == 0 && world.player()) {
                for (const auto& e : world.entities()) {
                    if (e->obj->removed || e->classname != "script_model" || e->obj->get("targetname").toString() != "explodable_barrel") continue;
                    if (e->obj->get("model").toString().find("prop_barrel_benzin") == std::string::npos) continue;
                    const auto o = e->obj->get("origin").asVector();
                    const int before = world.stats.hits;
                    static const float dirs[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}}; // shoot from +x, +y, −x, −y until one hits
                    for (const auto& d : dirs) {
                        world.playerFire({o[0] + 60.0f * d[0], o[1] + 60.0f * d[1], o[2] + 30.0f}, {-d[0], -d[1], 0});
                        if (world.stats.hits != before) break;
                    }
                    if (world.stats.hits == before) std::printf("barrel %d: shot missed at t=%.2f\n", e->number, f * 0.05f);
                }
            }
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
            if (turretTest >= 0 && world.player()) {
                iw2::game::World::PlayerCommand cmd;
                if (f == 19) {
                    const auto p0 = world.player()->obj->get("origin").asVector();
                    std::printf("turret: player start %.1f %.1f %.1f\n", p0[0], p0[1], p0[2]);
                    int k = 0;
                    for (const auto& e : world.entities())
                        if (e->turret && !e->obj->removed && (e->turret->flags & 0x1000u) && !e->turret->inUse && k++ == turretTest) {
                            testTurret = e.get();
                            break;
                        }
                    if (!testTurret) std::printf("turret: no usable turret %d\n", turretTest);
                }
                if (testTurret) {
                    const auto to = testTurret->obj->get("origin").asVector(), ta = testTurret->obj->get("angles").asVector();
                    const float yr = ta[1] * 0.0174532924f;
                    cmd.viewAngles = {0, ta[1], 0};
                    if (f == 19) {
                        // Stand on the ground 34 units behind the gun (where the turret code would put the player).
                        const iw2::game::Vec start{to[0] - std::cos(yr) * 34.0f, to[1] - std::sin(yr) * 34.0f, to[2]};
                        const auto tr = world.collision().trace(start, {start[0], start[1], start[2] - 200.0f}, {-15, -15, 0}, {15, 15, 70},
                                                               iw2::game::Contents::PlayerClipMask, world.player()->number);
                        world.setPlayerOrigin({tr.endPos[0], tr.endPos[1], tr.endPos[2]}, {0, ta[1], 0});
                        std::printf("turret %d at %.1f %.1f %.1f yaw %.1f: player at %.1f %.1f %.1f\n", testTurret->number, to[0], to[1],
                                    to[2], ta[1], tr.endPos[0], tr.endPos[1], tr.endPos[2]);
                    }
                    if (f == 20 || f == 80) cmd.buttons |= 8;                    // F
                    if (f >= 40 && f < 80) cmd.viewAngles = {10.0f, ta[1] + 80.0f, 0}; // past any arc
                    if (f >= 40 && f < 60) cmd.buttons |= 1;                    // fire
                    if (f == 21 || f == 39 || f == 45 || f == 59 || f == 81) {
                        float cp = cmd.viewAngles[0], cy = cmd.viewAngles[1];
                        world.clampPlayerView(cp, cy);
                        iw2::game::Vec3 eye{};
                        const bool hasEye = world.playerTurretEye(eye);
                        const auto po = world.player()->obj->get("origin").asVector();
                        std::printf("turret t=%.2f on %d inUse %d player %.1f %.1f %.1f eye %s %.1f %.1f %.1f view clamp %.1f %.1f turret "
                                    "angles %s shots %d\n",
                                    f * 0.05f, world.playerOnTurret(), testTurret->turret->inUse, po[0], po[1], po[2], hasEye ? "yes" : "no",
                                    eye[0], eye[1], eye[2], cp, cy, testTurret->obj->get("$turretangles").toString().c_str(), world.stats.shots);
                    }
                    if (const auto rel = world.takeTurretRelease())
                        std::printf("turret t=%.2f released: origin %.1f %.1f %.1f stance %d\n", f * 0.05f, rel->origin[0], rel->origin[1],
                                    rel->origin[2], rel->stance);
                }
                world.setPlayerCommand(cmd);
            }
            if (drive && world.player()) {
                iw2::game::World::PlayerCommand cmd;
                if (world.player()->usingVehicle >= 0) {
                    static int t = 0;
                    const int phase = t / 60; // 3 s each
                    cmd.forward = phase < 3 ? 127 : 0;
                    cmd.right = phase == 1 ? 127 : 0;
                    cmd.viewAngles = {0, 90, 0};
                    if (phase == 2) cmd.buttons |= 0x400;
                    if (phase == 3 && t % 20 == 0) cmd.buttons |= 1;
                    if (t % 20 == 0) {
                        const auto* v = world.entities()[0].get();
                        for (const auto& e : world.entities())
                            if (e->number == world.player()->usingVehicle) v = e.get();
                        const auto o = v->obj->get("origin").asVector(), a = v->obj->get("angles").asVector();
                        const auto ta = v->obj->get("$turretangles");
                        std::printf("drive t=%.1f phase %d: origin %.1f %.1f %.1f angles %.1f %.1f %.1f speed %.1f turret %s flags %x\n",
                                    t * 0.05f, phase, o[0], o[1], o[2], a[0], a[1], a[2], v->vehicle ? v->vehicle->forwardSpeed : 0.0f,
                                    ta.toString().c_str(), world.playerPmFlags());
                        if (v->vehicle) std::printf("  velocity %.1f %.1f %.1f\n", v->vehicle->velocity[0], v->vehicle->velocity[1], v->vehicle->velocity[2]);
                    }
                    ++t;
                }
                world.setPlayerCommand(cmd);
            }
            if (killVehiclesAt >= 0 && f * 0.05f >= killVehiclesAt) { // IW2_KILL_VEHICLES=<s>: 100000 damage to every vehicle
                killVehiclesAt = -1;
                killReportAt = f * 0.05f + 5.0f;
                int n = 0;
                for (const auto& ep : world.entities())
                    if (ep->vehicle && !ep->obj->removed) {
                        if (std::getenv("IW2_KILL_VERBOSE")) std::printf("  vehicle %d %s health %s\n", ep->number, ep->obj->get("model").toString().c_str(), ep->obj->get("health").toString().c_str());
                        world.debugDamage(*ep, 100000);
                        ++n;
                    }
                std::printf("killed %d vehicles at %.1f s\n", n, f * 0.05f);
            }
            if (combatSceneAt >= 0 && f * 0.05f >= combatSceneAt) { // IW2_COMBAT_SCENE=<s>: two actors of opposite teams fight, step log
                if (!sceneA) {
                    float best = 1e30f; // the closest living axis/allies pair
                    for (const auto& ea : world.entities()) {
                        if (!ea->isActor || ea->obj->removed || ea->obj->get("health").asInt() <= 0 || ea->obj->get("team").toString() != "axis") continue;
                        for (const auto& eb : world.entities()) {
                            if (!eb->isActor || eb->obj->removed || eb->obj->get("health").asInt() <= 0 || eb->obj->get("team").toString() != "allies" || eb->ai.stack[size_t(eb->ai.depth)] == 7) continue;
                            const auto oa = ea->obj->get("origin").asVector(), ob = eb->obj->get("origin").asVector();
                            const float d = (oa[0] - ob[0]) * (oa[0] - ob[0]) + (oa[1] - ob[1]) * (oa[1] - ob[1]) + (oa[2] - ob[2]) * (oa[2] - ob[2]);
                            if (d < best) { best = d; sceneA = ea.get(); sceneB = eb.get(); }
                        }
                    }
                    if (sceneA) std::printf("[scene] closest pair distance %.0f\n", std::sqrt(best));
                    // Search for an axis actor with a clear line to where B is placed (the shot must reach B).
                    if (sceneA && sceneB)
                        for (const auto& ea : world.entities()) {
                            if (!ea->isActor || ea->obj->removed || ea->obj->get("health").asInt() <= 0 || ea->obj->get("team").toString() != "axis") continue;
                            world.debugPlaceBefore(*sceneB, *ea);
                            if (world.debugClearShot(*ea, *sceneB)) { sceneA = ea.get(); break; }
                        }
                    if (sceneA && sceneB) {
                        world.debugPlaceBefore(*sceneB, *sceneA);
                        std::printf("[scene] B placed 120 in front of A\n");
                        if (!std::getenv("IW2_SCENE_LETHAL")) sceneB->obj->set("health", iw2::script::Value(1000)); // survive to show the pain reaction
                        world.debugSetEnemy(*sceneA, sceneB);
                        world.debugSetEnemy(*sceneB, sceneA);
                        std::printf("[scene %.2f] A=#%d (axis) B=#%d (allies) now enemies\n", f * 0.05f, sceneA->number, sceneB->number);
                    } else { std::printf("[scene] no axis/allies actor pair\n"); combatSceneAt = -1; }
                }
                if (sceneA && sceneB && f % 6 == 0 && f * 0.05f < combatSceneAt + 3.0f && (world.debugPlaceBefore(*sceneB, *sceneA), true)) std::printf("[scene %.2f] shot hit entity %d (shots %d hits %d)\n", f * 0.05f, world.debugShoot(*sceneA, *sceneB), world.stats.shots, world.stats.hits);
                if (sceneA && sceneB && f * 0.05f < combatSceneAt + 8.0f) {
                    for (auto* a : {sceneA, sceneB}) {
                        int planes = 0;
                        for (const auto& dp : a->dangerPlanes) planes += dp.timeMs != 0;
                        std::printf("[scene %.2f] #%d state %d sub %d hp %d planes %d pain %lld\n", f * 0.05f, a->number, a->ai.stack[size_t(a->ai.depth)], a->ai.substate, a->obj->get("health").asInt(), planes, (long long)a->ai.painStartMs);
                    }
                } else if (sceneA) combatSceneAt = -1;
            }
            if (touchTestAt >= 0 && f * 0.05f >= touchTestAt) { // IW2_TOUCH_TEST=<s> / IW2_ROCKET_TEST=<s>
                const bool rocket = touchTestIsRocket;
                touchTestAt = -1;
                auto* pl = world.player();
                if (std::getenv("IW2_TEST_ACTOR")) { // target the first living actor instead of the player
                    pl = nullptr;
                    for (const auto& ep : world.entities())
                        if (!pl && ep->isActor && !ep->obj->removed && ep->obj->get("health").asInt() > 0) pl = ep.get();
                }
                decltype(world.player()) veh = nullptr;
                for (const auto& ep : world.entities())
                    if (!veh && ep->vehicle && ep->vehicle->info && !ep->obj->removed && (rocket || ep->vehicle->info->collisionDamage > 0)) veh = ep.get();
                if (!pl || !veh) {
                    std::printf("touch/rocket test: no player or suitable vehicle\n");
                } else {
                    touchTarget = pl;
                    touchBefore = pl->obj->get("health").asInt();
                    if (rocket) world.debugVehicleRocket(*veh, *pl);
                    else world.debugVehicleTouch(*veh, *pl);
                    touchReportAt = f * 0.05f + (rocket ? 3.0f : 0.0f);
                    std::printf("%s test at %.1f s on vehicle %d (%s, collisionDamage %g, collisionSpeed %g), player health %d\n", rocket ? "rocket" : "touch", f * 0.05f, veh->number, veh->vehicle->info->name.c_str(), veh->vehicle->info->collisionDamage, veh->vehicle->info->collisionSpeed, touchBefore);
                }
            }
            if (touchReportAt >= 0 && f * 0.05f >= touchReportAt) {
                touchReportAt = -1;
                std::printf("target health %d -> %d\n", touchBefore, touchTarget->obj->get("health").asInt());
            }
            if (fireVehiclesAt >= 0 && f * 0.05f >= fireVehiclesAt) { // IW2_FIRE_VEHICLES=<s>: "turret_fire" on every vehicle
                fireVehiclesAt = -1;
                int n = 0;
                for (const auto& ep : world.entities())
                    if (ep->vehicle && !ep->obj->removed) {
                        world.vm().notify(ep->obj, "turret_fire", {});
                        ++n;
                    }
                std::printf("turret_fire sent to %d vehicles at %.1f s (rockets so far %d, bullets %d)\n", n, f * 0.05f, world.stats.rockets, world.stats.shots);
                fireVehiclesReportAt = f * 0.05f + 3.0f;
            }
            if (fireVehiclesReportAt >= 0 && f * 0.05f >= fireVehiclesReportAt) {
                fireVehiclesReportAt = -1;
                std::printf("3 s after turret_fire: rockets %d, bullets %d, bullet hits %d\n", world.stats.rockets, world.stats.shots, world.stats.hits);
            }
            if (killReportAt >= 0 && f * 0.05f >= killReportAt) { // 5 s after IW2_KILL_VEHICLES: how many became corpses
                killReportAt = -1;
                int corpses = 0, alive = 0;
                for (const auto& ep : world.entities()) {
                    if (ep->obj->removed) continue;
                    if (ep->classname == "script_vehicle_corpse") ++corpses;
                    else if (ep->vehicle) { ++alive; std::printf("  survivor #%d model %s targetname %s health %s player vehicle %d\n", ep->number, ep->obj->get("model").toString().c_str(), ep->obj->get("targetname").toString().c_str(), ep->obj->get("health").toString().c_str(), int(world.vm().level()->get("playervehicle").isObject() && world.vm().level()->get("playervehicle").asObject() == ep->obj)); }
                }
                std::printf("vehicles: %d corpses, %d still script_vehicle\n", corpses, alive);
            }
            for (auto it = levelNotifies.begin(); it != levelNotifies.end();)
                if (f * 0.05f >= it->first) {
                    world.vm().notify(world.vm().level(), it->second, {});
                    it = levelNotifies.erase(it);
                } else {
                    ++it;
                }
            if (!playerPath.empty() && world.player()) {
                const float t = f * 0.05f;
                size_t k = 0;
                while (k + 1 < playerPath.size() && playerPath[k + 1].first <= t) ++k;
                if (t >= playerPath[k].first) {
                    iw2::game::Vec3 p = playerPath[k].second;
                    if (k + 1 < playerPath.size()) {
                        const float u = (t - playerPath[k].first) / (playerPath[k + 1].first - playerPath[k].first);
                        for (int c = 0; c < 3; ++c) p[c] += (playerPath[k + 1].second[c] - p[c]) * u;
                    }
                    world.setPlayerOrigin(p, world.player()->obj->get("angles").asVector());
                }
            }
            if (!playerLooks.empty() && world.player()) {
                // --player-look: the view angles (yaw,pitch) from T on; the use key and the commands read them.
                for (const auto& l : playerLooks)
                    if (f * 0.05f >= l.first) {
                        playerLookAngles = {l.second[1], l.second[0], 0};
                        world.player()->obj->set("angles", iw2::script::Value(iw2::script::Vec3{l.second[1], l.second[0], 0}));
                    }
                iw2::game::World::PlayerCommand pc;
                for (const auto& a : playerAds)
                    if (f * 0.05f >= a.first) pc.adsFrac = a.second;
                pc.viewAngles = playerLookAngles;
                world.setPlayerCommand(pc);
            }
            for (auto it = playerFires.begin(); it != playerFires.end();)
                if (world.player() && f * 0.05f >= it->first) {
                    // --player-fire: one bullet from the eye along yaw,pitch.
                    const float yaw = it->second[0] * 3.14159265f / 180.0f, pitch = it->second[1] * 3.14159265f / 180.0f;
                    const auto o = world.player()->obj->get("origin").asVector();
                    const iw2::script::Vec3 eye{o[0], o[1], o[2] + 60.0f};
                    const iw2::script::Vec3 dir{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
                    const int before = world.stats.hits;
                    world.playerFire(eye, dir);
                    std::printf("player fired at %.2f s yaw %.0f pitch %.0f: hits %d\n", f * 0.05f, it->second[0], it->second[1], world.stats.hits - before);
                    it = playerFires.erase(it);
                } else {
                    ++it;
                }
            for (auto it = playerWeapons.begin(); it != playerWeapons.end();)
                if (world.player() && f * 0.05f >= it->first) {
                    world.player()->obj->set("$weapon", iw2::script::Value(it->second));
                    it = playerWeapons.erase(it);
                } else {
                    ++it;
                }
            for (auto it = playerMelees.begin(); it != playerMelees.end();)
                if (world.player() && f * 0.05f >= it->first) {
                    const auto o = world.player()->obj->get("origin").asVector();
                    const iw2::script::Vec3 eye{o[0], o[1], o[2] + 60.0f};
                    const float dx = it->second[0] - eye[0], dy = it->second[1] - eye[1], dz = it->second[2] - eye[2];
                    const float yaw = std::atan2(dy, dx) * 180.0f / 3.14159265f;
                    const float pitch = -std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 180.0f / 3.14159265f;
                    const int before = world.stats.hits;
                    world.playerMelee(eye, iw2::script::Vec3{pitch, yaw, 0});
                    std::printf("player meleed at %.2f s toward %.0f,%.0f,%.0f: hits %d\n", f * 0.05f, it->second[0], it->second[1], it->second[2], world.stats.hits - before);
                    it = playerMelees.erase(it);
                } else {
                    ++it;
                }
            for (auto it = playerFireAts.begin(); it != playerFireAts.end();)
                if (world.player() && f * 0.05f >= it->first) {
                    const auto o = world.player()->obj->get("origin").asVector();
                    const iw2::script::Vec3 eye{o[0], o[1], o[2] + 60.0f};
                    iw2::script::Vec3 dir{it->second[0] - eye[0], it->second[1] - eye[1], it->second[2] - eye[2]};
                    const float l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                    for (auto& c : dir) c /= l;
                    const int before = world.stats.hits;
                    world.playerFire(eye, dir);
                    std::printf("player fired at %.2f s at %.0f,%.0f,%.0f: hits %d\n", f * 0.05f, it->second[0], it->second[1], it->second[2], world.stats.hits - before);
                    it = playerFireAts.erase(it);
                } else {
                    ++it;
                }
            if (!playerUses.empty() && world.player()) {
                // --player-use: the use key (buttons 0x28 pressed) for one frame, looking along yaw,pitch.
                iw2::game::World::PlayerCommand pc;
                for (const auto& a : playerAds)
                    if (f * 0.05f >= a.first) pc.adsFrac = a.second;
                pc.viewAngles = playerLookAngles;
                const float t = f * 0.05f;
                for (const auto& u : playerUses)
                    if (t >= u.first) {
                        pc.viewAngles = {u.second[1], u.second[0], 0};
                        pc.buttons = t < u.first + 0.05f ? 0x28u : 0u;
                    }
                world.setPlayerCommand(pc);
            }
            if (pmove && world.player()) {
                if (!pmoveInit) {
                    const auto o = world.player()->obj->get("origin").asVector();
                    pmoveState.origin = {o[0], o[1], o[2] + 8.0f};
                    pmoveInit = true;
                }
                iw2::game::MoveParams mp;
                mp.passEntity = world.player()->number;
                iw2::game::PlayerInput in;
                in.yaw = world.player()->obj->get("angles").asVector()[1];
                if (std::getenv("IW2_PMOVE_FORWARD")) in.forward = 1; // walk ahead to make footstep noise
                for (int k = 0; k < 3; ++k) iw2::game::playerMove(world.collision(), pmoveState, in, 1.0f / 60.0f, mp);
                world.setPlayerOrigin({pmoveState.origin[0], pmoveState.origin[1], pmoveState.origin[2]}, world.player()->obj->get("angles").asVector());
                world.playerPmEvents(pmoveState.eventSeq, pmoveState.events, pmoveState.stance == iw2::game::Stance::Crouch);
            }
            std::map<const iw2::game::Entity*, iw2::script::Vec3> before;
            if (moveStats)
                for (const auto& e : world.entities())
                    if (e->isActor && !e->obj->removed && e->path.count > 0 && e->ai.animMode == 1 && e->ai.movemode >= 2)
                        before[e.get()] = e->obj->get("origin").asVector();
            world.frame();
            if (!menuAnswers.empty())
                for (const auto& cmd : world.takeClientCommands())
                    if (cmd.name == "popupopen") {
                        const int idx = std::atoi(cmd.arg.c_str());
                        std::cout << "menu opened: " << (size_t(idx) < world.menuNames().size() ? world.menuNames()[size_t(idx)] : "?") << "\n";
                        if (menuAnswered < menuAnswers.size()) {
                            std::cout << "menu response: " << menuAnswers[menuAnswered] << "\n";
                            world.menuResponse(idx, menuAnswers[menuAnswered++]);
                        }
                    }
            for (const auto& [e, o0] : before) {
                if (e->obj->removed) continue;
                const auto o = e->obj->get("origin").asVector();
                const float v = std::sqrt((o[0] - o0[0]) * (o[0] - o0[0]) + (o[1] - o0[1]) * (o[1] - o0[1])) * 20.0f;
                auto& st = moveStatsBy[e->ai.movemode];
                st.frames++;
                st.sum += v;
                st.stalls += v < 1.0f;
                st.max = std::max(st.max, v);
            }
        }
        for (const auto& [mode, st] : moveStatsBy)
            std::printf("move stats movemode %d: %d actor frames, mean %.1f u/s, max %.1f, stalled frames %d\n", mode,
                        st.frames, st.frames ? st.sum / st.frames : 0.0, st.max, st.stalls);
        if (playerShoots) std::cout << "player hits " << playerHits << "\n";
        if (shootBarrels) {
            int whole = 0, broken = 0;
            for (const auto& e : world.entities()) {
                if (e->obj->removed || e->classname != "script_model" || e->obj->get("targetname").toString() != "explodable_barrel") continue;
                const std::string m = e->obj->get("model").toString();
                if (m.find("prop_barrel_piece") != std::string::npos) ++broken;
                else if (m.find("prop_barrel_benzin") != std::string::npos) ++whole;
            }
            std::cout << "barrels: " << broken << " exploded, " << whole << " whole\n";
        }
        if (controls) {
            std::cout << "controls: weapon " << pweapon.name() << ", events";
            for (const auto& [k, n] : weaponEvents) std::cout << " " << k << "x" << n;
            std::cout << "\n";
        }
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
            if (std::getenv("IW2_SOUND_LIST"))
                for (const auto& [k, n] : soundsPlayed) std::cout << "; " << k << " x" << n;
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
                      << world.stats.grenades << ", exploded " << world.stats.grenadeExplosions << "; bullet impacts " << world.stats.bulletImpacts << ", penetrations " << world.stats.bulletPenetrations << ", player noise events " << world.stats.playerNoise << ", actor noise events " << world.stats.actorNoise << ", ground trace hit/miss " << world.stats.groundHit << "/" << world.stats.groundMiss << ", grenade away-flee/cower/plane-retry " << world.stats.grenadeFlee << "/" << world.stats.grenadeCower << "/" << world.stats.grenadeFleeRetry << "\n";
            std::cout << "actor animscripts:";
            for (const auto& [k, v] : states) std::cout << " " << k << "=" << v;
            std::cout << "\n";
            for (const auto& e : world.entities())
                if (e->isActor && e->animScript == "combat" && e->anim) {
                    std::cout << "combat actor anim state:\n" << e->anim->debugString();
                    break;
                }
            for (const auto& e : world.entities())
                if (e->isActor && e->animScript == "death" && e->anim) {
                    std::cout << "death actor anim state:\n" << e->anim->debugString();
                    break;
                }
            {
                // Weapon drop on death (death.gsc → DropAIWeapon → dropweapon): corpses still holding a gun, items spawned.
                int corpses = 0, armed = 0, items = 0;
                for (const auto& e : world.entities()) {
                    if (e->obj->removed) continue;
                    if (e->classname.rfind("weapon_", 0) == 0) ++items;
                    if (!e->isActor || !e->ai.corpse) continue;
                    ++corpses;
                    for (const auto& a : e->attachments)
                        if (a.tag == "tag_weapon_right" || a.tag == "tag_weapon_left") { ++armed; break; }
                }
                std::cout << "corpses " << corpses << ", still holding a gun " << armed << ", weapon items " << items << "\n";
            }
            if (mover) std::cout << "a moving actor is at " << mover->obj->get("origin").toString() << "\n";
        }
        if (const char* tp = std::getenv("IW2_TRIGGER_DUMP"); tp) { // debug: bounds of the brush entities whose targetname starts with tp
            for (const auto& e : world.entities()) {
                const auto tn = e->obj->get("targetname");
                iw2::game::Vec3 mn{}, mx{};
                if (e->obj->removed || !tn.isString() || tn.asString().rfind(tp, 0) != 0) continue;
                const auto m = e->obj->get("model");
                if (m.isString() && world.submodelBounds(m.asString(), mn, mx)) {
                    const auto o = e->obj->get("origin");
                    const iw2::game::Vec3 ov = o.isVector() ? o.asVector() : iw2::game::Vec3{0, 0, 0};
                    std::printf("trigger %s %s mins %.0f,%.0f,%.0f maxs %.0f,%.0f,%.0f\n", e->classname.c_str(), tn.asString().c_str(), mn[0] + ov[0],
                                mn[1] + ov[1], mn[2] + ov[2], mx[0] + ov[0], mx[1] + ov[1], mx[2] + ov[2]);
                }
            }
        }
        if (const char* pre = std::getenv("IW2_THREADS_PREFIX")) { // debug: every live thread whose wait site starts with the prefix
            for (const auto& d : world.vm().describeThreads())
                if (d.rfind(pre, 0) == 0) std::cout << "thread " << d << "\n";
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
            for (const auto& [n, k] : top)
                if (k.find("death") != std::string::npos) std::cout << "  death wait: " << n << " " << k << "\n";
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
