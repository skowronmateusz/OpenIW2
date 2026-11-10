// info_player_start spawn (0x4e84a0, docs/spec/entities.md, needs IW2_MAIN): the start point is renamed to
// info_player_deathmatch, so the animscript goal setup gives actors that target it the player as goal entity
// (cityhall: the start squad follows the player's friendly chain instead of standing at the start point).
#include "fs/vfs.h"
#include "game/world.h"

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
        std::cout << "playerstart test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "cityhall");
    w.start();
    for (int i = 0; i < 40; ++i) w.frame();
    int starts = 0, renamed = 0;
    for (const auto& e : w.entities()) {
        if (e->obj->get("classname").toString() == "info_player_start") ++starts;
        if (e->classname == "info_player_deathmatch" && e->obj->get("classname").toString() == "info_player_deathmatch") ++renamed;
    }
    CHECK(starts == 0);
    CHECK(renamed == 1);
    // The player starts inside the trigger of the first chain (target auto20); put him on it as his touch would.
    for (const auto& e : w.entities())
        if (e->classname == "trigger_friendlychain" && e->obj->get("target").toString() == "auto20" && w.player()) w.debugTouchFriendlyChain(*e, *w.player());
    for (int i = 0; i < 20; ++i) w.frame();
    int allies = 0, following = 0, onChain = 0;
    for (const auto& e : w.entities()) {
        if (!e->isActor || e->isSpawner || e->obj->removed || e->obj->get("team").toString() != "allies") continue;
        ++allies;
        if (const auto g = e->obj->get("$goalent"); g.isObject() && w.player() && g.asObject() == w.player()->obj) ++following;
        if (e->chainNode >= 0) ++onChain; // the actor's chain node (+0x78c)
    }
    CHECK(allies == 4);
    CHECK(following == allies); // animscripts/init.gsc:396-415 swaps the renamed start point for the player
    CHECK(onChain == allies);   // and they take nodes of the player's chain (trigger auto20 at the start)
    if (failures) return 1;
    std::cout << "playerstart test passed\n";
    return 0;
}
