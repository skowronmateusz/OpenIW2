// Swapping with a weapon item on the ground (Pickup_Weapon 0x521170 -> 0x5220f0 -> LaunchItem 0x521f70) puts the old weapon
// on the floor next to the player (needs IW2_MAIN=<game main dir>). Prints the dropped item's path.
#include "fs/vfs.h"
#include "game/world.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace iw2::game;

static int failures = 0;
#define CHECK(c)                                                                        \
    do {                                                                                \
        if (!(c)) { std::cout << "FAIL line " << __LINE__ << ": " #c "\n"; ++failures; } \
    } while (0)

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "swap drop test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "toujane");
    w.start();
    for (int i = 0; i < 5; ++i) w.frame();
    Inventory& inv = w.inventory(*w.player());
    inv = Inventory{};
    w.debugUseWeaponItem("mp40", 0, 0);
    w.debugUseWeaponItem("enfield", 0, 0);
    w.player()->obj->set("$weapon", iw2::script::Value("mp40"));
    const Vec3 p = w.player()->obj->get("origin").asVector();
    Entity* dropped = w.debugUseWeaponItem("kar98k", 0, 0);
    CHECK(dropped && dropped->classname == "weapon_mp40");
    if (!dropped) return 1;
    float maxZ = -1e9f, minZ = 1e9f;
    int settled = -1;
    Vec3 at{};
    for (int i = 1; i <= 120; ++i) {
        w.frame();
        at = dropped->obj->get("origin").asVector();
        maxZ = std::max(maxZ, at[2]);
        minZ = std::min(minZ, at[2]);
        if (settled < 0 && dropped->obj->get("$trtype").isNumber() && dropped->obj->get("$trtype").asInt() == 0) settled = i;
    }
    const float horiz = std::hypot(at[0] - p[0], at[1] - p[1]);
    std::cout << "player " << p[0] << " " << p[1] << " " << p[2] << "; item settled after " << settled << " frames at " << at[0] << " "
              << at[1] << " " << at[2] << ", rose to " << maxZ - p[2] << " above the player origin, " << horiz << " away\n";
    CHECK(settled > 0);
    CHECK(maxZ - p[2] < 70.0f);   // LaunchItem: start 35 above the origin, +200 +-50 up speed: the arc tops out about 25..32 higher
    CHECK(horiz < 150.0f);        // lands near the player (150 units/s for under a second)
    CHECK(at[2] <= p[2] + 40.0f); // and rests on the floor, not in the air
    std::cout << (failures ? "FAILED\n" : "swap drop test OK\n");
    return failures ? 1 : 0;
}
