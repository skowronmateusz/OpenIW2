#include "game/playerweapon.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace iw2::game {

std::string PlayerWeapon::fieldValue(const char* key) const {
    std::string k(key);
    for (char& c : k) c = char(std::tolower(static_cast<unsigned char>(c)));
    auto it = def_.find(k); // World::weapon() lower-cases keys
    return it == def_.end() ? std::string() : it->second;
}

float PlayerWeapon::time(const char* key, float fallback) const {
    const std::string v = fieldValue(key);
    return v.empty() ? fallback : float(std::atof(v.c_str()));
}

std::string PlayerWeapon::defValue(const Def& d, const char* key) {
    std::string k(key);
    for (char& c : k) c = char(std::tolower(static_cast<unsigned char>(c)));
    auto it = d.find(k);
    return it == d.end() ? std::string() : it->second;
}

float PlayerWeapon::defNumber(const Def& d, const char* key) { return float(std::atof(defValue(d, key).c_str())); }

std::string PlayerWeapon::gunModel() const {
    const std::string m = defValue(offhandActive_ ? offDef_ : def_, "gunModel");
    return m.empty() ? m : "xmodel/" + m;
}

std::string PlayerWeapon::handModel() const {
    const std::string m = defValue(offhandActive_ ? offDef_ : def_, "handModel");
    return m.empty() ? m : "xmodel/" + m;
}

void PlayerWeapon::enter(State s, const char* animKey, float duration) {
    state_ = s;
    timer_ = duration;
    const std::string a = animKey ? fieldValue(animKey) : std::string();
    anim_ = a.empty() ? fieldValue("idleAnim") : a;
    animTime_ = 0;
    const char* soundKey = s == State::Reloading ? "reloadSound" : s == State::Rechamber ? "rechamberSound" : nullptr;
    if (soundKey && !fieldValue(soundKey).empty()) sounds_.push_back(fieldValue(soundKey));
}

// A state whose view animation comes from another definition (the offhand's, or the new weapon's).
void PlayerWeapon::enterWith(const Def& d, State s, const char* animKey, float duration) {
    state_ = s;
    timer_ = duration;
    const std::string a = defValue(d, animKey);
    anim_ = a.empty() ? defValue(d, "idleAnim") : a;
    animTime_ = 0;
}

void PlayerWeapon::equip(const std::string& name, const Def& def) {
    if (!name_.empty()) ammo_[name_] = {clip_, stock_};
    name_ = name;
    def_ = def;
    if (auto it = ammo_.find(name); it != ammo_.end()) {
        clip_ = it->second.first;
        stock_ = it->second.second;
    } else {
        const int clipSize = std::max(1, std::atoi(fieldValue("clipSize").c_str()));
        const int start = std::atoi(fieldValue("startAmmo").c_str());
        clip_ = std::min(clipSize, std::max(start, clipSize));
        stock_ = std::max(0, start - clip_);
    }
    needsRechamber_ = false;
    triggerReleased_ = true;
    enter(State::Raising, "raiseAnim", time("raiseTime", 0.5f));
}

// Begin a weapon change (0x4ddb40): the drop takes altDropTime when the new weapon is this one's altWeapon, else
// dropTime; without a current weapon the drop is immediate.
void PlayerWeapon::startDrop(const std::string& next) {
    if (state_ == State::Dropping) return;
    next_ = next;
    delay_ = 0;
    fuse_ = 0;
    if (name_.empty()) {
        state_ = State::Dropping;
        timer_ = 0;
        return;
    }
    const bool alt = !next.empty() && next == fieldValue("altWeapon");
    enter(State::Dropping, alt ? "altDropAnim" : "dropAnim", time(alt ? "altDropTime" : "dropTime", 0));
}

// The drop finished (PM_Weapon state 2): the command's weapon becomes the player's; the raise takes altRaiseTime
// when it is the old weapon's altWeapon, else raiseTime.
void PlayerWeapon::finishDrop() {
    const Def* d = lookup && !next_.empty() ? lookup(next_) : nullptr;
    const std::string old = name_;
    const bool alt = !old.empty() && d && next_ == fieldValue("altWeapon");
    if (!d || next_ == old) {
        enter(State::Ready, nullptr, 0);
        return;
    }
    equip(next_, *d);
    if (alt) enter(State::Raising, "altRaiseAnim", time("altRaiseTime", 0));
    else enter(State::Raising, "raiseAnim", time("raiseTime", 0));
    events_.push_back({Event::WeaponRaised, name_});
}

// The melee blow lands (0x4dee30): the remaining time is at least meleeTime − meleeDelay.
void PlayerWeapon::meleeHit() {
    timer_ = std::max(timer_, time("meleeTime", 0) - time("meleeDelay", 0));
    events_.push_back({Event::MeleeHit, name_});
    state_ = State::MeleeEnd;
}

int PlayerWeapon::update(float dt, const Input& in) {
    if (name_.empty() && (in.weapon.empty() || !lookup)) return 0;
    // Aiming (0x4dd0d0): only weapons with aimDownSight; not while reloading, nor while rechambering unless
    // rechamberWhileAds. The fraction moves by ms / adsTransInTime (or adsTransOutTime) and is clamped to 0..1.
    {
        const bool can = time("aimDownSight", 0) != 0;
        const bool want = can && in.ads && state_ != State::Reloading &&
                          !(state_ == State::Rechamber && time("rechamberWhileAds", 0) == 0);
        if (!can) adsFrac_ = 0;
        else if (want) adsFrac_ += dt / std::max(0.001f, time("adsTransInTime", 0.3f));
        else adsFrac_ -= dt / std::max(0.001f, time("adsTransOutTime", 0.3f));
        adsFrac_ = std::clamp(adsFrac_, 0.0f, 1.0f);
        if (adsFrac_ == 0) zoomingIn_ = true;
        if (adsFrac_ == 1) zoomingIn_ = false;
    }
    animTime_ += dt;
    timer_ -= dt;
    // Weapon delay (0x4de720): reaching 0 lets this frame's state change happen at once.
    bool delayDone = false;
    if (delay_ > 0) {
        delay_ -= dt;
        if (delay_ <= 0) {
            delay_ = 0;
            delayDone = true;
        }
    }
    const bool offhandHeld = in.frag || in.smoke;

    // Cooking (0x4df5a0): a grenade with cookOffHold burns its fuse while held; at 0 it goes off in the hand.
    if (offhandActive_ && defValue(offDef_, "weaponType") == "grenade" && fuse_ > 0) {
        if (defNumber(offDef_, "cookOffHold") != 0) fuse_ -= dt;
        if (fuse_ <= 0) events_.push_back({Event::OffhandExplode, offhand_});
    }

    // Offhand (0x4df470): +frag / +smoke pick the first grenade of that class with ammo. Not during the throw
    // states 12–15 (16, the raise after a throw, may start another).
    if (offhandHeld && !(int(state_) >= 12 && int(state_) <= 15)) {
        const std::string cls = in.frag ? "Frag Grenade" : "Smoke Grenade";
        const std::string w = findOffhand ? findOffhand(cls) : std::string();
        const Def* d = w.empty() || !lookup ? nullptr : lookup(w);
        if (!d) {
            events_.push_back({Event::NoOffhand, cls});
        } else {
            offhand_ = w;
            offDef_ = *d;
            delay_ = 0;
            if (!name_.empty() && state_ != State::OffhandEnd) {
                // Put the weapon away quickly first.
                enter(State::OffhandInit, "quickDropAnim", time("quickDropTime", 0));
                offhandActive_ = false;
            } else {
                enterWith(offDef_, State::OffhandPrepare, "holdFireAnim", defNumber(offDef_, "holdFireTime"));
                offhandActive_ = true;
            }
            offhandHeldPrev_ = offhandHeld;
            return 0;
        }
    }

    // Weapon change (0x4de880): not during the offhand states; while the weapon is busy (timer running) only a
    // rechamber or reload may be interrupted.
    if (!inOffhand() && !in.weapon.empty() && in.weapon != name_ && state_ != State::Dropping) {
        const bool busy = timer_ > 0 && state_ != State::Rechamber && state_ != State::Reloading &&
                          (state_ == State::Firing || state_ == State::Melee || state_ == State::MeleeEnd || delay_ > 0);
        if (!busy && lookup && lookup(in.weapon)) startDrop(in.weapon);
    }

    // Melee (0x4def20): +melee pressed (latched until released) with a weapon that has meleeDamage, not while
    // aiming a weapon with an ADS overlay reticle; meleeDelay later the blow lands, meleeTime is the whole swing.
    if (!inOffhand() && !name_.empty()) {
        if (!in.melee) meleeLatch_ = false;
        else if (!meleeLatch_ && time("meleeDamage", 0) != 0 && !delayDone && (delay_ <= 0 || state_ == State::Reloading)) {
            meleeLatch_ = true;
            const std::string reticle = fieldValue("adsOverlayReticle");
            const bool canMelee = adsFrac_ <= 0 || reticle.empty() || reticle == "none";
            if (canMelee && state_ != State::Raising && state_ != State::Dropping && state_ != State::Melee &&
                state_ != State::MeleeEnd) {
                anim_ = fieldValue("meleeAnim").empty() ? fieldValue("idleAnim") : fieldValue("meleeAnim");
                animTime_ = 0;
                events_.push_back({Event::MeleeSwing, name_});
                if (time("meleeDelay", 0) == 0) {
                    meleeHit();
                } else {
                    state_ = State::Melee;
                    timer_ = time("meleeTime", 0);
                    delay_ = time("meleeDelay", 0);
                }
                offhandHeldPrev_ = offhandHeld;
                return 0;
            }
        }
    }

    // State changes once the timers ran out (PM_Weapon's switch).
    if (delayDone || (timer_ <= 0 && delay_ <= 0)) {
        switch (state_) {
        case State::Dropping:
            finishDrop();
            offhandHeldPrev_ = offhandHeld;
            return 0;
        case State::Melee:
            meleeHit();
            offhandHeldPrev_ = offhandHeld;
            return 0;
        case State::MeleeEnd:
            enter(State::Ready, nullptr, 0);
            break;
        case State::OffhandInit:
            enterWith(offDef_, State::OffhandPrepare, "holdFireAnim", defNumber(offDef_, "holdFireTime"));
            offhandActive_ = true;
            delay_ = 0;
            offhandHeldPrev_ = offhandHeld;
            return 0;
        case State::OffhandPrepare:
            state_ = State::OffhandHold;
            timer_ = delay_ = 0;
            fuse_ = defNumber(offDef_, "fuseTime");
            offhandHeldPrev_ = offhandHeld;
            return 0;
        case State::OffhandHold:
            if (offhandHeld && offhandHeldPrev_) { // still held: wait (weaponDelay 1)
                delay_ = 0.001f;
                offhandHeldPrev_ = offhandHeld;
                return 0;
            }
            enterWith(offDef_, State::OffhandThrow, "fireAnim", defNumber(offDef_, "fireTime"));
            delay_ = 0;
            events_.push_back({Event::OffhandThrow, offhand_});
            offhandHeldPrev_ = offhandHeld;
            return 0;
        case State::OffhandThrow:
            offhandActive_ = false;
            if (!name_.empty()) {
                enter(State::OffhandEnd, "quickRaiseAnim", time("quickRaiseTime", 0));
                delay_ = 0;
            } else {
                state_ = State::OffhandEnd;
                timer_ = 0;
                delay_ = 0.001f;
            }
            offhandHeldPrev_ = offhandHeld;
            return 0;
        case State::OffhandEnd: // 0x4df0a0
            offhandActive_ = false;
            enter(State::Ready, nullptr, 0);
            break;
        default:
            break;
        }
    }
    offhandHeldPrev_ = offhandHeld;
    if (inOffhand() || state_ == State::Dropping || state_ == State::Melee || state_ == State::MeleeEnd || name_.empty())
        return 0;
    const bool automatic = fieldValue("semiAuto") != "1";
    const int clipSize = std::max(1, std::atoi(fieldValue("clipSize").c_str()));
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
    default:
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
        needsRechamber_ = fieldValue("boltAction") == "1";
        const bool aimed = adsFrac_ == 1.0f;
        enter(State::Firing, clip_ == 0 ? (aimed ? "adsLastShotAnim" : "lastShotAnim") : (aimed ? "adsFireAnim" : "fireAnim"),
              time("fireTime", 0.1f));
    }
    return shots;
}

} // namespace iw2::game
