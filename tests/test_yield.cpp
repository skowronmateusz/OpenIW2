// The follower yield step (exe 0x501620, World::yieldFollowed): an actor that follows another entity steps out of its way instead of
// freezing. The followed actor is held 10 units away; the link is released once the actor is clear (0x4fedc0 release).
// Needs the game data (IW2_MAIN), otherwise skipped.
#include "fs/vfs.h"
#include "game/world.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using iw2::game::Entity;
using iw2::game::Vec3;

Vec3 originOf(const Entity& e) { return e.obj->get("origin").asVector(); }
float dist2d(const Vec3& a, const Vec3& b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }
}  // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "yield: IW2_MAIN not set, skipped\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    iw2::game::World world(vfs, "crossroads");
    world.start();
    world.spawnAllActors();
    for (int i = 0; i < 40; ++i) world.frame();

    Entity *a = nullptr, *b = nullptr;
    for (const auto& ep : world.entities()) {
        if (!ep->isActor || ep->obj->removed || ep->obj->get("health").asInt() <= 0) continue;
        if (ep->ai.stack[size_t(ep->ai.depth)] == 7 || ep->ai.groundEnt != 0x7fe) continue; // not scripted, standing on the world
        if (!a) a = ep.get();
        else if (!b) b = ep.get();
    }
    CHECK(a && b);
    if (!a || !b) return 1;

    // B stands 10 units from A and is held there; A follows B (the link of the bump, 0x501c8b) and has no goal of its own.
    const Vec3 ao = originOf(*a);
    const Vec3 bpos{ao[0] + 10.0f, ao[1], ao[2]};
    a->obj->set("$goalpos", iw2::script::Value());
    a->obj->set("enemy", iw2::script::Value());
    a->ai.followed = b;
    a->ai.animModeOverride = 1; // anim mode 1 whatever the animscript asks for (the yield step runs in mode 1 or 7)
    float maxDist = 0.0f;
    bool linked = true;
    for (int i = 0; i < 60; ++i) {
        b->obj->set("origin", iw2::script::Value(bpos));
        a->ai.animMode = 1; // the physics 0x501d00 calls the yield step in anim mode 1 or 7
        world.frame();
        maxDist = std::max(maxDist, dist2d(originOf(*a), bpos));
        if (!a->ai.followed) linked = false;
    }
    const Vec3 end = originOf(*a);
    CHECK(std::isfinite(end[0]) && std::isfinite(end[1]) && std::isfinite(end[2]));
    // The actor left the 10 units it started at (it yielded) instead of staying frozen, unless the link was released or it was
    // wedged with nowhere to go (then the bumped entity is the followed one: 0x501c67).
    const bool wedged = a->ai.bumpedEnt == b->number;
    CHECK(maxDist > 10.5f || wedged || !linked);
    std::cout << "yield: start 10.0, max " << maxDist << ", end " << dist2d(end, bpos) << ", linked " << linked << ", wedged " << wedged << "\n";
    return failures == 0 ? 0 : 1;
}
