// Libya (only with IW2_MAIN=<game main dir>): an explodable_barrel (script_model) targets a script_brushmodel clip
// (spawnflags 1). maps/_interactable_objects.gsc explodable_barrel_explode connects paths and deletes that brush; the
// entity clip of the brush must be gone from the collision world once the barrel has exploded.
#include "fs/vfs.h"
#include "game/collision.h"
#include "game/world.h"
#include "script/vm.h"

#include <cstdlib>
#include <iostream>

using namespace iw2::game;

static bool hasClip(const World& w, int id) {
    for (const EntityClip& c : w.collision().entityClips())
        if (c.id == id) return true;
    return false;
}

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "barrel clip test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "libya");
    w.start();
    for (int i = 0; i < 40; ++i) w.frame();
    Entity* barrel = nullptr;
    Entity* brush = nullptr;
    for (const auto& ep : w.entities()) {
        Entity& e = *ep;
        if (e.obj->removed || e.classname != "script_model" || e.obj->get("targetname").toString() != "explodable_barrel") continue;
        const std::string t = e.obj->get("target").toString();
        if (t.empty()) continue;
        for (const auto& bp : w.entities())
            if (!bp->obj->removed && bp->classname == "script_brushmodel" && bp->obj->get("targetname").toString() == t) brush = bp.get();
        if (brush) { barrel = &e; break; }
    }
    if (!barrel) {
        std::cerr << "no barrel with a clip brush found\n";
        return 1;
    }
    if (!hasClip(w, brush->number)) {
        std::cerr << "clip brush has no entity clip before the explosion\n";
        return 1;
    }
    w.debugDamage(*barrel, 300);
    for (int i = 0; i < 80; ++i) w.frame();
    std::cout << "brush removed=" << brush->obj->removed << " clip=" << hasClip(w, brush->number)
              << " barrel model=" << barrel->obj->get("model").toString() << "\n";
    if (!brush->obj->removed) { std::cerr << "brush not deleted\n"; return 1; }
    if (hasClip(w, brush->number)) { std::cerr << "deleted brush still has an entity clip\n"; return 1; }
    // Every shipped barrel map: after all barrels exploded no targeted clip brush keeps an entity clip.
    for (const char* map : {"toujane", "88ridge", "silotown_assault", "decoytrenches", "tankhunt", "moscow"}) {
        iw2::fs::Vfs v3;
        v3.mountDirectory(main);
        World m(v3, map);
        m.start();
        for (int i = 0; i < 40; ++i) m.frame();
        std::vector<int> brushes;
        int barrels = 0;
        for (const auto& ep : m.entities()) {
            Entity& e = *ep;
            if (e.obj->removed || e.classname != "script_model" || e.obj->get("targetname").toString() != "explodable_barrel") continue;
            ++barrels;
            const std::string t = e.obj->get("target").toString();
            for (const auto& bp : m.entities()) // getent: the first entity with that targetname
                if (!t.empty() && !bp->obj->removed && bp->obj->get("targetname").toString() == t) {
                    if (bp->classname == "script_brushmodel") brushes.push_back(bp->number);
                    break;
                }
            m.debugDamage(e, 300);
        }
        for (int i = 0; i < 200; ++i) m.frame();
        int left = 0;
        for (int id : brushes)
            if (hasClip(m, id)) ++left;
        std::cout << map << ": barrels " << barrels << " clip brushes " << brushes.size() << " still clipping " << left << "\n";
        if (left) { std::cerr << map << ": exploded barrels keep clip brushes\n"; return 1; }
    }
    // toujane: a destroyed halftrack's wreck (setmodel to the _d model, which has no collmap) keeps the intact
    // vehicle's collision (the collmap lookup 0x50dc40 runs at spawn / level load only).
    iw2::fs::Vfs vfs2;
    vfs2.mountDirectory(main);
    World h(vfs2, "toujane");
    h.start();
    Entity* veh = nullptr;
    for (int i = 0; i < 1200 && !veh; ++i) { // vehicles are spawned by the level script; take the first one with a collmap clip
        h.frame();
        for (const auto& ep : h.entities())
            if (!ep->obj->removed && ep->vehicle && hasClip(h, ep->number)) {
                veh = ep.get();
                break;
            }
    }
    if (!veh) { std::cerr << "no vehicle with a collmap clip on toujane\n"; return 1; }
    veh->obj->set("model", iw2::script::Value(std::string("xmodel/vehicle_halftrack_mg_woodland_d")));
    for (int i = 0; i < 5; ++i) h.frame();
    if (!hasClip(h, veh->number)) { std::cerr << "wreck lost the vehicle collision\n"; return 1; }
    std::cout << "wreck keeps its collmap clip\n";
    return 0;
}
