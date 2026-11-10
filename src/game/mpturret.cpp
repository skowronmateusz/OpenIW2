// Map turrets used by the non-local clients of a multiplayer server (docs/spec/turrets.md "Multiplayer"). The server keeps
// one turret use state per client (the player state fields the local player keeps in the world) and runs the same use,
// think and release as for the local player with that client's state and usercmd swapped in, once per server frame.

#include "game/world.h"

#include <utility>

namespace iw2::game {

namespace {

// The local player's turret and use fields, swapped with a client's for the length of one call.
struct UseSwap {
    World::PlayerCommand cmd;
    uint32_t heldButtons = 0, edgeButtons = 0, oldButtons = 0; // client +0x8634 / +0x8640 and the previous command's
    Entity* player = nullptr;
    Entity* turret = nullptr;
    int useState = 0, viewLocked = 0, useEntity = -1;
    uint32_t eFlags = 0;
    float clampBase[2] = {0, 0}, clampRange[2] = {0, 0};
};

} // namespace

bool World::mpClientOnTurret(int client) const {
    uint32_t bits = 0;
    int lock = 0;
    return mpClientTurretUse(client, bits, lock) != nullptr;
}

const Entity* World::mpClientTurretUse(int client, uint32_t& turretBits, int& viewLocked) const {
    turretBits = 0;
    viewLocked = 0;
    if (!mp_ || client < 0 || size_t(client) >= mp_->clients.size()) return nullptr;
    const MpClient& cl = mp_->clients[size_t(client)];
    if (cl.ent && cl.ent == player_) { // the client of the World's own player state (the listen server's own player)
        if (!playerOnTurret() || !playerTurret_ || !playerTurret_->turret || playerTurret_->obj->removed) return nullptr;
        turretBits = playerEFlags_ & 0x300u;
        viewLocked = playerViewLocked_;
        return playerTurret_;
    }
    const MpClient::TurretUse& u = cl.turretUse;
    if (!u.turret || !(u.eFlags & 0x300u) || u.turret->obj->removed || !u.turret->turret) return nullptr;
    turretBits = u.eFlags & 0x300u;
    viewLocked = u.viewLocked;
    return u.turret;
}

void World::mpSetView(int client, const Vec3& angles, float viewHeight) {
    if (!mp_ || client < 0 || size_t(client) >= mp_->clients.size()) return;
    MpClient& cl = mp_->clients[size_t(client)];
    cl.viewAngles = {angles[0], angles[1], angles[2]};
    cl.viewHeight = viewHeight;
}

bool World::mpClientTurret(size_t client, bool usePressed) {
    MpClient& cl = mp_->clients[client];
    MpClient::TurretUse& u = cl.turretUse;
    if (u.turret && (u.turret->obj->removed || !u.turret->turret)) u = {};
    if (!u.turret && !usePressed) return false;
    // Swap the client in for the local player's use fields; they come back even when a script notify throws.
    UseSwap saved;
    saved.cmd = playerCmd_;
    saved.heldButtons = heldButtons_;
    saved.edgeButtons = edgeButtons_;
    saved.oldButtons = playerOldButtons_;
    saved.player = player_;
    saved.turret = playerTurret_;
    saved.useState = playerUseState_;
    saved.viewLocked = playerViewLocked_;
    saved.useEntity = playerUseEntity_;
    saved.eFlags = playerEFlags_;
    for (int i = 0; i < 2; ++i) saved.clampBase[i] = viewClampBase_[i], saved.clampRange[i] = viewClampRange_[i];
    const PlayerUseHint savedHint = playerUseHint_;
    const auto savedRelease = turretRelease_;
    struct Restore {
        World& w;
        MpClient::TurretUse& u;
        const UseSwap& s;
        const PlayerUseHint& hint;
        const std::optional<TurretRelease>& release;
        ~Restore() {
            u.turret = w.playerTurret_;
            u.useState = w.playerUseState_;
            u.eFlags = w.playerEFlags_;
            u.viewLocked = w.playerViewLocked_;
            for (int i = 0; i < 2; ++i) u.clampBase[i] = w.viewClampBase_[i], u.clampRange[i] = w.viewClampRange_[i];
            w.playerCmd_ = s.cmd;
            w.heldButtons_ = s.heldButtons;
            w.edgeButtons_ = s.edgeButtons;
            w.playerOldButtons_ = s.oldButtons;
            w.player_ = s.player;
            w.playerTurret_ = s.turret;
            w.playerUseState_ = s.useState;
            w.playerViewLocked_ = s.viewLocked;
            w.playerUseEntity_ = s.useEntity;
            w.playerEFlags_ = s.eFlags;
            for (int i = 0; i < 2; ++i) w.viewClampBase_[i] = s.clampBase[i], w.viewClampRange_[i] = s.clampRange[i];
            w.playerUseHint_ = hint;
            w.turretRelease_ = release; // a client's release moves its entity; the local front end gets nothing
        }
    } restore{*this, u, saved, savedHint, savedRelease};
    player_ = cl.ent;
    playerTurret_ = u.turret;
    playerUseState_ = u.useState;
    playerEFlags_ = u.eFlags;
    playerViewLocked_ = u.viewLocked;
    for (int i = 0; i < 2; ++i) viewClampBase_[i] = u.clampBase[i], viewClampRange_[i] = u.clampRange[i];
    PlayerCommand c;
    c.viewAngles = {cl.viewAngles[0], cl.viewAngles[1], cl.viewAngles[2]};
    c.buttons = cl.buttons;
    c.viewHeight = cl.viewHeight;
    playerCmd_ = c;
    bool taken = false;
    // The use part of the client think (0x4e7f90 in SP; per client on the server): a press gets off the turret
    // (use state 2, the think releases) or, off it, uses the picked turret (only turrets here: items and use triggers of
    // non-local clients have their own paths, mpClientItems / mpClientTriggers).
    if (usePressed && playerUseState_ != 0) {
        playerUseState_ = playerOnTurret() ? 2 : 0;
        taken = true;
    } else if (usePressed && isAlive(*player_) && player_->obj->get("sessionstate").toString() == "playing") {
        // What the client's last use search picked (mpClientUseSearch, player state +0x5a0).
        Entity* e = cl.useEntity >= 0 ? entityByNumber(cl.useEntity) : nullptr;
        if (e && !e->obj->removed && e->turret) {
            usePlayerEntity(*e); // "trigger" with the client, then the turret use
            taken = true;
        }
    }
    thinkPlayerTurret(); // aim, hold the client behind the gun, fire; releases a dead client or one getting off
    return taken;
}

void World::mpClientTurretRelease(size_t client) {
    if (!mp_ || client >= mp_->clients.size()) return;
    MpClient::TurretUse& u = mp_->clients[client].turretUse;
    if (u.turret && !u.turret->obj->removed && u.turret->turret && (u.eFlags & 0x300u)) {
        TurretState& ts = *u.turret->turret;
        ts.fireCountdownMs = 0;
        ts.savedStance = -1;
        if (ts.inUse) vm_.notify(u.turret->obj, "turret_deactivate");
        ts.inUse = false;
        ts.owner.reset();
        ts.flags &= ~0x800u;
        vm_.notify(u.turret->obj, "turretownerchange");
    }
    u = {};
}

void World::mpRemoteTurretFire(Entity& turret) {
    if (!mpRemote_ || !turret.turret || turret.obj->removed) return;
    shootTurret(turret); // the world applies no damage in a network client (World::damage)
}

void World::mpSetRemoteTurret(Entity* turret, uint32_t turretBits, int viewLocked) {
    if (!mpRemote_ || !player_) return;
    if (turret && (turret->obj->removed || !turret->turret)) turret = nullptr;
    turretBits &= 0x300u;
    if (!turret || !turretBits) {
        if (playerTurret_ && playerTurret_->turret) {
            playerTurret_->turret->inUse = false;
            playerTurret_->turret->owner.reset();
        }
        playerTurret_ = nullptr;
        playerEFlags_ &= ~0x300u;
        playerViewLocked_ = 0;
        return;
    }
    if (playerTurret_ && playerTurret_ != turret && playerTurret_->turret) {
        playerTurret_->turret->inUse = false;
        playerTurret_->turret->owner.reset();
    }
    TurretState& ts = *turret->turret;
    if (playerTurret_ != turret) {
        // Getting on: the arcs of a player's use (as playerUseTurret) give the view clamp.
        ts.rightArc = ts.setupRightArc;
        ts.leftArc = ts.setupLeftArc;
        const Vec3 base = turret->obj->get("angles").isVector() ? turret->obj->get("angles").asVector() : Vec3{0, 0, 0};
        setTurretViewClamp(ts, base);
    }
    ts.inUse = true;
    ts.owner = player_->obj;
    playerTurret_ = turret;
    playerEFlags_ = (playerEFlags_ & ~0x300u) | turretBits;
    playerViewLocked_ = viewLocked;
}

} // namespace iw2::game
