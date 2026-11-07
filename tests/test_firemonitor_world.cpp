// Fire-rate monitor sampling in the world (World::sampleFireMonitor / World::noteAiShot, docs/debug-flags.md "Fire rate"), on a
// decoytrenches MG42 gunner. Needs the game data (IW2_MAIN=<game main dir>); skipped without it.
// 1. Visibility is the actor's own sight record of its enemy, not an eye trace: with the record saying "not visible" no stretch starts.
// 2. With the record saying "visible" for 12 s and no shot the gunner is a silent shooter.
// 3. A shot of the gunner's turret (shooter = the turret, attacker = the gunner) counts for the gunner: no silent stretch.
// 4. The suppression time and the main animscript of a stretch are recorded; a stretch spent suppressed is explained when the
//    scripts' issuppressed answer was yes.
// 5. The sight record counts while it is at most 250 ms old (cansee's default).
#include "fs/vfs.h"
#include "game/world.h"

#include <cstdlib>
#include <iostream>

using namespace iw2::game;
using iw2::script::Value;

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

size_t stretchesOf(const World& w, int number) {
    size_t n = 0;
    for (const auto& ss : w.fireMon.silent()) n += ss.number == number;
    return n;
}

} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "fire monitor world test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "decoytrenches");
    w.start();
    w.spawnAllActors();
    for (int f = 0; f < 200; ++f) w.frame();
    Entity* gunner = nullptr;
    Entity* gun = nullptr;
    for (const auto& up : w.entities()) {
        Entity& e = *up;
        if (gunner || !e.isActor || e.ai.stack[0] != AiTurret || !e.usingTurret) continue;
        for (const auto& o : w.entities())
            if (o->obj == e.usingTurret && o->turret) gunner = &e, gun = o.get();
    }
    Entity* foe = nullptr;
    if (gunner)
        for (const auto& up : w.entities())
            if (up->isActor && up->obj->get("health").isNumber() && up->obj->get("health").asInt() > 0 &&
                up->obj->get("team").toString() != gunner->obj->get("team").toString() && up->obj->get("team").toString() != "neutral" &&
                (!foe || !w.canSee(*gunner, *foe)))
                foe = up.get();
    CHECK(gunner && gun && foe);
    if (!gunner || !gun || !foe) return 1;
    // The monitor is sampled here without frames: the world stands still and only the record below changes.
    w.fireMon.enabled = true;
    gunner->obj->set("enemy", Value(foe->obj));
    SentientInfo& info = gunner->sentientInfo[foe->obj.get()];
    info.lastCheckMs = w.timeMs() > 0 ? w.timeMs() : 1;
    gunner->dangerPlanes = {};
    auto run = [&](int steps, bool visible, bool shootEvery20) {
        for (int i = 0; i < steps; ++i) {
            info.visible = visible;
            if (shootEvery20 && i % 20 == 0) w.noteAiShot(*gun, gunner);
            w.sampleFireMonitor();
        }
    };
    // 1. Not visible to the gunner's own sight: nothing, although the eye-to-eye trace (the old rule) is clear.
    CHECK(w.canSee(*gunner, *foe));
    run(240, false, false);
    CHECK(stretchesOf(w, gunner->number) == 0);
    // 3. Visible, the turret firing for the gunner: no silent stretch.
    run(240, true, true);
    CHECK(stretchesOf(w, gunner->number) == 0);
    // 2. Visible, no shot: flagged after 10 s.
    run(30, false, false); // ends the stretch above
    run(240, true, false);
    CHECK(stretchesOf(w, gunner->number) == 1);
    if (stretchesOf(w, gunner->number) == 1) {
        const SilentShooter& ss = w.fireMon.silent().back();
        CHECK(ss.visibleSecs >= 11.9);
        CHECK(ss.suppressedSecs == 0.0);
        CHECK(ss.mainScript == gunner->animScript && !ss.mainScript.empty());
        CHECK(ss.reason().empty());
    }
    // 4. A held suppression line (the engine state issuppressed reads) for the whole next stretch: recorded; it explains the
    // stretch only once the scripts were told so by issuppressed.
    run(30, false, false);
    gunner->dangerPlanes[0].timeMs = w.timeMs() > 0 ? w.timeMs() : 1;
    gunner->dangerPlanes[0].shortLived = false;
    CHECK(w.isSuppressed(*gunner));
    gunner->lastSuppressedAnswer = 0;
    run(240, true, false);
    CHECK(stretchesOf(w, gunner->number) == 2);
    if (stretchesOf(w, gunner->number) == 2) {
        const SilentShooter& ss = w.fireMon.silent().back();
        CHECK(ss.suppressedSecs >= 11.9);
        CHECK(ss.reason().empty());
    }
    run(30, false, false);
    gunner->lastSuppressedAnswer = 1;
    run(240, true, false);
    CHECK(stretchesOf(w, gunner->number) == 3);
    if (stretchesOf(w, gunner->number) == 3) CHECK(w.fireMon.silent().back().reason() == "suppressed");
    // A short-lived plane (same-team source) is not suppression.
    gunner->dangerPlanes[0].shortLived = true;
    CHECK(!w.isSuppressed(*gunner));
    // 5. Sight older than cansee's default 250 ms counts as not visible (0x53da00); 250 ms old still counts.
    gunner->dangerPlanes = {};
    run(30, false, false);
    info.lastCheckMs = w.timeMs() - 300;
    run(240, true, false);
    CHECK(stretchesOf(w, gunner->number) == 3);
    info.lastCheckMs = w.timeMs() - 250;
    run(240, true, false);
    CHECK(stretchesOf(w, gunner->number) == 4);
    if (failures) return 1;
    std::cout << "fire monitor world tests passed\n";
    return 0;
}
