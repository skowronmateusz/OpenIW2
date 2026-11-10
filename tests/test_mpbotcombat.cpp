// Multiplayer bot combat (docs/spec/mp-bots.md "Combat behaviour", "Cost limits", "Stuck watch"): the pure decisions,
// fights per skill (shots, hits, kills, grenades), the pistol switch, hearing, the cost of 15 bots and the stuck watch
// on every map. Needs the game files (IW2_MAIN); the graph cache goes to IW2_NAV_CACHE.
//   test_mpbotcombat                 pure tests and the fights on mp_toujane
//   test_mpbotcombat perf <map>      15 bots: frame cost and A* plans per frame
//   test_mpbotcombat stall <map> [gametype]  "wants to walk, stays on the spot" spells by hot spot (IW2_BOT_SOAK_SECONDS)
//   test_mpbotcombat stall-check <map>       the same with limits
//   test_mpbotcombat soak <map>      8 bots for IW2_BOT_SOAK_SECONDS (default 60) game seconds: no bot stuck

#include "game/mpbots.h"
#include "game/world.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <cstdlib>
#include <iostream>
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

using iw2::game::Entity;
using iw2::game::MpBotStats;
using iw2::game::NavGraph;
using iw2::game::Stance;
using iw2::game::World;
using iw2::script::Value;
using V3 = std::array<float, 3>;

void run(World& w, int frames) {
    for (int i = 0; i < frames; ++i) w.frame();
}

void pureTests() {
    using iw2::game::BotStanceIn;
    using iw2::game::botChooseStance;
    BotStanceIn in;
    in.weaponClass = "sniper";
    in.dist = 1200;
    CHECK(botChooseStance(in) == Stance::Stand); // not fighting
    in.fighting = true;
    CHECK(botChooseStance(in) == Stance::Prone);
    in.moving = true;
    CHECK(botChooseStance(in) == Stance::Stand);
    in.moving = false;
    in.weaponClass = "rifle";
    CHECK(botChooseStance(in) == Stance::Crouch);
    in.dist = 400;
    CHECK(botChooseStance(in) == Stance::Stand);
    in.coverCrouch = true;
    in.holdFire = true;
    CHECK(botChooseStance(in) == Stance::Crouch); // hides behind the cover while reloading
    in.holdFire = false;
    CHECK(botChooseStance(in) == Stance::Stand); // stands up to fire over it
    in.coverCrouch = false;
    in.weaponClass = "smg";
    in.dist = 1500;
    CHECK(botChooseStance(in) == Stance::Stand);

    using iw2::game::botWantAds;
    CHECK(botWantAds("sniper", 450) && !botWantAds("sniper", 300));
    CHECK(botWantAds("rifle", 600) && !botWantAds("rifle", 300));
    CHECK(botWantAds("smg", 1000) && !botWantAds("smg", 700));
    CHECK(!botWantAds("shotgun", 2000) && !botWantAds("pistol", 2000));

    using iw2::game::botBurst;
    CHECK(botBurst("smg", 300, 6, 350).shots == 6 && botBurst("smg", 300, 6, 350).pauseMs == 350);
    CHECK(botBurst("mg", 1000, 6, 350).shots == 3 && botBurst("mg", 1000, 6, 350).pauseMs == 525);
    CHECK(botBurst("mg", 1000, 3, 700).shots == 2);
    CHECK(botBurst("rifle", 300, 6, 350).shots == 1 && botBurst("rifle", 300, 6, 350).pauseMs == 0);
    CHECK(botBurst("pistol", 300, 8, 250).shots == 3);

    // The grenade arc lands on the target (simulated in small steps); out of reach gives no solution.
    using iw2::game::botGrenadeVelocity;
    const float g = 800, speed = 600;
    for (int high = 0; high < 2; ++high)
        for (const V3& to : {V3{400, 0, 0}, V3{300, 200, -100}, V3{-250, 100, 60}}) {
            V3 v{};
            CHECK(botGrenadeVelocity({0, 0, 0}, to, speed, g, high != 0, v));
            CHECK(std::fabs(std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) - speed) < 0.5f);
            const float x = std::hypot(to[0], to[1]);
            const float t = x / std::hypot(v[0], v[1]);
            const float z = v[2] * t - 0.5f * g * t * t;
            CHECK(std::fabs(v[0] * t - to[0]) < 0.5f && std::fabs(v[1] * t - to[1]) < 0.5f && std::fabs(z - to[2]) < 1.0f);
        }
    V3 lo{}, hi{};
    botGrenadeVelocity({0, 0, 0}, {400, 0, 0}, speed, g, false, lo);
    botGrenadeVelocity({0, 0, 0}, {400, 0, 0}, speed, g, true, hi);
    CHECK(hi[2] > lo[2]);
    V3 v{};
    CHECK(!botGrenadeVelocity({0, 0, 0}, {2000, 0, 0}, speed, g, false, v)); // max range v²/g = 450
}

// A world with the local client as a spectator (it never spawns) and `bots` bots at `skill`.
struct Game {
    World w;
    std::vector<int> bots;
    Game(iw2::fs::Vfs& vfs, const std::string& map, const std::string& gt, int nbots, int skill) : w(vfs, map, true) {
        w.mpStart(gt);
        const int c0 = w.mpConnect("player");
        w.mpBegin(c0);
        run(w, 1);
        w.mpSetBotSkill(skill);
        for (int i = 0; i < nbots; ++i) bots.push_back(w.mpAddBot());
    }
    MpBotStats sum() const {
        MpBotStats s;
        for (int c : bots)
            if (const MpBotStats* b = w.mpBotStats(c)) {
                s.shots += b->shots, s.hits += b->hits, s.kills += b->kills, s.deaths += b->deaths;
                s.grenades += b->grenades, s.grenadeHits += b->grenadeHits, s.smokes += b->smokes;
                s.pistolSwitches += b->pistolSwitches, s.adsShots += b->adsShots, s.crouchFrames += b->crouchFrames;
                s.proneFrames += b->proneFrames, s.heard += b->heard, s.cornerChecks += b->cornerChecks, s.stuckEvents += b->stuckEvents, s.trapped += b->trapped;
                s.longestStillMs = std::max(s.longestStillMs, b->longestStillMs);
            }
        return s;
    }
};

void print(const std::string& what, const MpBotStats& s) {
    std::cout << what << ": shots " << s.shots << " hits " << s.hits << " kills " << s.kills << " deaths " << s.deaths << " grenades "
              << s.grenades << " grenade hits " << s.grenadeHits << " smokes " << s.smokes << " pistol " << s.pistolSwitches << " ads shots "
              << s.adsShots << " crouch " << s.crouchFrames << " prone " << s.proneFrames << " heard " << s.heard << " corners "
              << s.cornerChecks << " stuck " << s.stuckEvents << " trapped " << s.trapped << " longest still " << s.longestStillMs / 1000.0 << " s\n";
}

// Two graph nodes `lo`..`hi` apart (flat), with the sight between their eyes clear (`visible`) or blocked.
bool nodePair(World& w, float lo, float hi, bool visible, V3& a, V3& b) {
    const NavGraph& g = w.mpNav();
    for (size_t i = 0; i < g.mainCount; i += 97)
        for (size_t j = 0; j < g.mainCount; j += 13) {
            const V3 p = g.nodes[i], q = g.nodes[j];
            const float d = std::hypot(p[0] - q[0], p[1] - q[1]);
            if (d < lo || d > hi || std::fabs(p[2] - q[2]) > 32) continue;
            const auto tr = w.collision().trace({p[0], p[1], p[2] + 60}, {q[0], q[1], q[2] + 60}, {0, 0, 0}, {0, 0, 0},
                                                iw2::game::Contents::SightMaskNoCharacters, -1);
            if ((tr.fraction >= 0.999f) != visible) continue;
            a = p, b = q;
            return true;
        }
    return false;
}

std::string field(World& w, const std::string& weapon, const char* key) {
    const auto* d = w.weapon(weapon);
    if (!d) return {};
    const auto it = d->find(key);
    return it == d->end() ? std::string() : it->second;
}

void place(Entity* e, const V3& at, const V3& faceTo) {
    e->obj->set("origin", Value(iw2::script::Vec3{at[0], at[1], at[2]}));
    const float yaw = std::atan2(faceTo[1] - at[1], faceTo[0] - at[0]) * 57.29578f;
    e->obj->set("angles", Value(iw2::script::Vec3{0, yaw, 0}));
}

// Every bullet weapon of weapons/mp: a bot fights an enemy that does not fire back and cannot die; its shots follow the
// player's weapon state machine (docs/spec/mp-bots.md "Weapon state"): at most one shot per frame, shots at least
// fireTime apart (plus rechamberTime for a bolt-action weapon), a reload from an empty clip of at least its reload time.
int weaponTests(iw2::fs::Vfs& vfs) {
    std::vector<std::string> names;
    for (const auto& path : vfs.list("weapons/mp/")) {
        const std::string n = path.substr(path.rfind('/') + 1);
        if (!n.empty()) names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    Game g(vfs, "mp_toujane", "dm", 2, 3);
    run(g.w, 40);
    V3 pa{}, pb{};
    CHECK(nodePair(g.w, 550, 900, true, pa, pb));
    int tested = 0;
    std::map<std::string, int> perClass;
    for (const std::string& wn : names) {
        const std::string cls = g.w.mpWeaponClass(wn);
        if (field(g.w, wn, "weapontype") != "bullet" || cls.empty() || cls == "grenade" || wn.find("bipod") != std::string::npos ||
            wn.find("30cal") != std::string::npos)
            continue;
        Entity* a = g.w.mpClientEntity(g.bots[0]);
        Entity* b = g.w.mpClientEntity(g.bots[1]);
        if (!a || !b) break;
        g.w.mpBotSetCombat(g.bots[1], false);
        auto& inv = g.w.inventory(*a);
        const std::string clipName = field(g.w, wn, "clipname"), ammoName = field(g.w, wn, "ammoname");
        const int clipSize = std::atoi(field(g.w, wn, "clipsize").c_str());
        inv.owned.insert(wn);
        inv.slot[1] = wn, inv.slot[2].clear();
        inv.clip[clipName] = clipSize;
        inv.ammo[ammoName] = clipSize * 3;
        a->obj->set("$weapon", Value(wn));
        auto num = [&](const char* k) { return float(std::atof(field(g.w, wn, k).c_str())); };
        const bool bolt = field(g.w, wn, "boltaction") == "1";
        const float minGap = num("firetime") + (bolt ? num("rechambertime") : 0.0f);
        const float reloadMin = std::min(num("reloadtime") + num("reloadstarttime"),
                                         num("reloademptytime") > 0 ? num("reloademptytime") : 1e9f);
        const MpBotStats* s = g.w.mpBotStats(g.bots[0]);
        int last = s->shots, lastShotFrame = -1000, shots = 0, multi = 0, reloads = 0;
        float minSeen = 1e9f, minReload = 1e9f;
        bool emptied = false;
        for (int f = 0; f < 20 * 14; ++f) {
            place(a, pa, pb), place(b, pb, pa);
            b->obj->set("health", Value(100000));
            g.w.frame();
            const int n = s->shots - last;
            last = s->shots;
            if (n <= 0) continue;
            if (n > 1) ++multi;
            const float gap = float(f - lastShotFrame) * 0.05f;
            if (lastShotFrame >= 0) {
                if (emptied) minReload = std::min(minReload, gap), ++reloads;
                else minSeen = std::min(minSeen, gap);
            }
            emptied = inv.clip[clipName] == 0;
            lastShotFrame = f;
            shots += n;
        }
        std::cout << wn << " (" << cls << (bolt ? ", bolt" : "") << "): shots " << shots << ", min gap " << minSeen << " s (>= " << minGap
                  << "), reloads " << reloads << ", min reload gap " << minReload << " s (>= " << reloadMin << ")\n";
        CHECK(shots >= 3);
        CHECK(multi == 0);
        CHECK(minSeen + 1e-3f >= minGap);
        CHECK(reloads == 0 || minReload + 1e-3f >= reloadMin);
        ++perClass[cls];
        ++tested;
    }
    std::cout << "weapons tested: " << tested << "\n";
    CHECK(tested >= 25);
    for (const char* c : {"rifle", "bolt", "sniper", "smg", "mg", "shotgun", "pistol"}) CHECK(perClass[c] > 0);
    return 0;
}

int combatTests(iw2::fs::Vfs& vfs) {
    // 1. Fights per skill: 6 bots of one skill in DM for 90 s on mp_toujane.
    MpBotStats st[4];
    for (int skill : {0, 3}) {
        Game g(vfs, "mp_toujane", "dm", 6, skill);
        run(g.w, 20 * 90);
        st[skill] = g.sum();
        print(std::string("skill ") + iw2::game::botSkill(skill).name, st[skill]);
        CHECK(st[skill].shots > 50);
        CHECK(st[skill].kills > 0);
        CHECK(g.w.vm().errors().empty());
    }
    const float r0 = float(st[0].hits) / float(std::max(1, st[0].shots)), r3 = float(st[3].hits) / float(std::max(1, st[3].shots));
    std::cout << "hit rate recruit " << r0 << " veteran " << r3 << "; kills " << st[0].kills << " / " << st[3].kills << "\n";
    CHECK(r3 > r0);
    CHECK(st[3].kills > st[0].kills);
    CHECK(st[0].grenades + st[3].grenades > 0);
    CHECK(st[3].cornerChecks > 0 && st[0].cornerChecks == 0);
    CHECK(st[3].crouchFrames + st[3].proneFrames > 0);

    // 1b. Smoke grenades: sd attackers before the plant throw them (6 bots with smgs, which the gametype gives smoke
    //     grenades, 3 minutes on mp_toujane).
    {
        Game g(vfs, "mp_toujane", "sd", 6, 2);
        for (int c : g.bots) g.w.mpBotSetWeaponClass(c, "smg");
        run(g.w, 20 * 180);
        const MpBotStats s = g.sum();
        print("sd hardened", s);
        CHECK(s.smokes > 0);
        CHECK(g.w.vm().errors().empty());
    }

    // 2. Pistol: a bot with an empty main weapon and an enemy 250-450 units away in sight switches to the pistol of
    //    slot primaryb and hits with it.
    {
        Game g(vfs, "mp_toujane", "dm", 2, 3);
        run(g.w, 40);
        Entity* a = g.w.mpClientEntity(g.bots[0]);
        Entity* b = g.w.mpClientEntity(g.bots[1]);
        CHECK(a->obj->get("sessionstate").toString() == "playing" && b->obj->get("sessionstate").toString() == "playing");
        auto& inv = g.w.inventory(*a);
        // givePistol's slot weapon lands in the first slot (both slots empty), the chosen weapon in the second.
        const bool p1 = g.w.mpWeaponClass(inv.slot[1]) == "pistol";
        const std::string main = p1 ? inv.slot[2] : inv.slot[1], pistol = p1 ? inv.slot[1] : inv.slot[2];
        std::cout << "loadout " << main << " / " << pistol << " clip " << inv.clip[field(g.w, main, "clipname")] << "\n";
        CHECK(g.w.mpWeaponClass(pistol) == "pistol");
        CHECK(inv.ammo[field(g.w, pistol, "ammoname")] + inv.clip[field(g.w, pistol, "clipname")] > 0);
        CHECK(inv.clip[field(g.w, main, "clipname")] > 0); // the gametype's loadout is the bot's ammunition
        inv.clip[field(g.w, main, "clipname")] = 0;
        inv.ammo[field(g.w, main, "ammoname")] = 0;
        g.w.mpBotSetCombat(g.bots[1], false);
        V3 pa{}, pb{};
        CHECK(nodePair(g.w, 250, 450, true, pa, pb));
        int f = 0;
        for (; f < 20 * 10 && a->obj->get("$weapon").toString() != pistol; ++f) {
            place(a, pa, pb), place(b, pb, pa);
            g.w.frame();
        }
        const MpBotStats* s = g.w.mpBotStats(g.bots[0]);
        std::cout << "pistol after " << f / 20.0f << " s; " << g.w.mpBotDescribe(g.bots[0]) << "\n";
        CHECK(a->obj->get("$weapon").toString() == pistol);
        CHECK(s->pistolSwitches == 1);
        run(g.w, 20 * 3);
        CHECK(s->shots > 0);
    }

    // 3. Hearing: an enemy out of sight 600-1500 units away fires; the bot takes it as its enemy.
    {
        Game g(vfs, "mp_toujane", "dm", 2, 3);
        run(g.w, 40);
        Entity* a = g.w.mpClientEntity(g.bots[0]);
        Entity* b = g.w.mpClientEntity(g.bots[1]);
        g.w.mpBotSetCombat(g.bots[1], false);
        V3 pa{}, pb{};
        CHECK(nodePair(g.w, 600, 1500, false, pa, pb));
        place(a, pa, pb), place(b, pb, pa);
        run(g.w, 2);
        CHECK(g.w.mpBotStats(g.bots[0])->heard == 0);
        const auto bo = b->obj->get("origin").asVector();
        const iw2::script::Vec3 eye{bo[0], bo[1], bo[2] + 60};
        place(a, pa, pb), place(b, pb, pa);
        g.w.debugFireBulletFrom(*b, eye, {eye[0], eye[1], eye[2] + 100});
        g.w.frame();
        std::cout << "hearing: " << g.w.mpBotDescribe(g.bots[0]) << "\n";
        CHECK(g.w.mpBotStats(g.bots[0])->heard == 1);
        CHECK(g.w.mpBotDescribe(g.bots[0]).find("enemy " + std::to_string(b->number)) != std::string::npos);
    }
    return 0;
}

int perfTest(iw2::fs::Vfs& vfs, const std::string& map) {
    Game g(vfs, map, "dm", 15, 1);
    (void)g.w.mpNav();
    run(g.w, 20 * 5); // join
    double sum = 0, worst = 0;
    const int frames = 20 * 60;
    for (int i = 0; i < frames; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        g.w.frame();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        sum += ms;
        worst = std::max(worst, ms);
    }
    const auto cost = g.w.mpBotCost();
    std::cout << map << " 15 bots: world frame mean " << sum / frames << " ms, max " << worst << " ms; bots mean " << cost.meanUs / 1000.0
              << " ms, max " << cost.maxUs / 1000.0 << " ms; most A* plans in a frame " << cost.maxPlans << "\n";
    print(map, g.sum());
    CHECK(cost.maxPlans <= 2);
    CHECK(sum / frames < 1000.0 / 60.0); // 60 frames of logic per second
    CHECK(g.w.vm().errors().empty());
    return 0;
}

int soakTest(iw2::fs::Vfs& vfs, const std::string& map) {
    const char* sv = std::getenv("IW2_BOT_SOAK_SECONDS");
    const int secs = sv ? std::max(1, std::atoi(sv)) : 60;
    Game g(vfs, map, "dm", 8, 1);
    run(g.w, 20 * secs);
    const MpBotStats s = g.sum();
    print(map + " " + std::to_string(secs) + " s", s);
    for (int c : g.bots)
        if (g.w.mpBotStats(c)->stuckEvents) std::cout << "  stuck: " << g.w.mpBotDescribe(c) << " at "
                                                      << g.w.mpClientEntity(c)->obj->get("origin").toString() << "\n";
    // No bot stays stuck: a 20 s stuck spell is left (a new goal) or, the second time at the same spot without a node in
    // reach, ended by the trapped bot's death; no bot stands within 64 units for a minute (fights and objectives aside).
    CHECK(s.longestStillMs < 60000);
    CHECK(s.trapped == 0); // no bot trapped where the graph has no way out
    CHECK(s.shots > 0);
    CHECK(g.w.vm().errors().empty());
    return 0;
}

// Stall report: spells of "wants to walk, stays within 48 units" by 128-unit hot spot, worst first.
int stallReport(iw2::fs::Vfs& vfs, const std::string& map, const std::string& gt, bool check) {
    const char* sv = std::getenv("IW2_BOT_SOAK_SECONDS");
    const int secs = sv ? std::max(1, std::atoi(sv)) : 60;
    Game g(vfs, map, gt, 8, 1);
    run(g.w, 20 * secs);
    const NavGraph& nav = g.w.mpNav();
    struct Spot { int n = 0, ms = 0, maxMs = 0, players = 0, noLink = 0; float travelled = 0; std::map<int, int> flags; V3 pos{}; };
    std::map<std::pair<int, int>, Spot> spots;
    int64_t total = 0;
    int over3 = 0, longest = 0;
    for (const auto& s : g.w.mpBotStalls()) {
        Spot& p = spots[{int(std::floor(s.pos[0] / 128)), int(std::floor(s.pos[1] / 128))}];
        ++p.n, p.ms += s.ms, p.maxMs = std::max(p.maxMs, s.ms), p.pos = s.pos, p.travelled += s.travelled;
        p.players += s.blockedByPlayer, p.noLink += s.from < 0, ++p.flags[s.flags];
        total += s.ms, over3 += s.ms > 3000, longest = std::max(longest, s.ms);
    }
    std::map<int, std::array<double, 3>> byFlags; // link flags -> spells, seconds, summed distance to the start node
    for (const auto& s : g.w.mpBotStalls()) {
        auto& f = byFlags[s.from < 0 ? -1 : s.flags];
        f[0] += 1, f[1] += s.ms / 1000.0, f[2] += s.fromDist;
    }
    for (const auto& [k, f] : byFlags) std::cout << "  link flags " << k << ": spells " << f[0] << " s " << f[1] << " mean dist to start " << f[2] / f[0] << "\n";
    std::vector<const Spot*> order;
    for (const auto& [k, p] : spots) order.push_back(&p);
    std::sort(order.begin(), order.end(), [](const Spot* a, const Spot* b) { return a->ms > b->ms; });
    std::cout << "stall " << map << " " << gt << " " << secs << " s nodes " << nav.size() << ": spells " << g.w.mpBotStalls().size() << " over3s " << over3
              << " longest " << longest / 1000.0 << " s, stalled " << total / 1000.0 << " s of " << g.w.mpBotWalkMs() / 1000.0 << " s walking ("
              << 100.0 * double(total) / double(std::max<int64_t>(1, g.w.mpBotWalkMs())) << "%)\n";
    // Limits with a margin over the fixed controller; before it mp_carentan stood at 16.8%, 16 spells over 3 s and 11 s
    // (docs/spec/mp-bots.md "Stuck watch"). The report mode itself prints only: the check is `stall-check`.
    if (check) {
        CHECK(100.0 * double(total) < 5.0 * double(g.w.mpBotWalkMs()));
        CHECK(over3 <= 5);
        CHECK(longest < 8000);
        CHECK(g.w.mpBotWalkMs() > 300000); // the bots did walk
        for (const auto& ls : nav.links)
            for (const auto& l : ls) CHECK(!((l.flags & iw2::game::NavLinkFlag::Crouch) && (l.flags & iw2::game::NavLinkFlag::Jump)));
    }
    for (size_t i = 0; i < order.size() && i < 12; ++i) {
        const Spot& p = *order[i];
        std::cout << "  spot " << p.pos[0] << " " << p.pos[1] << " " << p.pos[2] << ": n " << p.n << " sum " << p.ms / 1000.0 << " max " << p.maxMs / 1000.0
                  << " walked/s " << p.travelled / std::max(0.05f, float(p.ms) / 1000.0f) << " byPlayer " << p.players << " noLink " << p.noLink << " flags";
        for (const auto& [f, c] : p.flags) std::cout << " " << f << "x" << c;
        std::cout << "\n";
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    pureTests();
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return failures ? 1 : 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "probe" && argc > 8) { // probe <map> x y z x2 y2 z2: box traces at several heights (debugging)
        World w(vfs, argv[2], true);
        const V3 a{float(std::atof(argv[3])), float(std::atof(argv[4])), float(std::atof(argv[5]))};
        const V3 b{float(std::atof(argv[6])), float(std::atof(argv[7])), float(std::atof(argv[8]))};
        for (float z0 : {0.0f, 18.0f, 30.0f, 45.0f}) {
            const auto tr = w.collision().trace({a[0], a[1], a[2] + 1}, {b[0], b[1], a[2] + 1}, {-15, -15, z0}, {15, 15, z0 + 4},
                                                iw2::game::Contents::PlayerSolid, -1);
            std::cout << "z " << z0 << ": fraction " << tr.fraction << " at " << tr.endPos[0] << " " << tr.endPos[1] << " normal "
                      << tr.normal[0] << " " << tr.normal[1] << " " << tr.normal[2] << " startsolid " << tr.startSolid << "\n";
        }
        const auto up = w.collision().trace({a[0], a[1], a[2] + 1}, {a[0], a[1], a[2] + 200}, {-15, -15, 0}, {15, 15, 1},
                                            iw2::game::Contents::PlayerSolid, -1);
        std::cout << "ceiling " << up.endPos[2] << "\n";
        return 0;
    }
    if (mode == "perf" && argc > 2) perfTest(vfs, argv[2]);
    else if (mode == "soak" && argc > 2) soakTest(vfs, argv[2]);
    else if (mode == "stall" && argc > 2) stallReport(vfs, argv[2], argc > 3 ? argv[3] : "dm", false);
    else if (mode == "stall-check" && argc > 2) stallReport(vfs, argv[2], "dm", true);
    else if (mode == "weapons") weaponTests(vfs);
    else combatTests(vfs);
    return failures ? 1 : 0;
}
