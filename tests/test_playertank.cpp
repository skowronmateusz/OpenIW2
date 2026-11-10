// 88ridge (only with IW2_MAIN set): when the tank the player rides is destroyed the player must not keep driving the wreck
// (docs/spec/vehicles.md "Damage attacker, death and corpse"). The vehicle's lethal hit lets the rider out; the shipped
// death script then kills the player with dodamage, which the rider damage filter would drop while the player still rode.
#include "fs/vfs.h"
#include "game/world.h"

#include <cstdlib>
#include <iostream>

using namespace iw2::game;

static int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "player tank test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "88ridge");
    w.start();
    int tankId = -1;
    for (int i = 0; i < 400 && tankId < 0; ++i) {
        w.frame();
        if (w.player() && w.player()->usingVehicle >= 0) tankId = w.player()->usingVehicle;
    }
    std::cout << "player vehicle " << tankId << "\n";
    CHECK(tankId >= 0);
    if (tankId < 0) return 1;
    for (int i = 0; i < 40; ++i) w.frame();
    Entity* tank = nullptr;
    for (const auto& e : w.entities())
        if (e->number == tankId) tank = e.get();
    CHECK(tank && tank->vehicle);
    if (!tank || !tank->vehicle) return 1;
    CHECK(w.player()->usingVehicle == tankId); // still driving
    CHECK(!w.playerDeadWeaponless());

    w.debugDamage(*tank, 100000);
    for (int i = 0; i < 200; ++i) w.frame(); // the death script's repeated dodamage
    std::cout << "after: using " << w.player()->usingVehicle << " owner " << tank->vehicle->owner << " dead " << w.playerDeadWeaponless() << "\n";
    CHECK(w.player()->usingVehicle < 0);   // out of the wreck
    CHECK(tank->vehicle->owner < 0);
    CHECK(w.playerDeadWeaponless());       // and killed by the shipped script, not driving on
    std::cout << (failures ? "FAIL\n" : "ok\n");
    return failures ? 1 : 0;
}
