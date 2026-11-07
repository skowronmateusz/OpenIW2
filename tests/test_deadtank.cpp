// Rhine (only with IW2_MAIN set): a destroyed tiger keeps blocking the player (docs/spec/vehicles.md "Collision after
// destruction"). Covered: the two tigers spawned by the script, killed directly; a tiger whose model was swapped to a tread wreck
// model (the sticky bomb path, rhine.gsc) before a save / load and killed afterwards.
#include "fs/vfs.h"
#include "game/collision.h"
#include "game/pmove.h"
#include "game/savestate.h"
#include "game/world.h"

#include <cmath>
#include <map>
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

static Entity* ent(World& w, int id) {
    for (const auto& e : w.entities())
        if (e->number == id) return e.get();
    return nullptr;
}

static bool hasClip(const World& w, int id) {
    for (const EntityClip& c : w.collision().entityClips())
        if (c.id == id) return true;
    return false;
}

static std::vector<int> tigers(World& w) {
    std::vector<int> ids;
    for (const auto& e : w.entities())
        if (!e->obj->removed && (e->vehicle || e->classname == "script_vehicle_corpse") && e->obj->get("model").toString().find("tiger") != std::string::npos)
            ids.push_back(e->number);
    return ids;
}

// The player box walking at the tank from 8 directions must never end up with its origin inside the tank's collision brush.
static int walkInside(World& w, Entity& tank) {
    const Vec3 c = tank.obj->get("origin").asVector();
    int inside = 0;
    for (int d = 0; d < 8; ++d) {
        const float a = float(d) * 0.785398f, dx = std::cos(a), dy = std::sin(a);
        const Vec3 from{c[0] - dx * 250, c[1] - dy * 250, c[2] + 200};
        const auto g = w.collision().trace(from, {from[0], from[1], from[2] - 600}, {-15, -15, 0}, {15, 15, 70}, 0x281c011u, -1);
        PlayerState ps;
        ps.origin = g.endPos;
        PlayerInput in;
        in.forward = 1;
        in.yaw = a * 57.29578f;
        for (int f = 0; f < 400; ++f) {
            playerMove(w.collision(), ps, in, 1.0f / 60.0f);
            if (w.collision().trace(ps.origin, ps.origin, {-15, -15, 0}, {15, 15, 70}, 0x281c011u, -1).entity == tank.number) { ++inside; break; }
        }
    }
    return inside;
}

static void startTigers(World& w) {
    w.start();
    for (int i = 0; i < 40; ++i) w.frame();
    Entity* trig = nullptr;
    for (const auto& e : w.entities())
        if (!e->obj->removed && e->obj->get("targetname").toString() == "tiger_1") trig = e.get();
    Vec3 p{};
    if (trig && w.debugTriggerEntryPoint(*trig, p)) w.setPlayerOrigin(p, {0, 0, 0});
    for (int i = 0; i < 160; ++i) w.frame();
}

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "dead tank test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World a(vfs, "rhine");
    startTigers(a);
    std::vector<int> ids = tigers(a);
    CHECK(ids.size() == 2);
    if (ids.size() != 2) return 1;
    for (int id : ids) CHECK(hasClip(a, id));
    // The sticky bomb path swaps the live tiger's model to a tread wreck model (rhine.gsc), then the tiger is saved and loaded.
    Entity* second = ent(a, ids[1]);
    second->obj->set("model", iw2::script::Value(std::string("xmodel/vehicle_tiger_lefttread_d")));
    for (int i = 0; i < 5; ++i) a.frame();
    CHECK(hasClip(a, ids[1]));
    SaveRequest req;
    req.name = "test";
    req.description = "dead tank";
    LoadedSave ls;
    CHECK(parseSaveFile(buildSaveFile(a, req), ls).empty());
    World b(vfs, "rhine");
    b.prepareStart();
    restoreSave(b, ls);
    for (int i = 0; i < 5; ++i) b.frame();
    for (int id : ids) CHECK(hasClip(b, id)); // alive after the load
    for (int id : ids) b.debugDamage(*ent(b, id), 100000);
    for (int i = 0; i < 300; ++i) b.frame();
    for (int id : ids) {
        Entity* t = ent(b, id);
        CHECK(t && t->classname == "script_vehicle_corpse");
        CHECK(hasClip(b, id));
        if (t) CHECK(walkInside(b, *t) == 0);
        std::cout << "tank " << id << " clip " << hasClip(b, id) << "\n";
    }
    // Same without a save: both tigers killed straight away.
    World c(vfs, "rhine");
    startTigers(c);
    const std::vector<int> cIds = tigers(c);
    for (int id : cIds) {
        ent(c, id)->vehicle->turretYaw = 70.0f; // turned before the kill
        ent(c, id)->vehicle->turretPitch = -5.0f;
        c.debugDamage(*ent(c, id), 100000);
    }
    std::map<int, std::array<float, 2>> lastTurret;
    for (int i = 0; i < 300; ++i) {
        for (int id : cIds)
            if (Entity* t = ent(c, id); t && t->vehicle) lastTurret[id] = {t->vehicle->turretPitch, t->vehicle->turretYaw};
        c.frame();
    }
    for (int id : cIds) CHECK(hasClip(c, id));
    // The wreck keeps the turret where it was (freevehicle 0x511c80 leaves es +0x70 / +0x74; eType 0xd is drawn with them).
    for (int id : cIds) {
        Entity* t = ent(c, id);
        const auto ct = t ? World::corpseTurret(*t) : std::nullopt;
        CHECK(t && t->classname == "script_vehicle_corpse" && ct);
        if (ct) CHECK((*ct)[0] == lastTurret[id][0] && (*ct)[1] == lastTurret[id][1] && (*ct)[1] != 0.0f);
        if (ct) std::cout << "tank " << id << " wreck turret p/y " << (*ct)[0] << "/" << (*ct)[1] << "\n";
    }
    // Flak88 shells (flak88_turret, direct hit MOD_PROJECTILE, the player as the shooter) kill both tigers one after the other; a save
    // taken while the second one is dead but not yet freed (the tank_down autosave) must keep its clip after the load too.
    {
        World d(vfs, "rhine");
        startTigers(d);
        std::vector<int> t2 = tigers(d);
        CHECK(t2.size() == 2);
        if (t2.size() != 2) return 1;
        auto shoot = [&](int id) {
            for (int tries = 0; tries < 12; ++tries) {
                Entity* t = ent(d, id);
                if (!t->vehicle || t->obj->get("health").asFloat() <= 0) break;
                const Vec3 o = t->obj->get("origin").asVector();
                const float a = float(tries) * 0.5236f;
                Entity& r = d.debugSpawnRocket("flak88_turret", {o[0] - std::cos(a) * 500, o[1] - std::sin(a) * 500, o[2] + 70}, {std::cos(a), std::sin(a), 0});
                for (int i = 0; i < 60 && !r.obj->removed; ++i) d.frame();
            }
        };
        shoot(t2[0]);
        for (int i = 0; i < 100; ++i) d.frame();
        shoot(t2[1]);
        for (int i = 0; i < 5; ++i) d.frame();
        CHECK(ent(d, t2[1])->vehicle != nullptr); // dead (wreck model, health below 1) but not freed yet
        SaveRequest rq;
        rq.name = "t";
        rq.description = "tank_down";
        LoadedSave sv;
        CHECK(parseSaveFile(buildSaveFile(d, rq), sv).empty());
        World e2(vfs, "rhine");
        e2.prepareStart();
        restoreSave(e2, sv);
        for (int i = 0; i < 3; ++i) e2.frame();
        CHECK(hasClip(e2, t2[1]));
        for (int i = 0; i < 1500; ++i) { d.frame(); e2.frame(); }
        for (World* wp : {&d, &e2})
            for (int id : t2) {
                Entity* t = ent(*wp, id);
                CHECK(t && t->classname == "script_vehicle_corpse");
                CHECK(hasClip(*wp, id));
                if (t) CHECK(walkInside(*wp, *t) == 0);
                // bullets hit the wreck through the xmodel collision surfaces of the wreck model (ent+0xa2 stays 4 after freevehicle)
                if (t) {
                    const Vec3 o = t->obj->get("origin").asVector();
                    int hits = 0;
                    for (float dz : {60.f, 100.f, 140.f})
                        for (int dd = 0; dd < 8; ++dd) {
                            const float an = float(dd) * 0.785398f, dx = std::cos(an) * 500, dy = std::sin(an) * 500;
                            if (wp->debugBulletTraceEntity({o[0] + dx, o[1] + dy, o[2] + dz}, {o[0], o[1], o[2] + dz}) == id) ++hits;
                        }
                    CHECK(hits >= 4);
                }
            }
    }
    // A freed flak88 (rhine flak0..4) is still hit by bullet traces: its wreck model has collision surfaces.
    {
        World f(vfs, "rhine");
        f.start();
        for (int i = 0; i < 100; ++i) f.frame();
        Entity* flak = nullptr;
        for (const auto& e : f.entities())
            if (e->vehicle && !e->obj->removed && e->obj->get("model").toString().find("flak88") != std::string::npos) { flak = e.get(); break; }
        CHECK(flak != nullptr);
        if (flak) {
            const int id = flak->number;
            f.debugDamage(*flak, 100000);
            for (int i = 0; i < 400; ++i) f.frame();
            CHECK(flak->classname == "script_vehicle_corpse");
            const Vec3 o = flak->obj->get("origin").asVector();
            int hits = 0;
            for (float dz : {30.f, 60.f, 90.f})
                for (int dd = 0; dd < 4; ++dd) {
                    const float dx = (dd == 0 ? -1.f : dd == 1 ? 1.f : 0.f) * 400, dy = (dd == 2 ? -1.f : dd == 3 ? 1.f : 0.f) * 400;
                    if (f.debugBulletTraceEntity({o[0] + dx, o[1] + dy, o[2] + dz}, {o[0], o[1], o[2] + dz}) == id) ++hits;
                }
            CHECK(hits >= 4);
        }
    }
    std::cout << (failures ? "FAILED\n" : "dead tank test OK\n");
    return failures ? 1 : 0;
}
