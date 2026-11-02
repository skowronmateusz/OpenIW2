// Offhand grenades on a turret or in a vehicle. PM_Weapon returns before any state change while eFlags & 0x300 is set (0x4dfc59) and the
// offhand step returns with eFlags & 0x20300 (0x4df473). Getting off with frag held must not prime a cooking grenade (iw2_grenadeCook).
// Covers the MG42 / vehicle-turret path (the app skips the update and calls blockOffhand) and the vehicle path (offhandBlocked input).

#include "game/playerweapon.h"

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
using iw2::game::PlayerWeapon;
using Def = PlayerWeapon::Def;

struct Rig {
    Def g{{"weapontype", "grenade"}, {"offhandclass", "Frag Grenade"}, {"fusetime", "4"}, {"holdfiretime", "0.1"}, {"firetime", "0.1"}};
    Def a{{"quickdroptime", "0.1"}, {"quickraisetime", "0.1"}, {"raisetime", "0"}};
    PlayerWeapon w;
    PlayerWeapon::Input in;
    explicit Rig(bool cook) {
        w.grenadeCook = cook;
        w.lookup = [this](const std::string& k) -> const Def* { return k == "g" ? &g : &a; };
        w.findOffhand = [](const std::string& cls) { return cls == "Frag Grenade" ? std::string("g") : std::string(); };
        w.equip("a", a);
        in.weapon = "a";
        w.update(0.01f, in);
    }
    void run(int ticks) { for (int i = 0; i < ticks; ++i) w.update(0.05f, in); }
    bool primed() const { return w.stateNumber() >= 12 && w.stateNumber() <= 15; }
};
} // namespace

int main() {
    for (const bool cook : {true, false}) {
        // Vehicle: frag held the whole time, then out of the vehicle.
        Rig v(cook);
        v.in.frag = true;
        v.in.offhandBlocked = true;
        v.run(10);
        CHECK(!v.primed());
        v.in.offhandBlocked = false;
        v.run(4);
        CHECK(v.primed() == !cook); // cook: latched until released; shipped behaviour: starts at once
        v.in.frag = false;
        v.run(60);
        v.in.frag = true; // a fresh press works again
        v.run(2);
        CHECK(v.primed());

        // Turret: the app does not call update while mounted, only blockOffhand().
        Rig t(cook);
        t.in.frag = true;
        for (int i = 0; i < 10; ++i) t.w.blockOffhand();
        CHECK(!t.primed());
        t.run(4); // off the turret, key still down
        CHECK(t.primed() == !cook);
        t.in.frag = false;
        t.run(60);
        t.in.frag = true;
        t.run(2);
        CHECK(t.primed());

        // Released while mounted: nothing latched.
        Rig r(cook);
        r.in.frag = true;
        r.w.blockOffhand();
        r.in.frag = false;
        r.run(2);
        r.in.frag = true;
        r.run(2);
        CHECK(r.primed());
    }
    return failures ? 1 : 0;
}
