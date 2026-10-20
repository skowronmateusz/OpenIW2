// Player weapon state: firing cadence, rechambering, reloading and the view-model animation to show
// (docs/spec/weapons.md). Timings come from the weapons/sp/<name> file; the state machine itself is inferred.
#pragma once

#include <array>
#include <map>
#include <string>

namespace iw2::game {

class PlayerWeapon {
public:
    struct Input {
        bool fire = false;   // trigger held
        bool reload = false; // reload pressed this tick
    };

    // `def` is the parsed weapon file (World::weapon). Starts raising the weapon.
    void equip(const std::string& name, const std::map<std::string, std::string>& def);

    // Advances by dt seconds; returns the number of shots fired during this step.
    int update(float dt, const Input& in);

    const std::string& name() const { return name_; }
    bool equipped() const { return !name_.empty(); }
    int clip() const { return clip_; }
    int stock() const { return stock_; }

    // View model: xmodel paths for the gun and hands and the current animation with its elapsed time.
    std::string gunModel() const;
    std::string handModel() const;
    const std::string& anim() const { return anim_; }
    float animTime() const { return animTime_; }
    std::array<float, 3> standMove() const { return {time("standMoveF", 0), time("standMoveR", 0), time("standMoveU", 0)}; }
    bool animLoops() const { return state_ == State::Ready; }

private:
    enum class State { Raising, Ready, Firing, Rechamber, Reloading };
    std::string field(const char* key) const;
    float time(const char* key, float fallback) const;
    void enter(State s, const char* animKey, float duration);

    std::string name_;
    std::map<std::string, std::string> def_;
    State state_ = State::Ready;
    float timer_ = 0;
    std::string anim_;
    float animTime_ = 0;
    int clip_ = 0, stock_ = 0;
    bool triggerReleased_ = true;
    bool needsRechamber_ = false;
};

} // namespace iw2::game
