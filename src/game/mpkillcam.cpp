// Multiplayer frame archive and killcam, implemented from the spec in docs/spec/mp-hud.md "Killcam".
// VAs in the comments are VAs in the 1.3 Linux dedicated server.
#include "game/mpkillcam.h"
#include "game/clienttime.h"
#include "game/mpbots.h"
#include "game/playerweapon.h"
#include "game/pmove.h"
#include "game/world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace iw2::game {

using script::CallContext;
using script::Value;

namespace {
// sv_fps (int dvar registered at 0x8093ed7, default 20, 10..1000). This port's server always runs 50 ms frames.
constexpr int kServerFps = kMpServerFps;

// archivetime is a float field kept as integer milliseconds (setter 0x80f8420: seconds * 1000 truncated; getter
// 0x80f8460: ms * 0.001).
int archiveMsOf(const Value& v) { return v.isNumber() ? int(std::trunc(v.asFloat() * 1000.0f)) : 0; }
Value archiveValue(int ms) { return Value(float(ms) * 0.001f); }

float lerpAngle(float a, float b, float f) { return a + std::remainder(b - a, 360.0f) * f; }
} // namespace

int viewAnimKeyIndex(const std::string& key) {
    for (int i = 0; i < kViewAnimKeyCount; ++i)
        if (key == kViewAnimKeys[i]) return i;
    return 0;
}

const char* viewAnimKeyName(int index) { return index > 0 && index < kViewAnimKeyCount ? kViewAnimKeys[index] : kViewAnimKeys[0]; }

// The view state of a client as its movers record it: the weapon state machine's weapon, state, animation (by key, with
// its own elapsed time: animTime * animRate) and aim, the movement state's bob cycle, velocity, lean and view heights.
MpViewState mpViewStateOf(const PlayerWeapon& w, const PlayerState& ps) {
    MpViewState v;
    v.weapon = w.equipped() ? w.name() : std::string();
    v.viewWeapon = w.viewWeaponName();
    if (v.viewWeapon.empty()) v.viewWeapon = v.weapon;
    v.weaponState = w.stateNumber();
    v.animKey = w.animKey();
    v.animMs = int(w.animTime() * w.animRate() * 1000.0f);
    v.adsFrac = w.adsFrac();
    v.adsHeld = w.adsHeld();
    v.bobCycle = ps.bobCycle;
    v.velocity = {ps.velocity[0], ps.velocity[1], ps.velocity[2]};
    v.viewHeightTarget = ps.viewHeightTarget;
    v.viewHeight = ps.viewHeight;
    v.lean = ps.lean;
    v.aimSpread = ps.aimSpread;
    return v;
}

std::string mpViewStateText(const MpViewState& v) {
    char b[512];
    std::snprintf(b, sizeof b, "weapon '%s' view '%s' state %d anim %s@%d ads %.4f%s bob %d vel %.3f,%.3f,%.3f heights %d/%.3f lean %.3f spread %.1f",
                  v.weapon.c_str(), v.viewWeapon.c_str(), v.weaponState, v.animKey.c_str(), v.animMs, double(v.adsFrac), v.adsHeld ? " held" : "",
                  v.bobCycle, double(v.velocity[0]), double(v.velocity[1]), double(v.velocity[2]), v.viewHeightTarget, double(v.viewHeight),
                  double(v.lean), double(v.aimSpread));
    return b;
}

void World::mpSetViewState(int client, const MpViewState& v) {
    if (!mp_ || client < 0 || size_t(client) >= mp_->clients.size()) return;
    mp_->clients[size_t(client)].view = v;
}

void World::mpSetLocalViewState(const MpViewState& v) {
    if (mp_) mpSetViewState(mp_->localClient, v);
}

void World::mpPushClientEvent(int client, int event, const std::string& weapon) {
    if (!mp_ || mpRemote_ || client < 0 || size_t(client) >= mp_->clients.size()) return;
    mp_->clients[size_t(client)].events.push(event, weapon);
}

void World::mpPushLocalClientEvent(int event, const std::string& weapon) {
    if (mp_) mpPushClientEvent(mp_->localClient, event, weapon);
}

const MpEventRing* World::mpClientEvents(int client) const {
    if (!mp_ || client < 0 || size_t(client) >= mp_->clients.size()) return nullptr;
    return &mp_->clients[size_t(client)].events;
}

// spectatorclient is a client number; the scripts store the killer's entity number (_killcam.gsc:46), which is the same
// in the original (client entities are 0..sv_maxclients-1). This port numbers entities differently, so the client is the
// one whose entity has that number.
int World::mpClientOfNumber(int number) const {
    if (!mp_ || number < 0) return -1;
    for (size_t i = 0; i < mp_->clients.size(); ++i)
        if (mp_->clients[i].ent && mp_->clients[i].state != MpClientState::Free && mp_->clients[i].ent->number == number) return int(i);
    return -1;
}

const ArchivedEntity* ArchivedFrame::find(int number) const {
    const auto it = std::lower_bound(entities.begin(), entities.end(), number,
                                     [](const ArchivedEntity& e, int n) { return e.number < n; });
    return it != entities.end() && it->number == number ? &*it : nullptr;
}

// 0x8099c04.
int64_t archiveLookup(const MpArchive& a, int& ms, int fps) {
    if (!a.enabled || ms <= 0) return -1;
    int64_t start = a.count - int64_t(ms) * fps / 1000;
    if (start < a.count - MpArchive::kFrames) {
        start = a.count - MpArchive::kFrames;
        ms = int((a.count - start) * 1000 / fps);
    }
    if (start < 0) {
        start = 0;
        ms = int(a.count * 1000 / fps);
    }
    for (int64_t n = start; n < a.count; ++n)
        if (a.frame(n)) return n;
    ms = 0;
    return -1;
}

std::pair<int64_t, int64_t> killcamReplayRange(int64_t replayed, int64_t shown) {
    if (shown < 0) return {0, -1};
    if (replayed < 0 || replayed > shown) return {shown, shown};
    return {replayed + 1, shown};
}

int killcamFireAge(const MpArchive& a, int64_t n, int client, int maxFrames) {
    const ArchivedFrame* cur = a.frame(n);
    if (!cur || client < 0) return -1;
    for (int64_t m = n; m > n - maxFrames; --m) {
        const ArchivedFrame* f = a.frame(m);
        const ArchivedFrame* p = a.frame(m - 1);
        if (!f || !p) return -1;
        if (size_t(client) >= f->clients.size() || size_t(client) >= p->clients.size()) return -1;
        const ArchivedClient& fc = f->clients[size_t(client)];
        const ArchivedClient& pc = p->clients[size_t(client)];
        if (fc.active && pc.active && fc.fireCount != pc.fireCount) return int(cur->timeMs - f->timeMs);
    }
    return -1;
}

void World::registerKillcamBuiltins() {
    // setarchive(enable) (0x81178ea -> 0x8093312): turns the frame archive on or off.
    vm_.registerBuiltin("setarchive", [this](CallContext& c) {
        mp_->archive.enabled = c.argc() > 0 && c.arg(0).truthy();
        return Value();
    });
}

// The end of the server frame: the spectator's followed player state (0x80f69c4..0x80f6ad7), the snapshot's trimming of
// archivetime (0x809a408 -> 0x8099c04), then the frame is archived (0x809b016).
void World::mpKillcamEndFrame() {
    MpArchive& ar = mp_->archive;
    for (MpClient& cl : mp_->clients) {
        if (cl.state == MpClientState::Free || !cl.ent) continue;
        const auto& o = cl.ent->obj;
        int ms = archiveMsOf(o->get("archivetime"));
        const int follow = mpClientOfNumber(o->get("spectatorclient").isNumber() ? o->get("spectatorclient").asInt() : -1);
        if (ms < 0) ms = 0;
        if (o->get("sessionstate").toString() == "spectator" && o->get("spectatorclient").isNumber() &&
            o->get("spectatorclient").asInt() >= 0) {
            const int offset = o->get("psoffsettime").isNumber() ? o->get("psoffsettime").asInt() : 0;
            for (;;) {
                bool found = false;
                int t = ms - offset;
                if (t <= 0) { // the live state of an active client (0x8099d30)
                    found = size_t(follow) < mp_->clients.size() && mp_->clients[size_t(follow)].state == MpClientState::Active;
                } else if (const int64_t n = archiveLookup(ar, t, kServerFps); n >= 0) {
                    const ArchivedFrame* f = ar.frame(n);
                    found = size_t(follow) < f->clients.size() && f->clients[size_t(follow)].active;
                }
                if (found) break;
                if (ms == 0) {
                    o->set("spectatorclient", Value(int32_t(-1)));
                    break;
                }
                ms = std::max(0, ms - kServerFrameMs);
            }
        }
        if (ms > 0) archiveLookup(ar, ms, kServerFps); // the snapshot trims it to the archived history
        if (archiveMsOf(o->get("archivetime")) != ms) o->set("archivetime", archiveValue(ms));
    }
    if (!ar.enabled) {
        ar.pending.clear();
        return;
    }
    ArchivedFrame f;
    f.timeMs = timeMs_;
    f.events = std::exchange(ar.pending, {});
    for (const auto& up : entities_) {
        const Entity& e = *up;
        if (e.obj->removed || e.isSpawner || e.number < 0) continue;
        const Value m = e.obj->get("model");
        if (!m.isString() || m.asString().empty()) continue;
        const Value o = e.obj->get("origin"), a = e.obj->get("angles");
        if (!o.isVector()) continue;
        ArchivedEntity ae;
        ae.number = e.number;
        ae.origin = o.asVector();
        ae.angles = a.isVector() ? a.asVector() : Vec3{0, 0, 0};
        ae.model = m.asString();
        for (const auto& at : e.attachments) ae.attachments.emplace_back(at.model, at.tag);
        if (e.anim) {
            ae.tree = &e.anim->tree();
            const auto& ns = e.anim->nodeStates();
            for (size_t i = 0; i < ns.size(); ++i)
                if (ns[i].weight > 0 || ns[i].goalWeight > 0) ae.anim.emplace_back(int(i), ns[i]);
        }
        ae.hidden = e.obj->get("$hidden").truthy();
        if (e.classname.rfind("weapon_", 0) == 0) ae.itemWeapon = e.classname.substr(7);
        if (e.isPlayer) {
            ae.sessionstate = e.obj->get("sessionstate").toString();
            ae.weaponModel = mpPlayerWeaponModel(e);
        }
        if (const Value sl = e.obj->get("$bodyslot"); e.classname == "playercorpse" && sl.isNumber()) {
            const Value bc = e.obj->get("$bodyclient"), da = e.obj->get("$deathanim");
            ae.bodySlot = sl.asInt();
            ae.bodyClient = bc.isNumber() ? bc.asInt() : 0;
            ae.bodyAnim = da.isNumber() ? da.asInt() : -1;
            ae.bodyFlip = e.obj->get("$bodyflip").asInt();
            ae.bodyBornMs = e.obj->get("$bodyborn").asInt();
            ae.bodyThinkMs = e.obj->get("$bodythink").isNumber() ? e.obj->get("$bodythink").asInt() : ae.bodyBornMs;
        }
        // Missiles: the trajectories of their entity state (the archive keeps whole entity states, 0x809b016).
        if (e.grenade) {
            const GrenadeState& g = *e.grenade;
            ae.missile = 1;
            ae.trType = g.trType;
            ae.trTime = g.trTime;
            ae.trBase = g.trType == 5 ? g.trBase : ae.origin;
            ae.trDelta = g.trType == 5 ? g.trDelta : Vec3{0, 0, 0};
            if (g.trType != 0 && !g.inHand) {
                ae.aType = 2;
                ae.aTime = g.aTime;
                ae.aBase = g.aBase;
                ae.aDelta = g.aDelta;
            }
        } else if (e.rocket) {
            const RocketState& r = *e.rocket;
            ae.missile = 2;
            ae.trType = 2;
            ae.trTime = r.trTime;
            ae.trBase = r.trBase;
            ae.trDelta = r.trDelta;
        }
        f.entities.push_back(std::move(ae));
    }
    std::sort(f.entities.begin(), f.entities.end(), [](const ArchivedEntity& x, const ArchivedEntity& y) { return x.number < y.number; });
    f.clients.resize(mp_->clients.size());
    for (size_t i = 0; i < mp_->clients.size(); ++i) {
        const MpClient& cl = mp_->clients[i];
        ArchivedClient& ac = f.clients[i];
        if (cl.state != MpClientState::Active || !cl.ent) continue;
        ac.active = true;
        ac.entity = cl.ent->number;
        const Value o = cl.ent->obj->get("origin"), a = cl.ent->obj->get("angles");
        const Vec3 org = o.isVector() ? o.asVector() : Vec3{0, 0, 0};
        float pitch = 0, yaw = a.isVector() ? a.asVector()[1] : 0.0f, viewHeight = 60.0f;
        const MpBot* bot = nullptr;
        if (bots_)
            for (const MpBot& b : bots_->bots)
                if (b.client == int(i)) bot = &b;
        if (cl.ent == player_) {
            pitch = playerCmd_.viewAngles[0];
            yaw = playerCmd_.viewAngles[1];
            viewHeight = playerCmd_.viewHeight;
        } else if (bot) {
            pitch = bot->pitch;
            yaw = bot->yaw;
            if (bot->psValid) viewHeight = bot->ps.viewHeight;
        } else { // a network client: its newest command's angles and its movement's view height (mpSetView)
            pitch = cl.viewAngles[0];
            yaw = cl.viewAngles[1];
            viewHeight = cl.viewHeight;
        }
        ac.origin = org;
        ac.eye = {org[0], org[1], org[2] + viewHeight};
        ac.viewAngles = {pitch, yaw, 0};
        ac.weapon = cl.ent->obj->get("$weapon").toString();
        if (const Value vm = cl.ent->obj->get("$viewmodel"); vm.isString()) ac.hands = vm.asString();
        // The rest of the player state the client's HUD reads (0x80f6916 copies the whole player state).
        ac.team = mpTeamOf(cl.ent);
        ac.fireCount = cl.fireCount;
        ac.lastShot = cl.lastShot;
        const Value hv = cl.ent->obj->get("health"), mv = cl.ent->obj->get("maxhealth");
        ac.health = hv.isNumber() ? hv.asInt() : 0;
        ac.maxHealth = mv.isNumber() ? mv.asInt() : 100;
        // The weapon side the original's whole player state carries (weapon state and time, aim fraction, bob cycle,
        // velocity, view heights): the recorded view state of the client's mover, and its event ring.
        // A client that is not playing (dead, spectating) has nothing in hand (its mover does not record it any more).
        ac.view = cl.ent->obj->get("sessionstate").toString() == "playing" ? cl.view : MpViewState{};
        if (ac.view.weapon == "none") ac.view.weapon.clear();
        if (ac.view.viewWeapon.empty() || !(ac.view.weaponState >= 12 && ac.view.weaponState <= 16)) ac.view.viewWeapon = ac.view.weapon;
        // The weapon in hand is the weapon state machine's (the player state's weapon changes when a raise completes,
        // not when a script or the bot selects the next one); a client without a record keeps the entity's.
        if (!ac.view.weapon.empty()) ac.weapon = ac.view.weapon;
        ac.ring = cl.events;
        ac.shockIndex = cl.shockIndex;
        ac.shockStartMs = cl.shockStartMs;
        ac.shockDurationMs = cl.shockDurationMs;
        if (!ac.weapon.empty() && ac.weapon != "none") {
            Inventory& inv = inventory(*cl.ent);
            std::string k = ac.weapon;
            for (char& ch : k) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            if (inv.owned.count(k)) {
                const auto ci = inv.clip.find(weaponField(k, "clipName"));
                const auto si = inv.ammo.find(weaponField(k, "ammoName"));
                ac.clip = ci == inv.clip.end() ? 0 : ci->second;
                ac.stock = si == inv.ammo.end() ? 0 : si->second;
            }
        }
    }
    // Debug: IW2_KILLCAM_TRACE=1 logs every archived client state (docs/debug-flags.md); the killcam view logs its own
    // lines, so what the killcam shows can be compared with what the client did at that server time.
    static const bool trace = std::getenv("IW2_KILLCAM_TRACE") != nullptr;
    if (trace)
        for (size_t i = 0; i < f.clients.size(); ++i) {
            const ArchivedClient& ac = f.clients[i];
            if (!ac.active) continue;
            const ArchivedEntity* ae = f.find(ac.entity);
            std::printf("kctrace srv t=%lld c=%zu ent=%d org=%.1f,%.1f,%.1f eye=%.1f,%.1f,%.1f ang=%.1f,%.1f fire=%d clip=%d ss=%s hidden=%d\n",
                        static_cast<long long>(f.timeMs), i, ac.entity, double(ac.origin[0]), double(ac.origin[1]), double(ac.origin[2]),
                        double(ac.eye[0]), double(ac.eye[1]), double(ac.eye[2]), double(ac.viewAngles[0]), double(ac.viewAngles[1]),
                        ac.fireCount, ac.clip, ae ? ae->sessionstate.c_str() : "-", ae ? int(ae->hidden) : -1);
        }
    ar.frames.push_back(std::move(f));
    ++ar.count;
    while (ar.frames.size() > size_t(MpArchive::kFrames)) ar.frames.pop_front();
    mpKillcamReplayEvents();
}

// The local client in the killcam (a spectator following a client with a positive archivetime): the frame it is shown.
int64_t World::mpKillcamFrame() const {
    if (!mp_ || mp_->localClient < 0 || size_t(mp_->localClient) >= mp_->clients.size()) return -1;
    const MpClient& lc = mp_->clients[size_t(mp_->localClient)];
    if (!lc.ent || lc.ent->obj->get("sessionstate").toString() != "spectator") return -1;
    const Value sc = lc.ent->obj->get("spectatorclient");
    if (mpClientOfNumber(sc.isNumber() ? sc.asInt() : -1) < 0) return -1;
    int ms = archiveMsOf(lc.ent->obj->get("archivetime"));
    if (ms <= 0) return -1;
    return archiveLookup(mp_->archive, ms, kServerFps);
}

// The killcam's client receives the archived snapshots with their entity events, so the shots of the replayed frames
// are seen and heard again: the fire sounds (the followed client's own as its player sound without a position), the
// impact effects and the tracers.
void World::mpKillcamReplayEvents() {
    MpArchive& ar = mp_->archive;
    const int64_t shown = mpKillcamFrame();
    if (shown < 0) {
        ar.replayed = -1;
        return;
    }
    const auto [from, to] = killcamReplayRange(ar.replayed, shown);
    ar.replayed = shown;
    const MpClient& lc = mp_->clients[size_t(mp_->localClient)];
    const Value sc = lc.ent->obj->get("spectatorclient");
    const int follow = mpClientOfNumber(sc.isNumber() ? sc.asInt() : -1);
    for (int64_t n = from; n <= to; ++n)
        if (const ArchivedFrame* f = ar.frame(n)) {
            // The followed client's other events of the frame (its ring against the frame before: reload, melee swipe,
            // pull-back, throw) as the own view's sounds; the shots are the archived sound events below.
            if (const ArchivedFrame* p = ar.frame(n - 1); p && size_t(follow) < f->clients.size() && size_t(follow) < p->clients.size()) {
                const MpEventRing& r = f->clients[size_t(follow)].ring;
                const int prev = p->clients[size_t(follow)].ring.sequence;
                int first = prev;
                if (((r.sequence - prev) & 0xff) > 4) first = (r.sequence - 4) & 0xff;
                for (int q = first; q != r.sequence; q = (q + 1) & 0xff)
                    if (const int ev = r.events[size_t(q & 3)]; ev != mpev::kFireWeapon && ev != mpev::kFireWeaponLastShot)
                        mpFollowEventSound(ev, r.weapons[size_t(q & 3)]);
            }
            for (const ArchivedEvent& ev : f->events) {
                if (ev.kind == ArchivedEvent::Explosion) { // the explosion's sounds and effects, not logged again
                    killcamReplay_ = true;
                    if (ev.rocket) missileExplosionEvent(ev.name, ev.a, ev.b, ev.surface);
                    else grenadeExplosionEffects(ev.name, ev.a, ev.b, ev.surface);
                    killcamReplay_ = false;
                } else if (ev.kind == ArchivedEvent::Fx) fxEvents_.push_back({ev.name, ev.a, ev.b});
                else if (ev.kind == ArchivedEvent::Tracer) tracerEvents_.push_back({ev.a, ev.b});
                else if (ev.shooter >= 0 && ev.shooter == follow) {
                    float sec = 0;
                    playSound(ev.playerName.empty() ? ev.name : ev.playerName, nullptr, &sec, ev.owner);
                } else {
                    float sec = 0;
                    playSound(ev.name, &ev.a, &sec, ev.owner);
                }
            }
        }
}

bool World::mpLocalInKillcam() const { return mpKillcamFrame() >= 0; }

void World::mpArchiveEvent(ArchivedEvent ev) {
    if (mp_ && !mpRemote_ && mp_->archive.enabled) mp_->archive.pending.push_back(std::move(ev));
}

void World::mpLogExplosion(bool rocket, const std::string& weapon, const Vec3& at, const Vec3& dir, int surface) {
    if (!mp_ || mpRemote_ || killcamReplay_) return;
    mp_->explosionLog.push_back({rocket, weapon, at, dir, surface});
    ArchivedEvent ev;
    ev.kind = ArchivedEvent::Explosion;
    ev.name = weapon;
    ev.a = at;
    ev.b = dir;
    ev.rocket = rocket;
    ev.surface = surface;
    mpArchiveEvent(std::move(ev));
}

bool World::mpKillcamShellShock(ShellShockState& out) const {
    const int64_t n = mpKillcamFrame();
    const ArchivedFrame* f = n >= 0 ? mp_->archive.frame(n) : nullptr;
    if (!f) return false;
    const Value sc = mp_->clients[size_t(mp_->localClient)].ent->obj->get("spectatorclient");
    const int follow = mpClientOfNumber(sc.isNumber() ? sc.asInt() : -1);
    out = {};
    if (size_t(follow) < f->clients.size() && f->clients[size_t(follow)].shockIndex > 0) {
        const ArchivedClient& c = f->clients[size_t(follow)];
        // The start moves with the replay: the shown frame's time is now (the snapshot time stays live).
        out = {c.shockIndex, c.shockStartMs + (timeMs_ - f->timeMs), c.shockDurationMs};
    }
    return true;
}

// 0x8099dac: the archived player state origin of a client at `timeMs`. The newer frame is the one at or after the time
// (the last archived frame is the current server frame), the older one the frame before it; when a frame is missing the
// search goes on away from the time (older frames for the older one, newer ones for the newer one), at most 10 frames.
// Both found: (1 - f) * older + f * newer with f = (time mod frame) / (frame span between them); only one: that one.
bool World::mpArchivedClientOrigin(int client, int timeMs, Vec3& out) const {
    if (!mp_ || client < 0) return false;
    const MpArchive& ar = mp_->archive;
    const int frameMs = 1000 / kServerFps;
    const int base = int(timeMs_ / frameMs) * frameMs;
    const int back = (base - timeMs) / frameMs;
    const auto at = [&](int ms, Vec3& o) { // the archive lookup `ms` back (0x809a0b6 -> 0x8099c04) and the client in it
        int t = ms;
        const int64_t n = archiveLookup(ar, t, kServerFps);
        if (n < 0) return false;
        const ArchivedFrame* f = ar.frame(n);
        if (size_t(client) >= f->clients.size() || !f->clients[size_t(client)].active) return false;
        o = f->clients[size_t(client)].origin;
        return true;
    };
    // 0x817db80 = 2, 0x817db84 = 1: for a time between two frames these are the frames before and after it; a time
    // exactly on a frame takes that frame as the newer one with weight 0 (the frame before is used).
    int t0 = (back + 2) * frameMs, t1 = (back + 1) * frameMs;
    Vec3 older{}, newer{};
    bool haveOlder = false, haveNewer = false;
    for (int i = 0; i < 10 && !(haveOlder = at(t0, older)); ++i) t0 += frameMs;
    for (int i = 0; i < 10 && !(haveNewer = at(t1, newer)); ++i) t1 -= frameMs;
    if (!haveOlder && !haveNewer) return false;
    if (!haveNewer) { out = older; return true; }
    if (!haveOlder) { out = newer; return true; }
    const float f = t0 != t1 ? float(timeMs % frameMs) / float(t0 - t1) : 0.0f;
    for (size_t k = 0; k < 3; ++k) out[k] = (1.0f - f) * older[k] + newer[k] * f;
    return true;
}

// 0x811fc76: with g_antilag on and the time more than one server frame back, every playing client (connected, session
// state playing) with an archived origin at the time is moved there (unlinked, origin set, linked again).
void World::mpAntilagRewind(int timeMs) {
    mpAntilagRestore();
    if (!mp_ || cvarFloat("g_antilag", 1.0f) == 0) return;
    if (int(timeMs_) - timeMs <= 1000 / kServerFps) return;
    for (size_t i = 0; i < mp_->clients.size(); ++i) {
        const MpClient& cl = mp_->clients[i];
        if (cl.state != MpClientState::Active || !cl.ent || cl.ent->obj->get("sessionstate").toString() != "playing") continue;
        Vec3 o{};
        if (!mpArchivedClientOrigin(int(i), timeMs, o)) continue;
        antilagSaved_.emplace_back(cl.ent, cl.ent->obj->get("origin"));
        cl.ent->obj->set("origin", Value(o));
    }
    if (!antilagSaved_.empty()) antilagRelink();
}

// The unlink / link of 0x811fc76 and 0x811fdec: the clients' collision clips (their hit boxes) follow the origins.
void World::antilagRelink() {
    std::vector<EntityClip> clips = collision_->entityClips();
    for (const auto& [e, live] : antilagSaved_)
        for (EntityClip& c : clips)
            if (c.id == e->number)
                if (const Value o = e->obj->get("origin"); o.isVector()) c.origin = o.asVector();
    collision_->setEntityClips(std::move(clips));
}

// 0x811fdec: the rewound clients get their live origins back.
void World::mpAntilagRestore() {
    if (antilagSaved_.empty()) return;
    for (auto it = antilagSaved_.rbegin(); it != antilagSaved_.rend(); ++it) it->first->obj->set("origin", it->second);
    antilagRelink();
    antilagSaved_.clear();
}

World::MpKillcamView World::mpKillcamView(float lerpFrac) const { return mp_ ? mpKillcamViewFor(mp_->localClient, lerpFrac) : MpKillcamView{}; }

World::MpKillcamView World::mpKillcamViewFor(int spectator, float lerpFrac) const {
    MpKillcamView v;
    if (!mp_ || spectator < 0 || size_t(spectator) >= mp_->clients.size()) return v;
    const MpClient& lc = mp_->clients[size_t(spectator)];
    if (!lc.ent || lc.ent->obj->get("sessionstate").toString() != "spectator") return v;
    const Value sc = lc.ent->obj->get("spectatorclient");
    const int follow = mpClientOfNumber(sc.isNumber() ? sc.asInt() : -1);
    int ms = archiveMsOf(lc.ent->obj->get("archivetime"));
    if (follow < 0 || ms <= 0) return v;
    const int64_t n = archiveLookup(mp_->archive, ms, kServerFps);
    const ArchivedFrame* cur = n >= 0 ? mp_->archive.frame(n) : nullptr;
    if (!cur || size_t(follow) >= cur->clients.size() || !cur->clients[size_t(follow)].active) return v;
    const ArchivedFrame* prev = mp_->archive.frame(n - 1);
    const ArchivedClient& c = cur->clients[size_t(follow)];
    const ArchivedClient* p = prev && size_t(follow) < prev->clients.size() && prev->clients[size_t(follow)].active
                                  ? &prev->clients[size_t(follow)] : nullptr;
    v.active = true;
    v.killer = c.entity;
    v.weapon = c.weapon;
    v.hands = c.hands;
    v.frameTimeMs = cur->timeMs;
    v.frame = n;
    v.client = follow;
    v.team = c.team;
    v.clip = c.clip;
    v.stock = c.stock;
    v.health = c.health;
    v.maxHealth = c.maxHealth;
    v.fireCount = c.fireCount;
    v.lastShot = c.lastShot;
    v.view = c.view;
    v.ring = c.ring;
    v.ads = c.view.adsHeld;
    v.lerpFrac = lerpFrac;
    // The view runs between frame n - 1 (lerpFrac 0) and frame n (lerpFrac 1). The client sees a shot of frame n when it
    // receives snapshot n, i.e. from the start of that interval: the fire sound, impacts and tracers of frame n are
    // replayed when n becomes the shown frame (mpKillcamReplayEvents, lerpFrac 0), and the view weapon fires with them.
    v.fireAgeMs = killcamFireAge(mp_->archive, n, follow);
    if (v.fireAgeMs >= 0) v.fireAgeMs += int(lerpFrac * float(kServerFrameMs));
    v.eye = c.eye;
    v.angles = c.viewAngles;
    if (p) // the previous frame to this one, like the live client interpolation
        for (int k = 0; k < 3; ++k) {
            v.eye[size_t(k)] = p->eye[size_t(k)] + (c.eye[size_t(k)] - p->eye[size_t(k)]) * lerpFrac;
            v.angles[size_t(k)] = lerpAngle(p->viewAngles[size_t(k)], c.viewAngles[size_t(k)], lerpFrac);
        }
    return v;
}

bool World::mpKillcamApply(float lerpFrac) {
    mpKillcamRestore();
    const MpKillcamView v = mpKillcamView(lerpFrac);
    if (!v.active) return false;
    int ms = archiveMsOf(mp_->clients[size_t(mp_->localClient)].ent->obj->get("archivetime"));
    const int64_t n = archiveLookup(mp_->archive, ms, kServerFps);
    const ArchivedFrame* cur = mp_->archive.frame(n);
    const ArchivedFrame* prev = mp_->archive.frame(n - 1);
    if (!cur) return false;
    killcamSwapped_ = true;
    for (const auto& up : entities_) {
        Entity& e = *up;
        if (e.isSpawner || e.number < 0) continue;
        const ArchivedEntity* ae = cur->find(e.number);
        // A freed entity (a grenade that exploded since) is drawn while the shown frame has it: its removed flag is
        // cleared for the drawing and restored (entities stay in the list when removed).
        if (e.obj->removed && (!ae || e.isPlayer)) continue;
        const bool hasModel = e.obj->get("model").isString() && !e.obj->get("model").asString().empty();
        if (!ae && !hasModel) continue;
        KillcamSaved s;
        s.e = &e;
        s.origin = e.obj->get("origin");
        s.angles = e.obj->get("angles");
        s.model = e.obj->get("model");
        s.hidden = e.obj->get("$hidden");
        s.sessionstate = e.obj->get("sessionstate");
        s.client = mpClientOf(e);
        s.removed = e.obj->removed;
        e.obj->removed = false;
        if (!ae || e.number == v.killer) { // not in the frame, or the client whose eyes the camera uses
            e.obj->set("$hidden", Value(int32_t(1)));
            killcamSaved_.push_back(std::move(s));
            continue;
        }
        Vec3 org = ae->origin, ang = ae->angles;
        if (const ArchivedEntity* pe = prev ? prev->find(e.number) : nullptr)
            for (int k = 0; k < 3; ++k) {
                org[size_t(k)] = pe->origin[size_t(k)] + (ae->origin[size_t(k)] - pe->origin[size_t(k)]) * lerpFrac;
                ang[size_t(k)] = lerpAngle(pe->angles[size_t(k)], ae->angles[size_t(k)], lerpFrac);
            }
        e.obj->set("origin", Value(org));
        e.obj->set("angles", Value(ang));
        e.obj->set("model", Value(ae->model));
        e.obj->set("$hidden", ae->hidden ? Value(int32_t(1)) : Value());
        s.attachments = e.attachments;
        e.attachments.clear();
        for (const auto& [m, t] : ae->attachments) e.attachments.push_back({m, t});
        s.swappedAnim = true;
        s.anim = std::move(e.anim);
        if (ae->tree) {
            e.anim = std::make_unique<AnimState>(*ae->tree, animLib_);
            auto& ns = e.anim->nodeStates();
            for (const auto& [i, st] : ae->anim)
                if (size_t(i) < ns.size()) ns[size_t(i)] = st;
        }
        // The frame before: the front end advances it by lerpFrac of a server frame, as it draws the live entities
        // (clientAnimState), so the archived bodies do not step at the server rate.
        if (const ArchivedEntity* pe = prev ? prev->find(e.number) : nullptr; pe && pe->tree && pe->tree == ae->tree) {
            s.prevAnim = std::make_unique<AnimState>(*pe->tree, animLib_);
            auto& ns = s.prevAnim->nodeStates();
            for (const auto& [i, st] : pe->anim)
                if (size_t(i) < ns.size()) ns[size_t(i)] = st;
        }
        if (e.isPlayer) {
            e.obj->set("sessionstate", Value(ae->sessionstate));
            if (s.client >= 0) {
                MpPlayerAnim& pa = mp_->clients[size_t(s.client)].anim;
                s.weaponModel = pa.weaponModel;
                pa.weaponModel = ae->weaponModel;
            }
        }
        killcamSaved_.push_back(std::move(s));
    }
    return true;
}

const AnimState* World::mpKillcamPrevAnim(int number) const {
    for (const KillcamSaved& s : killcamSaved_)
        if (s.e && s.e->number == number) return s.prevAnim.get();
    return nullptr;
}

void World::mpKillcamRestore() {
    for (auto it = killcamSaved_.rbegin(); it != killcamSaved_.rend(); ++it) {
        Entity& e = *it->e;
        e.obj->set("origin", it->origin);
        e.obj->set("angles", it->angles);
        e.obj->set("model", it->model);
        e.obj->set("$hidden", it->hidden);
        e.obj->removed = it->removed;
        if (e.isPlayer) e.obj->set("sessionstate", it->sessionstate);
        if (it->swappedAnim) {
            e.attachments = std::move(it->attachments);
            e.anim = std::move(it->anim);
            if (e.isPlayer && it->client >= 0) mp_->clients[size_t(it->client)].anim.weaponModel = it->weaponModel;
        }
    }
    killcamSaved_.clear();
    killcamSwapped_ = false;
}

} // namespace iw2::game
