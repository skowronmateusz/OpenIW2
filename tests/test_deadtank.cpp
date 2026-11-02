// Rhine (only with IW2_MAIN set): a destroyed tiger keeps blocking the player (docs/spec/vehicles.md "Collision after
// destruction"). Covered: the two tigers spawned by the script, killed directly; a tiger whose model was swapped to a tread wreck
// model (the sticky bomb path, rhine.gsc) before a save / load and killed afterwards.
#include "fs/vfs.h"
#include "game/collision.h"
#include "game/pmove.h"
#include "game/savestate.h"
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
    for (int id : tigers(c)) c.debugDamage(*ent(c, id), 100000);
    for (int i = 0; i < 300; ++i) c.frame();
    for (int id : tigers(c)) CHECK(hasClip(c, id));
    std::cout << (failures ? "FAILED\n" : "dead tank test OK\n");
    return failures ? 1 : 0;
}
