// Prone built-ins (checkprone 0x5406f0); the fit test itself is src/game/prone.cpp, docs/spec/ai.md "Prone".
#include "game/prone.h"
#include "game/world.h"

#include <cstdio>
#include <cstdlib>

namespace iw2::game {

using namespace script;

void World::registerProneBuiltins() {
    // checkprone(origin, yaw, alreadyProne) (0x5406f0, actor only): the fit test 0x4fb530 with radius 15, height 48,
    // length 66, strict, the actor as pass entity and mask 0x810011 | 0x10000; the answer is returned as an int.
    vm_.registerBuiltin("checkprone", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || !e->isActor) throw std::runtime_error("not an actor");
        ProneFitArgs a;
        a.origin = c.arg(0).asVector();
        a.yaw = c.arg(1).asFloat();
        a.alreadyProne = c.arg(2).asInt() != 0;
        const int pass = e->number;
        const ProneFitResult r = proneFit(a, [this, pass](const Vec& s, const Vec& t, const Vec& mn, const Vec& mx) {
            return collision_->trace(s, t, mn, mx, 0x820011u, pass);
        });
        if (static const bool log = std::getenv("IW2_PRONE_LOG") != nullptr; log)
            std::fprintf(stderr, "[prone %lld] #%d checkprone (%.0f %.0f %.0f) yaw %.0f already %d -> %d (body %.1f legs %.1f)\n",
                         (long long)timeMs_, e->number, a.origin[0], a.origin[1], a.origin[2], a.yaw, int(a.alreadyProne),
                         int(r.fits), r.pitchBody, r.pitchLegs);
        return Value(r.fits ? 1 : 0);
    });
}

} // namespace iw2::game
