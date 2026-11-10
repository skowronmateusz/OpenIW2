// Implemented from the spec (docs/spec/net.md sections 7.3 and 10).
#include "netgame/netmove.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "game/clientinput.h"
#include "net/netfields.h"

namespace iw2::netgame {

namespace {
float getF(const std::vector<uint8_t>& v, int off) {
    float f;
    std::memcpy(&f, v.data() + off, 4);
    return f;
}
int32_t getI(const std::vector<uint8_t>& v, int off) {
    int32_t i;
    std::memcpy(&i, v.data() + off, 4);
    return i;
}
void putF(std::vector<uint8_t>& v, int off, float f) { std::memcpy(v.data() + off, &f, 4); }
void putI(std::vector<uint8_t>& v, int off, int32_t i) { std::memcpy(v.data() + off, &i, 4); }
float lerpAngle(float a, float b, float t) { return a + std::remainder(b - a, 360.0f) * t; }
} // namespace

game::PlayerInput inputFromUsercmd(const net::UserCmd& cmd) {
    game::PlayerInput in;
    in.forward = cmd.forwardmove / 127.0f;
    in.right = cmd.rightmove / 127.0f;
    in.yaw = static_cast<float>(cmd.angles[1] & 0xffff) * 360.0f / 65536.0f;
    in.pitch = static_cast<float>(static_cast<int16_t>(cmd.angles[0] & 0xffff)) * 360.0f / 65536.0f;
    const uint32_t b = static_cast<uint32_t>(cmd.buttons);
    in.jump = (b & game::Button::Jump) != 0;
    in.stanceHeld = (b & game::Button::StanceHeld) != 0;
    in.stance = (b & game::Button::Prone) ? game::Stance::Prone
                : (b & game::Button::Crouch) ? game::Stance::Crouch : game::Stance::Stand;
    in.leanLeft = (b & game::Button::LeanLeft) != 0;
    in.leanRight = (b & game::Button::LeanRight) != 0;
    in.adsHeld = (b & game::Button::Ads) != 0;
    in.holdBreathHeld = (b & game::Button::HoldBreath) != 0;
    return in;
}

int usercmdMsec(int prevTime, int time) {
    if (prevTime == 0) return 50;
    return std::clamp(time - prevTime, 0, 1000);
}

float usercmdStep(int prevTime, int time) { return static_cast<float>(usercmdMsec(prevTime, time)) * 0.001f; }

void runUsercmd(const game::CollisionWorld& world, game::PlayerState& ps, const net::UserCmd& cmd, int prevTime,
                const WeaponMove* weapon, int passEntity, bool controlsFrozen) {
    const int msec = usercmdMsec(prevTime, cmd.serverTime);
    if (msec <= 0) return;
    // MP movement types (docs/spec/mp-spectate.md): 5 intermission does not move; 4 is the free spectator's fly move
    // (pm_flags 0x400000, following, comes with the target's state and is not predicted here). TODO(spec): the free look
    // permission on the client side (ps speed 0 without it); 400 is assumed.
    if (ps.pmType == 5) return;
    // A network game is multiplayer: the MP dvar defaults (docs/spec/mp-pmove.md), on the server and in the prediction alike.
    game::MoveParams mp = game::multiplayerMoveParams();
    mp.passEntity = passEntity;
    game::PlayerInput in = inputFromUsercmd(cmd);
    in.dead = ps.pmType == 6 || ps.pmType == 7; // MP dead types: no command moves, the dead view height
    if (controlsFrozen) { // pm_flags 0x8000 (docs/spec/controls.md "freezecontrols"): no move bytes, no jump, aim or lean; velocity and stance are held in playerMove
        in.controlsFrozen = true;
        in.forward = in.right = 0.0f;
        in.jump = in.leanLeft = in.leanRight = in.adsHeld = in.holdBreathHeld = false;
    }
    if (ps.pmType == 4) {
        mp.mpSpectator = true;
        mp.traceMask = 0x800811u;
        const uint32_t b = static_cast<uint32_t>(cmd.buttons);
        in.leanLeft = (b & game::Button::LeanLeft) != 0; // down (0x40) / up (0x80) of the spectator move
        in.leanRight = (b & game::Button::LeanRight) != 0;
        in.adsHeld = in.holdBreathHeld = false;
    } else if (weapon) {
        // The weapon side (section 13): aim state, the weapon's move speed scale and hip spread.
        in.adsAllowed = weapon->adsAllowed;
        in.adsFrac = weapon->adsFrac;
        in.weaponState = weapon->weaponState;
        in.breathWeapon = weapon->breathWeapon;
        mp.weaponSpeedScale = weapon->params.weaponSpeedScale;
        mp.hipSpread = weapon->params.hipSpread;
    }
    forEachUsercmdPiece(msec, [&](float dt) { game::playerMove(world, ps, in, dt, mp); });
}

net::UserCmd makeUserCmd(int serverTime, uint32_t buttons, float forward, float right, float pitch, float yaw, int weapon) {
    net::UserCmd uc;
    uc.serverTime = serverTime;
    uc.buttons = int32_t(buttons);
    uc.forwardmove = int8_t(std::clamp(forward * 127.0f, -127.0f, 127.0f));
    uc.rightmove = int8_t(std::clamp(right * 127.0f, -127.0f, 127.0f));
    uc.angles[0] = int32_t(std::lround(pitch * 65536.0f / 360.0f)) & 0xffff;
    uc.angles[1] = int32_t(std::lround(yaw * 65536.0f / 360.0f)) & 0xffff;
    uc.weapon = uint8_t(weapon);
    return uc;
}

int viewAngleDelta(float angle, int32_t cmdAngle) {
    return ((int32_t(angle * (65536.0f / 360.0f)) & 0xffff) - cmdAngle) & 0xffff;
}

net::UserCmd withDeltaAngles(const net::UserCmd& cmd, const int (&delta)[3]) {
    net::UserCmd c = cmd;
    for (int i = 0; i < 3; ++i) c.angles[i] = (cmd.angles[i] + delta[i]) & 0xffff;
    return c;
}

PsFields readPs(const std::vector<uint8_t>& ps) {
    PsFields f;
    if (ps.size() < static_cast<size_t>(net::kPlayerStateSize)) return f;
    f.commandTime = getI(ps, 0);
    f.pmType = getI(ps, 4);
    f.pmFlags = getI(ps, 12);
    f.clientNum = getI(ps, 204);
    f.bobCycle = getI(ps, 8) & 0xff;
    for (int i = 0; i < 3; ++i) {
        f.origin[i] = getF(ps, 20 + 4 * i);
        f.velocity[i] = getF(ps, 32 + 4 * i);
        f.viewangles[i] = getF(ps, 232 + 4 * i);
    }
    f.eFlags = getI(ps, 160);
    f.viewLockMode = getI(ps, 1424);
    f.viewLockEntity = getI(ps, 1428);
    f.weaponTime = getI(ps, 52);
    f.weaponState = getI(ps, 216);
    f.weapon = getI(ps, 212);
    f.eventSequence = getI(ps, 164) & 0xff;
    for (int i = 0; i < 4; ++i) f.events[i] = getI(ps, 168 + 4 * i);
    for (int i = 0; i < 3; ++i) f.deltaAngles[i] = getI(ps, 84 + 4 * i) & 0xffff;
    return f;
}

void writePs(std::vector<uint8_t>& ps, const PsFields& f) {
    if (ps.size() < static_cast<size_t>(net::kPlayerStateSize)) ps.resize(static_cast<size_t>(net::kPlayerStateSize), 0);
    putI(ps, 0, f.commandTime);
    putI(ps, 4, f.pmType);
    putI(ps, 204, f.clientNum);
    putI(ps, 8, f.bobCycle & 0xff);
    for (int i = 0; i < 3; ++i) {
        putF(ps, 20 + 4 * i, f.origin[i]);
        putF(ps, 32 + 4 * i, f.velocity[i]);
        putF(ps, 232 + 4 * i, f.viewangles[i]);
    }
    putI(ps, 160, f.eFlags);
    putI(ps, 1424, f.viewLockMode);
    putI(ps, 1428, f.viewLockEntity);
    putI(ps, 52, f.weaponTime);
    putI(ps, 216, f.weaponState);
    for (int i = 0; i < 3; ++i) putI(ps, 84 + 4 * i, f.deltaAngles[i] & 0xffff);
}

game::PlayerState predictPlayer(const game::CollisionWorld& world, const PsFields& server,
                                const std::vector<net::UserCmd>& commands, const game::PlayerState* carry,
                                const WeaponMove* weapon, int passEntity) {
    // A client spawn clears the player state (docs/spec/mp.md "Client spawn"); a client goes from dead or spectating
    // (pm_type != 0) to playing only through a spawn, so its own movement state (stance, view height, lean, aim)
    // starts afresh then.
    const bool spawned = carry && carry->pmType != 0 && server.pmType == 0;
    game::PlayerState ps = carry && !spawned ? *carry : game::PlayerState{};
    ps.pmType = server.pmType;
    ps.origin = {server.origin[0], server.origin[1], server.origin[2]};
    ps.velocity = {server.velocity[0], server.velocity[1], server.velocity[2]};
    ps.bobCycle = server.bobCycle;
    int prev = server.commandTime;
    if (server.eFlags & 0x300) {
        ps.velocity = {0, 0, 0};
        return ps;
    }
    for (const net::UserCmd& c : commands) {
        if (c.serverTime <= server.commandTime) continue;
        runUsercmd(world, ps, withDeltaAngles(c, server.deltaAngles), prev, weapon, passEntity, (server.pmFlags & 0x8000) != 0);
        prev = c.serverTime;
    }
    return ps;
}

void Predictor::reset() {
    const float d = decayMs;
    *this = Predictor{};
    decayMs = d;
}

const game::PlayerState& Predictor::predict(const game::CollisionWorld& world, const PsFields& server,
                                            const std::vector<net::UserCmd>& commands, const WeaponMove* weapon,
                                            int64_t nowMs, int passEntity) {
    // The start: this client's own state after the acknowledged command, else the last prediction (first frame, a
    // command no longer kept). predictPlayer then puts the server's fields over it.
    auto acked = history_.upper_bound(server.commandTime);
    if (acked != history_.begin()) --acked;
    else acked = history_.end();
    const game::PlayerState* carry = acked != history_.end() ? &acked->second : have_ ? &ps_ : nullptr;
    game::PlayerState ps = predictPlayer(world, server, {}, carry, weapon, passEntity);
    history_.erase(history_.begin(), acked != history_.end() ? acked : history_.lower_bound(server.commandTime));
    const game::Vec oldOrigin = ps_.origin;
    const int oldTime = psTime_;
    bool compared = false;
    int prev = server.commandTime;
    const bool held = (server.eFlags & 0x300) != 0;
    for (const net::UserCmd& c : commands) {
        if (c.serverTime <= server.commandTime) continue;
        if (!held) runUsercmd(world, ps, withDeltaAngles(c, server.deltaAngles), prev, weapon, passEntity, (server.pmFlags & 0x8000) != 0);
        prev = c.serverTime;
        history_[c.serverTime] = ps;
        // The prediction for the command the last frame ended on: a difference is a correction, drawn away over
        // cg_errorDecay (only above 0.1 units), on top of what is left of the previous one.
        if (have_ && c.serverTime == oldTime) {
            compared = true;
            float d[3], len2 = 0;
            for (int i = 0; i < 3; ++i) {
                d[i] = oldOrigin[size_t(i)] - ps.origin[size_t(i)];
                len2 += d[i] * d[i];
            }
            lastMiss_ = std::sqrt(len2);
            if (lastMiss_ > 0.1f) {
                float keep = 0;
                if (decayMs > 0) keep = std::max(0.0f, (decayMs - static_cast<float>(nowMs - errorTime_)) / decayMs);
                for (int i = 0; i < 3; ++i) error_[i] = error_[i] * keep + d[i];
                errorTime_ = nowMs;
            }
        }
    }
    if (!compared) lastMiss_ = 0;
    if (prev == server.commandTime && have_ && oldTime > server.commandTime) {
        // No newer command in the list: keep the last prediction's time so the next frame compares against it.
        psTime_ = oldTime;
    } else {
        psTime_ = prev;
    }
    ps_ = ps;
    have_ = true;
    return ps_;
}

game::Vec Predictor::drawOrigin(int64_t nowMs) const {
    game::Vec o = ps_.origin;
    if (decayMs <= 0) return o;
    const float f = (decayMs - static_cast<float>(nowMs - errorTime_)) / decayMs;
    if (f <= 0) return o;
    for (int i = 0; i < 3; ++i) o[size_t(i)] += error_[i] * f;
    return o;
}

void PredictedAmmo::record(int commandTime, const Counters& before, const Counters& after) {
    Counters& pending = pending_[commandTime];
    auto difference = [](const std::map<std::string, int>& was, const std::map<std::string, int>& now,
                         std::map<std::string, int>& out) {
        for (const auto& [key, value] : now) {
            const auto it = was.find(key);
            if (it != was.end() && it->second != value) out[key] += value - it->second;
        }
    };
    difference(before.clip, after.clip, pending.clip);
    difference(before.stock, after.stock, pending.stock);
    if (pending.clip.empty() && pending.stock.empty()) pending_.erase(commandTime);
    while (pending_.size() > 256) pending_.erase(pending_.begin());
}

void PredictedAmmo::adjust(int commandTime, Counters& counters) {
    server_ = counters;
    pending_.erase(pending_.begin(), pending_.upper_bound(commandTime));
    auto add = [](const std::map<std::string, int>& change, std::map<std::string, int>& server) {
        for (const auto& [key, value] : change) {
            const auto it = server.find(key);
            if (it != server.end()) it->second = std::max(0, it->second + value);
        }
    };
    for (const auto& [time, change] : pending_) {
        add(change.clip, counters.clip);
        add(change.stock, counters.stock);
    }
}

void PredictedAmmo::clear() {
    *this = PredictedAmmo{};
}

std::vector<RemoteEntity> interpolateEntities(const net::ClientSnapshot& a, const net::ClientSnapshot& b, float frac) {
    std::vector<RemoteEntity> out;
    for (const auto& [number, es] : b.entities) {
        RemoteEntity e;
        e.number = number;
        e.eType = getI(es, 4) & 0xff;
        e.clientNum = getI(es, 144) & 0xff;
        const auto old = a.entities.find(number);
        const std::vector<uint8_t>& from = old != a.entities.end() ? old->second : es;  // new: no interpolation
        for (int i = 0; i < 3; ++i) {
            e.origin[i] = getF(from, 24 + 4 * i) + (getF(es, 24 + 4 * i) - getF(from, 24 + 4 * i)) * frac;
            e.angles[i] = lerpAngle(getF(from, 60 + 4 * i), getF(es, 60 + 4 * i), frac);
        }
        out.push_back(e);
    }
    return out;
}

Trajectory readTrajectory(const std::vector<uint8_t>& es, int offset) {
    Trajectory tr;
    tr.type = getI(es, offset);
    tr.time = getI(es, offset + 4);
    tr.duration = getI(es, offset + 8);
    for (int i = 0; i < 3; ++i) {
        tr.base[i] = getF(es, offset + 12 + 4 * i);
        tr.delta[i] = getF(es, offset + 24 + 4 * i);
    }
    return tr;
}

void writeTrajectory(std::vector<uint8_t>& es, int offset, const Trajectory& tr) {
    if (es.size() < static_cast<size_t>(net::kEntityStateSize)) es.resize(static_cast<size_t>(net::kEntityStateSize), 0);
    putI(es, offset, tr.type);
    putI(es, offset + 4, tr.time);
    putI(es, offset + 8, tr.duration);
    for (int i = 0; i < 3; ++i) {
        putF(es, offset + 12 + 4 * i, tr.base[i]);
        putF(es, offset + 24 + 4 * i, tr.delta[i]);
    }
}

void evaluateTrajectory(const Trajectory& tr, int atMs, float out[3]) {
    float t = static_cast<float>(atMs - tr.time) * 0.001f;
    switch (tr.type) {
    case 2:  // linear
        for (int i = 0; i < 3; ++i) out[i] = tr.base[i] + tr.delta[i] * t;
        return;
    case 3:  // linear stop: the move ends after the duration
        if (atMs - tr.time > tr.duration) t = static_cast<float>(tr.duration) * 0.001f;
        if (t < 0) t = 0;
        for (int i = 0; i < 3; ++i) out[i] = tr.base[i] + tr.delta[i] * t;
        return;
    case 5:  // gravity
        for (int i = 0; i < 3; ++i) out[i] = tr.base[i] + tr.delta[i] * t;
        out[2] -= 400.0f * t * t;
        return;
    default:  // stationary, interpolate, unknown: the base
        for (int i = 0; i < 3; ++i) out[i] = tr.base[i];
        return;
    }
}

void writeEntity(std::vector<uint8_t>& es, const RemoteEntity& e) {
    if (es.size() < static_cast<size_t>(net::kEntityStateSize)) es.resize(static_cast<size_t>(net::kEntityStateSize), 0);
    putI(es, 4, e.eType);
    putI(es, 144, e.clientNum);
    for (int i = 0; i < 3; ++i) {
        putF(es, 24 + 4 * i, e.origin[i]);
        putF(es, 60 + 4 * i, e.angles[i]);
    }
}

std::string clientStateName(const std::vector<uint8_t>& cs) {
    if (cs.size() < 92) return {};
    const char* p = reinterpret_cast<const char*>(cs.data() + 60);
    return std::string(p, strnlen(p, 32));
}

} // namespace iw2::netgame
