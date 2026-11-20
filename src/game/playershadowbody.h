// The pose of the player's shadow-only body (r_enhPlayerShadow, docs/render-enhanced.md "Player shadow"): the player's own state
// (stance, movement, weapon, aim) drives the multiplayer animation tree through the player animation script, the way a
// third-person multiplayer player is animated (docs/spec/mp-players.md, src/game/mpplayers.cpp). The caller gathers the state and
// the render clock; this unit chooses the animations, steps them in render time and gives the pose. Our own design: the
// first-person player has no body in the original.
#pragma once

#include "assets/xmodel.h"
#include "game/animtree.h"
#include "game/mpplayers.h"
#include "game/playeranimscript.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace iw2::game {

struct PlayerShadowInput {
    int64_t renderMs = 0;       // the client render clock (clientRenderTimeMs): steps the animation, never the 20 Hz world time
    // The cases with no body: on a turret, in a vehicle, linked by script, dead, spectating or in the killcam.
    bool onTurret = false, inVehicle = false, linked = false, dead = false, spectating = false;
    bool excluded() const { return onTurret || inVehicle || linked || dead || spectating; }
    int viewHeightTarget = 60;  // 60 stand, 40 crouch, 11 prone
    float yaw = 0, pitch = 0;   // the view angles (degrees; pitch positive looks down)
    float vx = 0, vy = 0;       // the horizontal velocity
    std::string weapon;         // the weapon in the hands ("" none); a change is a weapon switch
    std::string mpAnimType;     // that weapon's multiplayer playerAnimType ("" or "none": the weapon class stands for it)
    std::string weaponClass;
    int weaponState = 0;        // PM_Weapon state: 1 raising, 2 dropping, 3 firing, 5 / 7 reloading, 10 / 11 melee
    int clip = 0;               // rounds in the clip: a fall is a shot
    float adsFrac = 0;          // the view model's aim fraction 0..1: the weight of the body's aim pose, frame by frame
    bool aiming = false;        // the aim is held (the weapon's view aim, PlayerWeapon::viewAiming): the context of the events
};

class PlayerShadowBody {
public:
    PlayerShadowBody(AnimLibrary& lib, const PlayerAnimScript& script);

    // One render frame. False when the body is off (an excluded case, or the tree is missing): its state is dropped and the next
    // active frame starts afresh with the pose at once.
    bool update(const PlayerShadowInput& in);

    bool on() const { return on_; }
    const AnimState& anim() const { return *anim_; }
    const MpPlayerAnim& state() const { return st_; }
    // The move type chosen from the movement ("idle", "run", "idlecr", "idleprone", ...) and the strafing value.
    const std::string& moveType() const { return st_.moveType; }
    const std::string& strafing() const { return strafing_; }
    // The aim pose's animation (the move type's item with the aim, 0 while an event holds the legs or when it is the hip one) and
    // its share of the two legs animations' weight, which is the view model's aim fraction.
    int aimNode() const { return aimNode_; }
    float aimWeight() const;
    // The posed skeleton of a body or attachment model: the blended animation, then the torso pitch to the view (our own
    // rule, TODO(spec): the original's torso pitch controls are not traced) on the spine bones.
    assets::XPose pose(const assets::XModelParts& parts) const;

private:
    void reset();
    float nodeMoveSpeed(int node);
    bool event(const char* name, const PaContext& ctx, bool forced, int64_t now);

    AnimLibrary& lib_;
    const PlayerAnimScript& script_;
    const AnimTree* tree_ = nullptr;
    std::unique_ptr<AnimState> anim_;
    MpPlayerAnim st_;
    bool on_ = false;
    int64_t lastMs_ = 0;
    uint32_t seed_ = 0x2545f491u;
    std::map<int, float> moveSpeed_;
    std::string strafing_ = "not";
    float pitch_ = 0;
    bool switchHold_ = false; // the torso holds the weapon-switch pose while the weapon drops and rises
    // The previous frame's state, for the events.
    std::string prevWeapon_;
    int prevWeaponState_ = 0, prevClip_ = 0, prevViewHeight_ = 60;
    // The aim pose's legs animation and item, played beside the hip one (st_.legsNode) with the aim fraction's share of the weight.
    int aimNode_ = 0;
    const PaItem* aimItem_ = nullptr;
};

} // namespace iw2::game
