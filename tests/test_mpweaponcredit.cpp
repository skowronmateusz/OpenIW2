// The weapon a multiplayer shot and a multiplayer damage carry (docs/spec/mp.md "Player damage", docs/spec/mp-pmove.md
// "Spread weapons"): a bot fires the Trench Gun through the clients' shared fire path (8 pellets per shot) and its kill
// names the shotgun; a grenade kill names the grenade (obituary weapon and icon, the callbacks' sWeapon) even after the
// thrower has switched to another weapon. Needs the game files (IW2_MAIN).

#include "game/world.h"

#include <cmath>
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
using iw2::game::World;
using iw2::script::Value;
using V3 = iw2::script::Vec3;

void run(World& w, int frames) {
    for (int i = 0; i < frames; ++i) w.frame();
}

std::string def(World& w, const std::string& weapon, const char* key) {
    const auto* d = w.weapon(weapon);
    if (!d) return {};
    const auto it = d->find(key);
    return it == d->end() ? std::string() : it->second;
}

} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);

    // 1. A bot with the Trench Gun: every shot is 8 pellet traces, and its kill is credited to shotgun_mp.
    {
        World w(vfs, "mp_toujane", true);
        w.mpStart("dm");
        const int c0 = w.mpConnect("local");
        w.mpBegin(c0);
        run(w, 1);
        w.mpAutoJoin(c0);
        run(w, 2);
        const int bot = w.mpAddBot();
        run(w, 40);
        Entity* p0 = w.mpClientEntity(c0);
        Entity* be = w.mpClientEntity(bot);
        CHECK(p0 && be && be->obj->get("sessionstate").toString() == "playing");
        const std::string gun = "shotgun_mp";
        CHECK(def(w, gun, "weaponclass") == "spread" && def(w, gun, "shotcount") == "8");
        auto& inv = w.inventory(*be);
        inv.owned.insert(gun);
        inv.slot[1] = gun;
        inv.ammo[def(w, gun, "ammoname")] = 30;
        inv.clip[def(w, gun, "clipname")] = 6;
        be->obj->set("$weapon", Value(gun));
        int shotFrames = 0, badFrames = 0;
        size_t obits = w.mpState().obituaryLog.size();
        bool killed = false;
        for (int f = 0; f < 20 * 30 && !killed; ++f) {
            if (p0->obj->get("sessionstate").toString() == "playing") {
                // Keep the victim 150 units in front of the bot (it does not fire back: no input).
                const float yaw = be->obj->get("angles").asVector()[1] * 3.14159265f / 180.0f;
                const V3 o = be->obj->get("origin").asVector();
                p0->obj->set("origin", Value(V3{o[0] + std::cos(yaw) * 150.0f, o[1] + std::sin(yaw) * 150.0f, o[2]}));
            }
            const int clip = inv.clip[def(w, gun, "clipname")];
            const int before = w.stats.shots;
            w.frame();
            const int fired = clip - inv.clip[def(w, gun, "clipname")];
            if (fired > 0) {
                ++shotFrames;
                if (w.stats.shots - before != 8 * fired) ++badFrames;
            }
            const auto& log = w.mpState().obituaryLog;
            for (; obits < log.size(); ++obits)
                if (log[obits].victim == c0) {
                    std::cout << "bot kill: attacker " << log[obits].attacker << " weapon " << log[obits].weapon << "\n";
                    CHECK(log[obits].attacker == bot);
                    CHECK(log[obits].weapon == gun);
                    killed = true;
                }
        }
        std::cout << "bot shotgun: " << shotFrames << " shot frame(s), " << badFrames << " not 8 pellets\n";
        CHECK(shotFrames > 0 && badFrames == 0);
        CHECK(killed);
        CHECK(w.vm().errors().empty());
    }

    // 2. A grenade kill: the inflictor (the grenade) gives the weapon, not what the thrower holds when it goes off.
    {
        World w(vfs, "mp_toujane", true);
        w.mpStart("dm");
        const int c0 = w.mpConnect("victim");
        w.mpBegin(c0);
        run(w, 1);
        w.mpAutoJoin(c0);
        const int c1 = w.mpConnect("thrower");
        w.mpBegin(c1);
        run(w, 1);
        w.mpAutoJoin(c1);
        run(w, 2);
        Entity* p0 = w.mpClientEntity(c0);
        Entity* p1 = w.mpClientEntity(c1);
        CHECK(p0 && p1 && p1->obj->get("sessionstate").toString() == "playing");
        const std::string nade = "frag_grenade_german_mp";
        CHECK(!def(w, nade, "killicon").empty());
        const V3 vo = p0->obj->get("origin").asVector();
        p1->obj->set("origin", Value(V3{vo[0] + 40, vo[1], vo[2]}));
        p1->obj->set("$weapon", Value(std::string("kar98k_mp")));
        w.clientThrowGrenade(*p1, nade, V3{vo[0] + 40, vo[1], vo[2] + 60}, V3{80, 180, 0}, V3{0, 0, 0});
        // The thrower switches weapons and walks off before it explodes.
        run(w, 2);
        p1->obj->set("$weapon", Value(std::string("mp44_mp")));
        p1->obj->set("origin", Value(V3{vo[0] + 2000, vo[1], vo[2]}));
        const size_t obits = w.mpState().obituaryLog.size();
        bool found = false;
        for (int f = 0; f < 20 * 6 && !found; ++f) {
            p0->obj->set("origin", Value(vo));
            p1->obj->set("origin", Value(V3{vo[0] + 2000, vo[1], vo[2]}));
            w.frame();
            const auto& log = w.mpState().obituaryLog;
            for (size_t i = obits; i < log.size(); ++i)
                if (log[i].victim == c0) {
                    found = true;
                    std::cout << "grenade kill: attacker " << log[i].attacker << " weapon " << log[i].weapon << " mod " << log[i].mod << "\n";
                    CHECK(log[i].attacker == c1);
                    CHECK(log[i].weapon == nade);
                    const auto line = w.mpObituaryLine(log[i]);
                    std::cout << "icon " << line.icon << "\n";
                    CHECK(line.icon.find(def(w, nade, "killicon")) != std::string::npos);
                }
        }
        CHECK(found);
        CHECK(w.vm().errors().empty());
    }

    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
