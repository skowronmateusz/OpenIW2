// G_Damage in the world (docs/spec/weapons.md "G_Damage order"), without the god flag unless a case sets it: the rider and noclip
// gates, god on any entity, the floor to 1, hit location on actors only, the held lethal hit with "damage_armor", the delayed death
// across a save, and an actor's script shot with a projectile weapon. Needs IW2_MAIN.
#include "fs/vfs.h"
#include "game/savestate.h"
#include "game/vehicles.h"
#include "game/world.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

using namespace iw2::game;
using iw2::script::Value;
using iw2::script::Vec3;

static int failures = 0;
#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #c "\n"; \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

static int health(const Entity& e) { return e.obj->get("health").asInt(); }
static void setHealth(Entity& e, int h) { e.obj->set("health", Value(int32_t(h))); }
static void hold(Entity& a) {
    a.ai.stack[1] = AiNegotiation;
    a.ai.depth = 1;
}
static void release(Entity& a) { a.ai.depth = 0; }

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "damage order test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    const auto dir = std::filesystem::temp_directory_path() / ("iw2_test_dmg_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "maps");
    std::ofstream(dir / "maps/shoottest.gsc") << "main()\n{\n\tself shoot(1, self.origin + (500, 0, 40));\n\tself.shot = 1;\n}\n";
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir.string());
    World w(vfs, "cityhall");
    w.prepareStart();
    w.frame();
    Entity* spawner = nullptr;
    for (const auto& e : w.entities())
        if (e->isSpawner && !e->obj->removed) { spawner = e.get(); break; }
    Entity* p = w.player();
    CHECK(spawner && p);
    if (!spawner || !p) return 1;
    const auto spawn = [&]() {
        spawner->obj->set("count", Value(int32_t(5)));
        return w.debugSpawnActor(*spawner, true);
    };
    std::string seen;
    const iw2::script::Object* watched = nullptr;
    w.vm().notifyHook = [&](const iw2::script::ObjectPtr& o, const std::string& ev) {
        if (o.get() == watched) seen += ev + ",";
    };
    const Vec3 up{0, 0, 1}, none{0, 0, 0};

    // Floor and hit location on the player: 0 becomes 1, a location never scales.
    setHealth(*p, 1000);
    w.debugDamageAt(*p, nullptr, 0, none, "none");
    CHECK(health(*p) == 999);
    w.debugDamageAt(*p, nullptr, -7, none, "none");
    CHECK(health(*p) == 998);
    std::string loc;
    for (const char* l : {"head", "helmet", "neck", "torso_upper", "right_foot", "left_hand"})
        if (w.debugHitLocationMultiplier(l) != 1.0f && loc.empty()) loc = l;
    CHECK(!loc.empty());
    setHealth(*p, 1000);
    w.debugDamageAt(*p, nullptr, 100, none, loc);
    CHECK(health(*p) == 900);

    // Noclip and ufo: no damage and no knockback; without them both happen.
    const size_t kb = w.debugKnockbackCount();
    setHealth(*p, 1000);
    w.debugSetNoclip(true, false);
    w.debugDamageAt(*p, nullptr, 10, up, "none");
    w.debugSetNoclip(false, true);
    w.debugDamageAt(*p, nullptr, 10, up, "none");
    CHECK(health(*p) == 1000 && w.debugKnockbackCount() == kb);
    w.debugSetNoclip(false, false);
    w.debugDamageAt(*p, nullptr, 10, up, "none");
    CHECK(health(*p) == 990 && w.debugKnockbackCount() == kb + 1);

    // God: the knockback comes first; then nothing, on the player and on a plain entity.
    p->obj->set("$godmode", Value(int32_t(1)));
    w.debugDamageAt(*p, nullptr, 10, up, "none");
    CHECK(health(*p) == 990 && w.debugKnockbackCount() == kb + 2);
    p->obj->set("$godmode", Value(int32_t(0)));
    Entity& model = w.debugSpawn("script_model");
    model.obj->set("$takedamage", Value(int32_t(1)));
    setHealth(model, 50);
    model.obj->set("origin", Value(none));
    model.obj->set("$godmode", Value(int32_t(1)));
    w.debugDamageAt(model, nullptr, 10, none, "none");
    CHECK(health(model) == 50);
    model.obj->set("$godmode", Value(int32_t(0)));
    watched = model.obj.get();
    w.debugDamageAt(model, nullptr, 0, none, "none"); // floor without a sentient, and the notify goes out
    CHECK(health(model) == 49 && seen.find("damage,") != std::string::npos);
    watched = nullptr;

    // Rider gate: pm_flags 0x20000 without 0x80000 in a vehicle other than artillery.
    Entity& v = w.debugSpawn("script_model");
    v.vehicle = std::make_unique<VehicleState>();
    VehicleInfo info;
    info.type = 1;
    v.vehicle->info = &info;
    p->usingVehicle = v.number;
    w.setPlayerPmFlag(0x20000, true);
    setHealth(*p, 1000);
    w.debugDamageAt(*p, nullptr, 10, none, "none");
    CHECK(health(*p) == 1000);
    w.setPlayerPmFlag(0x80000, true);
    w.debugDamageAt(*p, nullptr, 10, none, "none");
    CHECK(health(*p) == 990);
    w.setPlayerPmFlag(0x80000, false);
    info.type = 4;
    w.debugDamageAt(*p, nullptr, 10, none, "none");
    CHECK(health(*p) == 980);
    w.setPlayerPmFlag(0x20000, false);
    p->usingVehicle = -1;
    v.vehicle.reset();

    // Actors: the location scales, a result of 0 does nothing (no floor).
    Entity* a = spawn();
    CHECK(a != nullptr);
    if (!a) return 1;
    setHealth(*a, 100000);
    w.debugDamageAt(*a, nullptr, 1000, none, loc);
    CHECK(health(*a) == 100000 - int(1000.0f * w.debugHitLocationMultiplier(loc)));
    // A scoped rifle counts a helmet hit as a head hit; other rifles keep the shipped helmet value.
    CHECK(w.debugLocMultiplier("enfield_scope", "helmet") == w.debugLocMultiplier("enfield_scope", "head"));
    CHECK(w.debugLocMultiplier("enfield", "helmet") == 0.5f);
    {
        World sandMap(vfs, "elalamein"); // large_dirthit_libya has no Decal element, the small effect has one
        CHECK(sandMap.rifleMarkFallback(18) == "fx/impacts/small_dirthit_libya.efx");
        CHECK(sandMap.rifleMarkFallback(7).empty());
    }
    setHealth(*a, 100);
    watched = a->obj.get();
    seen.clear();
    w.debugDamageAt(*a, nullptr, 0, none, "none");
    CHECK(health(*a) == 100 && seen.empty());

    // Held lethal hit: health 1, "damage_armor" before "damage", no death.
    a->obj->set("allowdeath", Value(int32_t(0)));
    hold(*a);
    w.debugDamageAt(*a, nullptr, 500, none, "none");
    CHECK(health(*a) == 1 && a->ai.deathPending);
    CHECK(seen.find("damage_armor,") == 0 && seen.find(",damage,") != std::string::npos && seen.find("death") == std::string::npos);
    // At 1 health nothing is taken: only "damage_armor".
    seen.clear();
    w.debugDamageAt(*a, nullptr, 500, none, "none");
    CHECK(health(*a) == 1 && seen == "damage_armor,");
    w.debugActorDelayedDeath(*a); // still held
    CHECK(health(*a) == 1);

    // The flag survives a save; the restored actor dies once released.
    const int number = a->number;
    release(*a);
    a->ai.stack[1] = AiNone;
    SaveRequest req;
    req.name = "dmg";
    req.description = "dmg";
    hold(*a);
    LoadedSave ls;
    CHECK(parseSaveFile(buildSaveFile(w, req), ls).empty());
    {
        World l(vfs, "cityhall");
        l.prepareStart();
        restoreSave(l, ls);
        Entity* r = nullptr;
        for (const auto& e : l.entities())
            if (e->number == number) r = e.get();
        CHECK(r && r->ai.deathPending && health(*r) == 1);
        if (r) {
            l.debugActorDelayedDeath(*r);
            CHECK(health(*r) == 1);
            release(*r);
            l.debugActorDelayedDeath(*r);
            CHECK(health(*r) <= 0);
        }
    }

    // Through the real think: released, the next frame kills it.
    seen.clear();
    release(*a);
    w.frame();
    CHECK(seen.find("death") != std::string::npos);
    watched = nullptr;

    // allowdeath, or no holding state: the hit kills at once.
    Entity* b = spawn();
    CHECK(b != nullptr);
    if (b) {
        setHealth(*b, 100);
        b->obj->set("allowdeath", Value(int32_t(1)));
        hold(*b);
        w.debugDamageAt(*b, nullptr, 500, none, "none");
        CHECK(health(*b) <= 0 && !b->ai.deathPending);
    }

    // An actor's shoot with a projectile weapon launches a rocket, a bullet weapon does not, a grenade fires nothing.
    Entity* c = spawn();
    CHECK(c != nullptr);
    if (c) {
        setHealth(*c, 100);
        const auto fire = [&](const char* weapon) {
            const int r0 = w.stats.rockets, s0 = w.stats.shots;
            c->obj->set("weapon", Value(std::string(weapon)));
            c->obj->set("shot", Value());
            w.vm().start("maps/shoottest", "main", {}, Value(c->obj));
            CHECK(c->obj->get("shot").isNumber());
            return std::pair<int, int>{w.stats.rockets - r0, w.stats.shots - s0};
        };
        const auto rocket = fire("panzerschreck");
        CHECK(rocket.first == 1 && rocket.second == 0);
        const auto bullet = fire("kar98k");
        CHECK(bullet.first == 0 && bullet.second == 1);
        const auto nade = fire("fraggrenade");
        CHECK(nade.first == 0 && nade.second == 0);
    }
    std::filesystem::remove_all(dir);
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
