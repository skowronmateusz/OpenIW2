// Actor death prediction built-ins (docs/spec/builtins.md "Prediction built-ins"): beginprediction 0x53f960,
// endprediction 0x53fb80, predictoriginandangles 0x53ff50 and predictanim 0x540020 (animscripts/predict.gsc).
#include "game/world.h"

#include <stdexcept>

namespace iw2::game {

using namespace script;

void World::registerPredictionBuiltins() {
    auto actor = [this](CallContext& c, const char* name) -> Entity& {
        Entity* e = entityOf(c.self);
        if (!e || !e->isActor) throw std::runtime_error(std::string(name) + ": not an actor");
        return *e;
    };
    // beginprediction (0x53f960): error "beginPrediction already called" when the counter 0x1c6104c is set, else it counts up and
    // saves the actor: the animation state (0x4796c0), the anim mode words, the move block, the facing blocks, origin and angles.
    // The port copies its ActorAi (which holds all of those), the entity origin / angles / velocity and the tree state.
    vm_.registerBuiltin("beginprediction", [this, actor](CallContext& c) {
        Entity& e = actor(c, "beginprediction");
        if (predictionDepth_ != 0) throw std::runtime_error("beginPrediction already called");
        ++predictionDepth_;
        PredictionSave s;
        s.ai = e.ai;
        const Value o = e.obj->get("origin"), a = e.obj->get("angles");
        s.origin = o.isVector() ? o.asVector() : Vec3{0, 0, 0};
        s.angles = a.isVector() ? a.asVector() : Vec3{0, 0, 0};
        s.velocity = e.velocity;
        if (e.anim) {
            s.anim = e.anim->snapshot();
            s.hasAnim = true;
        }
        predictionSaves_[e.number] = std::move(s);
        return Value();
    });
    // endprediction (0x53fb80): the counter goes down; "endPrediction already called" when it is not zero afterwards. Everything
    // beginprediction saved is put back (0x4796c0 / 0x478db0 / 0x478e10 for the tree).
    vm_.registerBuiltin("endprediction", [this, actor](CallContext& c) {
        Entity& e = actor(c, "endprediction");
        if (--predictionDepth_ != 0) {
            predictionDepth_ = 0;
            throw std::runtime_error("endPrediction already called");
        }
        auto it = predictionSaves_.find(e.number);
        if (it == predictionSaves_.end()) return Value(); // TODO(spec): the exe has one scratch snapshot; another actor's end restores that
        PredictionSave& s = it->second;
        e.ai = s.ai;
        e.obj->set("origin", Value(s.origin));
        e.obj->set("angles", Value(s.angles));
        e.velocity = s.velocity;
        if (e.anim && s.hasAnim) e.anim->restore(s.anim);
        predictionSaves_.erase(it);
        return Value();
    });
    // predictoriginandangles (0x53ff50): errors "cannot predict when linked to an entity" and "cannot predict when using no
    // physics" (override mode 8); the anim mode word (+0x218) = the override or 4 and +0x220 = 1 (anim deltas by goal weights);
    // then the path step 0x504160 (World::actorMoveFrame; no path walk happened this frame), the physics 0x501d00 (inside it) and the
    // weight update 0x447ec0(gentity, 0.05).
    vm_.registerBuiltin("predictoriginandangles", [this, actor](CallContext& c) {
        Entity& e = actor(c, "predictoriginandangles");
        if (links_.count(e.number)) throw std::runtime_error("cannot predict when linked to an entity");
        if (e.ai.animModeOverride == 8) throw std::runtime_error("cannot predict when using no physics");
        e.ai.animMode = e.ai.animModeOverride ? e.ai.animModeOverride : 4;
        e.ai.animDeltaGoalWeights = true;
        e.ai.walkedMs = -1;
        actorMoveFrame(e);
        if (e.anim) e.anim->updateWeights(0.05f);
        return Value();
    });
    // predictanim (0x540020 -> 0x5045c0 -> 0x478220(tree, flag, 0.05)): the flag is script argument 0. A missing animation
    // tree gives 0. Flag 0 is a plain 50 ms time update without notetracks; flag 1 stops at the next notetrack (notified to
    // the actor under the node's flag name) and gives 1 when it stopped there.
    vm_.registerBuiltin("predictanim", [this, actor](CallContext& c) {
        Entity& e = actor(c, "predictanim");
        if (!e.anim) return Value(int32_t(0));
        ObjectPtr obj = e.obj;
        const bool toNote = c.arg(0).truthy();
        const bool stopped = e.anim->updateTruncated(0.05f, toNote, [this, obj](const std::string& flag, const std::string& note) {
            vm_.notify(obj, flag, {Value(note)});
        });
        return Value(int32_t(stopped ? 1 : 0));
    });
}

} // namespace iw2::game
