// Using a weapon item sends "trigger" to the item with the player before the pickup (the common use, 0x5341a0). silotown_assault.gsc
// (objective_pickupSniperRifle, line 1187 on) relies on it: it spawns a suspended sniper rifle, hides it, shows a model in its place
// and waits for "trigger" on the item to delete the model and set the flag player_has_sniper_rifle. The pickup swaps with a held
// weapon of the same slot class. Needs IW2_MAIN=<game main dir>.
#include "fs/vfs.h"
#include "game/world.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace iw2::game;

static int failures = 0;
#define CHECK(c)                                                                        \
    do {                                                                                \
        if (!(c)) { std::cout << "FAIL line " << __LINE__ << ": " #c "\n"; ++failures; } \
    } while (0)

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "item trigger test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    const auto dir = std::filesystem::temp_directory_path() / ("iw2_test_itemtrigger_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "maps");
    // The shape of the silo script: a hidden suspended weapon item, a model at the same place, waittill on the item.
    std::ofstream(dir / "maps/itemtrigger.gsc") << "main()\n{\n\tlevel.n = 0;\n\tlevel.fake = spawn(\"script_model\", self.origin);\n"
        "\tlevel.item = spawn(\"weapon_kar98k_sniper\", self.origin, 1);\n\tlevel.item hide();\n"
        "\tlevel.item waittill(\"trigger\", who);\n\tlevel.n = level.n + 1;\n\tlevel.who = who;\n\tlevel.fake delete();\n}\n";
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir.string());
    World w(vfs, "toujane");
    w.start();
    for (int i = 0; i < 5; ++i) w.frame();
    Inventory& inv = w.inventory(*w.player());
    inv = Inventory{};
    w.debugUseWeaponItem("mp40", 0, 0);
    w.debugUseWeaponItem("springfield", 0, 0);
    w.player()->obj->set("$weapon", iw2::script::Value("mp40"));
    w.vm().start("maps/itemtrigger", "main", {}, iw2::script::Value(w.player()->obj));
    for (int i = 0; i < 3; ++i) w.frame();
    CHECK(w.vm().level()->get("n").asInt() == 0);
    Entity* item = nullptr;
    for (const auto& e : w.entities())
        if (!e->obj->removed && e->classname == "weapon_kar98k_sniper") item = e.get();
    CHECK(item != nullptr);
    if (!item) return 1;
    // The player stands at the item; use it (F). Both slots are full and the held MP40 shares the slot class: swap.
    Entity* dropped = w.debugUseItem(*item);
    for (int i = 0; i < 3; ++i) w.frame();
    CHECK(inv.owned.count("kar98k_sniper") && !inv.owned.count("mp40"));
    CHECK(dropped && dropped->classname == "weapon_mp40");
    CHECK(w.vm().level()->get("n").asInt() == 1);
    CHECK(w.vm().level()->get("who").isObject() && w.vm().level()->get("who").asObject() == w.player()->obj);
    // Swapping again with the dropped MP40 notifies nobody (the waiter is gone) and does not set the flag twice.
    if (dropped) w.debugUseItem(*dropped);
    for (int i = 0; i < 3; ++i) w.frame();
    CHECK(w.vm().level()->get("n").asInt() == 1);
    std::filesystem::remove_all(dir);
    std::cout << (failures ? "FAILED\n" : "item trigger test OK\n");
    return failures ? 1 : 0;
}
