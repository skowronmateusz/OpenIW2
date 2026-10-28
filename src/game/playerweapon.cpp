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
    const std::string m = defValue(binocActive_ ? binocDef_ : offhandActive_ ? offDef_ : def_, "gunModel");
    return m.empty() ? m : "xmodel/" + m;
}

std::string PlayerWeapon::handModel() const {
    const std::string m = defValue(binocActive_ ? binocDef_ : offhandActive_ ? offDef_ : def_, "handModel");
    return m.empty() ? m : "xmodel/" + m;
}

void PlayerWeapon::enter(State s, const char* animKey, float duration) {
    state_ = s;
    timer_ = duration;
    const std::string a = animKey ? fieldValue(animKey) : std::string();
    const std::string idle = fieldValue("idleAnim");
    pendingIdle_ = false;
    if (!animKey && animLength && anim_ != idle) {
        const float len = animLength(anim_);
        if (len > 0 && animTime_ < len) { // keep the action anim playing, idle follows when it ends
            pendingIdle_ = true;
        }
    }
    if (!pendingIdle_) {
        anim_ = a.empty() ? idle : a;
        animTime_ = 0;
    }
    // The cgame events of the state change play one sound each: the first non-empty of a priority chain (0x4ba8d0).
    if (s == State::Reloading) {
        // Reload 0x96 / reload-from-empty 0x97 (the latter for a bullet weapon with an empty clip, 0x4dd9d8): the player
        // variants first (reloadSoundPlayer, reloadEmptySoundPlayer; swapped for 0x97), then reloadSound, reloadEmptySound.
        const bool fromEmpty = clip_ == 0 && fieldValue("weaponType") == "bullet";
        if (fromEmpty) playFirst({"reloadEmptySoundPlayer", "reloadSoundPlayer", "reloadEmptySound", "reloadSound"});
        else playFirst({"reloadSoundPlayer", "reloadEmptySoundPlayer", "reloadSound", "reloadEmptySound"});
    } else if (s == State::ReloadStart) {
        playFirst({"reloadStartSoundPlayer", "reloadStartSound"}); // event 0x98
    } else if (s == State::ReloadEnd) {
        playFirst({"reloadEndSoundPlayer", "reloadEndSound"}); // event 0x99
    } else if (s == State::Rechamber) {
        playFirst({"rechamberSoundPlayer", "rechamberSound"}); // event 0xa1
    }
}

// The first non-empty sound of the current weapon among the keys (the cgame chains of 0x4ba8d0).
void PlayerWeapon::playFirst(std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        const std::string v = fieldValue(k);
        if (!v.empty()) {
            sounds_.push_back(v);
            return;
        }
    }
}

// A state whose view animation comes from another definition (the offhand's, or the new weapon's).
void PlayerWeapon::enterWith(const Def& d, State s, const char* animKey, float duration) {
    state_ = s;
    timer_ = duration;
    const std::string a = defValue(d, animKey);
    anim_ = a.empty() ? defValue(d, "idleAnim") : a;
    animTime_ = 0;
    pendingIdle_ = false;
    // Entering state 13 adds event 0xa5 (0x4df1b4, param = the offhand weapon) and state 15 event 0xa6 (0x4df2a4); cgame plays
    // the weapon info's +0xdc / +0xe0 sound (0x4b8ae0, 0x4b8b10), which the loader fills from the def's pullbackSound (+0xa4)
    // and fireSound (+0xa8) (0x4b3566-0x4b35db).
    const char* soundKey = s == State::OffhandPrepare ? "pullbackSound" : s == State::OffhandThrow ? "fireSound" : nullptr;
    if (soundKey && !defValue(d, soundKey).empty()) sounds_.push_back(defValue(d, soundKey));
}

void PlayerWeapon::equip(const std::string& name, const Def& def) {
    pendingIdle_ = false;
    if (!name_.empty()) {
        ammo_[name_] = {clip_, stock_};
        if (writeAmmo) writeAmmo(name_, clip_, stock_);
    }
    name_ = name;
    def_ = def;
    const std::optional<std::pair<int, int>> inv = readAmmo ? readAmmo(name) : std::nullopt;
    if (inv) {
        clip_ = inv->first;
        stock_ = inv->second;
    } else if (auto it = ammo_.find(name); it != ammo_.end()) {
        clip_ = it->second.first;
        stock_ = it->second.second;
    } else {
        const int clipSize = std::max(1, std::atoi(fieldValue("clipSize").c_str()));
        const int start = std::atoi(fieldValue("startAmmo").c_str());
        clip_ = std::min(clipSize, std::max(start, clipSize));
        stock_ = std::max(0, start - clip_);
    }
    needsRechamber_ = false;
    brassInChamber_ = false; // TODO(spec): the exe keeps the ps+0x530 bit per weapon across switches
    brassTimer_ = 0;
    addPending_ = false;
    reloadInterrupted_ = false;
    triggerReleased_ = true;
    enter(State::Raising, "raiseAnim", time("raiseTime", 0.5f));
    playFirst({"raiseSound"}); // event 0x9a (0x4dde44), once per raise (the alt raise re-enters with its own anim)
}

// Begin a weapon change (0x4ddb40): the drop takes altDropTime when the new weapon is this one's altWeapon, else
// dropTime; without a current weapon the drop is immediate.
void PlayerWeapon::startDrop(const std::string& next) {
    if (state_ == State::Dropping) return;
    next_ = next;
    delay_ = 0;
    addPending_ = false;
    fuse_ = 0;
    if (name_.empty()) {
        state_ = State::Dropping;
        timer_ = 0;
        return;
    }
    const bool alt = !next.empty() && next == fieldValue("altWeapon");
    enter(State::Dropping, alt ? "altDropAnim" : "dropAnim", time(alt ? "altDropTime" : "dropTime", 0));
    // 0x4ddb40: event 0x9c (altSwitchSound) when the new weapon is the altWeapon, else 0x9b (putawaySound).
    // The putaway event 0x9b is sent only when clipOnly (+0x340, field table 0x5a86fc) is 0 or the clip is not empty (0x4ddbd9).
    if (alt) playFirst({"altSwitchSound"});
    else if (std::atoi(fieldValue("clipOnly").c_str()) == 0 || clip_ != 0) playFirst({"putawaySound"});
}

// The drop finished (PM_Weapon state 2): the command's weapon becomes the player's; the raise takes altRaiseTime
// when it is the old weapon's altWeapon, else raiseTime.
void PlayerWeapon::finishDrop() {
    const Def* d = lookup && !next_.empty() ? lookup(next_) : nullptr;
    const std::string old = name_;
    const bool alt = !old.empty() && d && next_ == fieldValue("altWeapon");
    if (next_.empty() && !old.empty()) {
        // To weapon 0 (a holster): the player state has no weapon until the command's weapon is raised again.
        ammo_[old] = {clip_, stock_};
        name_.clear();
        holstered_ = true;
        enter(State::Ready, nullptr, 0);
        return;
    }
    if (!d || next_ == old) {
        enter(State::Ready, nullptr, 0);
        return;
    }
    holstered_ = false;
    equip(next_, *d);
    if (alt) enter(State::Raising, "altRaiseAnim", time("altRaiseTime", 0));
    else enter(State::Raising, "raiseAnim", time("raiseTime", 0));
    events_.push_back({Event::WeaponRaised, name_, alt});
}

// The melee blow lands (0x4dee30): the remaining time is at least meleeTime − meleeDelay.
void PlayerWeapon::meleeHit() {
    timer_ = std::max(timer_, time("meleeTime", 0) - time("meleeDelay", 0));
    events_.push_back({Event::MeleeHit, name_});
    state_ = State::MeleeEnd;
}

bool PlayerWeapon::viewAiming() const {
    if (inOffhand()) return false;
    if (state_ == State::Reloading && (segmented() || timer_ - time("adsReloadTransTime", 0) > 0)) return false;
    if (state_ == State::ReloadStart) return false;
    if (state_ == State::ReloadEnd && timer_ - time("adsReloadTransTime", 0) > 0) return false; // 0x4dd189
    return adsHeld_;
}

// 0x4df7f0: the binoculars come up: pm_flags |= 0x20000, state 0x13 for the binocular weapon's adsTransInTime, event 0xa8,
// view animation 0 (the binocular idle).
void PlayerWeapon::binocularEnter() {
    if (const Def* d = binocularDef ? binocularDef() : nullptr) binocDef_ = *d;
    binocActive_ = true;
    state_ = State::BinocEnter;
    timer_ = defNumber(binocDef_, "adsTransInTime");
    delay_ = 0;
    events_.push_back({Event::BinocularEnter, std::string()});
    anim_ = defValue(binocDef_, "idleAnim");
    animTime_ = 0;
    pendingIdle_ = false;
}

// 0x4df0a0 from a binocular state: offhand flags 0x810 cleared; 0x4df6c0: aim fraction 0, event 0x95, pm_flags &= ~0x20040,
// event 0xa9; timers 0, state 0, view animation 0; with attack held weaponTime 1 ms (0x4dfabd).
void PlayerWeapon::binocularCancel(bool attackHeld) {
    offhandActive_ = false;
    adsFrac_ = 0;
    zoomingIn_ = true;
    events_.push_back({Event::ReloadBegin, std::string()}); // 0x95: the cgame clears the toggled aim
    binocActive_ = false;
    events_.push_back({Event::BinocularExit, std::string()});
    state_ = State::Ready;
    timer_ = attackHeld ? 0.001f : 0.0f;
    delay_ = 0;
    anim_ = fieldValue("idleAnim");
    animTime_ = 0;
    pendingIdle_ = false;
}

void PlayerWeapon::binocularStep(const Input& in, bool delayDone) {
    // Cancel mask: attack 1, melee 4, reload 0x10, frag / smoke 0x30000; without attack in a vehicle (eFlags 0x20000).
    const bool maskHeld = in.melee || in.reload || in.frag || in.smoke || (!in.inVehicle && in.fire);
    const int st = int(state_);
    // With the hand weapon holstered (ps weapon 0, e.g. in a vehicle) the drop / raise times are weapon 0's, i.e. 0.
    auto handTime = [&](const char* k) { return name_.empty() ? 0.0f : time(k, 0); };
    if (!inBinoc()) {
        const std::string ret = fieldValue("adsOverlayReticle");
        if (adsFrac_ > 0 && !ret.empty() && ret != "none") return; // scoped aim (0x4df9da)
        if (maskHeld) return;
        if (st >= 1 && st <= 16) return; // only from the ready state (0x4df9fb..0x4dfa60)
    }
    // ps+0x104 (damage this frame) also cancels. TODO(spec): the command byte +0xd (0x4dfa8a) is not modelled (taken as unchanged).
    const bool cancel = in.damaged || in.weapon != prevCmdWeapon_ || maskHeld;
    if (inBinoc() && cancel) {
        binocularCancel(in.fire);
        return;
    }
    bool enter = false, exit = false;
    const bool exitable = st > 0x11 && st < 0x15; // 0x12, 0x13, 0x14 (0x4dfbe1)
    if (in.fire) goto sw; // attack held (0x4dfac9; in a binocular state only reached in a vehicle)
    if (state_ == State::BinocUp) { // use pressed / released (0x4dfad6..0x4dfb4c)
        if (!usePrev_ && in.use) events_.push_back({Event::BinocularFire, std::string()});
        else if (usePrev_ && !in.use) events_.push_back({Event::BinocularRelease, std::string()});
    }
    if (!in.toggleBinoculars) { // hold mode
        enter = in.binoculars && !inBinoc();
        exit = !in.binoculars && exitable;
    } else { // toggle mode: a fresh press enters, or leaves when up
        const bool pressed = !binocPrev_ && in.binoculars;
        if (pressed && !inBinoc()) enter = true;
        else if (pressed) exit = exitable;
    }
    if (enter) {
        if (!binocularDef || !binocularDef()) goto sw;
        if (in.weapon.empty() && name_.empty()) { // command weapon 0: straight up (0x4dfbb6)
            binocularEnter();
            goto sw;
        }
        if (!in.ads) { // quick-drop the hand weapon first (0x4dfbc4)
            state_ = State::BinocDrop;
            timer_ = handTime("quickDropTime");
            delay_ = 0;
            const std::string a = fieldValue("quickDropAnim");
            anim_ = a.empty() ? fieldValue("idleAnim") : a;
            animTime_ = 0;
            pendingIdle_ = false;
            goto sw;
        }
        // Aiming: leave the aim first (event 0x95, pm_flags &= ~0x40), wait the hand weapon's adsTransOutTime + player_adsExitDelay.
        events_.push_back({Event::ReloadBegin, std::string()});
        timer_ = 0;
        delay_ = handTime("adsTransOutTime") + in.adsExitDelay;
        state_ = State::BinocAdsOut;
        goto sw;
    }
    if (exit) {
        if (state_ == State::BinocDrop) { // still dropping the hand weapon (0x4dfc2b)
            binocActive_ = false;
            state_ = State::BinocRaise;
            events_.push_back({Event::BinocularExit, std::string()});
            if (!in.weapon.empty() || !name_.empty()) {
                timer_ = handTime("quickRaiseTime");
                delay_ = 0;
                const std::string a = fieldValue("quickRaiseAnim");
                anim_ = a.empty() ? fieldValue("idleAnim") : a;
                animTime_ = 0;
                pendingIdle_ = false;
            } else {
                timer_ = 0;
                delay_ = 0.001f;
            }
            goto sw;
        }
        // 0x4df890: lowering, the binocular weapon's adsTransOutTime + player_adsExitDelay, event 0xac, view animation 0.
        state_ = State::BinocExit;
        timer_ = defNumber(binocDef_, "adsTransOutTime") + in.adsExitDelay;
        delay_ = 0;
        events_.push_back({Event::BinocularDrop, std::string()});
        goto sw;
    }
    // PM_Weapon's switch for 0x11-0x16 once the timers ran out (0x4dfc30 cases; the delay flag is the one taken before 0x4df9b0).
sw:
    if (!inBinoc() || !(delayDone || (timer_ <= 0 && delay_ <= 0))) return;
    switch (state_) {
    case State::BinocAdsOut: // -> quick drop
        state_ = State::BinocDrop;
        timer_ = handTime("quickDropTime");
        delay_ = 0;
        anim_ = fieldValue("quickDropAnim").empty() ? fieldValue("idleAnim") : fieldValue("quickDropAnim");
        animTime_ = 0;
        pendingIdle_ = false;
        break;
    case State::BinocDrop:
        binocularEnter();
        break;
    case State::BinocEnter:
        state_ = State::BinocUp;
        timer_ = delay_ = 0;
        break;
    case State::BinocUp:
        break;
    case State::BinocExit: // pm_flags &= ~0x20000, 0x16, event 0xa9, quick raise (or a 1 ms delay without a weapon)
        binocActive_ = false;
        state_ = State::BinocRaise;
        events_.push_back({Event::BinocularExit, std::string()});
        if (!in.weapon.empty() || !name_.empty()) {
            timer_ = handTime("quickRaiseTime");
            delay_ = 0;
            anim_ = fieldValue("quickRaiseAnim").empty() ? fieldValue("idleAnim") : fieldValue("quickRaiseAnim");
            animTime_ = 0;
            pendingIdle_ = false;
        } else {
            timer_ = 0;
            delay_ = 0.001f;
        }
        break;
    case State::BinocRaise: // 0x4df0a0
        state_ = State::Ready;
        timer_ = delay_ = 0;
        anim_ = fieldValue("idleAnim");
        animTime_ = 0;
        pendingIdle_ = false;
        break;
    default:
        break;
    }
}

int PlayerWeapon::update(float dt, const Input& in) {
    // The ammunition lives in the player state (the world inventory): scripts, pickups and the HUD see the same counters.
    if (!name_.empty() && readAmmo)
        if (const auto a = readAmmo(name_)) clip_ = a->first, stock_ = a->second;
    const std::string before = name_;
    const int shots = updateImpl(dt, in);
    if (!before.empty() && before == name_ && writeAmmo) writeAmmo(name_, clip_, stock_);
    else if (!name_.empty() && writeAmmo) writeAmmo(name_, clip_, stock_);
    return shots;
}

int PlayerWeapon::updateImpl(float dt, const Input& in) {
    const bool binocPossible = inBinoc() || (in.binoculars && binocularDef && binocularDef());
    const bool idleNoWeapon = name_.empty() && (in.weapon.empty() || !lookup || (holstered_ && in.holster));
    if (idleNoWeapon && !binocPossible) {
        binocPrev_ = in.binoculars, usePrev_ = in.use, prevCmdWeapon_ = in.weapon;
        return 0;
    }
    if (!idleNoWeapon && !inBinoc() && name_.empty() && holstered_ && state_ == State::Ready) startDrop(in.weapon); // no weapon: raise at once
    adsHeld_ = in.ads;
    const bool fireEdge = in.fire && !firePrev_; // the press against the old command (0x4de3d3)
    firePrev_ = in.fire;
    // Aiming (0x4dd0d0): only weapons with aimDownSight; not while reloading, nor while rechambering unless
    // rechamberWhileAds. The fraction moves by ms / adsTransInTime (or adsTransOutTime) and is clamped to 0..1.
    {
        // The aim weapon is the binocular one while pm_flags 0x20000 is set (0x4dcf8d); in the binocular states the aim
        // flag is forced in 0x13 / 0x14 (0x4dd044) and impossible in 0x11, 0x12, 0x15, 0x16 (0x4dcfe5 -> 0x4dcf50).
        auto adsNum = [&](const char* k, float fb) {
            if (!binocActive_) return time(k, fb);
            const std::string v = defValue(binocDef_, k);
            return v.empty() ? fb : float(std::atof(v.c_str()));
        };
        const bool can = adsNum("aimDownSight", 0) != 0;
        bool want = can && in.ads && state_ != State::Reloading && state_ != State::ReloadStart &&
                    !(state_ == State::ReloadEnd && timer_ - time("adsReloadTransTime", 0) > 0) &&
                    !(state_ == State::Rechamber && time("rechamberWhileAds", 0) == 0);
        if (inBinoc()) want = can && (state_ == State::BinocEnter || state_ == State::BinocUp);
        if (!can) adsFrac_ = 0;
        else if (want) adsFrac_ += dt / std::max(0.001f, adsNum("adsTransInTime", 0.3f));
        else adsFrac_ -= dt / std::max(0.001f, adsNum("adsTransOutTime", 0.3f));
        adsFrac_ = std::clamp(adsFrac_, 0.0f, 1.0f);
        if (adsFrac_ == 0) zoomingIn_ = true;
        if (adsFrac_ == 1) zoomingIn_ = false;
    }
    animTime_ += dt;
    if (pendingIdle_) {
        const float len = animLength ? animLength(anim_) : 0.0f;
        if (len <= 0 || animTime_ >= len) {
            anim_ = fieldValue("idleAnim");
            animTime_ = 0;
            pendingIdle_ = false;
        }
    }
    timer_ -= dt;
    // Bolt-action brass (0x4dd700 in the rechamber state 4, 0x4ddfa0 in the reload states 5–8): when the delay set at the
    // state start expires with the chamber bit still set, the bit is cleared and event 0xa2 ejects the casing.
    if (brassTimer_ > 0) {
        brassTimer_ -= dt;
        if (brassTimer_ <= 0) {
            brassTimer_ = 0;
            if (brassInChamber_ && (state_ == State::Rechamber || inReloadStates() || state_ == State::ReloadEnd)) {
                brassInChamber_ = false;
                events_.push_back({Event::EjectBrass, name_});
            }
        }
    }
    if (addPending_) { // the reload's ammo add (weaponDelay, 0x4dd8c0 / 0x4dd5b0)
        addTimer_ -= dt;
        if (addTimer_ <= 0) applyReloadAdd();
    }
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

    // Binoculars (0x4df9b0, before the offhand step in PM_Weapon 0x4dfc8f) and the binocular cases of the state switch.
    {
        const State before = state_;
        binocularStep(in, delayDone);
        binocPrev_ = in.binoculars, usePrev_ = in.use, prevCmdWeapon_ = in.weapon;
        if (inBinoc() || before != state_) {
            offhandHeldPrev_ = offhandHeld;
            if (inBinoc()) return 0;
        }
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
    if (in.holster && !inOffhand() && !name_.empty() && state_ != State::Dropping) {
        const bool busy = timer_ > 0 && state_ != State::Rechamber && !inReloadStates() && state_ != State::ReloadEnd &&
                          (state_ == State::Firing || state_ == State::Melee || state_ == State::MeleeEnd || delay_ > 0);
        if (!busy) startDrop(std::string());
    } else if (!in.holster && !inOffhand() && !in.weapon.empty() && in.weapon != name_ && state_ != State::Dropping) {
        const bool busy = timer_ > 0 && state_ != State::Rechamber && !inReloadStates() && state_ != State::ReloadEnd &&
                          (state_ == State::Firing || state_ == State::Melee || state_ == State::MeleeEnd || delay_ > 0);
        if (!busy && lookup && lookup(in.weapon)) startDrop(in.weapon);
    }

    // Melee (0x4def20): +melee pressed (latched until released) with a weapon that has meleeDamage, not while
    // aiming a weapon with an ADS overlay reticle; meleeDelay later the blow lands, meleeTime is the whole swing.
    if (!inOffhand() && !name_.empty()) {
        if (!in.melee) meleeLatch_ = false;
        else if (!meleeLatch_ && time("meleeDamage", 0) != 0 && !delayDone && (delay_ <= 0 || inReloadStates())) {
            meleeLatch_ = true;
            const std::string reticle = fieldValue("adsOverlayReticle");
            const bool canMelee = adsFrac_ <= 0 || reticle.empty() || reticle == "none";
            if (canMelee && state_ != State::Raising && state_ != State::Dropping && state_ != State::Melee &&
                state_ != State::MeleeEnd) {
                pendingIdle_ = false;
                anim_ = fieldValue("meleeAnim").empty() ? fieldValue("idleAnim") : fieldValue("meleeAnim");
                animTime_ = 0;
                events_.push_back({Event::MeleeSwing, name_});
                playFirst({"meleeSwipeSound"}); // event 0xa3 (0x4df021)
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
    // A fresh attack press during a segmented reload round turns state 7 into 8 and 5 into 6 (0x4de3d3): the reload
    // then ends after the current round (once the clip is not empty).
    if (segmented() && inReloadStates() && fireEdge) reloadInterrupted_ = true;

    if (timer_ > 0) return 0;
    switch (state_) {
    case State::Reloading: {
        if (segmented()) { // 0x4de200: another round while rounds can still be loaded and no attack interrupted
            if (addPending_) applyReloadAdd();
            needsRechamber_ = false;
            if (!reloadInterrupted_ && canLoadMore()) beginRound();
            else endReload();
            break;
        }
        if (addPending_) applyReloadAdd();
        needsRechamber_ = false;
        enter(State::Ready, nullptr, 0);
        break;
    }
    case State::ReloadStart: // 0x4de0e0
        if (addPending_) applyReloadAdd();
        if (!(reloadInterrupted_ && clip_ != 0) && canLoadMore()) beginRound();
        else endReload();
        break;
    case State::ReloadEnd:
        enter(State::Ready, nullptr, 0);
        break;
    case State::Firing:
        if (needsRechamber_ && clip_ > 0) {
            enter(State::Rechamber, "rechamberAnim", time("rechamberTime", 1.0f));
            needsRechamber_ = false;
            // 0x4dd851..0x4dd87b: weaponTime = rechamberTime (+0x208), weaponDelay = rechamberBoltTime (+0x20c) when non-zero
            // and shorter, else 1 ms; the casing comes out when that delay ends (0x4dd75e).
            {
                const float rt = time("rechamberTime", 1.0f), bt = time("rechamberBoltTime", 0);
                if (brassInChamber_) brassTimer_ = bt != 0 && bt < rt ? bt : 0.001f;
            }
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

    if (timer_ > 0) return 0; // a reload round or the outro began in the switch above
    if (in.reload && startReload()) return 0;
    // Automatic reload (PM_Weapon 0x4de370, default of the state switch 0x4de429; states ready and rechamber, not while
    // firing): an empty clip with ammo in stock starts the reload (0x4dda60), except prone while moving (0x4de46e).
    if (clip_ == 0 && !in.proneMoving && (state_ == State::Ready || state_ == State::Rechamber) && startReload()) return 0;
    if (in.fire && (automatic || triggerReleased_)) {
        if (clip_ == 0) {
            if (!startReload()) {
                triggerReleased_ = false;
                // No ammo (0x4deb70): a non-grenade weapon sends event 0x92 (the cgame plays player_out_of_ammo, 0x4baec1) and
                // the next action waits 500 ms (0x4dec37).
                if (fieldValue("weaponType") != "grenade") {
                    events_.push_back({Event::OutOfAmmo, name_});
                    // The cgame plays the click only when clipOnly (+0x340) is 0 (0x4baec1).
                    if (std::atoi(fieldValue("clipOnly").c_str()) == 0) sounds_.push_back("player_out_of_ammo");
                    events_.push_back({Event::NoAmmo, std::string()});
                    timer_ = 0.5f;
                }
            }
            return 0;
        }
        --clip_;
        ++shots;
        triggerReleased_ = false;
        needsRechamber_ = fieldValue("boltAction") == "1";
        brassInChamber_ = needsRechamber_; // no brass on the shot itself (0x4b79d1), it waits in the chamber
        brassTimer_ = 0;
        const bool aimed = adsFrac_ == 1.0f;
        enter(State::Firing, clip_ == 0 ? (aimed ? "adsLastShotAnim" : "lastShotAnim") : (aimed ? "adsFireAnim" : "fireAnim"),
              time("fireTime", 0.1f));
    }
    return shots;
}

// 0x4ddf30: whether another round can be loaded. No rounds in stock or a full clip: no. With noPartialReload (+0x358)
// and no reloadAmmoAdd only an empty clip reloads; with reloadAmmoAdd below the clip size the room must hold a whole add.
bool PlayerWeapon::canLoadMore() const {
    const int clipSize = std::max(1, std::atoi(fieldValue("clipSize").c_str()));
    if (stock_ <= 0 || clip_ >= clipSize) return false;
    if (std::atoi(fieldValue("noPartialReload").c_str()) != 0) {
        const int add = std::atoi(fieldValue("reloadAmmoAdd").c_str());
        if (add == 0) return clip_ == 0;
        if (add < clipSize) return clipSize - clip_ >= add;
    }
    return true;
}

bool PlayerWeapon::segmented() const { return std::atoi(fieldValue("segmentedReload").c_str()) != 0; }

// 0x4dda60: begin a reload (events 0x95 and then 0x96 / 0x97 / 0x98). A segmented weapon with a reloadStartTime first plays
// the intro (state 7, reloadStartAnim, event 0x98); otherwise the round starts at once (0x4dd980).
bool PlayerWeapon::startReload() {
    if (!canLoadMore()) return false;
    reloadInterrupted_ = false;
    events_.push_back({Event::ReloadBegin, name_});
    if (segmented() && time("reloadStartTime", 0) != 0) {
        const float t = time("reloadStartTime", 0);
        enter(State::ReloadStart, "reloadStartAnim", t);
        const float a = time("reloadStartAddTime", 0);
        addTimer_ = a == 0 ? 0.0f : std::min(a, t); // 0x4dd8c0 for states 7 / 8
        addPending_ = a != 0;
        scheduleBrass();
        return true;
    }
    beginRound();
    return true;
}

// 0x4dd980 / 0x4dd8c0: one reload round of reloadTime (reloadEmptyTime from an empty clip). The rounds are added reloadAddTime
// into it when that is non-zero and shorter, less the fire delay (or 1 ms when that is not shorter).
void PlayerWeapon::beginRound() {
    // 0x4dd997..0x4dda14: the empty variant (anim 0xc, reloadEmptyTime +0x21c, event 0x97) only for a bullet weapon (weaponType
    // +0x78 == 0) with an empty clip; everything else (also an empty projectile weapon such as the panzerschreck, which has no
    // reloadEmptyAnim) takes anim 0xb, reloadTime (+0x218) and event 0x96.
    const bool empty = clip_ == 0 && fieldValue("weaponType") == "bullet";
    const float t = time(empty ? "reloadEmptyTime" : "reloadTime", 2.0f);
    enter(State::Reloading, empty ? "reloadEmptyAnim" : "reloadAnim", t);
    float d = t;
    const float a = time("reloadAddTime", 0);
    if (a != 0 && a < d) d = a;
    const float fd = time("fireDelay", 0);
    d -= fd < d ? fd : 0.001f;
    addTimer_ = std::max(0.0f, d);
    addPending_ = true;
    scheduleBrass();
}

// 0x4dd930..0x4dd970: a reload state started with the bolt-action chamber bit set takes weaponDelay = min(rechamberBoltTime,
// the add delay, or weaponTime without one), at least 1 ms; the casing is ejected when it ends (0x4ddfa0).
void PlayerWeapon::scheduleBrass() {
    if (!brassInChamber_) return;
    const float limit = addPending_ && addTimer_ > 0 ? addTimer_ : timer_;
    const float bt = time("rechamberBoltTime", 0);
    const float d = std::min(bt, limit);
    brassTimer_ = d > 0 ? d : 0.001f;
}

// 0x4dd5b0: the rounds a segmented reload step adds: the stock or the room, capped by reloadAmmoAdd (reloadStartAdd in the
// intro; the intro adds nothing when that is 0).
void PlayerWeapon::applyReloadAdd() {
    addPending_ = false;
    const int clipSize = std::max(1, std::atoi(fieldValue("clipSize").c_str()));
    const bool intro = state_ == State::ReloadStart;
    const int cap = std::atoi(fieldValue(intro ? "reloadStartAdd" : "reloadAmmoAdd").c_str());
    if (intro && cap == 0) return;
    int n = std::min(stock_, clipSize - clip_);
    if (cap != 0 && cap < clipSize && n > cap) n = cap;
    n = std::max(n, 0);
    clip_ += n;
    stock_ -= n;
}

// The end of a segmented reload: with a reloadEndTime the outro (state 9, reloadEndAnim, event 0x99), else ready.
void PlayerWeapon::endReload() {
    reloadInterrupted_ = false;
    const float t = time("reloadEndTime", 0);
    if (t != 0) enter(State::ReloadEnd, "reloadEndAnim", t);
    else enter(State::Ready, nullptr, 0);
}

} // namespace iw2::game
