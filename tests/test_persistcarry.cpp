// Persistent level change (writer 0x4e6c20, reader 0x4e6de0): besides the weapons the player keeps the
// entity's health (+0x13c) and maxhealth (+0x140, range table 0x56eeb0) and the stats words ps+0x108..+0x117 (0x56eec8), of
// which the port keeps the dead yaw (+0x10c). Needs the game files (IW2_MAIN).

#include "fs/vfs.h"
#include "game/world.h"

#include <cstdlib>
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

} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    using iw2::script::Value;
    std::optional<iw2::game::World::CarriedPlayer> carried;
    {
        iw2::game::World a(vfs, "moscow");
        a.prepareStart();
        CHECK(a.player() != nullptr);
        a.player()->obj->set("health", Value(int32_t(37)));
        a.player()->obj->set("maxhealth", Value(int32_t(120)));
        carried = a.carriedPlayerState();
    }
    CHECK(carried && carried->health == 37 && carried->maxhealth == 120);
    {
        iw2::game::World b(vfs, "moscow");
        b.prepareStart();
        const int spawnHealth = b.player()->obj->get("health").asInt();
        CHECK(spawnHealth != 37); // without a carry the client spawn sets g_player_maxhealth
    }
    {
        iw2::game::World c(vfs, "moscow");
        if (carried) c.setCarriedPlayer(*carried);
        c.prepareStart();
        CHECK(c.player()->obj->get("health").asInt() == 37);
        CHECK(c.player()->obj->get("maxhealth").asInt() == 120);
    }
    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "persistcarry ok\n";
    return failures ? 1 : 0;
}
