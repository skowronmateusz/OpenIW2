// Aim when the hand weapon is lowered (docs/spec/weapons.md "Aim when the weapon goes down"): the player aims a rifle and
// gets on a vehicle gun (the decoytown flak88), which holsters the hand weapon (0x4de880). The aim flag needs a weapon that
// may aim now (0x4dcf70: not while dropping), so the aim runs out during the drop, and with no weapon (weapon 0 has no
// aimDownSight) the aim fraction is 0 (0x4dd13b -> 0x4dd12b). Needs the game data (IW2_MAIN=<game main dir>).
#include "fs/vfs.h"
#include "game/playerweapon.h"
#include "game/world.h"

#include <cstdlib>
#include <iostream>

using namespace iw2::game;

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
        std::cout << "vehicle ads test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "decoytown");
    const auto* def = w.weapon("kar98k");
    CHECK(def != nullptr);
    if (!def) return 1;
    PlayerWeapon pw;
    pw.lookup = [&](const std::string& n) { return w.weapon(n); };
    pw.equip("kar98k", *def);
    PlayerWeapon::Input in;
    in.weapon = "kar98k";
    in.ads = true;
    for (int i = 0; i < 100; ++i) pw.update(0.02f, in);
    CHECK(pw.adsFrac() == 1.0f);

    // In the vehicle, aim still held: the weapon drops and the aim goes out with it.
    in.holster = true;
    in.inVehicle = true;
    bool holstered = false;
    for (int i = 0; i < 200; ++i) {
        pw.update(0.02f, in);
        if (!pw.equipped()) {
            holstered = true;
            CHECK(pw.adsFrac() == 0.0f);
        }
    }
    CHECK(holstered);
    CHECK(pw.adsFrac() == 0.0f);

    // Out of the vehicle: the weapon comes back up; the aim only returns once it is raised.
    in.holster = false;
    in.inVehicle = false;
    pw.update(0.02f, in);
    CHECK(pw.adsFrac() == 0.0f);
    for (int i = 0; i < 200; ++i) pw.update(0.02f, in);
    CHECK(pw.adsFrac() == 1.0f);
    return failures ? 1 : 0;
}
