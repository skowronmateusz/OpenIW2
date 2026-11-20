// Using a weapon item sends "touch" to the item with the player (0x5341a0); the item's "trigger" follows from the pickup itself
// (0x52173e..0x52176f), with the player and the weapon item dropped by the swap, or undefined when the weapon went into a free slot.
// silotown_assault.gsc (objective_pickupSniperRifle, line 1187 on) waits for it: it spawns a suspended sniper rifle, hides it, shows a
// model in its place and waits for "trigger" on the item to delete the model and set the flag player_has_sniper_rifle. beltot.gsc:1695
// (attach_weapon) takes the dropped item from the third waittill value and links it (linkto) to the truck: with an undefined value the
// call failed with "linkto needs two entities". The pickup swaps with a held weapon of the same slot class. Needs IW2_MAIN=<game main dir>.
#include "fs/vfs.h"
#include "game/world.h"

#include <cmath>
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
        "\tlevel.item waittill(\"trigger\", who, old);\n\tlevel.n = level.n + 1;\n\tlevel.who = who;\n\tlevel.old = old;\n\tlevel.fake delete();\n"
        "\tlevel.anchor = spawn(\"script_model\", (100, 0, 0));\n\tif (isdefined(old))\n\t\told thread attach(level.anchor);\n}\n"
        "main2()\n{\n\tlevel.item2 = spawn(\"weapon_mp40\", (500, 0, 0), 1);\n\tlevel.item2 waittill(\"trigger\", who, old);\n\tlevel.n2 = 1;\n\tlevel.old2 = old;\n}\n"
        "attach(ent)\n{\n\tself linkto(ent);\n\tlevel.linked = 1;\n}\n";
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
    // The second value is the dropped weapon item itself (a swap), and linking it to a moving anchor works (beltot's attach_weapon).
    CHECK(dropped && w.vm().level()->get("old").isObject() && w.vm().level()->get("old").asObject() == dropped->obj);
    CHECK(w.vm().level()->get("linked").truthy());
    if (dropped) {
        const float x0 = dropped->obj->get("origin").asVector()[0];
        for (const auto& e : w.entities())
            if (!e->obj->removed && e->obj == w.vm().level()->get("anchor").asObject()) e->obj->set("origin", iw2::script::Value(iw2::script::Vec3{300, 0, 0}));
        for (int i = 0; i < 3; ++i) w.frame();
        CHECK(std::fabs(dropped->obj->get("origin").asVector()[0] - (x0 + 200.0f)) < 1.0f);
    }
    // Swapping again with the dropped MP40 notifies nobody (the waiter is gone) and does not set the flag twice.
    if (dropped) w.debugUseItem(*dropped);
    for (int i = 0; i < 3; ++i) w.frame();
    CHECK(w.vm().level()->get("n").asInt() == 1);
    // A weapon that goes into a free slot leaves nothing to drop: the second value is undefined, the trigger still comes.
    inv = Inventory{};
    w.debugUseWeaponItem("springfield", 0, 0);
    w.vm().start("maps/itemtrigger", "main2", {}, iw2::script::Value(w.player()->obj));
    for (int i = 0; i < 3; ++i) w.frame();
    Entity* item2 = nullptr;
    for (const auto& e : w.entities())
        if (!e->obj->removed && e->classname == "weapon_mp40") item2 = e.get();
    CHECK(item2 != nullptr);
    if (item2) {
        Entity* dropped2 = w.debugUseItem(*item2);
        for (int i = 0; i < 3; ++i) w.frame();
        CHECK(dropped2 == nullptr);
        CHECK(inv.owned.count("mp40") && inv.owned.count("springfield"));
        CHECK(w.vm().level()->get("n2").asInt() == 1);
        CHECK(!w.vm().level()->get("old2").isObject());
    }
    std::filesystem::remove_all(dir);
    std::cout << (failures ? "FAILED\n" : "item trigger test OK\n");
    return failures ? 1 : 0;
}
