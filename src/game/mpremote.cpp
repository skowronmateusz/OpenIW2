// Network client view of a multiplayer map (docs/spec/net.md section 10): the server runs the game, this world only
// shows the map, the local player and the other players as puppets placed from snapshots.
#include "game/mpgame.h"
#include "game/mpplayers.h"
#include "game/playerevents.h"
#include "game/world.h"

#include <cmath>
#include <set>
#include <stdexcept>

namespace iw2::game {

using script::ObjectPtr;
using script::Value;

namespace {
// Client numbers are 6 bits on the wire (docs/spec/net.md section 7); one more slot for the local client's own body in
// a killcam replay (netgame::RemoteView::kOwnBodySlot).
constexpr size_t kNetClients = 64 + 1;
}

void World::mpStartRemote() {
    if (!mp_) throw std::runtime_error("mpStartRemote: not a multiplayer world");
    mpRemote_ = true;
    mp_->gametype.clear();
    mp_->clients.assign(kNetClients, MpClient{});
    mp_->localClient = 0;
    prepareStart();  // map entities and the player entity, no scripts
    if (player_) {
        // The local view: alive and playing as far as this process is concerned; the server decides the rest.
        player_->obj->set("health", Value(int32_t(100)));
        player_->obj->set("sessionstate", Value("playing"));
        player_->obj->set("classname", Value("player"));
    }
    levelInitializing_ = false;
}

void World::mpSetRemotePlayers(const std::vector<RemotePlayer>& players) {
    if (!mp_ || !mpRemote_) return;
    std::set<int> seen;
    for (const RemotePlayer& p : players) {
        if (p.client < 1 || size_t(p.client) >= mp_->clients.size()) continue;  // slot 0 is the local view
        seen.insert(p.client);
        MpClient& cl = mp_->clients[size_t(p.client)];
        if (cl.state == MpClientState::Free || !cl.ent) {
            cl = MpClient{};
            Entity& e = spawnEntity("player");
            e.isPlayer = true;
            cl.ent = &e;
            cl.state = MpClientState::Active;
            e.obj->set("classname", Value("player"));
            e.obj->set("sessionstate", Value("playing"));
            e.obj->set("health", Value(int32_t(100)));
            e.obj->set("$client", Value(int32_t(p.client)));
        }
        cl.name = p.name;
        // The body and attached models of the client state (docs/spec/net.md section 11): without them a puppet has
        // nothing to draw.
        if (!p.model.empty() && cl.ent->obj->get("model").toString() != p.model) cl.ent->obj->set("model", Value(p.model));
        if (!p.model.empty()) {
            bool same = cl.ent->attachments.size() == p.attachments.size();
            for (size_t a = 0; same && a < p.attachments.size(); ++a) same = cl.ent->attachments[a].model == p.attachments[a];
            if (!same) {
                cl.ent->attachments.clear();
                for (const auto& a : p.attachments) cl.ent->attachments.push_back({a, ""});
            }
        }
        cl.ent->obj->set("name", Value(p.name));
        cl.ent->obj->set("origin", Value(Vec3{p.origin[0], p.origin[1], p.origin[2]}));
        cl.ent->obj->set("angles", Value(Vec3{0.0f, p.yaw, 0.0f}));
        cl.ent->velocity = p.velocity;
        if (!p.team.empty()) cl.ent->obj->set("sessionteam", Value(p.team));
        cl.ent->obj->set("sessionstate", Value(p.hidden ? "spectator" : p.dead ? "dead" : "playing"));
        cl.ent->obj->set("health", Value(int32_t(p.dead ? 0 : 100)));
        cl.ent->obj->set("$clipheight", Value(p.boxHeight));
        // The animation input as the server's (stance and moves; src/game/mpplayers.cpp derives the rest).
        if (MpPlayerInput* pi = mpPlayerInput(p.client)) {
            pi->valid = true;
            pi->move.viewHeightTarget = p.viewHeight;
            pi->move.xySpeed = std::sqrt(p.velocity[0] * p.velocity[0] + p.velocity[1] * p.velocity[1]);
        }
    }
    for (size_t i = 1; i < mp_->clients.size(); ++i) {
        MpClient& cl = mp_->clients[i];
        if (cl.state == MpClientState::Free || seen.count(int(i))) continue;
        if (cl.ent) cl.ent->obj->removed = true;
        cl = MpClient{};
    }
    // The prediction clips against the other players where they are drawn, with their sent box (docs/spec/mp-pmove.md
    // "Players against each other"): the clips follow every placement, not only the next world frame.
    syncEntityClips();
}

void World::mpSetRemotePlayerWeapon(int client, const std::string& weapon, float lean) {
    if (!mp_ || !mpRemote_ || client < 1 || size_t(client) >= mp_->clients.size()) return;
    Entity* e = mp_->clients[size_t(client)].ent;
    if (!e) return;
    if (!weapon.empty() && e->obj->get("$weapon").toString() != weapon) e->obj->set("$weapon", Value(weapon));
    if (MpPlayerInput* pi = mpPlayerInput(client)) pi->move.slow = lean != 0; // TODO(spec): the aim slow of another player (not in the snapshot)
}

// Event numbers: the event-name table of the 1.3 Linux dedicated server (0x8183da0), docs/spec/net.md section 13.
void World::mpRemotePlayerEvent(int client, int event, const std::string& weapon, float pitch) {
    if (!mp_ || !mpRemote_ || client < 1 || size_t(client) >= mp_->clients.size()) return;
    Entity* e = mp_->clients[size_t(client)].ent;
    if (!e || !e->obj->get("origin").isVector()) return;
    const std::string wn = weapon.empty() ? e->obj->get("$weapon").toString() : weapon;
    if (!wn.empty()) e->obj->set("$weapon", Value(wn));
    MpPlayerInput* pi = mpPlayerInput(client);
    const Vec3 o = e->obj->get("origin").asVector();
    if (static const bool rt = std::getenv("IW2_REMOTE_TRACE") != nullptr; rt) // debug: other players' events (docs/debug-flags.md)
        std::printf("remotetrace t=%lld client=%d event=0x%x weapon=%s at %.0f,%.0f,%.0f\n", (long long)timeMs(), client, unsigned(event), wn.c_str(),
                    double(o[0]), double(o[1]), double(o[2]));
    auto play = [&](std::initializer_list<const char*> keys) {
        for (const char* k : keys) {
            const std::string alias = weaponField(wn, k);
            if (!alias.empty()) {
                startSound(alias, &o);
                return;
            }
        }
    };
    switch (event) {
    case 0x9e:    // EV_FIRE_WEAPON
    case 0xa0: {  // EV_FIRE_WEAPON_LASTSHOT
        const Value a = e->obj->get("angles");
        const float yaw = a.isVector() ? a.asVector()[1] : 0.0f;
        const float vh = pi && pi->valid ? float(pi->move.viewHeightTarget) : 60.0f;
        const float pr = pitch * float(M_PI) / 180.0f, yr = yaw * float(M_PI) / 180.0f;
        // The shot from the eye along the view: fire sound and compass ping as for a local shooter that is not this client,
        // and the impact of the trace. TODO(spec): the original's bullet hit events (0xb5..0xba) and the shooter's spread.
        clientFire(*e, {o[0], o[1], o[2] + vh}, {std::cos(pr) * std::cos(yr), std::cos(pr) * std::sin(yr), -std::sin(pr)},
                   event == 0xa0, 0.0f);
        if (pi) pi->fired = true;
        break;
    }
    case 0x96: play({"reloadSound", "reloadEmptySound"}); if (pi) pi->reloaded = true; break;  // EV_RELOAD
    case 0x97: play({"reloadEmptySound", "reloadSound"}); if (pi) pi->reloaded = true; break;  // EV_RELOAD_FROM_EMPTY
    case 0xa3: play({"meleeSwipeSound"}); break;                                                // EV_MELEE_SWIPE
    case 0xa6: play({"fireSound"}); break;                                                      // EV_USE_OFFHAND (the throw)
    default:  // footsteps, jumps, landings (1..0x8b): another player's movement sounds
        otherPlayerMoveEventSound(*e, event);
        break;
    }
}

void World::mpRemoteFollowEvent(int event, const std::string& weapon) {
    if (!mp_ || !mpRemote_) return;
    mpFollowEventSound(event, weapon);
}

void World::mpFollowEventSound(int event, const std::string& weapon) {
    if (!mp_) return;
    if (event >= 1 && event <= 0x8b) {  // the followed player's steps: its view is the camera, so the own "_plr" sounds
        const auto it = cvars.find("cg_footsteps");
        const bool footsteps = it == cvars.end() || it->second.empty() || std::atoi(it->second.c_str()) != 0;
        for (const std::string& alias : playerEventSounds(event, footsteps, true)) startSound(alias, nullptr);
        return;
    }
    if (weapon.empty() || !this->weapon(weapon)) return;
    auto play = [&](std::initializer_list<const char*> keys) {
        for (const char* k : keys) {
            const std::string a = weaponField(weapon, k);
            if (!a.empty()) {
                float sec = 0;
                playSound(a, nullptr, &sec, player_ ? player_->number : -1);
                return;
            }
        }
    };
    switch (event) {
    case mpev::kReload: play({"reloadSound", "reloadEmptySound"}); break;
    case mpev::kReloadFromEmpty: play({"reloadEmptySound", "reloadSound"}); break;
    case mpev::kMeleeSwipe: play({"meleeSwipeSound"}); break;
    case mpev::kPrepOffhand: play({"pullbackSound"}); break;
    case mpev::kUseOffhand: play({"fireSound"}); break;
    default: break;
    }
}

void World::mpRemoteFollowShot(const std::string& weapon, const Vec3& eye, const Vec3& forward, bool lastShot) {
    if (!mp_ || !mpRemote_ || !player_ || weapon.empty() || !this->weapon(weapon)) return;
    fireWeaponShot(*player_, weapon, eye, forward, lastShot, 0.0f);
}

void World::mpSetRemoteHud(std::vector<ObjectPtr> elems) {
    if (!mp_ || !mpRemote_) return;
    hudElems_ = std::move(elems);
}

void World::mpSetRemoteObjectives(const std::array<Objective, 16>& objectives) {
    if (!mp_ || !mpRemote_) return;
    objectives_ = objectives;
}

void World::mpPushRemoteObituary(const MpObituary& ob) {
    if (!mp_ || !mpRemote_) return;
    mp_->obituaries.push_back(ob);
    mp_->obituaryLog.push_back(ob);
}

void World::mpRemoteExplosion(bool rocket, const std::string& weapon, const Vec3& at, const Vec3& dir, int surface) {
    if (!mp_ || !mpRemote_ || !this->weapon(weapon)) return;
    if (rocket) missileExplosionEvent(weapon, at, dir, surface);
    else grenadeExplosionEffects(weapon, at, dir, surface);
}

void World::mpRemoteGrenadeBounce(int wire, int surface) {
    if (!mp_ || !mpRemote_ || surface < 0 || surface >= 23) return;
    const auto it = remoteModels_.find(wire);
    if (it == remoteModels_.end() || !it->second || it->second->obj->removed) return;
    const Vec3 at = it->second->obj->get("origin").asVector();
    static const char* const kSurf[] = {"default", "bark", "brick", "carpet", "cloth", "concrete", "dirt", "flesh",
                                        "foliage", "glass", "grass", "gravel", "ice", "metal", "mud", "paper",
                                        "plaster", "rock", "sand", "snow", "water", "wood", "asphalt"};
    startSound(std::string("grenade_bounce_") + kSurf[surface], &at, nullptr, {}, kWorldSoundOwner);
    // The effect's direction is the event parameter, the surface type, put through the direction-byte table (MP 0x4268c0).
    if (const std::string& fx = impactEffect(8, surface); !fx.empty()) fxEvents_.push_back({fx, at, mpBounceDirection(surface)});
}

Vec3 World::mpBounceDirection(int parameter) {
    // Entries 0..22 of the 162 unit vectors at MP 0x5ce768: the ones a surface type can select.
    static const float kDir[23][3] = {
        {-0.525731f, 0.000000f, 0.850651f}, {-0.442863f, 0.238856f, 0.864188f}, {-0.295242f, 0.000000f, 0.955423f},
        {-0.309017f, 0.500000f, 0.809017f}, {-0.162460f, 0.262866f, 0.951056f}, {0.000000f, 0.000000f, 1.000000f},
        {0.000000f, 0.850651f, 0.525731f},  {-0.147621f, 0.716567f, 0.681718f}, {0.147621f, 0.716567f, 0.681718f},
        {0.000000f, 0.525731f, 0.850651f},  {0.309017f, 0.500000f, 0.809017f},  {0.525731f, 0.000000f, 0.850651f},
        {0.295242f, 0.000000f, 0.955423f},  {0.442863f, 0.238856f, 0.864188f},  {0.162460f, 0.262866f, 0.951056f},
        {-0.681718f, 0.147621f, 0.716567f}, {-0.809017f, 0.309017f, 0.500000f}, {-0.587785f, 0.425325f, 0.688191f},
        {-0.850651f, 0.525731f, 0.000000f}, {-0.864188f, 0.442863f, 0.238856f}, {-0.716567f, 0.681718f, 0.147621f},
        {-0.688191f, 0.587785f, 0.425325f}, {-0.500000f, 0.809017f, 0.309017f},
    };
    if (parameter < 0 || parameter >= 23) return Vec3{0, 0, 0};
    return Vec3{kDir[parameter][0], kDir[parameter][1], kDir[parameter][2]};
}

void World::mpPlaceRemoteModel(int wire, const Vec3& origin, const Vec3* angles) {
    const auto it = remoteModels_.find(wire);
    if (it == remoteModels_.end() || !it->second || it->second->obj->removed) return;
    it->second->obj->set("origin", Value(origin));
    if (angles) it->second->obj->set("angles", Value(*angles));
}

void World::mpSetRemoteModels(const std::vector<RemoteModel>& models) {
    if (!mp_ || !mpRemote_) return;
    std::set<int> seen, seenMap;
    for (const RemoteModel& m : models) {
        Entity* e = nullptr;
        if (m.mapNumber >= 0) {
            for (const auto& p : entities_)
                if (p->number == m.mapNumber && !p->obj->removed) e = p.get();
            if (!e) continue;
            seenMap.insert(e->number);
            if (e->obj->get("$netabsent").truthy()) {
                e->obj->set("$netabsent", Value());
                e->obj->set("$notsolid", Value());
            }
            if (m.presentOnly) continue;
        } else {
            seen.insert(m.wire);
            auto it = remoteModels_.find(m.wire);
            if (it != remoteModels_.end()) e = it->second;
            if (!e) {
                e = &spawnEntity("script_model");
                remoteModels_[m.wire] = e;
            }
        }
        if (!m.model.empty() && e->obj->get("model").toString() != m.model) e->obj->set("model", Value(m.model));
        e->obj->set("origin", Value(m.origin));
        e->obj->set("angles", Value(m.angles));
        e->obj->set("$hidden", m.hidden ? Value(true) : Value());
        e->obj->set("$nettraj", m.trajectory ? Value(true) : Value());
        bool sameAttachments = e->attachments.size() == m.attachments.size();
        for (size_t a = 0; sameAttachments && a < m.attachments.size(); ++a) sameAttachments = e->attachments[a].model == m.attachments[a];
        if (!m.attachments.empty() && !sameAttachments) {
            e->attachments.clear();
            for (const auto& a : m.attachments) e->attachments.push_back({a, ""});
        }
        // A body plays its death animation once from its first snapshot and keeps the last frame (as cloneplayer); a
        // body first seen after its death (a killcam starting later, a client joining) is at the last frame already.
        const std::string bodyKey = std::to_string(m.animNode) + "/" + std::to_string(m.animFlip);
        if (m.animNode >= 0 && e->obj->get("$deathanim").toString() != bodyKey)
            if (const AnimTree* tree = animLib_.tree("multiplayer"); tree && size_t(m.animNode) < tree->nodes().size()) {
                e->anim = std::make_unique<AnimState>(*tree, animLib_);
                AnimState::SetArgs a;
                a.blendTime = 0;
                a.knob = true;
                e->anim->set(m.animNode, a);
                if (m.animDone) e->anim->update(30.0f, {}, false);
                e->obj->set("$deathanim", Value(bodyKey));
            }
    }
    // A map entity the server does not send (deleted by the gametype script, e.g. the objects of other gametypes) is
    // neither drawn nor solid here: the client shows only the entities of the snapshots (docs/spec/net.md section 11).
    for (const auto& p : entities_) {
        Entity& e = *p;
        if (e.mapIndex <= 0 || e.isPlayer || e.obj->removed || seenMap.count(e.number)) continue;
        const Value md = e.obj->get("model");
        if (!md.isString() || md.asString().empty()) continue;
        e.obj->set("$netabsent", Value(true));
        e.obj->set("$hidden", Value(true));
        e.obj->set("$notsolid", Value(true));
    }
    for (auto it = remoteModels_.begin(); it != remoteModels_.end();) {
        if (seen.count(it->first)) {
            ++it;
            continue;
        }
        it->second->obj->removed = true;
        it = remoteModels_.erase(it);
    }
}

} // namespace iw2::game
