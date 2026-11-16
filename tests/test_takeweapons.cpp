// takeallweapons (0x537960) and takeweapon (0x537850 -> 0x4dc8a0) clear the player state's weapon (ps+0xb0) at once when the
// held weapon is taken: the script side sets `$weapon` to "none" and flags `$weapontaken`, and the player weapon state machine
// then holds nothing, so no view weapon is drawn (duhoc_assault.gsc:1336, the rope climb). Needs the game files (IW2_MAIN) for
// the World part.
#include "fs/vfs.h"
#include "game/playerweapon.h"
#include "game/world.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

// The front end's side: a taken weapon leaves the hand at once; with no command weapon nothing is raised again.
void stateMachine() {
    using iw2::game::PlayerWeapon;
    PlayerWeapon w;
    w.equip("rifle", {{"firetime", "0.3"}, {"clipsize", "5"}, {"startammo", "20"}, {"raisetime", "0.5"}, {"idleanim", "idle"}});
    for (int i = 0; i < 100; ++i) w.update(0.01f, {});
    CHECK(w.equipped());
    w.weaponTaken();
    CHECK(!w.equipped());
    for (int i = 0; i < 100; ++i) w.update(0.01f, {});
    CHECK(!w.equipped());
}
} // namespace

int main() {
    stateMachine();
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, world part skipped\n";
        return failures ? 1 : 0;
    }
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("iw2_takeweapons_" + std::to_string(::getpid()));
    fs::create_directories(dir / "maps");
    {
        std::ofstream f(dir / "maps" / "zz_takeweapons.gsc");
        f << "give()\n{\n\tself takeallweapons();\n\tself giveweapon(\"thompson\");\n\tself giveweapon(\"colt\");\n"
             "\tself switchtoweapon(\"thompson\");\n}\n"
             "takeall()\n{\n\tself takeallweapons();\n}\n"
             "takecolt()\n{\n\tself takeweapon(\"colt\");\n}\n"
             "takethompson()\n{\n\tself takeweapon(\"thompson\");\n}\n";
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir.string());
    iw2::game::World w(vfs, "toujane");
    w.prepareStart();
    if (!w.player()) return 1;
    auto& p = *w.player();
    auto run = [&](const char* fn) {
        w.vm().start("maps/zz_takeweapons.gsc", fn, {}, iw2::script::Value(p.obj));
        w.vm().drainReady();
    };
    auto held = [&]() { return p.obj->get("$weapon").toString(); };
    auto taken = [&]() {
        const bool t = p.obj->get("$weapontaken").truthy();
        p.obj->set("$weapontaken", iw2::script::Value()); // consumed by the front end
        return t;
    };

    // takeallweapons with a weapon in hand: none in hand, taken at once.
    run("give");
    taken();
    CHECK(held() == "thompson");
    run("takeall");
    CHECK(held() == "none");
    CHECK(taken());
    // ... and with nothing in hand there is nothing to take.
    run("takeall");
    CHECK(!taken());

    // takeweapon of another weapon keeps the one in hand; of the held one clears it.
    run("give");
    taken();
    run("takecolt");
    CHECK(held() == "thompson");
    CHECK(!taken());
    run("takethompson");
    CHECK(held() == "none");
    CHECK(taken());

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "takeweapons ok\n";
    return failures ? 1 : 0;
}
