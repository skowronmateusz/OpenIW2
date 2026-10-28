// The AI turret gunner's pose (exe 0x549ba0) and the node delta it reads (0x4785d0 on blend nodes). Needs the game data
// (IW2_MAIN=<game main dir>); skipped without it.
// 1. nodeDelta of a blend node combines its children by goal weight, each sampled at its current time: one child at
//    weight 1 gives that child's delta, two at 0.5 give the mean of their translations.
// 2. The stand MG42 gunner's height children (generic_human.atr standMG42gunner_aim_15down / _level / _15up) are ordered by
//    the height of their delta, ascending in the exe's reversed child order, which the pose step's probe relies on.
// 3. decoytrenches: the MG42 gunners that take a turret run the pose step: setturretanim recorded the node, the node has a
//    weighted child with a weighted leaf below it (no reference pose), the slide offsets reach 0 and the gunner faces
//    along the turret.
#include "fs/vfs.h"
#include "game/animtree.h"
#include "game/savestate.h"
#include "game/world.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

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

bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

void nodeDeltaTests(AnimLibrary& lib) {
    const AnimTree* tree = lib.tree("generic_human");
    CHECK(tree != nullptr);
    if (!tree) return;
    const int level = tree->find("standmg42gunner_aim_level"), fwd = tree->find("standmg42gunner_aim_forward_level"),
              right = tree->find("standmg42gunner_aim_15right_level");
    CHECK(level >= 0 && fwd >= 0 && right >= 0);
    if (level < 0 || fwd < 0 || right < 0) return;
    AnimState st(*tree, lib);
    AnimState::SetArgs one;
    one.blendTime = 0;
    st.set(level, one);
    st.set(fwd, one);
    float qz, qw, lz, lw;
    std::array<float, 3> t, lt;
    st.nodeDelta(level, qz, qw, t);
    st.nodeDelta(fwd, lz, lw, lt);
    for (int k = 0; k < 3; ++k) CHECK(near(t[size_t(k)], lt[size_t(k)]));
    CHECK(near(qz, lz, 1e-4f) && near(qw, lw, 1e-4f));
    CHECK(std::fabs(lt[2]) > 1.0f); // the gunner animations carry the gunner's offset from tag_weapon

    // Two children at goal 0.5 (the current weight does not matter: the goal weights are read).
    AnimState::SetArgs half;
    half.weight = 0.5f;
    half.blendTime = 1.0f;
    st.set(fwd, half);
    st.set(right, half);
    std::array<float, 3> rt;
    float rz, rw;
    st.nodeDelta(right, rz, rw, rt);
    st.nodeDelta(level, qz, qw, t);
    for (int k = 0; k < 3; ++k) CHECK(near(t[size_t(k)], (lt[size_t(k)] + rt[size_t(k)]) * 0.5f));

    // Height children in ascending delta height.
    const char* kids[3] = {"standmg42gunner_aim_forward_15down", "standmg42gunner_aim_forward_level", "standmg42gunner_aim_forward_15up"};
    float z[3];
    for (int i = 0; i < 3; ++i) {
        const int n = tree->find(kids[i]);
        CHECK(n >= 0);
        if (n < 0) return;
        AnimState s2(*tree, lib);
        s2.nodeDelta(n, qz, qw, t);
        z[i] = t[2];
    }
    std::cout << "stand MG42 gunner delta heights (file order) " << z[0] << " " << z[1] << " " << z[2] << "\n";
    // File order 15down, level, 15up descends; the exe's reversed child order (0x452841 / 0x4530c0) ascends.
    CHECK(z[2] < z[1] && z[1] < z[0]);
}

void gunnerTest(iw2::fs::Vfs& vfs) {
    World w(vfs, "decoytrenches");
    w.start();
    w.spawnAllActors();
    int checked = 0;
    for (int f = 0; f < 240; ++f) { // 12 s
        w.frame();
        if (f < 200) continue;
        for (const auto& up : w.entities()) {
            Entity& e = *up;
            if (!e.isActor || e.ai.stack[size_t(e.ai.depth)] != AiTurret || !e.usingTurret || !e.anim) continue;
            Entity* t = nullptr;
            for (const auto& o : w.entities())
                if (o->obj == e.usingTurret) t = o.get();
            if (!t || !t->turret) continue;
            ++checked;
            CHECK(e.turretAnimSet);
            const auto& nodes = e.anim->tree().nodes();
            const auto& states = e.anim->nodeStates();
            bool leaf = false;
            for (int c : nodes[size_t(e.turretAnim)].children) {
                if (states[size_t(c)].weight <= 0.0f) continue;
                for (int g : nodes[size_t(c)].children)
                    if (states[size_t(g)].weight > 0.0f && nodes[size_t(g)].children.empty()) leaf = true;
            }
            CHECK(leaf);
            const TurretState& ts = *t->turret;
            CHECK(ts.flags & 0x20u);
            CHECK(ts.gunnerOriginOffset[0] == 0.0f && ts.gunnerOriginOffset[1] == 0.0f && ts.gunnerOriginOffset[2] == 0.0f);
            // The gunner yaw is the turret's base yaw + its tag_weapon yaw + the animation's yaw; it stays near the turret's.
            const float ty = t->obj->get("angles").asVector()[1] + ts.yaw, ey = e.obj->get("angles").asVector()[1];
            float d = std::fmod(std::fabs(ty - ey), 360.0f);
            if (d > 180.0f) d = 360.0f - d;
            CHECK(d < 45.0f);
        }
    }
    std::cout << "gunner frames checked " << checked << "\n";
    CHECK(checked > 0);

    // 4. Savegame (version 4): the setturretanim node / flag and the turret's pose fields survive a round trip.
    SaveRequest req;
    req.name = "turretpose";
    req.description = "turret pose";
    LoadedSave ls;
    const std::string err = parseSaveFile(buildSaveFile(w, req), ls);
    CHECK(err.empty());
    if (!err.empty()) return;
    World b(vfs, ls.info.mapName);
    b.prepareStart();
    restoreSave(b, ls);
    int compared = 0;
    for (size_t i = 0; i < w.entities().size() && i < b.entities().size(); ++i) {
        const Entity& x = *w.entities()[i];
        const Entity& y = *b.entities()[i];
        if (x.isActor && x.turretAnimSet) {
            ++compared;
            CHECK(y.turretAnimSet && y.turretAnim == x.turretAnim);
        }
        if (x.turret && y.turret && (x.turret->flags & 0x20u)) {
            ++compared;
            for (int k = 0; k < 3; ++k) {
                CHECK(y.turret->gunnerOriginOffset[size_t(k)] == x.turret->gunnerOriginOffset[size_t(k)]);
                CHECK(y.turret->gunnerAnglesOffset[size_t(k)] == x.turret->gunnerAnglesOffset[size_t(k)]);
            }
            CHECK(y.turret->pitchLimit == x.turret->pitchLimit && y.turret->drop == x.turret->drop);
        }
    }
    std::cout << "saved turret pose fields compared " << compared << "\n";
    CHECK(compared > 0);
}

} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "turret pose test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    AnimLibrary lib(vfs);
    nodeDeltaTests(lib);
    gunnerTest(vfs);
    if (failures) return 1;
    std::cout << "turret pose tests passed\n";
    return 0;
}
