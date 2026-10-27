// Tests for the player weapon state machine with hand-written weapon definitions.

#include "game/playerweapon.h"

#include <iostream>

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using iw2::game::PlayerWeapon;

int run(PlayerWeapon& w, float seconds, bool fire, bool reload = false) {
    int shots = 0;
    for (int i = 0; i < int(seconds * 100 + 0.5f); ++i) shots += w.update(0.01f, {fire, reload && i == 0});
    return shots;
}

void boltAction() {
    PlayerWeapon w;
    w.equip("rifle", {{"semiauto", "1"}, {"boltaction", "1"}, {"firetime", "0.3"}, {"rechambertime", "1"},
                      {"clipsize", "5"}, {"startammo", "20"}, {"raisetime", "0.5"}, {"reloadtime", "2"},
                      {"idleanim", "idle"}, {"fireanim", "fire"}, {"rechamberanim", "rechamber"}});
    CHECK(w.clip() == 5 && w.stock() == 15);
    CHECK(run(w, 0.4f, true) == 0);        // still raising
    CHECK(run(w, 0.2f, true) == 1);        // first shot once ready
    CHECK(w.anim() == "fire");
    CHECK(run(w, 0.5f, true) == 0);        // trigger held: semi-auto waits for release
    CHECK(w.anim() == "rechamber");
    CHECK(run(w, 0.9f, false) == 0);       // rechambering
    CHECK(run(w, 0.1f, true) == 1);
    CHECK(w.clip() == 3);
}

void automatic() {
    PlayerWeapon w;
    w.equip("smg", {{"semiauto", "0"}, {"firetime", "0.1"}, {"clipsize", "20"}, {"startammo", "40"},
                    {"raisetime", "0"}, {"reloadtime", "2"}, {"reloademptytime", "2.5"}});
    CHECK(run(w, 1.005f, true) == 10);
    CHECK(w.clip() == 10);
    CHECK(run(w, 1.5f, true) == 10);      // empties the clip, then starts the empty reload
    CHECK(w.clip() == 0);
    CHECK(run(w, 2.6f, false) == 0);
    CHECK(w.clip() == 20 && w.stock() == 0);
    run(w, 0.3f, true);
    CHECK(run(w, 0.1f, false, true) == 0); // manual reload with no stock does nothing
    CHECK(w.stock() == 0);
}

using Def = PlayerWeapon::Def;

// Weapon change (0x4ddb40 / PM_Weapon state 2): dropTime, then the new weapon's raiseTime.
void weaponChange() {
    const Def a{{"firetime", "0.1"}, {"clipsize", "5"}, {"startammo", "10"}, {"droptime", "0.3"}, {"raisetime", "0.4"},
                {"dropanim", "a_drop"}, {"raiseanim", "a_raise"}, {"meleedamage", "50"}};
    const Def b{{"firetime", "0.1"}, {"clipsize", "8"}, {"startammo", "8"}, {"droptime", "0.2"}, {"raisetime", "0.5"},
                {"raiseanim", "b_raise"}};
    PlayerWeapon w;
    w.lookup = [&](const std::string& n) -> const Def* { return n == "a" ? &a : n == "b" ? &b : nullptr; };
    w.equip("a", a);
    PlayerWeapon::Input in;
    in.weapon = "a";
    for (int i = 0; i < 50; ++i) w.update(0.01f, in); // raised
    in.fire = true;
    w.update(0.01f, in);
    CHECK(w.clip() == 4);
    in.fire = false;
    for (int i = 0; i < 20; ++i) w.update(0.01f, in);
    in.weapon = "b";
    w.update(0.01f, in);
    CHECK(w.anim() == "a_drop" && w.stateNumber() == 2);
    for (int i = 0; i < 29; ++i) w.update(0.01f, in);
    CHECK(w.name() == "a");
    w.update(0.01f, in);
    w.update(0.01f, in);
    CHECK(w.name() == "b" && w.anim() == "b_raise" && w.stateNumber() == 1 && w.clip() == 8);
    bool raised = false;
    for (const auto& e : w.takeEvents()) raised |= e.kind == PlayerWeapon::Event::WeaponRaised && e.weapon == "b";
    CHECK(raised);
    in.weapon = "a";
    for (int i = 0; i < 200; ++i) w.update(0.01f, in);
    CHECK(w.name() == "a" && w.clip() == 4); // ammo kept while put away
}

// Melee (0x4def20): meleeDelay until the blow (MeleeHit), meleeTime in all.
void melee() {
    const Def a{{"meleedamage", "50"}, {"meleetime", "0.6"}, {"meleedelay", "0.2"}, {"meleeanim", "melee"}, {"raisetime", "0"}};
    PlayerWeapon w;
    w.lookup = [&](const std::string&) { return &a; };
    w.equip("a", a);
    PlayerWeapon::Input in;
    in.weapon = "a";
    w.update(0.01f, in);
    in.melee = true;
    w.update(0.01f, in);
    CHECK(w.anim() == "melee" && w.stateNumber() == 10);
    int hits = 0;
    for (int i = 0; i < 100; ++i) {
        w.update(0.01f, in); // held: no second swing
        for (const auto& e : w.takeEvents()) hits += e.kind == PlayerWeapon::Event::MeleeHit;
        if (i == 15) CHECK(hits == 0);
        if (i == 25) CHECK(hits == 1);
    }
    CHECK(hits == 1 && w.stateNumber() == 0);
}

// Offhand (0x4df470 and states 12–16): quick drop, pull the pin (holdFireTime), hold, throw on release, quick raise.
void offhand() {
    const Def a{{"quickdroptime", "0.1"}, {"quickraisetime", "0.1"}, {"raisetime", "0"}, {"quickdropanim", "qd"}};
    const Def g{{"weapontype", "grenade"}, {"holdfiretime", "0.3"}, {"firetime", "0.2"}, {"fusetime", "3"},
                {"holdfireanim", "pullpin"}, {"fireanim", "throw"}, {"gunmodel", "grenade"}, {"pullbacksound", "pin"},
                {"firesound", "toss"}};
    PlayerWeapon w;
    w.lookup = [&](const std::string& n) -> const Def* { return n == "frag" ? &g : &a; };
    w.findOffhand = [](const std::string& cls) { return cls == "Frag Grenade" ? std::string("frag") : std::string(); };
    w.equip("a", a);
    PlayerWeapon::Input in;
    in.weapon = "a";
    w.update(0.01f, in);
    in.frag = true;
    w.update(0.01f, in);
    CHECK(w.stateNumber() == 12 && w.anim() == "qd");
    for (int i = 0; i < 11; ++i) w.update(0.01f, in);
    CHECK(w.stateNumber() == 13 && w.anim() == "pullpin" && w.gunModel() == "xmodel/grenade");
    CHECK(w.takeSounds() == std::vector<std::string>{"pin"}); // event 0xa5 on entering state 13
    for (int i = 0; i < 60; ++i) w.update(0.01f, in);
    CHECK(w.stateNumber() == 14); // held
    int throws = 0;
    in.frag = false;
    for (int i = 0; i < 2; ++i) {
        w.update(0.01f, in);
        for (const auto& e : w.takeEvents()) throws += e.kind == PlayerWeapon::Event::OffhandThrow && e.weapon == "frag";
    }
    CHECK(throws == 1 && w.stateNumber() == 15 && w.anim() == "throw");
    CHECK(w.takeSounds() == std::vector<std::string>{"toss"}); // event 0xa6 on entering state 15
    for (int i = 0; i < 40; ++i) w.update(0.01f, in);
    CHECK(w.stateNumber() == 0 && w.name() == "a");
}

} // namespace

int main() {
    boltAction();
    automatic();
    weaponChange();
    melee();
    offhand();
    if (failures) return 1;
    std::cout << "playerweapon tests passed\n";
    return 0;
}
