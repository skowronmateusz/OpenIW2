// toujane_ride (only with IW2_MAIN set): the player-driven armored car's machine gun must damage enemies
// (docs/spec/weapons.md "Damage range defaults": the turret weapon names only `damage`), and the car's own damage
// intake follows its vehicle file: bullets and splash damage leave its health alone.
#include "fs/vfs.h"
#include "game/world.h"

#include <cmath>
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
        std::cout << "armored car test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "toujane_ride");
    w.cvars["start"] = "ride"; // the level's own shortcut: spawn the armored car and put the player in it
    w.start();
    int carId = -1;
    for (int i = 0; i < 1200 && carId < 0; i++) {
        w.frame();
        if (w.player() && w.player()->usingVehicle >= 0) carId = w.player()->usingVehicle;
    }
    std::cout << "player vehicle " << carId << " after start\n";
    CHECK(carId >= 0);
    if (carId < 0) return 1;
    Entity* car = nullptr;
    for (const auto& e : w.entities())
        if (e->number == carId) car = e.get();
    for (int i = 0; i < 100; i++) w.frame(); // let the level settle (path setup, gunner removal)

    // An axis actor placed in front of the car, at muzzle height.
    Entity* target = nullptr;
    for (const auto& e : w.entities())
        if (e->isActor && !e->obj->removed && e->obj->get("health").isNumber() && e->obj->get("health").asInt() > 0 &&
            e->obj->get("team").toString() == "axis") {
            target = e.get();
            break;
        }
    std::cout << "target actor " << (target ? target->number : -1) << "\n";
    CHECK(target != nullptr);
    if (!target) return 1;
    const Vec3 carO = car->obj->get("origin").asVector(), carA = car->obj->get("angles").asVector();
    const float rad = 0.0174532925f;
    const Vec3 fwd{std::cos(carA[1] * rad), std::sin(carA[1] * rad), 0};
    const Vec3 at{carO[0] + fwd[0] * 700, carO[1] + fwd[1] * 700, carO[2]};
    target->obj->set("origin", Value(at));
    target->obj->set("health", Value(1000)); // survives the burst; the check is the damage, not the kill
    for (int i = 0; i < 5; i++) w.frame();

    // Hold fire through the real player command path: the turret turns to the view and FireTurret's bullet
    // must damage the enemy. Before the range-default fix every hit dealt minDamage 0.
    const Vec3 eye = w.player()->obj->get("origin").asVector();
    const Vec3 d{at[0] - eye[0], at[1] - eye[1], at[2] + 40.0f - eye[2]};
    const float yaw = std::atan2(d[1], d[0]) * 180.0f / float(M_PI);
    const float pitch = -std::atan2(d[2], std::sqrt(d[0] * d[0] + d[1] * d[1])) * 180.0f / float(M_PI);
    const int shots0 = w.stats.shots;
    for (int i = 0; i < 120; i++) { // 6 s: the turret turns, the fire timer repeats
        World::PlayerCommand cmd;
        cmd.viewAngles = {pitch, yaw, 0};
        cmd.buttons |= 1;
        w.setPlayerCommand(cmd);
        w.frame();
    }
    const int health1 = target->obj->get("health").asInt();
    std::cout << "mg: shots " << w.stats.shots - shots0 << " target health 1000 -> " << health1 << "\n";
    CHECK(w.stats.shots > shots0); // the gun fires
    CHECK(health1 < 1000);         // and damages the enemy

    // The car's own intake. Bullets: the armoredcar vehicle file filters them (bulletDamage 0, armorPiercingDamage 0),
    // so enemy rifles must not wear the car down.
    const int carHealth0 = int(car->obj->get("health").asFloat());
    w.debugFireBullet(w.player()->obj->get("origin").asVector(), carO);
    w.frame();
    CHECK(int(car->obj->get("health").asFloat()) == carHealth0); // a bullet leaves the car's health alone

    // Splash damage next to the car: the vehicle file has projectileSplashDamage 0, so it is ignored as well.
    w.debugRadiusDamage({carO[0] + fwd[0] * 100, carO[1] + fwd[1] * 100, carO[2] + 60}, target, 800, 10, 256, "MOD_PROJECTILE_SPLASH");
    w.frame();
    CHECK(int(car->obj->get("health").asFloat()) == carHealth0);
    std::cout << (failures ? "FAIL\n" : "ok\n");
    return failures ? 1 : 0;
}
