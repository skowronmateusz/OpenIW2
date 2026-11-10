// Navigation graphs of all 15 shipped MP maps (docs/spec/mp-bots.md "Navigation graph"): per map the node and link
// counts by kind and how many spawn points of every spawn class sit on the graph. Prints one report line per map.
// Needs the game files (IW2_MAIN); the graphs go to IW2_NAV_CACHE when it is set. IW2_NAVMAPS=<map,...> limits the maps.

#include "game/mpbots.h"
#include "game/world.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)
} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    std::vector<std::string> maps = {"mp_breakout", "mp_brecourt", "mp_burgundy", "mp_carentan", "mp_dawnville",
                                     "mp_decoy",    "mp_downtown", "mp_farmhouse", "mp_harbor",  "mp_leningrad",
                                     "mp_matmata",  "mp_railyard", "mp_rhine",     "mp_toujane", "mp_trainstation"};
    if (const char* only = std::getenv("IW2_NAVMAPS")) {
        maps.clear();
        std::stringstream ss(only);
        for (std::string m; std::getline(ss, m, ',');) maps.push_back(m);
    }
    namespace NavLinkFlag = iw2::game::NavLinkFlag;
    for (const std::string& name : maps) {
        iw2::game::World w(vfs, name, true);
        w.mpStart("dm");
        if (const char* pr = std::getenv("IW2_NAVPROBE")) { // x0,y0,x1,y1,z0,z1: ladder surfaces and mantle brushes in a box
            float x0, y0, x1, y1, z0, z1;
            std::map<std::string, int> found;
            if (std::sscanf(pr, "%f,%f,%f,%f,%f,%f", &x0, &y0, &x1, &y1, &z0, &z1) == 6)
                for (float z = z0; z <= z1; z += 16)
                    for (float y = y0; y <= y1; y += 16)
                        for (float x = x0; x <= x1; x += 16)
                            for (int a = 0; a < 4; ++a) {
                                const float r = float(a) * 90.0f * 3.14159265f / 180.0f;
                                for (uint32_t mask : {0x2810011u /*nav-ish*/, 0x1000000u}) {
                                    const auto tr = w.collision().trace({x, y, z}, {x + std::cos(r) * 16, y + std::sin(r) * 16, z}, {-0.1f, -0.1f, 0},
                                                                        {0.1f, 0.1f, 1}, mask, -1, true, false);
                                    if (tr.fraction < 1 && !tr.startSolid && ((tr.surfaceFlags & 8) || mask == 0x1000000u)) {
                                        char buf[160];
                                        std::snprintf(buf, sizeof buf, "%s %d %d %d c %x s %x", mask == 0x1000000u ? "mantle" : "ladder",
                                                      int(tr.endPos[0]) / 32 * 32, int(tr.endPos[1]) / 32 * 32, int(tr.endPos[2]) / 32 * 32,
                                                      tr.contents, tr.surfaceFlags);
                                        ++found[buf];
                                    }
                                }
                            }
            for (const auto& [k, c] : found) std::cout << "probe " << k << " x" << c << "\n";
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const iw2::game::NavGraph& g = w.mpNav();
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        size_t links = 0, ladders = 0, mantles = 0, leaps = 0, crouches = 0;
        for (const auto& ls : g.links)
            for (const auto& l : ls) {
                ++links;
                ladders += (l.flags & NavLinkFlag::Ladder) != 0;
                crouches += (l.flags & NavLinkFlag::Crouch) != 0;
                mantles += (l.flags & NavLinkFlag::Mantle) != 0;
                leaps += (l.flags & NavLinkFlag::Leap) != 0;
            }
        int movers = 0; // brush entities (doors and other movers): solid in the generator's traces, so the graph avoids them
        for (const auto& e : w.entities())
            if (!e->obj->removed && (e->classname == "script_brushmodel" || e->classname.rfind("func_", 0) == 0)) ++movers;
        std::map<std::string, std::pair<int, int>> spawns; // class -> on graph, total
        int on = 0, total = 0;
        for (const auto& e : w.entities())
            if (!e->obj->removed && e->classname.rfind("mp_", 0) == 0 && e->classname.find("spawn") != std::string::npos) {
                const auto o = e->obj->get("origin").asVector();
                const bool ok = g.nearest(iw2::game::navDropToFloor(w.collision(), o), 160.0f) >= 0;
                auto& s = spawns[e->classname];
                s.first += ok;
                ++s.second;
                on += ok;
                ++total;
                if (!ok) {
                    const auto f = iw2::game::navDropToFloor(w.collision(), o);
                    std::cout << "  " << name << " off graph: " << e->classname << " " << o[0] << " " << o[1] << " " << o[2] << " (floor "
                              << f[2] << ")\n";
                }
            }
        if (std::getenv("IW2_NAVDIAG") && on != total) {
            // Diagnosis: the unpruned graph around every spawn that is off the graph.
            iw2::game::NavParams np;
            np.mantle = nullptr;
            std::array<float, 3> mins{-20000, -20000, -2000}, maxs{20000, 20000, 2000};
            const auto full = iw2::game::generateNavGraph(w.collision(), mins, maxs, {}, np);
            for (const auto& e : w.entities())
                if (!e->obj->removed && e->classname.rfind("mp_", 0) == 0 && e->classname.find("spawn") != std::string::npos) {
                    const auto o = e->obj->get("origin").asVector();
                    if (g.nearest(iw2::game::navDropToFloor(w.collision(), o), 160.0f) >= 0) continue;
                    const int n = full.nearest(o, 160.0f);
                    std::cout << "   diag " << e->classname << " " << o[0] << " " << o[1] << " " << o[2] << ": unpruned node " << n;
                    if (n >= 0) {
                        int in = 0;
                        for (const auto& ls : full.links)
                            for (const auto& l : ls) in += int(l.target) == n;
                        const auto& q = full.nodes[size_t(n)];
                        std::cout << " at " << q[0] << " " << q[1] << " " << q[2] << " out " << full.links[size_t(n)].size() << " in " << in;
                        for (const auto& l : full.links[size_t(n)]) {
                            const auto& t = full.nodes[l.target];
                            std::cout << " ->(" << t[0] << " " << t[1] << " " << t[2] << " f" << int(l.flags) << " onG " << (g.nearest(t, 8.0f) >= 0) << ")";
                        }
                        // the patch around it: size, bounds and what its edge nodes face
                        std::vector<char> seen(full.size(), 0);
                        std::vector<int> st{n}, comp;
                        seen[size_t(n)] = 1;
                        while (!st.empty()) {
                            const int v = st.back();
                            st.pop_back();
                            comp.push_back(v);
                            for (const auto& l : full.links[size_t(v)])
                                if (!seen[l.target]) seen[l.target] = 1, st.push_back(int(l.target));
                        }
                        { // the way out: BFS from the spawn node to the first node of the kept graph
                            std::vector<int> prev(full.size(), -2);
                            std::vector<int> q{n};
                            prev[size_t(n)] = -1;
                            int hit = -1;
                            for (size_t qi = 0; qi < q.size() && hit < 0; ++qi)
                                for (const auto& l : full.links[size_t(q[qi])])
                                    if (prev[l.target] == -2) {
                                        prev[l.target] = q[qi];
                                        q.push_back(int(l.target));
                                        if (g.nearest(full.nodes[l.target], 4.0f) >= 0) { hit = int(l.target); break; }
                                    }
                            std::cout << "\n     way out:";
                            for (int v = hit, k = 0; v >= 0 && k < 6; v = prev[size_t(v)], ++k) {
                                const auto& a = full.nodes[size_t(v)];
                                std::cout << " (" << a[0] << " " << a[1] << " " << a[2] << ")";
                            }
                        }
                        { // the nodes that reach the spawn node (reverse flood): count, bounds
                            std::vector<std::vector<int>> back(full.size());
                            for (size_t i = 0; i < full.size(); ++i)
                                for (const auto& l : full.links[i]) back[l.target].push_back(int(i));
                            std::vector<char> rs(full.size(), 0);
                            std::vector<int> st2{n};
                            rs[size_t(n)] = 1;
                            float a0[3] = {1e9f, 1e9f, 1e9f}, a1[3] = {-1e9f, -1e9f, -1e9f};
                            size_t cnt = 0;
                            while (!st2.empty()) {
                                const int v = st2.back();
                                st2.pop_back();
                                ++cnt;
                                for (int k = 0; k < 3; ++k) a0[k] = std::min(a0[k], full.nodes[size_t(v)][k]), a1[k] = std::max(a1[k], full.nodes[size_t(v)][k]);
                                for (int u : back[size_t(v)])
                                    if (!rs[size_t(u)]) rs[size_t(u)] = 1, st2.push_back(u);
                            }
                            int shown = 0;
                            for (size_t v = 0; v < full.size() && shown < 12; ++v) {
                                if (!rs[v]) continue;
                                const auto& a = full.nodes[v];
                                for (size_t u2 = 0; u2 < full.size(); ++u2) {
                                    if (rs[u2]) continue;
                                    const auto& b2 = full.nodes[u2];
                                    if (std::fabs(b2[0] - a[0]) > 16.5f || std::fabs(b2[1] - a[1]) > 16.5f || std::fabs(b2[2] - a[2]) > 40) continue;
                                    uint8_t fl = 0;
                                    const bool ab = iw2::game::navWalkTest(w.collision(), a, b2, {}, fl);
                                    const bool ba = iw2::game::navWalkTest(w.collision(), b2, a, {}, fl);
                                    std::cout << "\n     border (" << a[0] << " " << a[1] << " " << a[2] << ") - (" << b2[0] << " " << b2[1] << " " << b2[2]
                                              << ") walk " << ab << ba;
                                    ++shown;
                                }
                            }
                            std::map<std::string, int> edge;
                            for (size_t v = 0; v < full.size(); ++v) {
                                if (!rs[v] || full.links[v].size() >= 8) continue;
                                const auto& a = full.nodes[v];
                                for (int k = 0; k < 8; ++k) {
                                    const float r = float(k) * 45.0f * 3.14159265f / 180.0f;
                                    const float x = a[0] + std::round(std::cos(r)) * 16, y = a[1] + std::round(std::sin(r)) * 16;
                                    const auto tr = w.collision().trace({x, y, a[2] + 30}, {x, y, a[2] - 60}, {0, 0, 0}, {0, 0, 0}, 0x281c011u & ~0x2004000u, -1, true, false);
                                    char buf[128];
                                    std::snprintf(buf, sizeof buf, "ss %d fr %.1f nz %.2f dz %d c %x", int(tr.startSolid), tr.fraction, tr.normal[2],
                                                  int(tr.endPos[2] - a[2]), tr.contents);
                                    ++edge[buf];
                                }
                            }
                            for (const auto& [k2, c2] : edge) std::cout << "\n     edge " << k2 << " x" << c2;
                            std::cout << "\n     reached from " << cnt << " nodes " << a0[0] << " " << a0[1] << " " << a0[2] << " .. " << a1[0] << " " << a1[1] << " " << a1[2];
                        }
                        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
                        std::map<uint32_t, int> surf;
                        for (int v : comp) {
                            const auto& q2 = full.nodes[size_t(v)];
                            for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], q2[k]), hi[k] = std::max(hi[k], q2[k]);
                            for (int a = 0; a < 16; ++a) {
                                const float r = float(a) * 22.5f * 3.14159265f / 180.0f;
                                for (float z : {8.0f, 40.0f}) {
                                    const auto tr = w.collision().trace({q2[0], q2[1], q2[2] + z}, {q2[0] + std::cos(r) * 40, q2[1] + std::sin(r) * 40, q2[2] + z},
                                                                        {-1, -1, 0}, {1, 1, 1}, 0xffffffffu, -1, true, false);
                                    if (tr.fraction < 1 && !tr.startSolid) ++surf[tr.surfaceFlags & 0x0f00ffffu ? tr.surfaceFlags : 0], ++surf[0x80000000u | tr.contents];
                                }
                            }
                        }
                        for (const auto& e2 : w.entities()) {
                            if (e2->obj->removed) continue;
                            const auto p2 = e2->obj->get("origin");
                            if (!p2.isVector()) continue;
                            const auto o2 = p2.asVector();
                            if (std::hypot(o2[0] - o[0], o2[1] - o[1]) < 700 && e2->classname.find("spawn") == std::string::npos)
                                std::cout << "\n     ent " << e2->classname << " " << e2->obj->get("model").toString() << " " << o2[0] << " " << o2[1] << " " << o2[2];
                        }
                        std::cout << "\n     patch " << comp.size() << " nodes " << lo[0] << " " << lo[1] << " " << lo[2] << " .. " << hi[0] << " " << hi[1] << " " << hi[2] << " faces:";
                        for (const auto& [k, c] : surf) std::cout << " " << std::hex << k << std::dec << "x" << c;
                    }
                    std::cout << "\n";
                }
        }
        std::cout << name << ": " << g.size() << " nodes, " << links << " links (" << ladders << " ladder, " << mantles
                  << " mantle, " << leaps << " leap, " << crouches << " crouch), " << movers << " brush entities, " << secs << " s; spawns " << on << "/" << total;
        for (const auto& [k, v] : spawns) std::cout << " " << k << " " << v.first << "/" << v.second;
        std::cout << "\n";
        CHECK(g.size() > 300 && g.size() < 65536);
        // Every spawn point is on the graph: the upper floors left only by a drop (mp_dawnville, mp_downtown,
        // mp_farmhouse, mp_trainstation) are exit-only nodes (docs/spec/mp-bots.md "Off-graph spawn points"); from every
        // spawn node a path leads into the main part, and goals never pick an exit-only node.
        CHECK(total > 0 && total == on);
        CHECK(g.mainCount > 0 && g.mainCount <= g.size());
        int exitSpawns = 0;
        for (const auto& e : w.entities())
            if (!e->obj->removed && e->classname.rfind("mp_", 0) == 0 && e->classname.find("spawn") != std::string::npos) {
                const auto f = iw2::game::navDropToFloor(w.collision(), e->obj->get("origin").asVector());
                const int n = g.nearest(f, 160.0f);
                if (n < 0 || size_t(n) < g.mainCount) continue;
                ++exitSpawns;
                CHECK(w.paths().findPath(n, 0).size() > 1);
                const int m = g.nearest(f, 512.0f, true);
                CHECK(m < 0 || size_t(m) < g.mainCount);
            }
        std::cout << "  " << name << ": " << g.size() - g.mainCount << " exit-only nodes, " << exitSpawns << " spawn(s) on them\n";
        // Spots the soak found bots trapped in (docs/spec/mp-bots.md "Coverage of the shipped maps"): the inside of the
        // goods wagons of mp_railyard (a ceiling 86 units over the floor), the spawn room of mp_farmhouse entered over a
        // window sill (exit-only nodes with a reversed jump link out).
        struct Spot { const char* map; float x, y, z; };
        static const Spot kSpots[] = {{"mp_railyard", -1453, -847, 60}, {"mp_railyard", 50, -100, 61}, {"mp_farmhouse", -729, 420, 64}};
        for (const Spot& sp : kSpots) {
            if (name != sp.map) continue;
            const int n = g.nearest({sp.x, sp.y, sp.z}, 32.0f);
            std::cout << "  " << name << " spot " << sp.x << " " << sp.y << " " << sp.z << ": node " << n << "\n";
            CHECK(n >= 0);
            if (n >= 0) CHECK(size_t(n) < g.mainCount || w.paths().findPath(n, 0).size() > 1);
        }
    }
    return failures ? 1 : 0;
}
