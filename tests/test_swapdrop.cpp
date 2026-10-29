// Swapping with a weapon item on the ground (Touch_Item 0x521ad0 -> Pickup_Weapon 0x521170 -> 0x5220f0) is static: 0x5220f0
// spawns the old weapon through LaunchItem 0x521f70, but Pickup_Weapon then copies spawnflags / groundEntityNum from the used
// item and calls G_SetOrigin / G_SetAngles (0x4d8380 / 0x4d83c0) with the used item's currentOrigin / currentAngles
// (0x521551..0x52157c): the old weapon lies where the used item lay, stationary, and the used item is freed (0x521c4a).
// Needs IW2_MAIN=<game main dir>.
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
    // A kar98k item 40 units in front of the player (+x), left to settle on the floor like a map item.
    Entity& ground = w.debugSpawnWeaponItem("kar98k", Vec3{p[0] + 40.0f, p[1], p[2] + 10.0f}, Vec3{0, 30, 90});
    for (int i = 0; i < 20; ++i) w.frame();
    CHECK(!ground.obj->removed);
    const Vec3 g = ground.obj->get("origin").asVector();
    const Vec3 ga = ground.obj->get("angles").asVector();
    const iw2::script::Value gGround = ground.obj->get("$groundent");
    Entity* dropped = w.debugUseItem(ground);
    CHECK(dropped && dropped->classname == "weapon_mp40");
    if (!dropped) return 1;
    CHECK(ground.obj->removed);
    CHECK(inv.owned.count("kar98k") && !inv.owned.count("mp40"));
    // Right after the swap: the used item's origin / angles, stationary, zero delta, same ground entity.
    const Vec3 d = dropped->obj->get("origin").asVector();
    CHECK(d[0] == g[0] && d[1] == g[1] && d[2] == g[2]);
    const Vec3 da = dropped->obj->get("angles").asVector();
    CHECK(da[0] == ga[0] && da[1] == ga[1] && da[2] == ga[2]);
    CHECK(dropped->obj->get("$trtype").asInt() == 0);
    const Vec3 dd = dropped->obj->get("$trdelta").asVector();
    CHECK(dd[0] == 0.0f && dd[1] == 0.0f && dd[2] == 0.0f);
    CHECK(gGround.isNumber() && dropped->obj->get("$groundent").asInt() == gGround.asInt());
    // It stays there (no toss, no fall).
    float maxMove = 0.0f;
    for (int i = 1; i <= 60; ++i) {
        w.frame();
        if (dropped->obj->removed) break;
        const Vec3 at = dropped->obj->get("origin").asVector();
        maxMove = std::max(maxMove, std::fabs(at[0] - g[0]) + std::fabs(at[1] - g[1]) + std::fabs(at[2] - g[2]));
    }
    CHECK(!dropped->obj->removed);
    std::cout << "used item at " << g[0] << " " << g[1] << " " << g[2] << "; dropped item moved at most " << maxMove << " in 60 frames\n";
    CHECK(maxMove == 0.0f);
    std::cout << (failures ? "FAILED\n" : "swap drop test OK\n");
    return failures ? 1 : 0;
}
