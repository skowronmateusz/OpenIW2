// Multiplayer first-person hands (docs/spec/weapons.md "Multiplayer hands model"): the character script's setviewmodel
// model replaces the weapon's handModel in an MP game; single player keeps the weapon's handModel. Needs the game files
// (IW2_MAIN) for the world part.

#include "game/viewweapon.h"
#include "game/world.h"

#include <cstdlib>
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

} // namespace

int main() {
    using iw2::game::viewHandsModel;
    // SP never changes: always the weapon's handModel.
    CHECK(viewHandsModel(false, "xmodel/viewmodel_hands_german", "xmodel/viewmodel_hands_cloth") == "xmodel/viewmodel_hands_cloth");
    // MP: the script model when set, else the weapon's.
    CHECK(viewHandsModel(true, "xmodel/viewmodel_hands_german", "xmodel/viewmodel_hands_cloth") == "xmodel/viewmodel_hands_german");
    CHECK(viewHandsModel(true, "", "xmodel/viewmodel_hands_cloth") == "xmodel/viewmodel_hands_cloth");

    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, world part skipped\n";
        return failures ? 1 : 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    // mp_carentan: allies are Americans (cloth hands), axis Germans; the character script runs at spawn (dm.gsc spawnPlayer).
    for (const char* team : {"allies", "axis"}) {
        iw2::game::World w(vfs, "mp_carentan", true);
        w.mpStart("dm");
        const int c0 = w.mpConnect("local");
        w.mpBegin(c0);
        for (int i = 0; i < 20; ++i) w.frame();
        w.mpAutoJoin(c0, "", team);
        for (int i = 0; i < 20; ++i) w.frame();
        iw2::game::Entity* e = w.mpClientEntity(c0);
        CHECK(e != nullptr);
        if (!e) continue;
        const iw2::script::Value vm = e->obj->get("$viewmodel");
        CHECK(vm.isString());
        const std::string name = vm.isString() ? vm.asString() : std::string();
        std::cout << team << " hands " << name << "\n";
        CHECK(name.rfind("xmodel/viewmodel_hands", 0) == 0);
        CHECK(vfs.read(name).has_value());
        if (std::string(team) == "axis") CHECK(name.find("german") != std::string::npos);
    }
    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "mphands ok\n";
    return failures ? 1 : 0;
}
