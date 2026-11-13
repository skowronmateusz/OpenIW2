// The weapon side of a network player on the server; implemented from the spec (docs/spec/net.md section 13,
// docs/spec/weapons.md for the weapon state machine).
#include "netgame/netcombat.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include "game/clientinput.h"
#include "game/mpplayers.h"
#include "game/world.h"

namespace iw2::netgame {

using game::Entity;
using game::World;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string defField(World& w, const std::string& weapon, const char* key) {
    const auto* d = w.weapon(weapon);
    if (!d) return {};
    const auto it = d->find(key);
    return it == d->end() ? std::string() : it->second;
}

float defNumber(World& w, const std::string& weapon, const char* key) {
    return float(std::atof(defField(w, weapon, key).c_str()));
}

}  // namespace

void NetCombat::reset() {
    game::PlayerWeapon fresh;
    fresh.lookup = weapon_.lookup;
    fresh.findOffhand = weapon_.findOffhand;
    fresh.readAmmo = weapon_.readAmmo;
    fresh.writeAmmo = weapon_.writeAmmo;
    fresh.binocularDef = weapon_.binocularDef;
    weapon_ = std::move(fresh);
    select_.clear();
    written_.clear();
}

void NetCombat::wire(World& w, int client) {
    if (wired_) return;
    wired_ = true;
    World* wp = &w;
    weapon_.lookup = [wp](const std::string& n) { return wp->weapon(n); };
    weapon_.findOffhand = [wp, client](const std::string& cls) {
        Entity* e = wp->mpClientEntity(client);
        return e ? wp->clientOffhandFor(*e, cls) : std::string();
    };
    weapon_.binocularDef = [wp]() { return wp->binocularWeapon(); };
    // Clip and stock are the inventory's (clip by clipName, stock by ammoName), only for owned weapons.
    weapon_.readAmmo = [wp, client](const std::string& n) -> std::optional<std::pair<int, int>> {
        Entity* e = wp->mpClientEntity(client);
        if (!e) return std::nullopt;
        game::Inventory& inv = wp->inventory(*e);
        const std::string k = lower(n);
        if (!inv.owned.count(k)) return std::nullopt;
        return std::make_pair(inv.clip[defField(*wp, k, "clipname")], inv.ammo[defField(*wp, k, "ammoname")]);
    };
    weapon_.writeAmmo = [wp, client](const std::string& n, int clip, int stock) {
        Entity* e = wp->mpClientEntity(client);
        if (!e) return;
        game::Inventory& inv = wp->inventory(*e);
        const std::string k = lower(n);
        if (!inv.owned.count(k)) return;
        inv.clip[defField(*wp, k, "clipname")] = clip;
        inv.ammo[defField(*wp, k, "ammoname")] = stock;
    };
}

WeaponMove weaponMoveOf(const game::PlayerWeapon& w) {
    WeaponMove m;
    m.adsAllowed = w.adsAllowed();
    m.adsFrac = w.adsFrac();
    m.weaponState = w.stateNumber();
    if (w.equipped()) {
        const std::string ret = w.field("adsOverlayReticle");
        m.breathWeapon = !ret.empty() && ret != "none" && w.field("weaponClass") != "item";
        auto n = [&](const char* k) { return w.number(k); };
        m.params.weaponSpeedScale = n("moveSpeedScale");
        m.params.hipSpread = {n("hipSpreadStandMin"), n("hipSpreadDuckedMin"), n("hipSpreadProneMin"), n("hipSpreadMax"),
                              n("hipSpreadDuckedMax"), n("hipSpreadProneMax"), n("hipSpreadDecayRate"), n("hipSpreadFireAdd"),
                              n("hipSpreadTurnAdd"), n("hipSpreadMoveAdd"), n("hipSpreadDuckedDecay"), n("hipSpreadProneDecay")};
    }
    return m;
}

int weaponStep(game::PlayerWeapon& weapon, int msec, game::PlayerWeapon::Input in) {
    int shots = 0;
    forEachUsercmdPiece(msec, [&](float dt) {
        shots += weapon.update(dt, in);
        in.reload = false;  // the command is its own old command from the second piece on
    });
    return shots;
}

int PredictedWeapon::pendingShots() const {
    int n = 0;
    for (auto it = records_.upper_bound(ackTime_); it != records_.end(); ++it) n += it->second.shots;
    return n;
}

int PredictedWeapon::takeFires() {
    const int predicted = serverFires_ + pendingShots();
    // The played number is ahead: the prediction fell back (the server did not fire a shot predicted in a stall). The
    // server's next shot within kAheadMs takes that number without being played again; later the number is given back.
    if (predicted < played_) {
        if (aheadSince_ == 0) aheadSince_ = std::max(1, stepTime_);
        else if (stepTime_ - aheadSince_ > kAheadMs || stepTime_ < aheadSince_) played_ = predicted, aheadSince_ = 0;
    } else {
        aheadSince_ = 0;
    }
    const int fires = std::max(0, predicted - played_);
    played_ = std::max(played_, predicted);
    return fires;
}

void PredictedWeapon::clear() { *this = PredictedWeapon{}; }

int PredictedWeapon::idle(int commandTime) {
    stepTime_ = commandTime;
    lastStepShots_ = 0;
    return takeFires();
}

int PredictedWeapon::command(int commandTime, const game::PlayerWeapon::Input& in, game::PlayerWeapon& weapon,
                             const Counters& clip, const Counters& stock) {
    // Before the level's first snapshot the commands carry time 0: no step. A time not newer than the last stepped
    // one: the server does not run that command, so nothing runs here either. An older one (the clock was set back)
    // is the new reference, as on the server.
    if (commandTime <= 0) return takeFires();
    const int msec = stepTime_ == 0 || commandTime > stepTime_ ? usercmdMsec(stepTime_, commandTime) : 0;
    stepTime_ = commandTime;
    lastStepShots_ = 0;
    if (msec > 0) {
        const PredictedAmmo::Counters before{clip, stock};
        lastStepShots_ = weaponStep(weapon, msec, in);
        ammo.record(commandTime, before, {clip, stock});
        Record& r = records_[commandTime];
        r.weapon = weapon;
        r.weapon.dropOutput();
        r.in = in;
        r.shots = lastStepShots_;
        while (records_.size() > 256) records_.erase(records_.begin());
    }
    return takeFires();
}

bool PredictedWeapon::snapshot(const PsFields& server, const std::string& serverWeapon, game::PlayerWeapon& weapon,
                               Counters& clip, Counters& stock) {
    // The server's fire events since the last snapshot. The ring holds the last four events; when more were added (a
    // long command after a stall fires once per piece) the older ones are fire events as far as the acknowledged clip
    // of the weapon fell by more than the fire events still in the ring.
    int serverClip = -1;
    if (!serverWeapon.empty() && weapon.readAmmo) {
        const Counters predictedClip = clip, predictedStock = stock;
        clip = ammo.server().clip;
        stock = ammo.server().stock;
        if (const auto a = weapon.readAmmo(serverWeapon)) serverClip = a->first;
        clip = predictedClip;
        stock = predictedStock;
    }
    if (!haveRing_) {
        haveRing_ = true;
    } else {
        int added = (server.eventSequence - ringSequence_) & 0xff;
        if (added > 128) added = 0;  // the sequence went back: a ring started again, not events
        const int lost = std::max(0, added - 4);
        int seen = 0;
        for (int s = (server.eventSequence - (added - lost)) & 0xff; s != server.eventSequence; s = (s + 1) & 0xff) {
            const int ev = server.events[s & 3];
            if (ev == kEvFireWeapon || ev == kEvFireWeaponLastShot) ++seen;
        }
        int fires = seen;
        if (lost > 0 && serverClip >= 0 && ackClip_ >= 0 && serverWeapon == ackWeapon_)
            fires = std::clamp(ackClip_ - serverClip, seen, seen + lost);
        serverFires_ += fires;
    }
    ackClip_ = serverClip;
    ackWeapon_ = serverWeapon;
    ringSequence_ = server.eventSequence;
    ackTime_ = server.commandTime;

    // This client's weapon after the newest command at or before the snapshot's commandTime.
    auto base = records_.upper_bound(server.commandTime);
    if (base == records_.begin()) return false;  // nothing kept that far back
    --base;
    records_.erase(records_.begin(), base);
    const float serverTimer = static_cast<float>(server.weaponTime) * 0.001f;
    const game::PlayerWeapon& kept = base->second.weapon;
    constexpr float kToleranceSeconds = 0.002f;  // the wire's whole milliseconds
    if (base->first == server.commandTime && kept.name() == serverWeapon && kept.stateNumber() == server.weaponState &&
        std::fabs(std::max(0.0f, kept.timer()) - serverTimer) <= kToleranceSeconds)
        return false;  // as predicted: the kept commands would run to the same result
    // A rebuild needs the same weapon in hand from the kept command on, on both sides, and no change of the weapon by
    // the front end since the newest kept command (an equip, a weapon taken away): the kept weapons do not have it.
    if (serverWeapon.empty() || base->second.in.dead) return false;
    if (const game::PlayerWeapon& newest = records_.rbegin()->second.weapon;
        newest.name() != weapon.name() || newest.holstered() != weapon.holstered())
        return false;
    for (auto it = base; it != records_.end(); ++it)
        if (it->second.weapon.name() != serverWeapon || it->second.in.dead) return false;
    // The inventory as the server sent it; the replayed commands change it again and are recorded anew.
    const Counters clipBefore = clip, stockBefore = stock;
    clip = ammo.server().clip;
    stock = ammo.server().stock;
    game::PlayerWeapon w = kept;
    if (!w.applyServer(server.weaponState, serverTimer)) {
        clip = clipBefore;
        stock = stockBefore;
        return false;
    }
    std::vector<std::string> sounds = weapon.takeSounds();
    std::vector<game::PlayerWeapon::Event> events = weapon.takeEvents();
    for (std::string& s : w.takeSounds()) sounds.push_back(std::move(s));
    for (game::PlayerWeapon::Event& e : w.takeEvents()) events.push_back(std::move(e));
    ammo.clearPending();
    Record start{w, base->second.in, 0};
    if (base->first != server.commandTime) records_.erase(base);
    records_[server.commandTime] = std::move(start);
    int prev = server.commandTime;
    for (auto it = records_.upper_bound(server.commandTime); it != records_.end(); ++it) {
        const PredictedAmmo::Counters before{clip, stock};
        it->second.shots = weaponStep(w, usercmdMsec(prev, it->first), it->second.in);
        w.dropOutput();
        ammo.record(it->first, before, {clip, stock});
        it->second.weapon = w;
        prev = it->first;
    }
    weapon = std::move(w);
    weapon.addOutput(std::move(sounds), std::move(events));
    ++rebuilds_;
    return true;
}

void NetCombat::think(World& w, int client, const net::UserCmd& cmd, game::PlayerState& ps, int msec,
                      const std::string& cmdWeapon) {
    wire(w, client);
    Entity* e = w.mpClientEntity(client);
    if (!e) return;
    const uint32_t buttons = static_cast<uint32_t>(cmd.buttons);
    const uint32_t pressed = buttons & ~oldButtons_;
    oldButtons_ = buttons;
    const script::Value hp = e->obj->get("health");
    const bool alive = hp.isNumber() && hp.asFloat() > 0;
    // Not in the game (spectator, dead, intermission): nothing in hand; the next life starts with a fresh weapon state.
    if (!alive || e->obj->get("sessionstate").toString() != "playing") {
        if (weapon_.equipped() || weapon_.holstered()) reset();
        lastCmdWeapon_ = cmdWeapon;
        return;
    }
    // The command's weapon (the client's weapon select): a scripted change of $weapon wins until the client chooses
    // another weapon (a change of the command's weapon byte) that it owns.
    if (e->obj->get("$weapontaken").truthy()) {
        e->obj->set("$weapontaken", script::Value());
        weapon_.weaponTaken();
    }
    const script::Value wv = e->obj->get("$weapon");
    const std::string scripted = wv.isString() && wv.asString() != "none" ? wv.asString() : std::string();
    if (scripted != written_) {
        select_ = scripted;
        if (!scripted.empty() && scripted != weapon_.name()) scriptedSelect_ = scripted;
    }
    if (!cmdWeapon.empty() && cmdWeapon != lastCmdWeapon_ && w.inventory(*e).owned.count(lower(cmdWeapon))) select_ = cmdWeapon;
    lastCmdWeapon_ = cmdWeapon;
    if (!weapon_.equipped() && !weapon_.holstered())
        if (const auto* def = w.weapon(select_)) weapon_.equip(select_, *def);

    game::PlayerWeapon::Input wi;
    wi.fire = (buttons & game::Button::Attack) != 0;
    wi.ads = ps.ads;
    wi.reload = (pressed & (game::Button::Reload | game::Button::UseReload)) != 0;
    wi.melee = (buttons & game::Button::Melee) != 0;
    wi.frag = (buttons & game::Button::Frag) != 0;
    wi.smoke = (buttons & game::Button::Smoke) != 0;
    wi.weapon = select_;
    wi.holster = game::moveLowersWeapon(ps, false);
    // disableWeapon (MP builtin): the weapon is put away until enableWeapon.
    if (size_t(client) < w.mpState().clients.size() && w.mpState().clients[size_t(client)].weaponDisabled) wi.holster = true;
    wi.proneMoving = ps.stance == game::Stance::Prone && (cmd.forwardmove != 0 || cmd.rightmove != 0);
    wi.binoculars = (buttons & game::Button::Binoculars) != 0;
    wi.use = (buttons & (game::Button::Activate | game::Button::UseReload)) != 0;
    weapon_.grenadeCook = w.cvarFloat("iw2_grenadeCook", 1.0f) != 0;
    const int shots = weaponStep(weapon_, msec, wi);
    // Entering the offhand prepare state adds the prepare event (0x4df1b4, parm the offhand weapon).
    if (weapon_.stateNumber() == 13 && statePrev_ != 13) {
        ring_.push(kEvPrepOffhand, weaponIndex ? weaponIndex(weapon_.viewWeaponName()) : 0);
        w.mpPushClientEvent(client, kEvPrepOffhand, weapon_.viewWeaponName());
    }
    statePrev_ = weapon_.stateNumber();

    // The view: eye at the view height plus the lean offset, the command's angles.
    const float pitch = float(int16_t(cmd.angles[0] & 0xffff)) * 360.0f / 65536.0f;
    const float yaw = float(cmd.angles[1] & 0xffff) * 360.0f / 65536.0f;
    const auto lo = game::leanOffset(yaw, ps.lean);
    const game::Vec3 eye{ps.origin[0] + lo[0], ps.origin[1] + lo[1], ps.origin[2] + ps.viewHeight + lo[2]};
    const float pr = pitch * float(M_PI) / 180.0f, yr = yaw * float(M_PI) / 180.0f;
    const game::Vec3 fwd{std::cos(pr) * std::cos(yr), std::cos(pr) * std::sin(yr), -std::sin(pr)};
    const game::Vec3 angles{pitch, yaw, 0};
    const int windex = weaponIndex ? weaponIndex(weapon_.name()) : 0;

    game::MpPlayerInput* anim = w.mpPlayerInput(client);
    for (int i = 0; i < shots; ++i) {
        const std::string wn = weapon_.name();
        game::HipSpread hs;
        hs.standMin = defNumber(w, wn, "hipspreadstandmin");
        hs.duckedMin = defNumber(w, wn, "hipspreadduckedmin");
        hs.proneMin = defNumber(w, wn, "hipspreadpronemin");
        hs.max = defNumber(w, wn, "hipspreadmax");
        hs.duckedMax = defNumber(w, wn, "hipspreadduckedmax");
        hs.proneMax = defNumber(w, wn, "hipspreadpronemax");
        hs.fireAdd = defNumber(w, wn, "hipspreadfireadd");
        const float spread = game::fireSpreadDegrees(hs, ps.viewHeight, ps.aimSpread, weapon_.adsFrac(), defNumber(w, wn, "adsspread"));
        game::addFireSpread(ps, hs, weapon_.adsFrac());
        const bool last = weapon_.clip() == 0 && i == shots - 1;
        e = w.mpClientEntity(client);
        if (!e) return;
        e->obj->set("$weapon", script::Value(wn));
        w.fireAntilagTime = antilagTime;
        w.fireAntilagFine = antilagFine;
        const game::Vec3 kick = w.clientFire(*e, eye, fwd, last, spread);
        w.fireAntilagTime = -1;
        w.fireAntilagFine = false;
        for (int k = 0; k < 3; ++k) ps.velocity[size_t(k)] -= kick[size_t(k)];
        ring_.push(last ? kEvFireWeaponLastShot : kEvFireWeapon, windex);
        ++shots_;
        if (anim) anim->fired = true;
    }
    for (const auto& ev : weapon_.takeEvents()) {
        e = w.mpClientEntity(client);
        if (!e) return;
        using K = game::PlayerWeapon::Event;
        switch (ev.kind) {
        case K::ReloadBegin:
            if (anim && !ev.weapon.empty()) anim->reloaded = true; // a reload begins (the empty-named ones only clear the aim)
            break;
        case K::StateEvent:
            // Reload, reload start / end, rechamber, raise, put away: the event of the state change (docs/spec/net.md
            // section 13 "Entity events"), for the other clients and the killcam.
            ring_.push(ev.code, weaponIndex ? weaponIndex(ev.weapon) : windex);
            w.mpPushClientEvent(client, ev.code, ev.weapon);
            break;
        case K::MeleeSwing:
            ring_.push(kEvMeleeSwipe, windex);
            w.mpPushClientEvent(client, kEvMeleeSwipe, weapon_.name());
            break;
        case K::MeleeHit:
            ring_.push(kEvFireMelee, windex);
            w.mpPushClientEvent(client, kEvFireMelee, weapon_.name());
            w.clientMelee(*e, eye, angles);
            break;
        case K::OffhandThrow:
            ring_.push(kEvUseOffhand, weaponIndex ? weaponIndex(ev.weapon) : 0);
            w.mpPushClientEvent(client, kEvUseOffhand, ev.weapon);
            w.clientThrowGrenade(*e, ev.weapon, eye, angles, ps.velocity, weapon_.takeFuseMs());
            break;
        case K::OffhandExplode:
            ring_.push(kEvGrenadeSuicide, 0);
            w.clientGrenadeInHand(*e, eye, ev.weapon);
            break;
        case K::OutOfAmmo: {
            const std::string next = w.outOfAmmoSwitch(w.inventory(*e), ev.weapon);
            if (!next.empty()) select_ = next;
            break;
        }
        case K::NoAmmo: ring_.push(kEvNoAmmo, windex); break;
        case K::WeaponRaised: ps.aimSpread = ev.alt ? std::max(ps.aimSpread, 128.0f) : 255.0f; break;
        default: break;
        }
    }
    (void)weapon_.takeSounds();  // the clients play the sounds from the events
    e = w.mpClientEntity(client);
    if (e && weapon_.equipped() && alive) {
        e->obj->set("$weapon", script::Value(weapon_.name()));
        written_ = weapon_.name();
    }
    if (anim) anim->ads = weapon_.adsFrac() > 0.5f;
}

} // namespace iw2::netgame
