// Player weapon state: firing cadence, rechambering, reloading, weapon changes, melee and offhand grenades, and the
// view-model animation to show (docs/spec/weapons.md). Timings come from the weapons/sp/<name> file. Weapon
// changes, melee and the offhand throw follow PM_Weapon (0x4dfc30); firing and reloading are still inferred.
#pragma once

#include <array>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace iw2::game {

class PlayerWeapon {
public:
    using Def = std::map<std::string, std::string>;

    struct Input {
        bool fire = false;   // trigger held
        bool reload = false; // reload pressed this tick
        bool ads = false;    // aim-down-sights button held
        bool melee = false;  // +melee held (usercmd 4)
        bool frag = false;   // +frag held (0x10000)
        bool smoke = false;  // +smoke held (0x20000)
        std::string weapon;  // the command's weapon (cg.weaponSelect); empty = keep the current one
        bool proneMoving = false; // prone with a forward/right command (0x4de46e: no automatic reload then)
        bool holster = false; // 0x4de880: mantling (mantleLowersWeapon), on a ladder or in a vehicle: weapon 0
        bool binoculars = false;  // usercmd 0x4000 (+binoculars / +breath_binoculars)
        bool use = false;         // usercmd 0x28 (+activate 0x8 | +usereload 0x20): the binocular "fire"
        bool inVehicle = false;   // ps eFlags 0x20000: the binocular cancel mask lacks attack (0x30014 instead of 0x30015)
        bool toggleBinoculars = true; // player_toggleBinoculars (0x1c7f004, registered 0x4faaac, default 1)
        float adsExitDelay = 0;   // player_adsExitDelay (0x1c7efb8, int ms 0..1000, default 0) in seconds
        bool dead = false;        // pm_type >= 4 (health <= 0): PM_Weapon (0x4dfc30) only clears ps weapon (+0xb0) and returns
        bool damaged = false;     // ps+0x104 (damageCount of P_DamageFeedback 0x4e7040) != 0: the player was hurt this frame
        bool scopeExitOnDamage = false; // player_scopeExitOnDamage (0x1c7eff0, registered 0x4faacc, default 0, flags 0x1082)
    };

    // Things the game side acts on (player-state events of PM_Weapon).
    struct Event {
        enum Kind {
            MeleeSwing,      // 0xa3
            MeleeHit,        // 0xa4: the game traces and damages (0x531da0)
            OffhandThrow,    // 0xa6: the game spawns the grenade (0x531d60)
            OffhandExplode,  // 200: a cooked grenade went off in the hand
            NoOffhand,       // 0x94 + 0xc9 / 0xca: no grenade of that class with ammo
            ReloadBegin,     // 0x95 (0x4dda60): the cgame clears the toggled aim (0x4bafbc, flag 0x621530 = toggleads 0x409260)
            NoAmmo,          // 0x92: fired an empty non-grenade weapon (the cgame shows the WEAPON_NO_AMMO hint)
            OutOfAmmo,       // 0x92 (0x4baec1): the cgame auto-switches weapon (0x4b7090)
            WeaponRaised,    // the player state's weapon changed (end of the drop)
            BinocularEnter,  // 0xa8: script notify "binocular_enter" on the player (ClientEvents 0x4e76e0)
            BinocularExit,   // 0xa9: "binocular_exit" (0x4e76fd)
            BinocularFire,   // 0xaa: "binocular_fire" (0x4e7706)
            BinocularRelease,// 0xab: "binocular_release" (0x4e7723)
            BinocularDrop,   // 0xac: "binocular_drop" (0x4e7740)
            EjectBrass,      // 0xa2: the cgame ejects the brass (0x4bb113 -> CG_EjectWeaponBrass 0x4b7730, shellEjectEffect)
        } kind;
        std::string weapon;
        bool alt = false; // WeaponRaised: the new weapon is the old one's altWeapon (aim spread kept at >= 128, 0x4dde9f)
    };

    // Weapon definitions for weapon changes and offhands (World::weapon).
    std::function<const Def*(const std::string&)> lookup;
    // Length in seconds of a view animation; <= 0 for looping or unknown ones (set by the viewer from the XAnim).
    std::function<float(const std::string&)> animLength;
    // 0x4dc6d0: the first owned weapon of an offhand class ("Frag Grenade", "Smoke Grenade") with ammo, or "".
    std::function<std::string(const std::string& offhandClass)> findOffhand;
    // The player state's ammunition (ps+0x318 clip by clipName index, ps+0x118 stock by ammoName index; the world inventory):
    // read before each update and on equip, written back after; nullopt (weapon not owned there) keeps the local counters.
    // Without these callbacks the local per-weapon-name memory is used (tests).
    std::function<std::optional<std::pair<int, int>>(const std::string& weapon)> readAmmo; // {clip, stock}
    std::function<void(const std::string& weapon, int clip, int stock)> writeAmmo;
    // 0x4dc660: the first registered weapon whose weaponType is binoculars (def +0x78 == 3), nullptr when none.
    std::function<const Def*()> binocularDef;

    // `def` is the parsed weapon file (World::weapon). Starts raising the weapon.
    void equip(const std::string& name, const Def& def);

    // Advances by dt seconds; returns the number of shots fired during this step.
    int update(float dt, const Input& in);
    int updateImpl(float dt, const Input& in);

    const std::string& name() const { return name_; }
    bool equipped() const { return !name_.empty(); }
    // Put away by a holster (weapon 0) and waiting to raise the command's weapon again.
    bool holstered() const { return holstered_; }
    // The held weapon was taken away (swap with a weapon on the ground, 0x4dc8a0 sets ps weapon 0 at once): nothing is in hand, so
    // the next weapon is raised without a putaway (PM_BeginWeaponChange with weapon 0 → state 2 with timer 0).
    void weaponTaken() { name_.clear(); holstered_ = true; }
    int clip() const { return clip_; }
    int stock() const { return stock_; }
    // PM_Weapon's weapon state number (0 ready, 1 raising, 2 dropping, 10/11 melee, 12–16 offhand, …).
    int stateNumber() const { return int(state_); }
    // The offhand fuse (ps+0x3c) in ms: non-zero while a grenade is being cooked.
    int fuseMs() const { return int(fuse_ * 1000.0f); }
    // The throw consumes ps+0x3c (0x527625 stores 0 after the grenade takes it as its fuse); returns the cooked fuse in ms.
    bool hasBrassPending() const { return brassInChamber_; } // ps+0x530 bit of the equipped weapon
    int takeFuseMs() { const int ms = fuseMs(); fuse_ = 0; return ms; }

    // View model: xmodel paths for the gun and hands and the current animation with its elapsed time. During an
    // offhand throw these are the offhand weapon's (ps+0xac while flag 0x10 is set).
    std::string gunModel() const;
    std::string handModel() const;
    const std::string& anim() const { return anim_; }
    float animTime() const { return animTime_; }
    // Sound aliases started by state changes since the last call (reload, rechamber, raise, putaway, melee swipe, empty click).
    std::vector<std::string> takeSounds() { return std::exchange(sounds_, {}); }
    std::vector<Event> takeEvents() { return std::exchange(events_, {}); }
    // Aim fraction 0..1 and whether it is rising (docs/spec/weapons.md, 0x4dd0d0).
    float adsFrac() const { return adsFrac_; }
    bool adsZoomingIn() const { return zoomingIn_; }
    // Which aim layer the view model wants (0x4b2918-0x4b2969): the aim flag (pm_flags 0x40), not while reloading with
    // more than adsReloadTransTime left (state 5, 0x4b292d-0x4b293a), never during the offhand states (flag 0x10).
    bool viewAiming() const;
    // Fields of the weapon the client shows: the offhand's while flag 0x10 is set (ps+0xac), else the player
    // state's weapon (ps+0xb0), as cgame selects it (e.g. 0x4b6420, 0x4bc8a0). This feeds the view model's aim
    // animations, so a grenade throw is not blended with the hand weapon's adsDownAnim.
    // With pm_flags 0x20000 (binoculars up) the client uses the binocular weapon (0x4dc660; e.g. 0x4bc8a0, 0x4ae210, 0x4bd760).
    float number(const char* key) const {
        if (binocActive_) return float(std::atof(defValue(binocDef_, key).c_str()));
        return offhandActive_ ? float(std::atof(defValue(offDef_, key).c_str())) : time(key, 0.0f);
    }
    std::string field(const char* key) const {
        if (binocActive_) return defValue(binocDef_, key);
        return offhandActive_ ? defValue(offDef_, key) : fieldValue(key);
    }
    // The same without the binocular switch: the HUD ammo / name ownerdraws read the player state's weapon (0xf70998,
    // 0x4a7855) even while the binoculars are up.
    float hudNumber(const char* key) const {
        return offhandActive_ ? float(std::atof(defValue(offDef_, key).c_str())) : time(key, 0.0f);
    }
    std::string hudField(const char* key) const { return offhandActive_ ? defValue(offDef_, key) : fieldValue(key); }
    // pm_flags 0x20000: set when the binoculars come up (0x4df7f0 / 0x4df640), cleared when they go down (0x4df680, 0x4df6c0,
    // states 0x15 / 0x12 exit).
    bool binocularsUp() const { return binocActive_; }
    // 0x4dcf70 for a player on foot: the weapon has aimDownSight (def +0x32c) and the state is not raising,
    // dropping, melee or offhand (1, 2, 10, 11, 12–16); in the binocular states 0x11–0x16 only 0x13 / 0x14 (0x4dcf50), with the
    // binocular weapon's aimDownSight while pm_flags 0x20000 is set.
    bool adsAllowed() const {
        const int s = int(state_);
        if (s >= 0x11 && s <= 0x16) return (s == 0x13 || s == 0x14) && binocActive_ && defNumber(binocDef_, "aimDownSight") != 0;
        return equipped() && time("aimDownSight", 0) != 0 && s != 1 && s != 2 && s != 10 && s != 11 && !(s >= 12 && s <= 16);
    }
    bool animLoops() const { return state_ == State::Ready; }

private:
    enum class State {
        Ready = 0, Raising = 1, Dropping = 2, Firing = 3, Rechamber = 4, Reloading = 5, ReloadStart = 7, ReloadEnd = 9, Melee = 10, MeleeEnd = 11,
        OffhandInit = 12, OffhandPrepare = 13, OffhandHold = 14, OffhandThrow = 15, OffhandEnd = 16,
        // Binoculars (0x4df9b0 and PM_Weapon's switch): 0x11 waits for the aim to go out, 0x12 quick-drops the hand weapon,
        // 0x13 raises the binoculars (aim in), 0x14 up, 0x15 lowering (aim out), 0x16 quick-raises the hand weapon.
        BinocAdsOut = 0x11, BinocDrop = 0x12, BinocEnter = 0x13, BinocUp = 0x14, BinocExit = 0x15, BinocRaise = 0x16
    };
    bool inBinoc() const { return int(state_) >= 0x11 && int(state_) <= 0x16; }
    // Returns true when PM_Weapon returns after the binocular step (0x4df9b0 and the 0x11-0x16 cases of the switch).
    void binocularStep(const Input& in, bool delayDone);
    void binocularEnter();   // 0x4df7f0
    void binocularCancel(bool attackHeld); // 0x4df0a0 (+ 0x4df6c0) from a binocular state
    std::string fieldValue(const char* key) const;
    void playFirst(std::initializer_list<const char*> keys);
    float time(const char* key, float fallback) const;
    static std::string defValue(const Def& d, const char* key);
    static float defNumber(const Def& d, const char* key);
    void enter(State s, const char* animKey, float duration);
    void enterWith(const Def& d, State s, const char* animKey, float duration);
    void startDrop(const std::string& next);
    void finishDrop();
    void meleeHit();
    // Segmented (shotgun-style) reload, PM_Weapon 0x4dda60 / 0x4dd980 / 0x4dd5b0 / 0x4ddf30.
    bool segmented() const;
    bool canLoadMore() const;
    bool startReload();
    void beginRound();
    void scheduleBrass();
    void endReload();
    void applyReloadAdd();
    bool inReloadStates() const { return state_ == State::Reloading || state_ == State::ReloadStart; }
    bool inOffhand() const { return int(state_) >= 12 && int(state_) <= 16; }

    std::string name_;
    Def def_;
    State state_ = State::Ready;
    float timer_ = 0;  // ps weaponTime (+0x34)
    float delay_ = 0;  // ps weaponDelay (+0x38)
    std::string anim_;
    float animTime_ = 0;
    bool pendingIdle_ = false; // cgame 0x4b299d: idle waits until the running action anim has finished
    int clip_ = 0, stock_ = 0;
    std::map<std::string, std::pair<int, int>> ammo_; // clip and stock of weapons put away
    bool triggerReleased_ = true;
    bool firePrev_ = false;
    bool reloadInterrupted_ = false; // states 6 / 8: a fresh attack press during a segmented reload (0x4de3d3)
    float addTimer_ = 0;             // time until the reload adds its rounds (ps weaponDelay in 7/8 and 5/6)
    bool addPending_ = false;
    bool needsRechamber_ = false;
    // boltAction casing still in the chamber (ps+0x530 bit of the weapon, set by a shot): ejected (event 0xa2) when the
    // weapon delay set at the rechamber / reload start runs out (0x4dd700 / 0x4ddfa0). The exe uses ps weaponDelay (+0x38).
    bool brassInChamber_ = false;
    std::map<std::string, bool> brassBits_; // the ps+0x530 bit of the weapons not currently equipped
    float brassTimer_ = 0;
    bool meleeLatch_ = false;      // ps+0xc 0x2000
    bool offhandActive_ = false;   // ps+0xc 0x10
    bool offhandHeldPrev_ = false; // previous command's 0x30000
    std::string next_;             // weapon the drop leads to
    bool holstered_ = false;
    std::string offhand_;          // ps+0xac
    Def offDef_;
    float fuse_ = 0;               // ps+0x3c, seconds
    std::vector<std::string> sounds_;
    std::vector<Event> events_;
    bool binocActive_ = false;     // pm_flags 0x20000
    Def binocDef_;
    bool binocPrev_ = false;       // previous command's 0x4000
    bool usePrev_ = false;         // previous command's 0x28
    std::string prevCmdWeapon_;    // previous command's weapon (the 0x4df9b0 cancel test)
    float adsFrac_ = 0;
    bool deferredFire_ = false; // adsFire: state 3 waiting for the aim before the shot
    float adsExitTimer_ = 0; // player_adsExitDelay hold, seconds left
    bool adsHeld_ = false; // the last update's aim command
    bool zoomingIn_ = true;
};

} // namespace iw2::game
