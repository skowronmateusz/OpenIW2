// The player's shadow-only body pose. Implemented from docs/spec/mp-players.md and docs/render-enhanced.md "Player shadow".

#include "game/playershadowbody.h"
#include "game/renderenhanced.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace iw2::game {

namespace {

constexpr float kDefaultBlend = 0.2f; // a command without "blendtime" (as playPaCommand)
constexpr float kMaxStepS = 0.1f;   // a longer gap (a pause, a stall) steps this much
constexpr int kStandHeight = 60, kCrouchHeight = 40, kProneHeight = 11;
constexpr int kStateRaising = 1, kStateDropping = 2, kStateReload = 5, kStateReloadEmpty = 7, kStateMelee = 10, kStateMeleeHit = 11;

bool reloading(int state) { return state == kStateReload || state == kStateReloadEmpty; }
bool melee(int state) { return state == kStateMelee || state == kStateMeleeHit; }

std::string lower(std::string v) {
    for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
    return v;
}

// The spine bones that follow the view pitch and their share of it (TODO(spec): our own rule; the sum is the part of the pitch the
// torso takes, the rest stays in the arms' animation).
struct PitchBone {
    const char* name;
    float share;
};
constexpr PitchBone kPitchBones[] = {{"j_spine1", 0.15f}, {"j_spine2", 0.15f}, {"j_spine3", 0.15f}, {"j_spine4", 0.15f}, {"j_neck", 0.10f}};
constexpr float kPitchLimit = 60.0f, kPronePitchScale = 0.15f;

} // namespace

PlayerShadowBody::PlayerShadowBody(AnimLibrary& lib, const PlayerAnimScript& script) : lib_(lib), script_(script), tree_(lib.tree("multiplayer")) {
    if (tree_) anim_ = std::make_unique<AnimState>(*tree_, lib);
}

void PlayerShadowBody::reset() {
    on_ = false;
    st_ = MpPlayerAnim{};
    if (tree_) anim_ = std::make_unique<AnimState>(*tree_, lib_);
    strafing_ = "not";
    pitch_ = 0;
    prevWeapon_.clear();
    prevWeaponState_ = prevClip_ = 0;
    prevViewHeight_ = kStandHeight;
    switchHold_ = false;
    aimNode_ = 0, aimItem_ = nullptr;
}

float PlayerShadowBody::nodeMoveSpeed(int node) {
    auto it = moveSpeed_.find(node);
    if (it == moveSpeed_.end()) it = moveSpeed_.emplace(node, animNodeMoveSpeed(lib_, tree_, node)).first;
    return it->second;
}

bool PlayerShadowBody::event(const char* name, const PaContext& ctx, bool forced, int64_t now) {
    const PaItem* it = script_.event(name, ctx);
    if (!it || it->commands.empty() || !paEventAllowed(*it, st_, now, forced)) return false;
    seed_ = seed_ * 1103515245u + 12345u;
    const PaCommand& cmd = it->commands[size_t((seed_ >> 16) % it->commands.size())];
    const int d = playPaCommand(*anim_, lib_, &st_, cmd, true, now);
    bool legs = false;
    for (const PaCommand& c : it->commands)
        for (const PaPart& p : c.parts) legs |= p.body != PaBody::Torso;
    if (legs) st_.legsHeldUntilMs = now + std::max(d, 0), st_.legsItem = nullptr;
    return true;
}

bool PlayerShadowBody::update(const PlayerShadowInput& in) {
    if (in.excluded() || !tree_ || !anim_) {
        if (on_ || st_.legsItem) reset();
        return false;
    }
    const int64_t now = in.renderMs;
    if (on_ && now < lastMs_) reset(); // the clock went back (a level change): the holds are in its old time, start afresh
    const bool fresh = !on_;
    // The render clock steps the animation: the change since the last frame, in seconds (0 on the first frame or when the clock
    // went back, as at a level change).
    const float dtS = fresh ? 0.0f : std::clamp(float(now - lastMs_) * 0.001f, 0.0f, kMaxStepS);
    lastMs_ = now;
    on_ = true;

    // The animation context: the weapon's anim type and class, the aim, the movement.
    PaContext ctx;
    if (!in.weapon.empty()) {
        ctx.weaponClass = lower(in.weaponClass);
        ctx.playerAnimType = playerBodyAnimType(in.mpAnimType, ctx.weaponClass);
    }
    PaMovement mv;
    mv.viewHeightTarget = in.viewHeightTarget;
    mv.xySpeed = std::sqrt(in.vx * in.vx + in.vy * in.vy);
    playerBodyMoves(in.vx, in.vy, in.yaw, mv.forwardMove, mv.rightMove);
    // The legs play two items of the move type: the hip one and the aim one (aiming slows the body to a walk), weighted by the view
    // model's aim fraction. The events see the held aim.
    PaContext hip = ctx, aim = ctx;
    mv.slow = false;
    hip.moveType = paMoveType(mv), hip.strafing = paStrafing(mv), hip.weaponPosition = "hip";
    mv.slow = true;
    aim.moveType = paMoveType(mv), aim.strafing = paStrafing(mv), aim.weaponPosition = "ads";
    mv.slow = in.aiming;
    ctx = in.aiming ? aim : hip;
    strafing_ = ctx.strafing;
    const float aimFrac = std::clamp(in.adsFrac, 0.0f, 1.0f);

    // Events, from the changes of the state since the last frame (the player's own weapon and stance code does not call the
    // animation driver as the multiplayer server's does). The move type of the previous step is the one they see.
    if (!fresh) {
        PaContext ec = ctx;
        if (!st_.moveType.empty()) ec.moveType = st_.moveType;
        const bool sameWeapon = in.weapon == prevWeapon_;
        // Stance changes (TODO(spec): the original's trigger is not traced; the script has the prone transitions only).
        if (in.viewHeightTarget != prevViewHeight_) {
            const int from = prevViewHeight_, to = in.viewHeightTarget;
            const char* name = from == kCrouchHeight && to == kProneHeight   ? "crouch_to_prone"
                               : from == kProneHeight && to == kCrouchHeight ? "prone_to_crouch"
                               : from == kStandHeight && to == kCrouchHeight ? "stand_to_crouch"
                               : from == kCrouchHeight && to == kStandHeight ? "crouch_to_stand"
                               : from == kStandHeight && to == kProneHeight  ? "stand_to_prone"
                               : from == kProneHeight && to == kStandHeight  ? "prone_to_stand" : nullptr;
            if (name) event(name, ec, false, now);
        }
        if (in.weaponState == kStateDropping && prevWeaponState_ != kStateDropping) switchHold_ = event("dropweapon", ec, false, now);
        if (in.weaponState == kStateRaising && prevWeaponState_ != kStateRaising) event("raiseweapon", ec, false, now);
        if (reloading(in.weaponState) && !reloading(prevWeaponState_)) event("reload", ec, true, now);
        if (melee(in.weaponState) && !melee(prevWeaponState_)) event("meleeattack", ec, true, now);
        if (sameWeapon && in.clip < prevClip_ && !reloading(in.weaponState)) {
            ec.firing = true;
            event("fireweapon", ec, true, now);
        }
    }
    prevWeapon_ = in.weapon;
    prevWeaponState_ = in.weaponState;
    prevClip_ = in.clip;
    prevViewHeight_ = in.viewHeightTarget;
    st_.moveType = ctx.moveType;

    // The weapon switch holds the torso's pose while the weapon drops and rises (the pose animation has no length of its own;
    // TODO(spec): the original's hold for it is not traced).
    if (switchHold_ && st_.torsoActive && (in.weaponState == kStateDropping || in.weaponState == kStateRaising))
        st_.torsoHeldUntilMs = std::max(st_.torsoHeldUntilMs, now + 60);
    // The torso falls back to the legs' animation when its hold ends.
    if (st_.torsoActive && now >= st_.torsoHeldUntilMs) {
        switchHold_ = false;
        anim_->clear(tree_->find("torso"), 0.2f);
        st_.torsoActive = false;
    }

    // The move type's animations, re-chosen when an item changes; they wait for the legs' hold to run out to its last 49 ms. The
    // first pose is taken at once (no blend from the empty state).
    if (now + 49 >= st_.legsHeldUntilMs) {
        const PaItem* item = script_.animation(hip.moveType, hip);
        float blend = kDefaultBlend;
        bool hipPlayed = false;
        if (item && item != st_.legsItem && !item->commands.empty()) {
            seed_ = seed_ * 1103515245u + 12345u;
            PaCommand cmd = item->commands[size_t((seed_ >> 16) % item->commands.size())];
            if (fresh) cmd.blendMs = 0;
            blend = cmd.blendMs >= 0 ? float(cmd.blendMs) / 1000.0f : kDefaultBlend;
            playPaCommand(*anim_, lib_, &st_, cmd, false, now);
            st_.legsItem = item;
            hipPlayed = true;
        }
        const PaItem* aimItem = script_.animation(aim.moveType, aim);
        if (aimItem != aimItem_ || hipPlayed) {
            int node = 0;
            if (aimItem && !aimItem->commands.empty()) {
                seed_ = seed_ * 1103515245u + 12345u;
                const PaCommand& cmd = aimItem->commands[size_t((seed_ >> 16) % aimItem->commands.size())];
                if (aimItem != aimItem_ || hipPlayed) blend = fresh ? 0.0f : cmd.blendMs >= 0 ? float(cmd.blendMs) / 1000.0f : kDefaultBlend;
                for (const PaPart& p : cmd.parts)
                    if (p.body != PaBody::Torso && (node = tree_->find(p.anim)) > 0) break;
            }
            if (node == st_.legsNode || node < 0) node = 0; // the same animation with the aim: nothing to weigh
            if (aimNode_ && aimNode_ != node && aimNode_ != st_.legsNode) anim_->clear(aimNode_, blend);
            if (node && node != aimNode_) {
                // It starts in step with the hip animation (the same normalized time), at no weight; the split below weighs it.
                AnimState::SetArgs a;
                a.weight = 1.0f, a.blendTime = 0.0f;
                const float hipTime = st_.legsNode > 0 ? anim_->nodeStates()[size_t(st_.legsNode)].time : 0.0f;
                anim_->set(node, a); // not a knob: the hip animation keeps its state
                AnimNodeState& s = anim_->nodeStates()[size_t(node)];
                s.time = s.oldTime = hipTime, s.weight = s.goalWeight = 0.0f, s.blendLeft = 0.0f;
            }
            aimNode_ = node, aimItem_ = aimItem;
        }
    } else if (aimNode_) {
        anim_->clear(aimNode_, kDefaultBlend); // an event holds the legs: the aim animation goes with the hip one
        aimNode_ = 0, aimItem_ = nullptr;
    }
    // The split: the hip and aim animations share their weight (and goal) by the aim fraction, every frame, so the body's aim pose
    // follows the view model's raise and lower exactly; a blend between move types under way goes on in the shared total.
    if (aimNode_ && st_.legsNode > 0) {
        AnimNodeState& h = anim_->nodeStates()[size_t(st_.legsNode)];
        AnimNodeState& a = anim_->nodeStates()[size_t(aimNode_)];
        const float total = h.weight + a.weight, goal = h.goalWeight + a.goalWeight, left = std::max(h.blendLeft, a.blendLeft);
        // Weighed in again after a frame without weight (the released node's time went back to 0): it starts in step with the other.
        if (a.weight == 0.0f && aimFrac > 0.0f) a.time = a.oldTime = h.time;
        if (h.weight == 0.0f && aimFrac < 1.0f) h.time = h.oldTime = a.time;
        h.weight = total * (1.0f - aimFrac), a.weight = total * aimFrac;
        h.goalWeight = goal * (1.0f - aimFrac), a.goalWeight = goal * aimFrac;
        h.blendLeft = a.blendLeft = left;
    }

    // The legs and torso play at the body's speed over the animation's own (docs/spec/mp-players.md "Animation rate from the
    // movement"); the speed is the player's velocity.
    for (const int n : {st_.legsNode, aimNode_, st_.torsoActive ? st_.torsoNode : 0}) {
        if (n <= 0 || size_t(n) >= anim_->nodeStates().size()) continue;
        const float ms = nodeMoveSpeed(n);
        anim_->nodeStates()[size_t(n)].rate = ms > 0 ? paMoveRate(mv.xySpeed / ms, ms, false) : 1.0f;
    }
    anim_->update(fresh ? 0.0f : dtS, [](const std::string&, const std::string&) {}, false);

    // Lying, the torso bends little: a full bend would put the head through the ground.
    pitch_ = std::clamp(in.pitch, -kPitchLimit, kPitchLimit) * (in.viewHeightTarget <= kProneHeight ? kPronePitchScale : 1.0f);
    return true;
}

float PlayerShadowBody::aimWeight() const {
    if (!aimNode_ || st_.legsNode <= 0) return 0.0f;
    const AnimNodeState& h = anim_->nodeStates()[size_t(st_.legsNode)];
    const AnimNodeState& a = anim_->nodeStates()[size_t(aimNode_)];
    return h.weight + a.weight > 0.0f ? a.weight / (h.weight + a.weight) : 0.0f;
}

assets::XPose PlayerShadowBody::pose(const assets::XModelParts& parts) const {
    assets::XPose p = anim_->pose(parts);
    if (std::fabs(pitch_) < 0.5f) return p;
    // The pitch is a turn about the body's lateral (model y) axis; a control turns in the bone's own frame, so the axis is taken
    // into it through the bone's model-space rotation (the bones' local axes are not the model's).
    std::vector<assets::BoneControl> ctl;
    for (const PitchBone& pb : kPitchBones)
        for (size_t i = 0; i < parts.bones.size() && i < p.rotation.size(); ++i)
            if (lower(parts.bones[i].name) == pb.name) {
                const assets::Quat& r = p.rotation[i];
                const auto axis = assets::rotate(assets::Quat{-r.x, -r.y, -r.z, r.w}, {0.0f, 1.0f, 0.0f});
                const float half = pitch_ * pb.share * 0.00872664626f; // degrees to half-angle radians
                const float sn = std::sin(half), cs = std::cos(half);
                ctl.push_back({int(i), assets::Quat{axis[0] * sn, axis[1] * sn, axis[2] * sn, cs}, {0, 0, 0}});
                break;
            }
    std::sort(ctl.begin(), ctl.end(), [](const assets::BoneControl& a, const assets::BoneControl& b) { return a.bone < b.bone; });
    return ctl.empty() ? p : assets::applyControls(parts, p, ctl);
}

} // namespace iw2::game
