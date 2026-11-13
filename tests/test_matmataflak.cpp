// matmata's Stuka attack (maps/matmata.gsc:1168-1310, 1335-1360) end to end: the player takes the flak gun with the use button, the level
// swaps it for the player version (maps/matmata.gsc:1182-1224), its turret shots kill the planes and are credited to the gun itself
// (planeThink compares the attacker with level.flakgun: fireturret 0x514260 hands Bullet_Fire 0x531340 the vehicle as the attacker,
// docs/spec/vehicles.md "Turret attacker"), and the level ends with the next mission. Uses the level's `planes` start; the gun crew that
// start leaves alive is killed first. Needs IW2_MAIN.
#include "fs/vfs.h"
#include "game/world.h"

#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

using namespace iw2::game;

static int failures = 0;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) { std::cout << "FAIL line " << __LINE__ << ": " #c "\n"; ++failures; } \
    } while (0)

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "matmata flak test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    const auto dir = std::filesystem::temp_directory_path() / ("iw2_test_mflak_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir / "maps");
    std::ofstream(dir / "maps/mflak.gsc") << "main()\n{\n\tself dodamage(self.health + 1000, self.origin);\n}\n";
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir.string());
    World w(vfs, "matmata");
    w.cvars["start"] = "planes";
    w.start();
    const auto lvl = w.vm().level();
    auto flakGun = [&]() -> Entity* {
        const Value f = lvl->get("flakgun");
        for (const auto& e : w.entities())
            if (!e->obj->removed && f.isObject() && e->obj == f.asObject()) return e.get();
        return nullptr;
    };
    auto killAxis = [&] {
        std::vector<ObjectPtr> axis;
        for (const auto& e : w.entities())
            if (e->isActor && !e->isSpawner && !e->obj->removed && e->ai.alive && e->obj->get("team").toString() == "axis")
                axis.push_back(e->obj);
        for (const auto& o : axis) w.vm().start("maps/mflak", "main", {}, Value(o));
    };
    for (int i = 0; i < 20; ++i) w.frame();
    killAxis();
    for (int i = 0; i < 40; ++i) w.frame();

    // Walk up to the gun and press use (+activate 8 and +usereload 0x20 together, src/game/playeruse.cpp).
    Entity* gun = flakGun();
    CHECK(gun != nullptr);
    if (!gun) return 1;
    const Vec3 go = gun->obj->get("origin").asVector();
    w.player()->obj->set("origin", Value(Vec3{go[0] - 40, go[1], go[2]}));
    for (int i = 0; i < 40 && w.player()->usingVehicle < 0; ++i) {
        World::PlayerCommand c;
        c.viewAngles = {10, 0, 0};
        c.buttons = (i % 2) ? 0x28u : 0u;
        w.setPlayerCommand(c);
        w.frame();
    }
    gun = flakGun();
    CHECK(gun && w.player()->usingVehicle == gun->number);
    CHECK(gun && gun->obj->get("model").toString() == "xmodel/german_artillery_flakveirling_viewmodel");
    if (!gun || w.player()->usingVehicle != gun->number) return 1;

    // Aim at the nearest live Stuka and hold fire. The player is kept alive: the bombing damage is the level's, not under test.
    bool changed = false;
    for (int s = 0; s < 1600 && !changed; ++s) {
        Vec3 eye = w.player()->obj->get("origin").asVector();
        eye[2] += 60;
        Entity* best = nullptr;
        float bestD = 1e18f;
        for (const auto& e : w.entities())
            if (!e->obj->removed && e->obj->get("model").toString() == "xmodel/vehicle_stuka_flying" && e->obj->get("health").asInt() > 0) {
                const Vec3 o = e->obj->get("origin").asVector();
                const float d = (o[0] - eye[0]) * (o[0] - eye[0]) + (o[1] - eye[1]) * (o[1] - eye[1]) + (o[2] - eye[2]) * (o[2] - eye[2]);
                if (d < bestD) bestD = d, best = e.get();
            }
        World::PlayerCommand c;
        if (best) {
            const Vec3 o = best->obj->get("origin").asVector();
            const Vec3 d{o[0] - eye[0], o[1] - eye[1], o[2] - eye[2]};
            c.viewAngles = {-float(std::atan2(d[2], std::sqrt(d[0] * d[0] + d[1] * d[1])) * 180 / M_PI), float(std::atan2(d[1], d[0]) * 180 / M_PI), 0};
            c.buttons = 1;
        }
        w.setPlayerCommand(c);
        w.player()->obj->set("health", Value(100));
        if (s % 20 == 0) killAxis();
        w.frame();
        changed = w.dueLevelChange().has_value();
    }
    const int killed = w.player()->obj->get("planeskilled").asInt();
    std::cout << "planes killed " << killed << ", level change " << changed << ", still on the gun " << (w.player()->usingVehicle == gun->number) << "\n";
    CHECK(killed >= 5);
    CHECK(changed); // planeSequence ends with maps\_endmission::nextmission
    for (const auto& er : w.vm().errors()) std::cout << "ERR " << er << "\n";
    CHECK(w.vm().errors().empty());
    std::filesystem::remove_all(dir);
    if (failures) return 1;
    std::cout << "matmata flak test passed\n";
    return 0;
}
