// Shared movement and snapshot helpers of the network game (docs/spec/net.md section 10): the server runs user
// commands with runUsercmd, the client predicts its own player with the same code and interpolates the others.
#pragma once

#include "game/playerweapon.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "game/pmove.h"
#include "net/delta.h"
#include "net/gameclient.h"

namespace iw2::game {
class CollisionWorld;
}

namespace iw2::netgame {

// User command -> movement input (moves, view angles, jump, stance, lean, aim and hold-breath buttons; docs/spec/net.md
// section 6 for the bits).
game::PlayerInput inputFromUsercmd(const net::UserCmd& cmd);
// What the movement needs from the weapon side (each side's own PlayerWeapon: the server's for its clients, the
// client's predicted one): whether it can aim now, the aim fraction, the weapon state, and the weapon's move parameters.
struct WeaponMove {
    bool adsAllowed = false;
    float adsFrac = 0;
    int weaponState = 0;
    // Hold breath applies to this weapon: it has an aim overlay reticle and is not of class item (MP 0x4f2aa0 tests
    // weapon def +0x278 and +0x7c != 9, as the SP 0x4dd350 does).
    bool breathWeapon = false;
    game::MoveParams params;
};
// The time a command runs (docs/spec/net.md section 10 "Command steps"): its time minus the previous command's in ms,
// 50 for the first command (prevTime 0), at most 1000; 0 when it is not newer (the command is not run at all).
int usercmdMsec(int prevTime, int time);
// The same in seconds.
float usercmdStep(int prevTime, int time);
// The pieces a command's time is run in: at most 66 ms each (0x80e9464).
constexpr int kUsercmdPieceMs = 66;
template <class F> void forEachUsercmdPiece(int msec, F&& run) {
    while (msec > 0) {
        const int piece = msec < kUsercmdPieceMs ? msec : kUsercmdPieceMs;
        run(static_cast<float>(piece) * 0.001f);
        msec -= piece;
    }
}
// Runs one command on ps. passEntity: the moving client's own entity, which its traces skip (pm->clientNum as the
// pass entity of the player move): the client's body clip is linked where it stood, and a move that starts elsewhere
// (the prediction from an older acknowledged origin) would otherwise run into it.
void runUsercmd(const game::CollisionWorld& world, game::PlayerState& ps, const net::UserCmd& cmd, int prevTime,
                const WeaponMove* weapon = nullptr, int passEntity = -1, bool controlsFrozen = false);

// The player-state fields the network game uses (byte offsets of docs/spec/net.md section 7.3).
struct PsFields {
    int commandTime = 0;  // serverTime of the last command the server ran
    int pmType = 0;
    int bobCycle = 0;     // ps+8, 0..255 (the walk cycle of the gun and view bob)
    int clientNum = 0;
    float origin[3] = {0, 0, 0};
    float velocity[3] = {0, 0, 0};
    float viewangles[3] = {0, 0, 0};
    int pmFlags = 0;         // ps+12 (0x8000: freezecontrols)
    int eFlags = 0;          // ps+160 (0x300 bits: on a map turret)
    int viewLockMode = 0;    // ps+1424: 1 on a turret, 2 in a frame the turret fired
    int viewLockEntity = 1023; // ps+1428: the turret's entity number, 1023 none
    int weaponTime = 0;      // ps+52: the time left in the weapon state, ms
    int weaponState = 0;     // ps+216
    int weapon = 0;          // ps+212: the weapon number of the weapon list
    int eventSequence = 0;   // ps+164, 8 bits
    int events[4] = {0, 0, 0, 0};  // ps+168
    int deltaAngles[3] = {0, 0, 0}; // ps+84: added to the command's angles, 16-bit angles (docs/spec/net.md section 10)
};
// SetClientViewAngle's delta (0x80f85d8 in the 1.3 Linux dedicated server): the 16-bit angle of the wanted view angle,
// trunc(angle * 65536 / 360) & 0xffff, minus the angle of the client's last command.
int viewAngleDelta(float angle, int32_t cmdAngle);
// The command's angles with the player state's delta_angles added (PM_UpdateViewAngles), kept to 16 bits.
net::UserCmd withDeltaAngles(const net::UserCmd& cmd, const int (&delta)[3]);
// A command whose angles hold the client's view (the drawn view = command + delta_angles): the angles to send, the
// view minus the delta of the player state `ps` (empty or short: unchanged).
net::UserCmd viewCommand(const net::UserCmd& view, const std::vector<uint8_t>& ps);
PsFields readPs(const std::vector<uint8_t>& ps);
// The user command a network client sends (iw2view's live play, also on a turret): buttons, the move fractions -1..1,
// the view angles in degrees as 16-bit angles and the weapon number of the weapon list configstring (0 none).
net::UserCmd makeUserCmd(int serverTime, uint32_t buttons, float forward, float right, float pitch, float yaw, int weapon);
void writePs(std::vector<uint8_t>& ps, const PsFields& f);

// Prediction: start from the server's player state (origin, velocity, pm_type) and run every command newer than its
// commandTime. carry: the previous prediction, whose other movement state (stance, view height, timers) is kept.
// The bob cycle also starts from the server's (ps+8): carried over, it would advance again by every replayed command
// on each frame. On a map turret (eFlags & 0x300) the server holds the player behind the gun: no command moves it.
game::PlayerState predictPlayer(const game::CollisionWorld& world, const PsFields& server,
                                const std::vector<net::UserCmd>& commands, const game::PlayerState* carry = nullptr,
                                const WeaponMove* weapon = nullptr, int passEntity = -1);

// Intermission (pm_type 5): the view input is ignored and the view keeps the angles the server set (the spawn's
// mp_global_intermission angles), as for the dead pm_types (the command angles are not applied, SP 0x50b12b).
// TODO(spec): the matching check in the patch 1.3 MP executable.
inline bool viewInputIgnored(int pmType) { return pmType == 5; }
// The client's prediction of its own player (docs/spec/net.md section 10, "Prediction and interpolation"). Every frame starts again from
// the player state of the newest snapshot and runs the commands the server has not run yet, so the same commands give
// the same states (and the same event sequence numbers) on every frame. The wire player state carries only part of the
// movement state; the rest (ground, jump flag and origin, pm_time, stance, timers, the event ring) is taken from this
// client's own result for the newest command at or before the snapshot's commandTime, kept per command time.
class Predictor {
public:
    // decayMs: cg_errorDecay. Default 100 (docs/spec/net.md).
    float decayMs = 100.0f;
    // Runs the prediction for this frame; nowMs is the client's time for the error decay.
    const game::PlayerState& predict(const game::CollisionWorld& world, const PsFields& server,
                                     const std::vector<net::UserCmd>& commands, const WeaponMove* weapon, int64_t nowMs,
                                     int passEntity = -1);
    const game::PlayerState& state() const { return ps_; }
    // The origin to draw: the prediction plus the remaining part of the last correction.
    game::Vec drawOrigin(int64_t nowMs) const;
    // The correction of the last frame whose prediction changed (0 when none), for tests and IW2_NET_TRACE.
    float lastMiss() const { return lastMiss_; }
    void reset();

private:
    std::map<int, game::PlayerState> history_; // the state after the command with that serverTime
    game::PlayerState ps_{};
    int psTime_ = 0;  // serverTime of the last command in ps_ (0 none)
    bool have_ = false;
    float error_[3] = {0, 0, 0};
    int64_t errorTime_ = 0;
    float lastMiss_ = 0;
};

// The client's predicted ammunition (docs/spec/net.md section 10, "Prediction and interpolation"): the original's
// prediction starts from a copy of the snapshot's whole player state and replays the unacknowledged commands, so the
// counters the HUD draws are the server's with those commands' weapon step (shots, reloads) applied. openiw2's weapon
// simulation keeps the counters in the view world's inventory instead of the player state and runs once per frame, so
// what each command's weapon step changed is kept per command time; a snapshot's counters get the changes of the
// commands it has not seen. Only the weapon step's own changes are kept: a snapshot's correction is not one of them.
class PredictedAmmo {
public:
    struct Counters {
        std::map<std::string, int> clip, stock;  // clip by clipName, stock by ammoName
    };
    // The counters before and after the weapon step of the command with that time.
    void record(int commandTime, const Counters& before, const Counters& after);
    // Forgets the commands up to commandTime (the snapshot has seen them) and adds the newer ones' changes to the
    // snapshot's counters, in place. A counter the snapshot does not carry stays absent.
    void adjust(int commandTime, Counters& counters);
    // The counters of the last adjust() as the server sent them, and a restart of the kept changes (a rebuilt weapon
    // records its commands again on top of those counters).
    const Counters& server() const { return server_; }
    void clearPending() { pending_.clear(); }
    void clear();

private:
    Counters server_;
    std::map<int, Counters> pending_;  // what the weapon step changed, by command time
};

// Another entity at the render time, interpolated between two snapshots.
struct RemoteEntity {
    int number = 0;
    int eType = 0;
    int clientNum = 0;
    float origin[3] = {0, 0, 0};
    float angles[3] = {0, 0, 0};
};
std::vector<RemoteEntity> interpolateEntities(const net::ClientSnapshot& a, const net::ClientSnapshot& b, float frac);
// A trajectory of the entity state (pos at offset 12, apos at 48; docs/spec/net.md section 11 "Trajectories"): the
// Quake III kinds 0 stationary, 1 interpolate (base), 2 linear, 3 linear stop (clamped to the duration), 5 gravity
// (800 units/s^2 down), evaluated at a time in ms.
struct Trajectory {
    int type = 0;
    int time = 0;
    int duration = 0;
    float base[3] = {0, 0, 0};
    float delta[3] = {0, 0, 0};
};
constexpr int kPosTrajectory = 12, kAposTrajectory = 48;
Trajectory readTrajectory(const std::vector<uint8_t>& es, int offset);
void writeTrajectory(std::vector<uint8_t>& es, int offset, const Trajectory& tr);
void evaluateTrajectory(const Trajectory& tr, int atMs, float out[3]);
// A trajectory the client evaluates in its own time (not a stationary or interpolated one).
inline bool trajectoryMoves(const Trajectory& tr) { return tr.type != 0 && tr.type != 1; }
// Entity state writing for the server (pos.trBase, apos.trBase, eType, clientNum).
void writeEntity(std::vector<uint8_t>& es, const RemoteEntity& e);
// The name in a client state.
std::string clientStateName(const std::vector<uint8_t>& cs);

// Menu names are configstrings from this index (docs/spec/net.md section 10).
constexpr int kMenuConfigstrings = 1246;

} // namespace iw2::netgame
