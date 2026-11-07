// Weapons across a persistent level change (campaign.md "Level change", inventory.md): the mission list in maps/_endmission.gsc marks
// which levels keep the player's weapons (decoytrenches -> decoytown, downtown_assault -> cityhall, toujane_ride -> toujane,
// bergstein -> hill400_assault, ...). The carry (changelevel with the persistent flag, writer 0x4e6c20 / reader 0x4e6de0) hands over the
// owned weapons, the two primary slots, the current weapon, the offhand and the ammunition; the destination's loadout then only
// refills ammunition (maps/_loadout.gsc, game["gaveweapons"] / game["expectedlevel"]) and does not give or take a weapon.
// Needs the game files (IW2_MAIN).
#include "fs/vfs.h"
#include "game/world.h"

#include <cstdlib>
#include <iostream>

using namespace iw2::game;
using namespace iw2::script;

namespace {

int failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

std::string describe(World& w) {
    const Inventory& i = w.inventory(*w.player());
    std::string s;
    for (const auto& o : i.owned) s += o + " ";
    return s + "| " + i.slot[1] + "," + i.slot[2] + " offhand " + i.offhand + " current " + w.player()->obj->get("$weapon").toString();
}

// Plays `from` for a moment, optionally swaps the second primary for the MP40 (what the player does with the use key on a dropped
// weapon), ends the mission through maps/_endmission.gsc (as decoytrenches does) and starts `to` with the carried state, the way the
// front end does for a persistent change. Returns the destination world after `frames` frames.
struct Run {
    Inventory before, after;
    std::string curBefore, curAfter;
    bool persistent = false;
    std::string nextMap;
    Value gaveWeapons, expectedLevel;
    bool ok = false;
};

Run carry(iw2::fs::Vfs& vfs, const std::string& from, const std::string& to, bool swapInMp40, int frames) {
    Run r;
    World a(vfs, from);
    a.start();
    for (int i = 0; i < 100; ++i) a.frame();
    Inventory& inv = a.inventory(*a.player());
    if (swapInMp40) {
        // Thompson (second slot) replaced by an MP40, which is held; the Enfield stays in the first slot.
        inv.owned.erase("thompson");
        inv.slot[2] = "mp40";
        inv.owned.insert("mp40");
        inv.ammo["mp40"] = 77;
        inv.clip["mp40"] = 20;
        a.player()->obj->set("$weapon", Value("mp40"));
    }
    r.before = inv;
    r.curBefore = a.player()->obj->get("$weapon").toString();
    a.vm().start("maps/_endmission.gsc", "nextmission", {}, Value(a.vm().level()));
    std::optional<LevelChange> change;
    for (int i = 0; i < 400 && !change; ++i) {
        a.frame();
        change = a.takeDueLevelChange();
    }
    if (!change) return r;
    r.persistent = change->persistent;
    r.nextMap = change->map;
    const auto carried = a.carriedPlayerState();
    if (!carried) return r;
    World b(vfs, to);
    for (const auto& [k, v] : a.vm().game()->fields) b.vm().game()->set(k, v);
    b.setCarriedPlayer(*carried);
    b.start();
    for (int i = 0; i < frames; ++i) b.frame();
    r.after = b.inventory(*b.player());
    r.curAfter = b.player()->obj->get("$weapon").toString();
    r.gaveWeapons = b.vm().game()->get("gaveweapons");
    r.expectedLevel = b.vm().game()->get("expectedlevel");
    std::cout << from << " -> " << to << ": " << describe(a) << "  =>  " << describe(b) << "\n";
    r.ok = true;
    return r;
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

    // The reported case: an Enfield and a picked-up MP40 leave decoytrenches and both are still there in decoytown, with the MP40
    // in hand, after the loadout's refill thread has run (33 weapons x 0.05 s, 40 frames are enough).
    {
        const Run r = carry(vfs, "decoytrenches", "decoytown", true, 120);
        CHECK(r.ok);
        CHECK(r.persistent && r.nextMap == "decoytown");
        CHECK(r.after.owned.count("enfield") && r.after.owned.count("mp40"));
        CHECK(!r.after.owned.count("thompson") && !r.after.owned.count("bren")); // the destination's own loadout is not given on top
        CHECK(r.after.slot[1] == "enfield" && r.after.slot[2] == "mp40");
        CHECK(r.curAfter == "mp40" && r.after.offhand == "mk1britishfrag");
        CHECK(r.gaveWeapons.isNumber() && r.gaveWeapons.asInt() == 1);
        CHECK(r.expectedLevel.isString() && r.expectedLevel.asString() == "decoytown");
        // refill_ammo gives maximum ammunition for every owned bullet weapon
        CHECK(r.after.ammo.at("mp40") >= 77 && r.after.ammo.at("enfield") > 0);
    }

    // The other levels of the campaign that keep their weapons: whatever the source level had, the destination still has, except
    // what its own script takes (trainyard, downtown_sniper and duhoc_defend take grenades, maps/<map>.gsc).
    struct Pair {
        const char* from;
        const char* to;
        std::set<std::string> scriptTakes;
    };
    const Pair pairs[] = {{"downtown_assault", "cityhall", {}},
                          {"toujane_ride", "toujane", {}},
                          {"bergstein", "hill400_assault", {}},
                          {"newvillers", "breakout", {}}};
    for (const Pair& p : pairs) {
        const Run r = carry(vfs, p.from, p.to, false, 60);
        CHECK(r.ok);
        CHECK(r.persistent);
        for (const auto& w : r.before.owned) CHECK(r.after.owned.count(w) || p.scriptTakes.count(w));
        CHECK(r.after.slot[1] == r.before.slot[1] && r.after.slot[2] == r.before.slot[2]);
        CHECK(r.curAfter == r.curBefore);
    }

    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "levelcarry ok\n";
    return failures ? 1 : 0;
}
