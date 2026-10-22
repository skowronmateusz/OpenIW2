// iw2game: run a level headless (scripts, entities, no rendering).
//
//   iw2game <main-dir> <map> [seconds] [--cvar name=value ...]
//   iw2game <main-dir> <map> --walk-test     drop the player at the spawn point and walk forward
//   iw2game <main-dir> <map> --fall-test [walks] [seconds]   random player walks (tunnels, falls, solid)

#include "audio/sound.h"
#include "game/pmove.h"
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
            std::cout << "shots " << world.stats.shots << ", hits " << world.stats.hits << ", kills " << world.stats.kills << "\n";
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
