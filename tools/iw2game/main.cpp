// iw2game: run a level headless (scripts, entities, no rendering).
//
//   iw2game <main-dir> <map> [seconds] [--cvar name=value ...]
//   iw2game <main-dir> <map> --walk-test     drop the player at the spawn point and walk forward

#include "game/pmove.h"
#include "game/world.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
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
        int seconds = 10;
        bool spawnAll = false;
        for (int i = 3; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--spawn-all") {
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
        const auto t0 = std::chrono::steady_clock::now();
        world.start();
        if (spawnAll) std::cout << "spawned " << world.spawnAllActors() << " actors\n";
        for (int f = 0; f < seconds * 20; ++f) world.frame();
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        std::cout << "simulated " << seconds << " s in " << wall << " s; entities " << world.entities().size() << ", nodes "
                  << world.nodes().size() << ", live threads " << world.vm().liveThreadCount() << ", runtime errors "
                  << world.vm().errors().size() << "\n";
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
