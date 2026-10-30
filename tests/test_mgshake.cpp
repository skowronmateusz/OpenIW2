// Mounted MG42 fire: the player on a turret firing it queues a view shake per shot (EV_FIRE_WEAPON_MG42, cgame 0x4bf5e0), and
// the shake reaches the view through ViewShake (docs/spec/turrets.md "Fire view shake"). Needs the game data
// (IW2_MAIN=<game main dir>); skipped without it.
#include "fs/vfs.h"
#include "game/viewweapon.h"
#include "game/world.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace iw2::game;

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
        std::cout << "mg shake test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "toujane");
    w.start();
    for (int i = 0; i < 20; ++i) w.frame();
    Entity* t = nullptr;
    for (const auto& e : w.entities())
        if (e->turret && !e->obj->removed && !t && e->turret->weapon == "mg42_bipod_stand") t = e.get();
    CHECK(t != nullptr);
    if (!t) return 1;
    const Vec3 o = t->obj->get("origin").asVector(), a = t->obj->get("angles").asVector();
    const float yaw = a[1] * float(M_PI) / 180.0f;
    const Vec3 fwd{std::cos(yaw), std::sin(yaw), 0};
    w.player()->obj->set("origin", Value(Vec3{o[0] - fwd[0] * 34.0f, o[1] - fwd[1] * 34.0f, o[2] - 20.0f}));
    World::PlayerCommand c;
    c.viewAngles = {0, a[1], 0};
    w.setPlayerCommand(c);
    w.frame();
    c.buttons = 8;
    w.setPlayerCommand(c);
    w.frame();
    c.buttons = 0;
    w.setPlayerCommand(c);
    w.frame();
    CHECK(w.playerOnTurret());
    w.takeShakeEvents();
    c.buttons = 1;
    w.setPlayerCommand(c);
    int events = 0;
    float minD = 1e9f, maxD = 0;
    for (int f = 0; f < 40; ++f) {
        w.frame();
        Vec3 eye;
        const bool ok = w.playerTurretEye(eye);
        CHECK(ok);
        for (const auto& ev : w.takeShakeEvents()) {
            ++events;
            const float d = std::sqrt((eye[0] - ev.origin[0]) * (eye[0] - ev.origin[0]) + (eye[1] - ev.origin[1]) * (eye[1] - ev.origin[1]) +
                                      (eye[2] - ev.origin[2]) * (eye[2] - ev.origin[2]));
            minD = std::min(minD, d);
            maxD = std::max(maxD, d);
        }
    }
    std::cout << "events " << events << " eye distance " << minD << ".." << maxD << "\n";
    CHECK(events > 0);
    // The shake is queued at the turret origin; the eye (tag_player) is 49.7 units from it, inside the 100 radius.
    CHECK(minD > 40.0f && maxD < 60.0f);
    // Firing: the view jitter is active with the weapon's vertViewJitter / horizViewJitter (0.5 / 0.5).
    float vj = 0, hj = 0;
    CHECK(w.playerTurretFireJitter(vj, hj));
    CHECK(vj == 0.5f && hj == 0.5f);
    // Not firing: the next think sets the view lock back to idle.
    c.buttons = 0;
    w.setPlayerCommand(c);
    for (int f = 0; f < 3; ++f) w.frame();
    CHECK(w.playerOnTurret());
    CHECK(!w.playerTurretFireJitter(vj, hj));

    // The jitter draw: pitch within +-vert, yaw within +-horiz, fresh each call, deterministic per seed.
    unsigned s1 = 5, s2 = 5;
    float maxP = 0, maxY = 0;
    bool varied = false;
    V3 prev{};
    for (int i = 0; i < 200; ++i) {
        const V3 j = turretViewJitter(0.5f, 0.25f, s1);
        const V3 k = turretViewJitter(0.5f, 0.25f, s2);
        CHECK(j[0] == k[0] && j[1] == k[1] && j[2] == 0.0f);
        maxP = std::max(maxP, std::fabs(j[0]));
        maxY = std::max(maxY, std::fabs(j[1]));
        if (i && (j[0] != prev[0] || j[1] != prev[1])) varied = true;
        prev = j;
    }
    CHECK(varied);
    CHECK(maxP <= 0.5f && maxP > 0.4f);
    CHECK(maxY <= 0.25f && maxY > 0.2f);
    unsigned s3 = 1;
    const V3 z = turretViewJitter(0.0f, 0.0f, s3);
    CHECK(z[0] == 0.0f && z[1] == 0.0f);

    // The screen shake stays tiny at the eye distance (a = 1 - 49.7 / 100; amplitude a * b * b * 18 with b <= 0.05).
    ViewShake vs;
    const V3 eye{0, 0, 0};
    vs.add({49.7f, 0, 0}, 0.05f, 100, 100.0f, 1000, eye);
    float peak = 0;
    for (int ms = 1000; ms < 1100; ms += 5) {
        const V3 sh = vs.update(ms, eye);
        peak = std::max({peak, std::fabs(sh[0]), std::fabs(sh[1]), std::fabs(sh[2])});
    }
    CHECK(peak > 0.0f && peak < 0.05f);
    return failures ? 1 : 0;
}
