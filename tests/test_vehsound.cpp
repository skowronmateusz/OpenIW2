// A freed vehicle or turret leaves no voice behind: the vehicle's idle / engine / turret-spin voices and a mounted MG's fire loop end
// with the entity (docs/spec/sound.md "Loops of a freed entity"). Crossroads (only with IW2_MAIN set).
#include "fs/vfs.h"
#include "game/world.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace iw2::game;

static int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

// deleteTank: the live tiger is freed (a script delete) instead of shot.
static int run(iw2::fs::Vfs& vfs, bool deleteTank) {
    World w(vfs, "crossroads");
    std::set<int> live; // the voices that were started and not stopped
    std::map<int, std::string> names;
    int next = 100;
    w.playSound = [&](const std::string& alias, const Vec3*, float*, int) {
        live.insert(++next);
        names[next] = alias;
        return next;
    };
    w.stopSound = [&](int h) { live.erase(h); };
    w.playBlendSound = [&](const std::string& a, const std::string&, float, const Vec3&, int) {
        live.insert(++next);
        names[next] = a;
        return next;
    };
    w.setBlendSound = [](int, float, const Vec3&) { return true; };
    w.start();
    for (int i = 0; i < 40; ++i) w.frame();
    std::set<int> oldTurrets;
    Entity* turret = nullptr;
    for (const auto& e : w.entities())
        if (e->turret && !e->obj->removed) {
            oldTurrets.insert(e->number);
            if (!turret) turret = e.get();
        }
    // 1. A freed mounted MG ends its fire loop (a free of the engine and of a script delete share the helper).
    CHECK(turret != nullptr);
    if (turret) {
        turret->turret->fireCountdownMs = 3000;
        w.frame();
        const int h = turret->turret->loopHandle;
        CHECK(h > 0 && live.count(h));
        w.debugFree(*turret);
        CHECK(!live.count(h));
        CHECK(turret->turret->loopHandle <= 0);
    }
    // 2. The communication-outpost tiger spawns when the player reaches its trigger.
    for (const auto& e : w.entities())
        if (!e->obj->removed && e->obj->get("targetname").toString() == "tank_comm_trigger") {
            Vec3 p{};
            if (w.debugTriggerEntryPoint(*e, p)) w.setPlayerOrigin(p, {0, 0, 0});
        }
    Entity* tank = nullptr;
    for (int i = 0; i < 1200 && !tank; ++i) {
        w.frame();
        for (const auto& e : w.entities())
            if (!e->obj->removed && e->vehicle && !tank) tank = e.get();
    }
    CHECK(tank != nullptr);
    if (!tank) return 1;
    for (int i = 0; i < 5; ++i) w.frame();
    VehicleState& v = *tank->vehicle;
    std::vector<int> handles;
    for (int h : {v.idleHandle, v.engHandle, v.spinHandle})
        if (h > 0) handles.push_back(h);
    std::vector<Entity*> mgs;
    for (const auto& e : w.entities())
        if (e->turret && !e->obj->removed && !oldTurrets.count(e->number)) mgs.push_back(e.get());
    std::cout << "tank " << tank->number << " engine voices " << handles.size() << " mg turrets " << mgs.size() << "\n";
    CHECK(!handles.empty());
    CHECK(!mgs.empty());
    for (Entity* m : mgs) m->turret->fireCountdownMs = 100000; // the MG is in the middle of a burst
    w.frame();
    for (Entity* m : mgs) {
        CHECK(m->turret->loopHandle > 0);
        // Freeing only the vehicle leaves its MG to the script (_tank.gsc deletes it on death), so its loop is only checked for the shot tank.
        if (m->turret->loopHandle > 0 && !deleteTank) handles.push_back(m->turret->loopHandle);
    }
    for (int h : handles) CHECK(live.count(h));
    // The tank is destroyed (the script swaps the model, shuts the MG down and deletes it), or deleted with the MG still firing.
    if (deleteTank) w.debugFree(*tank);
    else w.debugDamage(*tank, 100000);
    for (int i = 0; i < 300; ++i) w.frame();
    int survivors = 0;
    for (int h : handles)
        if (live.count(h)) {
            ++survivors;
            std::cerr << "voice survived: " << names[h] << "\n";
        }
    CHECK(survivors == 0);
    for (Entity* m : mgs) CHECK(!m->obj->removed || m->turret->loopHandle <= 0);
    return 0;
}

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "vehicle sound test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    run(vfs, false);
    run(vfs, true);
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
