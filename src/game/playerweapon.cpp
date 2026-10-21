#include "game/playerweapon.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace iw2::game {

std::string PlayerWeapon::field(const char* key) const {
    std::string k(key);
    for (char& c : k) c = char(std::tolower(static_cast<unsigned char>(c)));
    auto it = def_.find(k); // World::weapon() lower-cases keys
    return it == def_.end() ? std::string() : it->second;
}

float PlayerWeapon::time(const char* key, float fallback) const {
    const std::string v = field(key);
    return v.empty() ? fallback : float(std::atof(v.c_str()));
}

std::string PlayerWeapon::gunModel() const {
    const std::string m = field("gunModel");
    return m.empty() ? m : "xmodel/" + m;
}

std::string PlayerWeapon::handModel() const {
    const std::string m = field("handModel");
    return m.empty() ? m : "xmodel/" + m;
}

void PlayerWeapon::enter(State s, const char* animKey, float duration) {
    state_ = s;
    timer_ = duration;
    const std::string a = animKey ? field(animKey) : std::string();
    anim_ = a.empty() ? field("idleAnim") : a;
    animTime_ = 0;
    const char* soundKey = s == State::Reloading ? "reloadSound" : s == State::Rechamber ? "rechamberSound" : nullptr;
    if (soundKey && !field(soundKey).empty()) sounds_.push_back(field(soundKey));
}

void PlayerWeapon::equip(const std::string& name, const std::map<std::string, std::string>& def) {
    name_ = name;
    def_ = def;
    const int clipSize = std::max(1, std::atoi(field("clipSize").c_str()));
    const int start = std::atoi(field("startAmmo").c_str());
    clip_ = std::min(clipSize, std::max(start, clipSize));
    stock_ = std::max(0, start - clip_);
    needsRechamber_ = false;
    triggerReleased_ = true;
    enter(State::Raising, "raiseAnim", time("raiseTime", 0.5f));
}

int PlayerWeapon::update(float dt, const Input& in) {
    if (name_.empty()) return 0;
    animTime_ += dt;
    timer_ -= dt;
    const bool automatic = field("semiAuto") != "1";
    const int clipSize = std::max(1, std::atoi(field("clipSize").c_str()));
    int shots = 0;
    if (!in.fire) triggerReleased_ = true;

    auto startReload = [&]() {
        if (stock_ <= 0 || clip_ >= clipSize) return false;
        const bool empty = clip_ == 0;
        enter(State::Reloading, empty ? "reloadEmptyAnim" : "reloadAnim", time(empty ? "reloadEmptyTime" : "reloadTime", 2.0f));
        return true;
    };

    if (timer_ > 0) return 0;
    switch (state_) {
    case State::Reloading: {
        const int take = std::min(clipSize - clip_, stock_);
        clip_ += take;
        stock_ -= take;
        needsRechamber_ = false;
        enter(State::Ready, nullptr, 0);
        break;
    }
    case State::Firing:
        if (needsRechamber_ && clip_ > 0) {
            enter(State::Rechamber, "rechamberAnim", time("rechamberTime", 1.0f));
            needsRechamber_ = false;
            return 0;
        }
        enter(State::Ready, nullptr, 0);
        break;
    case State::Raising:
    case State::Rechamber:
        enter(State::Ready, nullptr, 0);
        break;
    case State::Ready:
        break;
    }

    if (in.reload && startReload()) return 0;
    if (in.fire && (automatic || triggerReleased_)) {
        if (clip_ == 0) {
            if (!startReload()) triggerReleased_ = false;
            return 0;
        }
        --clip_;
        ++shots;
        triggerReleased_ = false;
        needsRechamber_ = field("boltAction") == "1";
        enter(State::Firing, clip_ == 0 ? "lastShotAnim" : "fireAnim", time("fireTime", 0.1f));
    }
    return shots;
}

} // namespace iw2::game
