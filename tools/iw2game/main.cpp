// iw2game: run a level headless (scripts, entities, no rendering).
//
//   iw2game <main-dir> <map> [seconds] [--cvar name=value ...]

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
        int seconds = 10;
        for (int i = 3; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--cvar" && i + 1 < argc) {
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
