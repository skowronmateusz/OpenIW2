#include "game/itempickup.h"
// Multiplayer dropped weapons and the MP load screen text (docs/spec/mp.md "Dropped weapons", docs/spec/mp-ui.md
// "Load screen"): a killed client drops its weapon as a weapon_<name> item through dm.gsc's dropWeapon -> dropItem; the item
// carries the client's ammunition, has its world model, falls and settles; g_maxDroppedWeapons frees the item farthest from
// the players; another client touching the item takes it. Needs the game files (IW2_MAIN).

#include "game/world.h"
#include "ui/mpserver.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <algorithm>
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

void run(World& w, int frames) {
    for (int i = 0; i < frames; ++i) w.frame();
}

std::vector<Entity*> items(World& w, const std::string& cls) {
    std::vector<Entity*> out;
    for (const auto& e : w.entities())
        if (!e->obj->removed && e->classname == cls) out.push_back(e.get());
    return out;
}

std::string def(World& w, const std::string& weapon, const char* key) {
    const auto* d = w.weapon(weapon);
    if (!d) return {};
    const auto it = d->find(key);
    return it == d->end() ? std::string() : it->second;
}

void giveWeapon(World& w, Entity& p, const std::string& weapon, int stock, int clip) {
    auto& inv = w.inventory(p);
    inv.owned.insert(weapon);
    inv.slot[1] = weapon;
    inv.ammo[def(w, weapon, "ammoname")] = stock;
    inv.clip[def(w, weapon, "clipname")] = clip;
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

    // Load screen text: the gametype's display string from maps/mp/gametypes/<g>.txt and the arena long name; raw names
    // when there is no entry.
    {
        const auto t = iw2::ui::mpLoadScreenText(vfs, "tdm", "mp_toujane");
        CHECK(t.gametype == "MPUI_TEAM_DEATHMATCH");
        CHECK(t.map == "Toujane, Tunisia");
        const auto u = iw2::ui::mpLoadScreenText(vfs, "nosuch", "mp_nosuch");
        CHECK(u.gametype == "nosuch");
        CHECK(u.map == "mp_nosuch");
    }

    // A finished load waits for the pregame click only for SP maps; MP maps (rotation, restart) go on at once.
    CHECK(iw2::ui::loadWaitsForClick("libya", false, false));
    CHECK(!iw2::ui::loadWaitsForClick("libya", true, false));
    CHECK(!iw2::ui::loadWaitsForClick("libya", false, true));
    CHECK(!iw2::ui::loadWaitsForClick("mp/mp_rhine", false, false));

    World w(vfs, "mp_toujane", true);
    w.mpStart("dm");
    const int c0 = w.mpConnect("local");
    w.mpBegin(c0);
    run(w, 1);
    w.mpAutoJoin(c0);
    const int c1 = w.mpConnect("second");
    w.mpBegin(c1);
    run(w, 1);
    w.mpAutoJoin(c1);
    run(w, 2);
    Entity* p0 = w.mpClientEntity(c0);
    Entity* p1 = w.mpClientEntity(c1);
    CHECK(p0 && p1 && p1->obj->get("sessionstate").toString() == "playing");
    const std::string w1 = p1->obj->get("$weapon").toString();
    CHECK(w1 != "none" && !w1.empty());

    // Death: dm.gsc's Callback_PlayerKilled runs _weapons::dropWeapon, which calls dropItem with the current weapon.
    {
        auto& inv = w.inventory(*p1);
        const int stock = inv.ammo[def(w, w1, "ammoname")], clip = inv.clip[def(w, w1, "clipname")];
        CHECK(stock > 0 || clip > 0);
        const auto before = items(w, "weapon_" + w1).size();
        w.debugDamage(*p1, 1000);
        run(w, 1);
        CHECK(p1->obj->get("sessionstate").toString() == "dead");
        const auto after = items(w, "weapon_" + w1);
        CHECK(after.size() == before + 1);
        if (!after.empty()) {
            Entity* it = after.back();
            CHECK(it->obj->get("model").toString() == def(w, w1, "worldmodel"));
            CHECK(it->obj->get("$ammoStock").asInt() == (stock ? stock : -1));
            CHECK(it->obj->get("$ammoClip").asInt() == (clip ? clip : -1));
            CHECK(!w.inventory(*p1).owned.count(w1));
            const Value rate = it->obj->get("$arate"); // the drop spins while it falls
            CHECK(rate.isVector() && rate.asVector()[0] <= 50.0f && rate.asVector()[1] <= 40.0f && rate.asVector()[2] <= 60.0f);
            const float z0 = it->obj->get("origin").asVector()[2];
            run(w, 40); // falls under G_RunItem and settles on the ground
            CHECK(it->obj->get("$itemsettled").truthy());
            CHECK(!it->obj->get("$arate").isVector());
            CHECK(it->obj->get("origin").asVector()[2] < z0);
        }
        for (const auto& e : w.vm().errors()) std::cerr << "  script error: " << e << "\n";
        CHECK(w.vm().errors().empty());
    }

    CHECK(iw2::game::mpDropperKeepsOff(5, 1000, 5, 999) && !iw2::game::mpDropperKeepsOff(5, 1000, 5, 1000));
    CHECK(!iw2::game::mpDropperKeepsOff(5, 1000, 6, 0));
    // The g_maxDroppedWeapons slots: with 2 slots a third drop frees the item farthest from the playing clients.
    {
        w.cvars["g_maxDroppedWeapons"] = "2";
        const auto o0 = p0->obj->get("origin").asVector();
        giveWeapon(w, *p0, "kar98k_mp", 10, 5);
        Entity* a = w.mpDropWeapon(*p0, "kar98k_mp");
        giveWeapon(w, *p0, "kar98k_mp", 10, 5);
        Entity* b = w.mpDropWeapon(*p0, "kar98k_mp");
        CHECK(a && b);
        if (a && b) {
            // a far away, b near the local player: the third drop frees a.
            a->obj->set("origin", Value(iw2::script::Vec3{o0[0] + 3000, o0[1], o0[2]}));
            giveWeapon(w, *p0, "kar98k_mp", 10, 5);
            Entity* c = w.mpDropWeapon(*p0, "kar98k_mp");
            CHECK(c != nullptr);
            CHECK(a->obj->removed);
            CHECK(!b->obj->removed);
        }
        // A clip-only weapon with an empty clip is only taken away.
        giveWeapon(w, *p0, "frag_grenade_american_mp", 0, 0);
        if (def(w, "frag_grenade_american_mp", "cliponly") == "1") CHECK(w.mpDropWeapon(*p0, "frag_grenade_american_mp") == nullptr);
        w.cvars["g_maxDroppedWeapons"] = "16";
    }

    // A non-local client touching a dropped weapon it owns takes the ammunition (G_TouchTriggers per client).
    {
        w.cvars["scr_forcerespawn"] = "1";
        for (int i = 0; i < 200 && p1->obj->get("sessionstate").toString() != "playing"; ++i) {
            w.mpSetButtons(c1, (i & 1) ? 0x1u : 0u); // dm.gsc respawn(): a button press after the killcam
            run(w, 1);
        }
        w.mpSetButtons(c1, 0);
        CHECK(p1->obj->get("sessionstate").toString() == "playing");
        // Spawn ammunition: the pistol (setWeaponSlotWeapon + giveMaxAmmo) and the main weapon have full clips.
        for (int s = 1; s <= 2; ++s) {
            const std::string sw = w.inventory(*p1).slot[size_t(s)];
            CHECK(!sw.empty());
            if (!sw.empty()) CHECK(w.inventory(*p1).clip[def(w, sw, "clipname")] == std::atoi(def(w, sw, "clipsize").c_str()));
        }
        const std::string pw = p1->obj->get("$weapon").toString();
        auto& inv = w.inventory(*p1);
        const std::string ammoName = def(w, pw, "ammoname");
        inv.ammo[ammoName] = 0;
        giveWeapon(w, *p0, pw, 30, 0);
        // A tag the model does not have: the box centre and the dropper's yaw, with the roll raised by 90 degrees.
        giveWeapon(w, *p0, pw, 30, 1);
        if (Entity* t = w.mpDropWeapon(*p0, pw, "no_such_tag")) {
            const auto o0 = p0->obj->get("origin").asVector();
            CHECK(std::fabs(t->obj->get("origin").asVector()[2] - (o0[2] + 35.0f)) < 1.0f);
            CHECK(std::fabs(t->obj->get("angles").asVector()[2] - (p0->obj->get("angles").asVector()[2] + 90.0f)) < 1e-3f);
            t->obj->removed = true;
        } else {
            CHECK(false);
        }
        giveWeapon(w, *p0, pw, 30, 0);
        Entity* item = w.mpDropWeapon(*p0, pw);
        CHECK(item != nullptr);
        if (item) {
            item->obj->set("origin", p1->obj->get("origin"));
            item->obj->set("$trtype", Value(int32_t(0)));
            item->obj->set("$trbase", p1->obj->get("origin"));
            run(w, 2);
            CHECK(item->obj->removed);
            CHECK(w.inventory(*p1).ammo[ammoName] == 30);
        }
    }

    // Statistics: 100 bot deaths in play (bots fight, switch to the pistol, throw grenades, fall). Each time the GSC
    // dropWeapon condition holds (a current slot weapon with clip or reserve ammunition), a weapon_<current> item with
    // its world model exists in the frame of the death, lies within reach of the body and settles above the floor
    // below it. After each respawn the pistol and the main weapon have full clips.
    {
        w.cvars["scr_forcerespawn"] = "1";
        w.cvars["g_maxDroppedWeapons"] = "16";
        std::vector<int> bots;
        for (int i = 0; i < 4; ++i) bots.push_back(w.mpAddBot());
        run(w, 100);
        int deaths = 0, expected = 0, dropped = 0, settled = 0, respawns = 0, fullClips = 0, otherThanLocal = 0;
        const std::string localWeapon = p0->obj->get("$weapon").toString();
        for (int step = 0; step < 6000 && deaths < 100; ++step) {
            run(w, 1);
            for (int c : bots) {
                Entity* b = w.mpClientEntity(c);
                if (!b || b->obj->get("sessionstate").toString() != "playing") continue;
                if (b->obj->get("$fullclipchecked").truthy() == false) {
                    // Spawn ammunition: every slot weapon has a full clip.
                    auto& inv = w.inventory(*b);
                    bool full = true;
                    for (int s = 1; s <= 2; ++s) {
                        const std::string sw = inv.slot[size_t(s)];
                        if (sw.empty()) continue;
                        const int size = std::atoi(def(w, sw, "clipsize").c_str());
                        // The pistol is not fired in the frame of the spawn; the main weapon may already have been.
                        if (def(w, sw, "weaponclass") != "pistol") continue;
                        if (inv.clip[def(w, sw, "clipname")] != size) {
                            full = false;
                            std::cerr << "  respawn clip " << sw << " " << inv.clip[def(w, sw, "clipname")] << "/" << size << "\n";
                        }
                    }
                    ++respawns;
                    if (full) ++fullClips;
                    b->obj->set("$fullclipchecked", Value(true));
                }
                if ((step + c * 7) % 37 != 0) continue;
                const std::string cur = b->obj->get("$weapon").toString();
                auto& inv = w.inventory(*b);
                const bool inSlot = cur == inv.slot[1] || cur == inv.slot[2];
                const bool want = cur != "none" && !cur.empty() && inSlot &&
                                  (inv.clip[def(w, cur, "clipname")] > 0 || inv.ammo[def(w, cur, "ammoname")] > 0);
                const auto before = items(w, "weapon_" + cur);
                const auto o = b->obj->get("origin").asVector();
                w.debugDamage(*b, 1000);
                run(w, 1);
                if (b->obj->get("sessionstate").toString() != "dead") continue;
                b->obj->set("$fullclipchecked", Value(false));
                ++deaths;
                if (!want) continue;
                ++expected;
                if (cur != localWeapon) ++otherThanLocal;
                Entity* it = nullptr; // the new item (an old one may have been freed for its slot in the same frame)
                for (Entity* a : items(w, "weapon_" + cur))
                    if (std::find(before.begin(), before.end(), a) == before.end()) it = a;
                if (!it) {
                    std::cerr << "  no drop of " << cur << " (death " << deaths << ")\n";
                    continue;
                }
                if (it->obj->get("model").toString() != def(w, cur, "worldmodel")) continue;
                ++dropped;
                // The item's path, frame by frame: it never passes through a world surface and comes to rest on one.
                // (Items do not collide with players, clip mask 0x491, and drift along the dropper's yaw, so a bot killed
                // in the air, on another player or at a ledge drops its weapon to the floor below, not under its body.)
                bool through = false;
                iw2::script::Vec3 prev = it->obj->get("origin").asVector();
                for (int k = 0; k < 40 && !it->obj->removed; ++k) {
                    run(w, 1);
                    const auto now = it->obj->get("origin").asVector();
                    const auto seg = w.collision().traceWorld({prev[0], prev[1], prev[2]}, {now[0], now[1], now[2]}, {-1, -1, -1}, {1, 1, 1}, 0x491);
                    if (seg.fraction < 1.0f && now[2] < seg.endPos[2] - 1.0f) through = true; // went on past the surface
                    prev = now;
                }
                if (it->obj->removed) { ++settled; continue; } // taken by a bot walking over it
                const auto io = it->obj->get("origin").asVector();
                const float dx = io[0] - o[0], dy = io[1] - o[1];
                // Rests on the world or on an entity with collision (a script_brushmodel); players are not in the item clip mask.
                const auto below = w.collision().trace({io[0], io[1], io[2]}, {io[0], io[1], io[2] - 2.0f}, {-1, -1, -1}, {1, 1, 1}, 0x491, it->number);
                if (it->obj->get("$itemsettled").truthy() && !through && below.fraction < 1.0f && dx * dx + dy * dy < 200.0f * 200.0f)
                    ++settled;
                else
                    std::cerr << "  item " << cur << " lost: at " << io[0] << "," << io[1] << "," << io[2] << " body " << o[0] << ","
                              << o[1] << "," << o[2] << " through " << through << " floor " << (below.fraction < 1.0f) << "\n";
            }
        }
        std::cout << "mpdrop stats: deaths " << deaths << " expected " << expected << " dropped " << dropped << " settled "
                  << settled << " other-than-local " << otherThanLocal << " respawns " << respawns << " full-clips "
                  << fullClips << "\n";
        CHECK(deaths >= 90);
        CHECK(expected > 50);
        CHECK(dropped == expected);
        CHECK(settled == expected);
        CHECK(otherThanLocal > 0);
        CHECK(fullClips == respawns);
    }

    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "mpdrop: ok\n";
    return 0;
}
