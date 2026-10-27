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
    bool leanLeft = false, leanRight = false; // +leanleft / +leanright (usercmd bits 0x40 / 0x80)
    float yaw = 0;                // view yaw in degrees
    bool adsHeld = false;         // usercmd 0x1000, or the weapon state forces aim (0x13 / 0x14)
    bool adsAllowed = false;      // 0x4dcf70: the weapon can aim now (PlayerWeapon::adsAllowed)
    int weaponState = 0;          // ps+0xb4
    float pitch = 0;              // view pitch in degrees (with yaw: the command angles for the aim spread)
    float adsFrac = 0;            // ps+0xb8: the aim fraction
    bool dead = false;            // pm_type ≥ 4
};

struct PlayerState {
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
    int landSeq = 0;             // landings so far (PM_CrashLand 0x508fe0 reached the ground from the air)
    float landHeight = 0;        // the last landing's fall height
    int landDamage = 0;          // the last landing's fall damage 0..100 (applied to health by World::applyPlayerEvent)
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
};

// PM_AddEvent (0x507ba0): event 0 is ignored; the ring keeps the last 4.
inline void addPmEvent(PlayerState& ps, int ev) {
    addPlayerEvent(ps, ev, 0);
}

// Advances the player by dt seconds (split into steps of at most 66 ms like the original).
void playerMove(const CollisionWorld& world, PlayerState& ps, const PlayerInput& in, float dt, const MoveParams& mp = {});

} // namespace iw2::game
