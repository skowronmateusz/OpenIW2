// Player movement against world collision (docs/spec/pmove.md): stances with view-height transitions,
// friction, acceleration, command scaling, jump, ground trace, slide and step moves.
#pragma once

#include "game/collision.h"

#include <array>

#include <cstdint>

namespace iw2::assets { struct XAnim; }

namespace iw2::game {

enum class Stance { Stand, Crouch, Prone };

struct PlayerInput {
    float forward = 0, right = 0; // -1..1 (the original's -127..127 command bytes)
    bool jump = false;            // held (usercmd 0x400, also the mantle button)
    Stance stance = Stance::Stand; // requested stance
    bool stanceHeld = false;      // usercmd 0x2000: +prone / +movedown held (CL stance 0x409500); no blocked-rise event then
    bool leanLeft = false, leanRight = false; // +leanleft / +leanright (usercmd bits 0x40 / 0x80)
    float yaw = 0;                // view yaw in degrees
    bool adsHeld = false;         // usercmd 0x1000, or the weapon state forces aim (0x13 / 0x14)
    bool adsAllowed = false;      // 0x4dcf70: the weapon can aim now (PlayerWeapon::adsAllowed)
    int weaponState = 0;          // ps+0xb4
    float pitch = 0;              // view pitch in degrees (with yaw: the command angles for the aim spread)
    float adsFrac = 0;            // ps+0xb8: the aim fraction
    bool dead = false;            // pm_type ≥ 4
    // Lean gates of 0x50ade0 (leanInputAllowed): ps+0xc & 0x8000 (freezecontrols or an absolute playerlinkto, ClientThink 0x4e7bc0) and
    // ps+0x7c (eFlags) & 0x20300 (in a vehicle / turret: 0x20000, on a turret: 0x300).
    bool controlsFrozen = false;
    bool eFlagsNoLean = false;
    bool holdBreathHeld = false;  // usercmd 0x8000 (+holdbreath)
    bool breathWeapon = false;    // the weapon has an adsOverlayReticle (def +0x278) and weaponClass != item (9) (0x4dd350)
};

// Hold-breath dvars (registered at 0x4fa9a2-0x4faa7d, flags 0x1082), defaults from the registration calls.
struct BreathParams {
    float holdTime = 4.5f;   // player_breath_hold_time (0x1c7f030), seconds, 0 disables the system
    float gaspTime = 1.0f;   // player_breath_gasp_time (0x1c7f018)
    float fireDelay = 0.0f;  // player_breath_fire_delay (0x1c7effc)
    float gaspScale = 4.5f;  // player_breath_gasp_scale (0x1c7efc8)
    float holdLerp = 4.0f;   // player_breath_hold_lerp (0x1c7f00c)
    float gaspLerp = 6.0f;   // player_breath_gasp_lerp (0x1c7f020)
};

// Speed factor of the stance (0x508140 after 0x509760, and the same blend in the bob cycle 0x50a9a0..0x50ab62): 1 standing, 0.65
// crouched, 0.15 prone, blended over the view-height transition. ps+0xd0 = viewHeightTarget, +0xd8 = lerp time (start; none = -1 here),
// +0xdc = lerp target, +0xe0 = lerp down (the port keeps one target and the flag in PlayerState). While a lerp towards prone runs
// (0x509760 with target 0xb: fraction f = clamp((time - start) / 400) when the lerp target is prone) a nonzero f gives
// (1 - f) * 0.65 + f * 0.15. Otherwise, a lerp towards crouch that is not a downward one (rising from prone, 400 ms) with
// t = (time - start) * 0.0025 clamped above at 1 gives (1 - t) * 0.15 + t * 0.65 for t > 0. Anything else (including f or t exactly 0)
// is the flat factor of the target: crouch 0.65, prone 0.15, other 1.
inline float stanceSpeedFactor(int viewHeightTarget, int64_t timeMs, int64_t lerpStartMs, bool lerpDown) {
    const bool lerping = lerpStartMs >= 0;
    if (lerping && viewHeightTarget == 11) {
        float f = float(timeMs - lerpStartMs) / 400.0f;
        f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
        if (f != 0.0f) return (1.0f - f) * 0.65f + f * 0.15f;
    }
    if (lerping && viewHeightTarget == 40 && !lerpDown) {
        float t = float(timeMs - lerpStartMs) * 0.0025f;
        if (t > 1.0f) t = 1.0f;
        if (t > 0.0f) return (1.0f - t) * 0.15f + t * 0.65f;
    }
    return viewHeightTarget == 40 ? 0.65f : viewHeightTarget == 11 ? 0.15f : 1.0f;
}

// Speed factor of the movement types noclip (ps+4 == 2: x3) and ufo (3: x6) in the command scale (0x5080a0, 0x508140: constants
// 0x591f8c = 3.0 and 0x56fcc8 = 6.0); applied after the aim / lean factor 0.4, and in the walking scale it replaces the stance, weapon
// and shell shock factors (the function returns right after it). 1 for every other type.
inline float pmTypeSpeedFactor(int pmType) { return pmType == 2 ? 3.0f : pmType == 3 ? 6.0f : 1.0f; }

// Whether the lean buttons count this step (0x50adf1..0x50ae37): not with pm_flags 0x8000 (controlsFrozen), not with pm_type >= 4, not
// airborne (groundEntityNum ps+0x60 == 0x7ff) unless pm_type == 1 (linked), and not with eFlags & 0x20300.
inline bool leanInputAllowed(int pmType, bool controlsFrozen, bool onGround, bool eFlagsNoLean) {
    return !controlsFrozen && pmType < 4 && (onGround || pmType == 1) && !eFlagsNoLean;
}

struct PlayerState {
    int pmType = 0;              // ps+4: 0 normal, 1 linked, 2 noclip, 3 ufo, >= 4 spectator / dead (the port also has PlayerInput::dead)
    Vec origin{};
    Vec velocity{};
    bool onGround = false;       // groundEntityNum (ps+0x60) != ENTITYNUM_NONE
    bool onLadder = false;       // pm_flags 0x20: climbing a ladder (PM_CheckLadderMove 0x50bd90)
    bool ladderLeft = false;     // pm_flags 0x40000: set when the ladder was just left, cleared on the ground
    Vec vLadder{};               // ps+0x64: the normal of the ladder surface
    bool jumping = false;        // pm_flags 0x80000: set by a jump, cleared by the landing timer
    float jumpOriginZ = 0;       // ps+0x74: origin z at the jump
    int pmTimeMs = 0;            // pm_time (ps+0x10)
    Stance stance = Stance::Stand;
    int viewHeightTarget = 60;   // 60 stand, 40 crouch, 11 prone
    float viewHeight = 60;       // current eye height above the origin
    int64_t timeMs = 0;
    int64_t transitionStartMs = -1;
    bool transitionDown = false;
    int64_t lastJumpMs = -100000;
    bool jumpHeld = false;
    float lean = 0;              // leanf: −0.5..0.5 (−0.25..0.25 prone), negative = left
    bool ads = false;            // pm_flags 0x40: aiming down the sights (0x4dd020)
    bool adsSlow = false;        // pm_flags 0x100: aiming slows movement (0x50bd00)
    bool prevAdsHeld = false;    // the previous command's 0x1000 (pm+0x38)
    // Mantle (ps+0x598..0x5a4, pm_flags 4). Flags: 1 over, 2 crouch at the start, 4 stand at the end, 8 hint.
    bool mantling = false;
    float mantleYaw = 0;
    int mantleTimeMs = 0;
    int mantleTrans = 0;
    uint32_t mantleFlags = 0;
    int stanceEvent = -1;        // client stance set by a mantle event this move (0x8d → 1, 0x8c → 0), else −1
    int64_t lastFoliageMs = 0;   // ps+0x44: time of the last foliage event, 0 after the reset interval at low speed
    int landSeq = 0;             // landings so far (PM_CrashLand 0x508fe0 reached the ground from the air)
    float landHeight = 0;        // the last landing's fall height
    int landDamage = 0;          // the last landing's fall damage 0..100 (applied to health by World::applyPlayerEvent)
    bool movingBack = false;     // pm_flags 0x80: set by a backward command, cleared by forward (or sideways-only); raises the standing bob rate
    bool spawnSlide = false;     // pm_flags 0x400: set by ClientSpawn (0x4e925b) with pm_time 100; walk accel 1 and gravity on the ground
    bool landTimer = false;      // pm_flags 0x200: the landing slowdown, cleared with pm_time
    // The player-state event ring (ps+0x80 sequence, +0x84 events, +0x94 parameters; 4 slots, PM_AddEvent 0x507ba0 /
    // 0x4fb3b0): footsteps, jumps and landings the cgame turns into sounds (CG_EntityEvent 0x4ba8d0) and the game into
    // fall damage (ClientEvents 0x4e75f0).
    int eventSeq = 0;
    int events[4]{};
    int eventParms[4]{};
    float aimSpread = 255;       // ps+0x5b0: aim spread scale 0..255 (0x4de540)
    int bobCycle = 0;            // ps+8: 0..255 (PM_Footsteps 0x50a850)
    float prevCmdPitch = 0, prevCmdYaw = 0; // the previous command's angles (pm+0x40)
    float prevCmdFwd = 0, prevCmdRight = 0; // the previous command's move bytes (pm+0x4c / 0x4d), -1..1
    bool proneAimLatch = false;  // pm_flags 0x800: set with the prone aim (0x4dd070), zeroes the moves while prone (0x50c748)
    // Hold breath (0x4dd350, called from PM_Weapon every step after the aim update).
    bool holdingBreath = false;  // pm_flags 0x4000
    int breathTimerMs = 0;       // ps+0x594: counts up while holding, down otherwise
    float breathScale = 0;       // ps+0x590: the scope sway scale (not initialised by the spawn in the exe)
    bool havePrevCmd = false;
};

// Appends an event (PM_AddEvent 0x4fb3b0: nothing for event 0).
void addPlayerEvent(PlayerState& ps, int event, int parm = 0);

// The result of PM_CrashLand (0x508fe0) for a fall height h: the damage 0..100, the view dip, the new pm_time and
// movement scale, and the event added. `surfaceFlags` are the ground trace's.
struct LandingOutcome {
    int damage = 0;
    int dip = 0;            // the view dip parameter of a hard landing (0x509107), 0..24
    int pmTimeMs = 0;       // pm_time set by a damaging landing (0x50914b), 0 = unchanged
    float velScale = 1.0f;  // the velocity is multiplied by this (0x5091a7 / 0x5091ca / 0x5092a8)
    int event = 0;          // 0 = none
    int eventParm = 0;
    bool landTimer = false; // pm_flags 0x200 set
};
LandingOutcome landingOutcome(float h, uint32_t surfaceFlags, bool dead);

// Hip spread fields of the current weapon (weapon def +0x28c … +0x2b8).
struct HipSpread {
    float standMin = 0, duckedMin = 0, proneMin = 0, max = 0, duckedMax = 0, proneMax = 0;
    float decayRate = 0, fireAdd = 0, turnAdd = 0, moveAdd = 0, duckedDecay = 0, proneDecay = 0;
};
// The spread angle range for the current view height (0x4dceb0): between ducked (40) and stand (60) above 40,
// between prone (11) and ducked below. Returns {min, max} in degrees.
std::array<float, 2> hipSpreadRange(const HipSpread& h, float viewHeight);
// The spread angle FireWeapon uses (0x531c40): scale = aimSpread / 255; fully aimed (aim == 1.0) it is
// (hipMax - adsSpread) * scale + adsSpread (adsSpread = weapon def +0x4bc), otherwise range min + (max - min) * scale.
float fireSpreadDegrees(const HipSpread& h, float viewHeight, float aimSpread, float adsFrac, float adsSpread);
// A shot adds hipSpreadFireAdd · 255 to the aim spread scale unless fully aimed (0x4dece0).
void addFireSpread(PlayerState& ps, const HipSpread& h, float adsFrac);

// The mantle animations of the PLAYER_MANTLE tree (0x525360, names at 0x5a950c): index 0 player_mantle_root,
// 1–7 player_mantle_up_57 … up_21, 8–10 player_mantle_over_high, over_mid, over_low. Only their delta
// translation is used.
struct MantleAnims {
    std::array<const assets::XAnim*, 11> anims{};
    bool loaded() const;
};
// player_mantle_* animation names by tree index.
const char* mantleAnimName(int index);
// PM_Weapon's weapon change (0x4de880) puts the weapon away while mantling (mantle_enable and pm_flags 4) unless the
// mantle's over animation is player_mantle_over_low (tree index 10, table 0x5a94bc).
bool mantleLowersWeapon(const PlayerState& ps);
// Mantle view yaw cap (0x525b60): while mantling the view yaw stays within mantle yaw ± mantle_view_yawcap.
float mantleClampYaw(const PlayerState& ps, float yaw);

// View offset of a lean (0x440380 with roll factor 16 and distance 20): g = (2 − |lean|)·lean, roll = 16·g
// degrees; offset = 20·g·(cos(roll)·right − sin(roll)·up) for yaw in degrees.
Vec leanOffset(float yawDeg, float lean);

// Movement settings: dvar defaults and constants read from CoD2SP_s.exe (docs/spec/pmove.md).
struct MoveParams {
    int speed = 190;                  // g_speed
    float gravity = 800.0f;           // g_gravity
    float friction = 5.5f;            // friction
    float stopSpeed = 100.0f;         // stopspeed
    float jumpHeight = 39.0f;         // jump_height
    float backSpeedScale = 0.7f;      // player_backSpeedScale
    float strafeSpeedScale = 0.8f;    // player_strafeSpeedScale
    float weaponSpeedScale = 0.0f;    // current weapon's moveSpeedScale (0 = none)
    bool shellShockSlow = false;      // usercmd button 0x800: set by CL_CmdButtons (0x409c82) while the cgame shell shock runs (0x621560, 0x4b24db)
    float jumpStepSize = 18.0f;       // jump_stepSize
    uint32_t traceMask = Contents::PlayerClipMask; // pm->tracemask: the player's clip mask (0x4e90e2)
    int passEntity = -1;              // the player's entity number, skipped by the traces
    Vec mins{-15, -15, 0};
    Vec maxs{15, 15, 70};             // standing; crouch 50, prone 30
    const MantleAnims* mantle = nullptr; // no mantling without them
    HipSpread hipSpread;              // the current weapon's
    float aimSpreadMoveSpeedThreshold = 11.0f; // bg_aimSpreadMoveSpeedThreshold (0x4fa98e)
    float jumpSpreadAdd = 64.0f;      // jump_spreadAdd (0x541240)
    float moveThreshold = 10.0f;      // player_moveThreshhold (0x4fab25)
    // PM_FoliageSounds dvars (registered at 0x4fa6fd..0x4fa813)
    float foliageMinSpeed = 40.0f;    // bg_foliagesnd_minspeed
    float foliageMaxSpeed = 180.0f;   // bg_foliagesnd_maxspeed
    int foliageSlowInterval = 1500;   // bg_foliagesnd_slowinterval (ms)
    int foliageFastInterval = 500;    // bg_foliagesnd_fastinterval (ms)
    int foliageResetInterval = 500;   // bg_foliagesnd_resetinterval (ms)
    BreathParams breath;
};

// A shot while holding the breath on a scoped weapon (0x4dd2b0, after the fire event at 0x4dee18): the timer gains
// player_breath_fire_delay (capped at the hold time) and the hold ends. Only with the aim fraction 1 and a reticle weapon.
void breathOnFire(PlayerState& ps, const MoveParams& mp, float adsFrac, bool breathWeapon);

// PM_AddEvent (0x507ba0): event 0 is ignored; the ring keeps the last 4.
// ClientSpawn (0x4e9255..0x4e9263): pm_flags |= 0x400, pm_time = 100.
inline void markSpawned(PlayerState& ps) {
    ps.spawnSlide = true;
    ps.pmTimeMs = 100;
}

inline void addPmEvent(PlayerState& ps, int ev) {
    addPlayerEvent(ps, ev, 0);
}

// Advances the player by dt seconds (split into steps of at most 66 ms like the original).
void playerMove(const CollisionWorld& world, PlayerState& ps, const PlayerInput& in, float dt, const MoveParams& mp = {});

} // namespace iw2::game
