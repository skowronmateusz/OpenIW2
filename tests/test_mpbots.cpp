// Multiplayer bots (docs/spec/mp-bots.md): the navigation graph generated from mp_toujane's collision (connected over
// the DM spawn points, cached), the bot controller walking to a goal through pmove, bots fighting in DM through the
// gametype callbacks (kills, score), addtestclient. Needs the game files (IW2_MAIN); the cache goes to IW2_NAV_CACHE.

#include "game/mpbots.h"
#include "game/world.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using iw2::game::Entity;
using iw2::game::NavGraph;
using iw2::game::World;
using iw2::script::Value;

std::string field(Entity* e, const char* f) { return e ? e->obj->get(f).toString() : std::string("<null>"); }
int ifield(Entity* e, const char* f) {
    const Value v = e ? e->obj->get(f) : Value();
    return v.isNumber() ? v.asInt() : -9999;
}
void run(World& w, int frames) {
    for (int i = 0; i < frames; ++i) w.frame();
}
std::array<float, 3> originOf(Entity* e) { return e->obj->get("origin").asVector(); }

void pureTests() {
    using iw2::game::botMoveInput;
    using iw2::game::botTurn;
    // Facing +x: +x is forward, -y is right.
    auto r = botMoveInput({1, 0}, 0);
    CHECK(std::fabs(r[0] - 1) < 1e-4f && std::fabs(r[1]) < 1e-4f);
    r = botMoveInput({0, -1}, 0);
    CHECK(std::fabs(r[0]) < 1e-4f && std::fabs(r[1] - 1) < 1e-4f);
    // Facing +y (yaw 90): +x is right.
    r = botMoveInput({1, 0}, 90);
    CHECK(std::fabs(r[0]) < 1e-4f && std::fabs(r[1] - 1) < 1e-4f);
    // Diagonal: both axes at 1.
    r = botMoveInput({0.7071f, -0.7071f}, 0);
    CHECK(std::fabs(r[0] - 1) < 1e-3f && std::fabs(r[1] - 1) < 1e-3f);
    CHECK(std::fabs(botTurn(170, -170, 5) - 175) < 1e-3f);  // the short way over 180
    CHECK(std::fabs(botTurn(0, 90, 30) - 30) < 1e-3f);
    CHECK(std::fabs(botTurn(10, 12, 30) - 12) < 1e-3f);
}

} // namespace

int main() {
    pureTests();
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return failures ? 1 : 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    const char* cache = std::getenv("IW2_NAV_CACHE");
    // One cache file per map and key (mp_toujane_<crc>.nav): remove them all so the first graph is generated.
    auto toujaneCaches = [&] {
        std::vector<std::filesystem::path> v;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(cache, ec))
            if (e.path().filename().string().rfind("mp_toujane_", 0) == 0 && e.path().extension() == ".nav") v.push_back(e.path());
        return v;
    };
    if (cache)
        for (const auto& p : toujaneCaches()) std::filesystem::remove(p);

    // 1. The graph: generated once, then loaded from the cache; all DM spawn points sit on one strongly connected graph.
    {
        World w(vfs, "mp_toujane", true);
        w.mpStart("dm");
        const auto t0 = std::chrono::steady_clock::now();
        const NavGraph& g = w.mpNav();
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        size_t links = 0, jumps = 0, drops = 0;
        for (const auto& ls : g.links)
            for (const auto& l : ls) {
                ++links;
                jumps += (l.flags & iw2::game::NavLinkFlag::Jump) != 0;
                drops += (l.flags & iw2::game::NavLinkFlag::Drop) != 0;
            }
        std::cout << "mp_toujane nav: " << g.size() << " nodes, " << links << " links (" << jumps << " jump, " << drops
                  << " drop), " << secs << " s\n";
        CHECK(g.size() > 500 && g.size() < 65536);
        CHECK(jumps > 0 && drops > 0);
        std::vector<int> spawnNodes;
        int spawns = 0;
        for (const auto& e : w.entities())
            if (e->classname == "mp_dm_spawn") {
                ++spawns;
                const int n = g.nearest(e->obj->get("origin").asVector(), 160.0f);
                if (n < 0) {
                    const auto o = e->obj->get("origin").asVector();
                    const int m = g.nearest(o, 1000.0f);
                    std::cerr << "spawn off the graph at " << o[0] << " " << o[1] << " " << o[2] << "; nearest node "
                              << (m >= 0 ? std::to_string(g.nodes[size_t(m)][0]) + " " + std::to_string(g.nodes[size_t(m)][1]) + " " +
                                               std::to_string(g.nodes[size_t(m)][2])
                                         : std::string("none"))
                              << "\n";
                }
                if (n >= 0) spawnNodes.push_back(n);
            }
        CHECK(spawns >= 16);
        // With the ladder, mantle and leap links every DM spawn is on the graph.
        CHECK(spawnNodes.size() == size_t(spawns));
        // Every node reaches every spawn node (the generator keeps the part that the spawns reach and that leads back).
        std::vector<char> seen(g.size(), 0);
        std::deque<int> q{spawnNodes.empty() ? 0 : spawnNodes[0]};
        seen[size_t(q.front())] = 1;
        size_t reached = 1;
        while (!q.empty()) {
            const int n = q.front();
            q.pop_front();
            for (const auto& l : g.links[size_t(n)])
                if (!seen[l.target]) seen[l.target] = 1, ++reached, q.push_back(int(l.target));
        }
        CHECK(reached == g.mainCount); // the exit-only nodes after them are not reached from the main part
        for (int s : spawnNodes) CHECK(seen[size_t(s)]);
        // A* across the map between the two spawn points farthest apart.
        float far = 0;
        int a = 0, b = 0;
        for (int i : spawnNodes)
            for (int j : spawnNodes) {
                const auto& p = g.nodes[size_t(i)];
                const auto& q2 = g.nodes[size_t(j)];
                const float d = std::hypot(p[0] - q2[0], p[1] - q2[1]);
                if (d > far) far = d, a = i, b = j;
            }
        const auto path = w.paths().findPath(a, b);
        CHECK(path.size() > 5);
        if (cache) {
            CHECK(!toujaneCaches().empty());
            World w2(vfs, "mp_toujane", true);
            w2.mpStart("dm");
            const auto t1 = std::chrono::steady_clock::now();
            const NavGraph& g2 = w2.mpNav();
            const double secs2 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
            std::cout << "cached load: " << secs2 << " s\n";
            CHECK(g2.size() == g.size());
            CHECK(g2.links[0].size() == g.links[0].size());
        }
    }

    // 2. A bot walks to a goal across the map through pmove (no enemy: the local client stays a spectator).
    {
        World w(vfs, "mp_toujane", true);
        w.mpStart("dm");
        const int c0 = w.mpConnect("player");
        w.mpBegin(c0);
        run(w, 1);
        const int bot = w.mpAddBot();
        CHECK(bot == 1);
        CHECK(w.mpIsBot(bot) && !w.mpIsBot(c0));
        Entity* be = w.mpClientEntity(bot);
        run(w, 40); // begin, team menu, weapon menu, spawn
        CHECK(field(be, "sessionstate") == "playing");
        CHECK(field(be, "name") == "bot0");
        CHECK(field(be, "$weapon") != "none");
        // The gametype's character script gave the bot its team's player model (_teams.gsc setPlayerModels).
        std::cout << "bot model " << field(be, "model") << ", " << be->attachments.size() << " attachment(s)\n";
        CHECK(field(be, "model").rfind("xmodel/", 0) == 0);
        const NavGraph& g = w.mpNav();
        const auto start = originOf(be);
        // The goal: the DM spawn farthest from the bot that is on the graph.
        std::array<float, 3> goal = start;
        float far = 0;
        for (const auto& e : w.entities())
            if (e->classname == "mp_dm_spawn") {
                const auto o = e->obj->get("origin").asVector();
                if (g.nearest(o, 160.0f) < 0) continue;
                const float d = std::hypot(o[0] - start[0], o[1] - start[1]);
                if (d > far) far = d, goal = o;
            }
        CHECK(far > 1000);
        w.mpBotSetGoal(bot, goal);
        int frames = 0;
        float travelled = 0;
        auto prev = start;
        for (; frames < 20 * 120 && w.mpBotDescribe(bot).find("goal reached") == std::string::npos; ++frames) {
            w.frame();
            const auto o = originOf(be);
            travelled += std::hypot(o[0] - prev[0], o[1] - prev[1]);
            prev = o;
        }
        const auto end = originOf(be);
        std::cout << "walk: " << far << " units straight, " << travelled << " walked in " << frames / 20.0f << " s; "
                  << w.mpBotDescribe(bot) << "\n";
        CHECK(std::hypot(end[0] - goal[0], end[1] - goal[1]) < 96.0f);
        CHECK(frames < 20 * 120);
        CHECK(field(be, "sessionstate") == "playing");
        CHECK(w.vm().errors().empty());
    }

    // 2b. Ladder (up and down), mantle and leap links: the bot is put on a link's start node with the link's end as its
    //     goal and must get there through pmove with the traversal input (mp_toujane has all four kinds).
    {
        World w(vfs, "mp_toujane", true);
        w.mpStart("dm");
        const int c0 = w.mpConnect("player");
        w.mpBegin(c0);
        run(w, 1);
        const int bot = w.mpAddBot();
        Entity* be = w.mpClientEntity(bot);
        run(w, 40);
        CHECK(field(be, "sessionstate") == "playing");
        const NavGraph& g = w.mpNav();
        namespace F = iw2::game::NavLinkFlag;
        struct Kind { const char* name; uint8_t flags; int up; };
        const Kind kinds[] = {{"ladder up", F::Ladder, 1}, {"ladder down", F::Ladder, -1}, {"mantle", F::Mantle, 0}, {"leap", F::Leap, 0}};
        for (const Kind& k : kinds) {
            int tried = 0, ok = 0;
            for (size_t i = 0; i < g.size() && tried < 4; ++i)
                for (const auto& l : g.links[i]) {
                    if (tried >= 4) break;
                    if ((l.flags & F::Special) != k.flags) continue;
                    const auto a = g.nodes[i], b = g.nodes[l.target];
                    if (k.up && (b[2] - a[2]) * float(k.up) < 0) continue;
                    if (w.paths().findPath(int(i), int(l.target)).size() != 2) continue; // the link is the best way
                    // Spaced out: one link per spot.
                    ++tried;
                    be->obj->set("origin", iw2::script::Value(iw2::script::Vec3{a[0], a[1], a[2]}));
                    w.mpBotSetGoal(bot, {b[0], b[1], b[2]}, 8.0f); // a radius below the link length (the default 64 counts short links as reached)
                    std::array<float, 3> o{};
                    int f = 0;
                    for (; f < 20 * 15; ++f) {
                        w.frame();
                        o = originOf(be);
                        if (std::hypot(o[0] - b[0], o[1] - b[1]) < 24 && std::fabs(o[2] - b[2]) < 24) break;
                    }
                    const bool reached = f < 20 * 15;
                    ok += reached;
                    std::cout << k.name << " (" << a[0] << " " << a[1] << " " << a[2] << ") -> (" << b[0] << " " << b[1] << " " << b[2]
                              << "): " << (reached ? "reached in " + std::to_string(f / 20.0f) + " s" : "not reached, at " +
                                                         std::to_string(o[0]) + " " + std::to_string(o[1]) + " " + std::to_string(o[2]))
                              << "\n";
                    i += 40; // the next spot
                    break;
                }
            CHECK(tried > 0);
            CHECK(ok == tried);
        }
    }

    // 3. DM with bots: three bots and the local player (standing at its spawn) on mp_toujane. Bots find, shoot and kill
    //    through the bullet / CodeCallback_PlayerDamage / finishPlayerDamage path; the gametype keeps the score
    //    (attacker +1, victim deaths +1); dead bots respawn with the use button.
    {
        World w(vfs, "mp_toujane", true);
        w.cvars["scr_dm_scorelimit"] = "0";
        w.cvars["scr_dm_timelimit"] = "0";
        w.mpStart("dm");
        const int c0 = w.mpConnect("player");
        w.mpBegin(c0);
        run(w, 1);
        w.mpAutoJoin(c0);
        w.mpSetBotSkill(3);
        std::vector<int> bots;
        for (int i = 0; i < 3; ++i) {
            bots.push_back(w.mpAddBot());
            run(w, 1);
        }
        run(w, 20 * 140);
        // The dead respawn: at some moment of the last 10 s at least two clients are playing (a snapshot at one frame
        // can fall on two deaths in a row).
        int playingMax = 0;
        for (int f = 0; f < 20 * 10; ++f) {
            w.frame();
            int p = 0;
            for (int c : {c0, bots[0], bots[1], bots[2]}) p += field(w.mpClientEntity(c), "sessionstate") == "playing";
            playingMax = std::max(playingMax, p);
        }
        int scores = 0, deaths = 0, botKills = 0, playing = 0;
        for (int c : {c0, bots[0], bots[1], bots[2]}) {
            Entity* e = w.mpClientEntity(c);
            scores += ifield(e, "score");
            deaths += ifield(e, "deaths");
            if (c != c0) botKills += ifield(e, "score");
            playing += field(e, "sessionstate") == "playing";
            std::cout << "client " << c << " " << field(e, "name") << ": score " << ifield(e, "score") << " deaths " << ifield(e, "deaths")
                      << " " << field(e, "sessionstate") << "; " << w.mpBotDescribe(c) << "\n";
        }
        int killLines = 0, ownDeaths = 0; // ownDeaths: killed by himself (a bot's own grenade) or by the world
        for (const auto& l : w.mpState().log) {
            if (l.rfind("K;", 0) != 0) continue;
            ++killLines;
            std::vector<std::string> f;
            std::string cur;
            for (char ch : l) {
                if (ch == ';') f.push_back(cur), cur.clear();
                else cur += ch;
            }
            f.push_back(cur);
            if (f.size() > 6 && (f[2] == f[6] || f[6] == "-1")) ++ownDeaths;
        }
        std::cout << "dm: " << killLines << " kills logged, bot score " << botKills << "\n";
        CHECK(botKills >= 3);
        CHECK(killLines == deaths);
        // Every death is one point for the killer; a death by one's own grenade or the world takes a point instead
        // (dm.gsc:334-362).
        CHECK(scores == deaths - 2 * ownDeaths);
        CHECK(ifield(w.mpClientEntity(c0), "deaths") >= 1); // the player was killed too
        CHECK(playingMax >= 2);  // the dead respawn
        (void)playing;
        CHECK(w.vm().errors().empty());
        for (const auto& e : w.vm().errors()) std::cerr << "script error: " << e << "\n";
    }

    // 4. addtestclient from a script returns the new client's entity; the bot joins by itself.
    {
        const auto dir = std::filesystem::temp_directory_path() / ("iw2_test_mpbots_" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir / "maps" / "mp");
        {
            std::ofstream f(dir / "maps" / "mp" / "bottest.gsc");
            f << "main()\n{\n\tlevel.testbot = addtestclient();\n}\n";
        }
        iw2::fs::Vfs vfs2;
        vfs2.mountDirectory(main);
        vfs2.mountDirectory(dir);
        World w(vfs2, "mp_toujane", true);
        w.mpStart("dm");
        const int c0 = w.mpConnect("player");
        w.mpBegin(c0);
        run(w, 1);
        w.vm().start("maps/mp/bottest.gsc", "main", {}, Value(w.vm().level()));
        w.vm().drainReady();
        const Value tb = w.vm().level()->get("testbot");
        CHECK(tb.isObject());
        Entity* be = w.mpClientEntity(1);
        CHECK(be && tb.isObject() && tb.asObject() == be->obj);
        CHECK(w.mpIsBot(1));
        run(w, 40);
        CHECK(field(be, "sessionstate") == "playing");
        CHECK(w.vm().errors().empty());
        std::filesystem::remove_all(dir);
    }

    if (failures) std::cerr << failures << " check(s) failed\n";
    return failures ? 1 : 0;
}
