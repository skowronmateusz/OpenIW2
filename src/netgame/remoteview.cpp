// Client side of the network game; implemented from the spec (docs/spec/net.md section 11, docs/spec/mp-hud.md).
#include "netgame/remoteview.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "game/mphud.h"
#include "net/netfields.h"
#include "net/playerstate.h"
#include "netgame/netcombat.h"
#include "netgame/netmove.h"
#include "game/clienttime.h"
#include "netgame/worldgame.h"

namespace iw2::netgame {

using script::Value;

namespace {
int32_t getI(const std::vector<uint8_t>& v, int off) {
    int32_t i = 0;
    if (off + 4 <= int(v.size())) std::memcpy(&i, v.data() + off, 4);
    return i;
}
float getF(const std::vector<uint8_t>& v, int off) {
    float f = 0;
    if (off + 4 <= int(v.size())) std::memcpy(&f, v.data() + off, 4);
    return f;
}
const std::string* cfg(const std::map<int, std::string>& cs, int index) {
    auto it = cs.find(index);
    return it == cs.end() ? nullptr : &it->second;
}
// The body model (+8) and attached models (+12..+32) of a client state as model names (docs/spec/net.md section 11).
void clientModels(const std::vector<uint8_t>& st, const std::map<int, std::string>& cs, std::string& model,
                  std::vector<std::string>& attachments) {
    if (const int mi = getI(st, 8) & 0xff; mi > 0)
        if (const std::string* n = cfg(cs, kModelConfigstrings + mi - 1)) model = *n;
    for (int a = 0; a < 6; ++a)
        if (const int ai = getI(st, 12 + 4 * a); ai > 0)
            if (const std::string* n = cfg(cs, kModelConfigstrings + ai - 1)) attachments.push_back(*n);
}
// A model with a moving trajectory: its pose at time t (the snapshot's origin is the trajectory base).
void trajectoryPose(const std::vector<uint8_t>& es, int t, game::World::RemoteModel& m) {
    const Trajectory pos = readTrajectory(es, kPosTrajectory);
    if (!trajectoryMoves(pos)) return;
    m.trajectory = true;
    float o[3];
    evaluateTrajectory(pos, t, o);
    m.origin = {o[0], o[1], o[2]};
    if (const Trajectory apos = readTrajectory(es, kAposTrajectory); trajectoryMoves(apos)) {
        float a[3];
        evaluateTrajectory(apos, t, a);
        m.angles = {a[0], a[1], a[2]};
    }
}
const char* teamName(int t) { return t == 1 ? "axis" : t == 2 ? "allies" : t == 3 ? "spectator" : "none"; }
}  // namespace

bool RemoteView::clientInfo(const net::ClientSnapshot& snap, int client, std::string& name, int& team) {
    const auto cs = snap.clients.find(client);
    if (cs == snap.clients.end()) return false;
    name = clientStateName(cs->second);
    team = getI(cs->second, 4) & 3;
    return true;
}

std::vector<game::World::RemotePlayer> RemoteView::players(const net::ClientSnapshot& a, const net::ClientSnapshot& b,
                                                           float frac, int self, const std::map<int, std::string>& cfgs) {
    // The last frames of a killcam: the newer snapshot is live already, the drawn time is still in the replay.
    if (following(a) && !following(b)) return players(a, a, frac, self, cfgs);
    std::vector<game::World::RemotePlayer> out;
    const auto now = interpolateEntities(a, b, frac);
    // In the killcam (the follow bit) the server sends the local client's own entity at its archived place: it is drawn
    // as a puppet in a slot of its own.
    const bool replay = following(b);
    for (const RemoteEntity& e : now) {
        if (e.eType != kEtPlayer || (e.clientNum == self && !replay)) continue;
        const auto& es = b.entities.at(e.number);
        game::World::RemotePlayer rp;
        rp.client = e.clientNum == self ? kOwnBodySlot : worldSlot(e.clientNum, self);
        rp.origin = {e.origin[0], e.origin[1], e.origin[2]};
        rp.yaw = e.angles[1];
        rp.velocity = {getF(es, 36), getF(es, 40), getF(es, 44)};  // pos.trDelta
        const int flags = getI(es, 8);
        rp.dead = (flags & kEfDead) != 0;
        rp.hidden = (flags & kEfHidden) != 0;
        rp.viewHeight = (flags & kEfProne) ? 11 : (flags & kEfCrouch) ? 40 : 60;
        // solid (+156): x/y half size, down, up + 32 in three bytes (docs/spec/net.md section 11)
        if (const int solid = getI(es, 156); solid && solid != 0xffffff) rp.boxHeight = float(((solid >> 16) & 0xff) - 32);
        if (const auto cs = b.clients.find(e.clientNum); cs != b.clients.end()) {
            rp.name = clientStateName(cs->second);
            rp.team = teamName(getI(cs->second, 4) & 3);
            clientModels(cs->second, cfgs, rp.model, rp.attachments);
        }
        out.push_back(rp);
    }
    return out;
}

void RemoteView::playerEvents(game::World& w, const net::ClientSnapshot& snap, const std::map<int, std::string>& cs, int self) {
    if (!snap.valid || snap.messageNum == lastEventSnapshot_) return;
    lastEventSnapshot_ = snap.messageNum;
    // A killcam starting or ending swaps the puppets' rings between the live and the archived ones (docs/spec/mp-hud.md
    // "The recorded view state"): the first rings of the other side are a new start, not missed events.
    const bool follow = following(snap);
    if (follow != lastEventsFollowing_) eventSeq_.clear();
    lastEventsFollowing_ = follow;
    for (const auto& [number, es] : snap.entities) {
        if ((getI(es, 4) & 0xff) != kEtPlayer) continue;
        const int client = getI(es, 144) & 0xff;
        const std::string weapon = weaponNameFromList(cs, getI(es, 200));
        if (client == self) {  // the own body of a killcam replay: its archived weapon, no events
            if (following(snap)) w.mpSetRemotePlayerWeapon(kOwnBodySlot, weapon, 0.0f);
            continue;
        }
        const int slot = worldSlot(client, self);
        w.mpSetRemotePlayerWeapon(slot, weapon, getF(es, 212));
        const int seq = getI(es, 164) & 0xff;
        auto it = eventSeq_.find(client);
        if (it == eventSeq_.end()) {  // first sight: the events already in the ring are old
            eventSeq_[client] = seq;
            continue;
        }
        int from = it->second;
        if (((seq - from) & 0xff) > 4) from = (seq - 4) & 0xff;  // more than the ring holds were missed
        for (int s = from; s != seq; s = (s + 1) & 0xff) {
            const int ev = getI(es, 168 + 4 * (s & 3));
            const int parm = getI(es, 184 + 4 * (s & 3));
            const std::string pn = weaponNameFromList(cs, parm);
            w.mpRemotePlayerEvent(slot, ev, pn.empty() ? weapon : pn, getF(es, 60));
        }
        it->second = seq;
    }
}

std::map<std::string, std::string> RemoteView::syncedDvars(const std::map<int, std::string>& cs) {
    std::map<std::string, std::string> out;
    for (int i = 0; i < kDvarConfigstringCount; ++i) {
        const auto n = cs.find(kDvarNameConfigstrings + i);
        if (n == cs.end() || n->second.empty()) continue;
        const auto v = cs.find(kDvarValueConfigstrings + i);
        out[n->second] = v == cs.end() ? std::string() : v->second;
    }
    return out;
}

bool RemoteView::following(const net::ClientSnapshot& snap) {
    return snap.ps.size() >= 252 && (getI(snap.ps, 12) & kPmfFollow) != 0;
}

bool RemoteView::followView(const net::ClientSnapshot& a, const net::ClientSnapshot& b, float frac, float eye[3],
                            float angles[3]) {
    if (!following(a) && !following(b)) return false;
    // The newer snapshot no longer follows: the drawn time is still in the replay, the view stays on the last followed state.
    const net::ClientSnapshot& to = following(b) ? b : a;
    const net::ClientSnapshot& from = following(a) ? a : b;
    const float f = std::clamp(frac, 0.0f, 1.0f);
    for (int i = 0; i < 3; ++i) {
        const float o0 = getF(from.ps, 20 + 4 * i), o1 = getF(to.ps, 20 + 4 * i);
        const float a0 = getF(from.ps, 232 + 4 * i), a1 = getF(to.ps, 232 + 4 * i);
        eye[i] = o0 + (o1 - o0) * f;
        angles[i] = a0 + std::remainder(a1 - a0, 360.0f) * f;
    }
    const float h0 = getF(from.ps, 248), h1 = getF(to.ps, 248);
    eye[2] += h0 + (h1 - h0) * f;
    return true;
}

void RemoteView::followEvents(game::World& w, const net::ClientSnapshot& snap, const std::map<int, std::string>& cs,
                              int64_t nowMs) {
    if (!snap.valid || snap.messageNum == lastFollowSnapshot_) return;
    lastFollowSnapshot_ = snap.messageNum;
    if (!following(snap)) { // the counts stay for the last drawn frames of the replay, a new killcam clears them
        followSeq_ = -1;
        return;
    }
    const int seq = getI(snap.ps, 164) & 0xff;
    if (followSeq_ < 0) {  // the first followed snapshot: the events already in the ring are older than the replay
        followSeq_ = seq;
        followFires_ = 0;
        followFireMs_ = -1;
        return;
    }
    int from = followSeq_;
    if (((seq - from) & 0xff) > 4) from = (seq - 4) & 0xff;
    float eye[3], ang[3];
    followView(snap, snap, 1.0f, eye, ang);
    const float pr = ang[0] * float(M_PI) / 180.0f, yr = ang[1] * float(M_PI) / 180.0f;
    const game::Vec3 fwd{std::cos(pr) * std::cos(yr), std::cos(pr) * std::sin(yr), -std::sin(pr)};
    for (int s = from; s != seq; s = (s + 1) & 0xff) {
        const int ev = getI(snap.ps, 168 + 4 * (s & 3));
        std::string weapon = weaponNameFromList(cs, getI(snap.ps, 184 + 4 * (s & 3)));
        if (weapon.empty()) weapon = weaponNameFromList(cs, getI(snap.ps, 212));
        if (ev != kEvFireWeapon && ev != kEvFireWeaponLastShot) {
            w.mpRemoteFollowEvent(ev, weapon); // reload, melee, throw: the sound the own view would play
            continue;
        }
        w.mpRemoteFollowShot(weapon, {eye[0], eye[1], eye[2]}, fwd, ev == kEvFireWeaponLastShot);
        ++followFires_;
        followLastShot_ = ev == kEvFireWeaponLastShot;
        followFireMs_ = nowMs;
    }
    followSeq_ = seq;
}

game::World::MpKillcamView RemoteView::killcamView(const net::ClientSnapshot& a, const net::ClientSnapshot& newer, float frac,
                                                   const std::map<int, std::string>& cs, int64_t nowMs) const {
    game::World::MpKillcamView v;
    float eye[3], ang[3];
    if (!followView(a, newer, frac, eye, ang)) return v;
    const net::ClientSnapshot& b = following(newer) ? newer : a;
    v.active = true;
    v.eye = {eye[0], eye[1], eye[2]};
    v.angles = {ang[0], ang[1], ang[2]};
    const int wn = getI(b.ps, 212);
    v.weapon = weaponNameFromList(cs, wn);
    if (const int hi = getI(b.ps, 228); hi > 0)
        if (const std::string* n = cfg(cs, kModelConfigstrings + hi - 1)) v.hands = *n;
    v.health = getI(b.ps, 300);
    v.maxHealth = getI(b.ps, 308);
    // The clip and stock the server put in the followed weapon's ammo slots (the only ones it fills in the killcam).
    for (int i = 0; i < 64; ++i) {
        v.stock = std::max(v.stock, getI(b.ps, 324 + 4 * i));
        v.clip = std::max(v.clip, getI(b.ps, 836 + 4 * i));
    }
    v.fireCount = followFires_;
    v.lastShot = followLastShot_;
    if (followFireMs_ >= 0 && nowMs - followFireMs_ < 2000) v.fireAgeMs = game::elapsedMs(nowMs, followFireMs_);
    v.lerpFrac = std::clamp(frac, 0.0f, 1.0f);
    v.frameTimeMs = b.serverTime;
    // The followed client (the copied player state's clientNum) in world slots, and the recorded view state the server
    // wrote into the followed player state (docs/spec/mp-hud.md "The recorded view state").
    v.client = getI(b.ps, 204);
    const int pmf = getI(b.ps, 12);
    game::MpViewState& vs = v.view;
    vs.weapon = v.weapon;
    vs.weaponState = getI(b.ps, 216) & 31;
    const int off = getI(b.ps, 208);
    vs.viewWeapon = vs.weaponState >= 12 && vs.weaponState <= 16 && off > 0 ? weaponNameFromList(cs, off) : v.weapon;
    vs.animKey = game::viewAnimKeyName(getI(b.ps, 1488));
    vs.animMs = getI(b.ps, 224);
    vs.adsFrac = getF(b.ps, 220);
    vs.adsHeld = (pmf & kPmfAds) != 0;
    v.ads = vs.adsHeld;
    vs.bobCycle = getI(b.ps, 8) & 0xff;
    vs.velocity = {getF(b.ps, 32), getF(b.ps, 36), getF(b.ps, 40)};
    vs.viewHeightTarget = getI(b.ps, 244);
    vs.viewHeight = getF(b.ps, 248);
    vs.lean = getF(b.ps, 76);
    vs.aimSpread = getF(b.ps, 1492);
    v.ring.sequence = getI(b.ps, 164) & 0xff;
    for (int i = 0; i < 4; ++i) {
        v.ring.events[size_t(i)] = getI(b.ps, 168 + 4 * i);
        v.ring.weapons[size_t(i)] = weaponNameFromList(cs, getI(b.ps, 184 + 4 * i));
    }
    return v;
}

int RemoteView::stance(const net::ClientSnapshot& snap) {
    const int f = getI(snap.ps, 12);
    return (f & kPmfProne) ? 2 : (f & kPmfCrouch) ? 1 : 0;
}

void RemoteView::apply(game::World& w, const net::ClientSnapshot& snap, const std::map<int, std::string>& cs, int self,
                       int64_t worldTimeMs, PredictedAmmo* predicted) {
    if (!snap.valid || snap.messageNum == lastSnapshot_) return;
    lastSnapshot_ = snap.messageNum;
    const std::vector<uint8_t>& ps = snap.ps;
    auto weaponName = [&](int i) -> std::string {
        return weaponNameFromList(cs, i);
    };

    // The cursor hint (ps hintType +0x598, hintStringIndex +0x59c): a network client shows the server's hint, not one
    // found in its own copy of the map, whose entities the server's scripts may have removed (docs/spec/net.md section 11).
    {
        const int idx = getI(ps, 1436);
        const std::string* text = idx >= 0 && idx < kHintConfigstringCount ? cfg(cs, kHintConfigstrings + idx) : nullptr;
        const int hint = getI(ps, 1432) & 0xff;
        w.mpSetRemoteWeapon(weaponName(getI(ps, 212))); // the weapon the pickup prompt compares with (the player state's, not the view model's)
        w.mpSetRemoteUseHint(hint, text ? *text : std::string(), hint > 4 ? weaponName(hint - 4) : std::string());
    }

    // A map turret the server put this client on (docs/spec/turrets.md "Multiplayer"): eFlags 0x300 bits, viewLockMode
    // (+1424) and the turret's entity number in viewLockEntity (+1428). Not while following another client.
    {
        const int ef = getI(ps, 160), lockEnt = getI(ps, 1428);
        game::Entity* turret = nullptr;
        if ((ef & 0x300) && lockEnt >= kFirstMapEntity && lockEnt < 1022 && !following(snap))
            for (const auto& e : w.entities())
                if (e->mapIndex > 0 && kFirstMapEntity + e->mapIndex - 1 == lockEnt && e->turret) {
                    turret = e.get();
                    break;
                }
        w.mpSetRemoteTurret(turret, turret ? uint32_t(ef) : 0u, getI(ps, 1424));
    }

    // The local player's shell shock (ps+0x5d8 index, +0x5dc start in server time, +0x5e0 duration): the name is the
    // configstring of the index; the start moves to the world's clock like the HUD times.
    {
        const int index = getI(ps, 1496) & 15;
        const std::string* name = index ? cfg(cs, kShockConfigstrings + index) : nullptr;
        w.mpSetRemoteShellShock(name ? *name : std::string(), int64_t(getI(ps, 1500)) + (worldTimeMs - snap.serverTime),
                                getI(ps, 1504) & 0xffff);
    }

    // The local player: health, team, name, inventory.
    if (game::Entity* p = w.player()) {
        p->obj->set("health", Value(getI(ps, 300)));
        // The hands model: viewModel (ps+0xe4) is a model index (docs/spec/net.md section 11); the front end draws
        // the hands the player's $viewmodel names, as for setviewmodel in a local game.
        if (const int vm = getI(ps, 228) & 0xff; vm > 0)
            if (const std::string* n = cfg(cs, kModelConfigstrings + vm - 1)) p->obj->set("$viewmodel", Value(*n));
        p->obj->set("maxhealth", Value(getI(ps, 308)));
        if (const auto c = snap.clients.find(self); c != snap.clients.end()) {
            p->obj->set("name", Value(clientStateName(c->second)));
            p->obj->set("sessionteam", Value(std::string(teamName(getI(c->second, 4) & 3))));
        }
        game::Inventory& inv = w.inventory(*p);
        inv.owned.clear();
        inv.ammo.clear();
        inv.clip.clear();
        const std::vector<std::string> list = weaponList(cs);
        // The server's counters, with the shots and reloads of the commands it has not run yet on top (docs/spec/net.md
        // section 10): those are what the HUD draws.
        PredictedAmmo::Counters counters;
        for (int i = 0; i < 64; ++i) {
            if (!((uint32_t(getI(ps, 1348 + 4 * (i / 32))) >> (i % 32)) & 1u)) continue;
            const std::string wn = weaponName(i);
            if (wn.empty()) continue;
            inv.owned.insert(wn);
            const int as = weaponAmmoSlot(w, list, wn, false), cs2 = weaponAmmoSlot(w, list, wn, true);
            if (as >= 0 && as < 64) counters.stock[weaponAmmoName(w, wn, false)] = getI(ps, 324 + 4 * as);
            if (cs2 >= 0 && cs2 < 64) counters.clip[weaponAmmoName(w, wn, true)] = getI(ps, 836 + 4 * cs2);
        }
        if (predicted) predicted->adjust(readPs(ps).commandTime, counters);
        inv.ammo = std::move(counters.stock);
        inv.clip = std::move(counters.clip);
        const int slots = getI(ps, 1364);
        inv.slot[1] = weaponName((slots >> 8) & 0xff);
        inv.slot[2] = weaponName((slots >> 16) & 0xff);
        const std::string cur = weaponName(getI(ps, 212));
        if (!cur.empty() && inv.owned.count(cur)) p->obj->set("$weapon", Value(cur));
        else if (inv.owned.empty()) p->obj->set("$weapon", Value());
        inv.offhand = inv.owned.count(weaponName(getI(ps, 208))) ? weaponName(getI(ps, 208)) : std::string();
    }

    // HUD elements: the archival array first, then the second one (docs/spec/net.md section 7.3).
    std::vector<script::ObjectPtr> hud;
    const int64_t offset = worldTimeMs - snap.serverTime;
    for (int base : {net::kPsArchivalHudElemsOffset, net::kPsHudElemsOffset})
        for (int i = 0; i < net::kPsHudElemCount; ++i) {
            if (base + (i + 1) * net::kHudElemSize > int(ps.size())) break;
            auto h = decodeHudElem(ps.data() + base + i * net::kHudElemSize, [&] { return w.vm().newObject("hudelem"); }, cs, offset);
            if (!h) break;
            hud.push_back(std::move(h));
        }
    w.mpSetRemoteHud(std::move(hud));

    // Objectives.
    std::array<game::Objective, 16> objs{};
    for (int i = 0; i < 16; ++i) {
        const int o = net::kPsObjectivesOffset + i * net::kObjectiveSize;
        game::Objective& ob = objs[size_t(i)];
        ob.state = getI(ps, o);
        if (ob.state == 0) continue;
        ob.positions[0] = {getF(ps, o + 4), getF(ps, o + 8), getF(ps, o + 12)};
        const int t = getI(ps, o + 20);
        ob.team = t == 1 ? "axis" : t == 2 ? "allies" : std::string();
        if (const std::string* m = getI(ps, o + 24) > 0 ? cfg(cs, kMaterialConfigstrings + getI(ps, o + 24) - 1) : nullptr) ob.icon = *m;
    }
    w.mpSetRemoteObjectives(objs);

    // Entities: obituary events (once each) and script models.
    std::vector<game::World::RemoteModel> models;
    std::set<std::pair<int, int>> live;
    for (const auto& [number, es] : snap.entities) {
        const int type = getI(es, 4) & 0xff;
        if (type == kEtEvents + kEventObituary) {
            const std::pair<int, int> key{number, getI(es, 84)};
            live.insert(key);
            if (seenEvents_.count(key)) continue;
            game::MpObituary ob;
            const int vic = getI(es, 116) & 1023;
            ob.victim = worldSlot(vic, self);
            const int att = getI(es, 120) & 1023;
            ob.attacker = att >= 1022 ? -1 : worldSlot(att, self);
            // The names and teams from the client states (the client info the obituary reads, docs/spec/mp-hud.md),
            // not from the puppets: a dead victim's player entity may already be out of the snapshot.
            ob.hasClientInfo = true;
            clientInfo(snap, vic, ob.victimName, ob.victimTeam);
            if (att < 1022) clientInfo(snap, att, ob.attackerName, ob.attackerTeam);
            const int b = getI(es, 160) & 0xff;
            ob.modByte = (b & 0x80) != 0;
            if (ob.modByte) ob.mod = b & 0x7f;
            else ob.weapon = weaponName(b);
            w.mpPushRemoteObituary(ob);
        } else if (type == kEtEvents + kEventGrenadeExplode || type == kEtEvents + kEventRocketExplode) {
            // A missile explosion (docs/spec/net.md section 13): its sounds and effects once, without damage.
            const std::pair<int, int> key{number, getI(es, 84)};
            live.insert(key);
            if (seenEvents_.count(key)) continue;
            w.mpRemoteExplosion(type == kEtEvents + kEventRocketExplode, weaponNameFromList(cs, getI(es, 200)),
                                {getF(es, 24), getF(es, 28), getF(es, 32)}, {getF(es, 92), getF(es, 96), getF(es, 100)},
                                getI(es, 136) & 0xff);
        } else if (type == kEtCorpse || type == kEtItem) {
            // A body (the body model of its client's client state) or a dropped weapon (its world model).
            game::World::RemoteModel m;
            m.wire = number;
            if (type == kEtCorpse) {
                // The body model and the attached models (head, helmet) of the client's client state, and the death
                // animation in legsAnim (docs/spec/net.md section 11).
                if (const auto c = snap.clients.find(getI(es, 144) & 63); c != snap.clients.end())
                    clientModels(c->second, cs, m.model, m.attachments);
                if (const int la = getI(es, 204) & 1023; la > 0) m.animNode = la;
                m.animFlip = getI(es, 8) & 2;
                m.animDone = (getI(es, 8) & kEfBodyNew) == 0;
            } else if (const std::string wn = weaponNameFromList(cs, getI(es, 140)); !wn.empty()) {
                if (const auto* def = w.weapon(wn))
                    if (const auto it = def->find("worldmodel"); it != def->end()) m.model = it->second;
            }
            m.origin = {getF(es, 24), getF(es, 28), getF(es, 32)};
            m.angles = {getF(es, 60), getF(es, 64), getF(es, 68)};
            trajectoryPose(es, snap.serverTime, m);
            if (!m.model.empty()) models.push_back(m);
        } else if (type == kEtTurret && number >= kFirstMapEntity) {
            // A map turret: drawn from the map, aimed as the snapshot says.
            game::World::RemoteModel m;
            m.wire = number;
            m.presentOnly = true;
            for (const auto& e : w.entities())
                if (e->mapIndex > 0 && kFirstMapEntity + e->mapIndex - 1 == number) {
                    m.mapNumber = e->number;
                    // Its aim relative to its angles: angles2 pitch and yaw (the server's turret think).
                    if (e->turret) {
                        e->turret->pitch = getF(es, 104);
                        e->turret->yaw = getF(es, 108);
                        // Its shots: each new EV_FIRE_WEAPON_MG42 of the event ring (first sight: none replayed).
                        const int seq = getI(es, 164) & 0xff;
                        auto ts = turretSeq_.find(number);
                        if (ts == turretSeq_.end()) {
                            turretSeq_[number] = seq;
                        } else {
                            int from = ts->second;
                            if (((seq - from) & 0xff) > 4) from = (seq - 4) & 0xff;
                            for (int s = from; s != seq; s = (s + 1) & 0xff)
                                if ((getI(es, 168 + 4 * (s & 3)) & 0xff) == kEvFireWeaponMg42) w.mpRemoteTurretFire(*e);
                            ts->second = seq;
                        }
                    }
                }
            if (m.mapNumber >= 0) models.push_back(m);
        } else if (type == kEtGeneral) {
            game::World::RemoteModel m;
            m.wire = number;
            // A map entity is known by its number (kFirstMapEntity + position in the map's entity list - 1).
            m.mapNumber = -1;
            if (number >= kFirstMapEntity)
                for (const auto& e : w.entities())
                    if (e->mapIndex > 0 && kFirstMapEntity + e->mapIndex - 1 == number) {
                        m.mapNumber = e->number;
                        break;
                    }
            if (getI(es, 156) == 0xffffff) m.model = "*" + std::to_string(getI(es, 140));  // a brush model
            else if (const int mi = getI(es, 140); mi > 0)
                if (const std::string* n = cfg(cs, kModelConfigstrings + mi - 1)) m.model = *n;
            m.origin = {getF(es, 24), getF(es, 28), getF(es, 32)};
            m.angles = {getF(es, 60), getF(es, 64), getF(es, 68)};
            m.hidden = (getI(es, 8) & kEfHidden) != 0;
            trajectoryPose(es, snap.serverTime, m);
            models.push_back(m);
        }
    }
    seenEvents_.insert(live.begin(), live.end());
    // Forget events no longer in the snapshots (their slot may carry a new one with another time).
    std::erase_if(seenEvents_, [&](const auto& k) { return !live.count(k); });
    w.mpSetRemoteModels(models);
}

void RemoteView::trajectoryAt(const std::vector<uint8_t>& ea, const std::vector<uint8_t>* eb, int t, float out[3], int spanMs) {
    const Trajectory pa = readTrajectory(ea, kPosTrajectory);
    if (!eb) {
        evaluateTrajectory(pa, t, out);
        return;
    }
    const Trajectory pb = readTrajectory(*eb, kPosTrajectory);
    const bool rebased = pb.type != pa.type || pb.time != pa.time || std::memcmp(pb.base, pa.base, sizeof pb.base) != 0;
    if (!rebased) {
        evaluateTrajectory(pa, t, out);
        return;
    }
    if (t >= pb.time || !trajectoryMoves(pa)) {
        // The new path, never before its start (a grenade that left the hand waits at the throw point until then).
        evaluateTrajectory(pb, std::max(t, pb.time), out);
        return;
    }
    // The server re-based the trajectory between the two snapshots, in the frame that ends at pb.time: on the old path, the base of the new one
    // is where the hit was (the move ends there for the rest of that frame). Find the time the old path passes closest to
    // it in that span before pb.time and hold the model at the base from then on.
    int hitMs = pb.time;
    float best = -1;
    for (int ms = pb.time - spanMs; ms <= pb.time; ms += 5) {
        float p[3];
        evaluateTrajectory(pa, ms, p);
        const float d = (p[0] - pb.base[0]) * (p[0] - pb.base[0]) + (p[1] - pb.base[1]) * (p[1] - pb.base[1]) + (p[2] - pb.base[2]) * (p[2] - pb.base[2]);
        if (best < 0 || d < best) {
            best = d;
            hitMs = ms;
        }
    }
    if (t < hitMs) evaluateTrajectory(pa, t, out);
    else std::memcpy(out, pb.base, sizeof pb.base);
}

void RemoteView::placeTrajectories(game::World& w, const net::ClientSnapshot& a, const net::ClientSnapshot& b, int t) {
    if (!b.valid) return;
    // The server's frame length is not known here (sv_fps): the snapshots themselves give the span.
    const int spanMs = a.valid && b.serverTime > a.serverTime ? b.serverTime - a.serverTime : game::kServerFrameMs;
    // Over the newer snapshot's entities: one the older snapshot lacks (just thrown) starts its path at its start time.
    for (const auto& [number, es] : b.entities) {
        const int type = getI(es, 4) & 0xff;
        if (type != kEtGeneral && type != kEtItem) continue;
        const std::vector<uint8_t>* ea = nullptr;
        if (a.valid && &a != &b)
            if (const auto it = a.entities.find(number); it != a.entities.end() && (getI(it->second, 4) & 0xff) == type) ea = &it->second;
        const Trajectory pb = readTrajectory(es, kPosTrajectory);
        if (!trajectoryMoves(pb) && !(ea && trajectoryMoves(readTrajectory(*ea, kPosTrajectory)))) continue;
        float o[3];
        if (ea) trajectoryAt(*ea, &es, t, o, spanMs);
        else evaluateTrajectory(pb, std::max(t, pb.time), o);
        game::Vec3 angles{};
        // The angles turn along the current path (TODO(spec): a bounce's new spin before its snapshot time).
        const Trajectory apos = readTrajectory(ea && t < pb.time ? *ea : es, kAposTrajectory);
        const bool turns = trajectoryMoves(apos);
        if (turns) {
            float an[3];
            evaluateTrajectory(apos, std::max(t, apos.time), an);
            angles = {an[0], an[1], an[2]};
        }
        w.mpPlaceRemoteModel(number, {o[0], o[1], o[2]}, turns ? &angles : nullptr);
    }
}

std::string sayCommand(bool team, const std::string& text) {
    return std::string(team ? "say_team" : "say") + " \x15" + text;
}

} // namespace iw2::netgame
