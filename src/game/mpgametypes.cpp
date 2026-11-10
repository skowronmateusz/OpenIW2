// Multiplayer team gametypes (tdm, sd, ctf, hq): team scores, spawn point placement, the telefrag test, team-only
// use triggers, objective teams, weapon disabling, touch / use triggers for every client and map_restart.
// Implemented from the spec in docs/spec/mp.md ("Team gametypes"). VAs are VAs in the 1.3 Linux dedicated server.
#include "game/mpbots.h"
#include "game/mpgame.h"
#include "game/mpplayers.h"
#include "game/world.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <new>
#include <random>
#include <stdexcept>

namespace iw2::game {

using script::CallContext;
using script::Value;

namespace {
// The standing player box the spawn builtins use (0x8187ba0 / 0x8187bac).
constexpr Vec3 kBoxMins{-15, -15, 0};
constexpr Vec3 kBoxMaxs{15, 15, 70};

// allies / axis -> index into MpState::teamScore; anything else is the engine's error.
int teamIndex(const Value& v) {
    const std::string s = v.toString();
    if (s == "allies") return 0;
    if (s == "axis") return 1;
    throw std::runtime_error("Illegal team string '" + s + "'. Must be allies, or axis.");
}

// The team byte of triggers and objectives: allies 2, axis 1, none 0.
int teamCode(const std::string& s) {
    if (s == "allies") return 2;
    if (s == "axis") return 1;
    if (s == "none") return 0;
    return -1;
}
} // namespace

int World::mpTeamScore(const std::string& team) const {
    if (!mp_) return 0;
    return team == "allies" ? mp_->teamScore[0] : team == "axis" ? mp_->teamScore[1] : 0;
}

void World::registerMpGametypeBuiltins() {
    // getTeamScore(team) (0x8115d5a) / setTeamScore(team, score) (0x8115dda): one int per team, reset at game init
    // (0x81098dc). The setter also sends the score to every client (server command 'H' allies / 'G' axis).
    vm_.registerBuiltin("getteamscore", [this](CallContext& c) { return Value(int32_t(mp_->teamScore[teamIndex(c.arg(0))])); });
    vm_.registerBuiltin("setteamscore", [this](CallContext& c) {
        const int t = teamIndex(c.arg(0));
        mp_->teamScore[t] = c.arg(1).asInt();
        return Value();
    });

    // placeSpawnPoint() (0x81167ae), with the standing box, mask 0x2810011 and the entity itself skipped: a trace from
    // the origin up by 128 units (stopping under a ceiling), from there a trace down by 262144 units, and the entity
    // moves to where that one stops. A box that is in solid there prints a warning. TODO(spec): the ground entity
    // it also records (+0x7c).
    vm_.registerBuiltin("placespawnpoint", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e) return Value();
        const Vec3 o = e->obj->get("origin").isVector() ? e->obj->get("origin").asVector() : Vec3{0, 0, 0};
        const uint32_t mask = 0x2810011u;
        const TraceResult up = collision_->trace(o, {o[0], o[1], o[2] + 128.0f}, kBoxMins, kBoxMaxs, mask, e->number);
        const Vec3 top{o[0], o[1], o[2] + 128.0f * up.fraction};
        const TraceResult down = collision_->trace(top, {top[0], top[1], top[2] - 262144.0f}, kBoxMins, kBoxMaxs, mask, e->number);
        const Vec3 at{top[0], top[1], top[2] - 262144.0f * down.fraction};
        const TraceResult in = collision_->trace(at, at, kBoxMins, kBoxMaxs, mask, e->number);
        if (in.startSolid) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "WARNING: Spawn point entity %i is in solid at (%i, %i, %i)\n", e->number, int(at[0]),
                          int(at[1]), int(at[2]));
            print(buf);
        }
        e->obj->set("origin", Value(at));
        return Value();
    });

    // positionWouldTelefrag(origin) (0x811322c): true when the standing box at origin overlaps a client's body
    // (entities with player body contents, i.e. playing clients).
    vm_.registerBuiltin("positionwouldtelefrag", [this](CallContext& c) {
        const Vec3 p = c.arg(0).asVector();
        for (const MpClient& cl : mp_->clients) {
            if (cl.state == MpClientState::Free || !cl.ent) continue;
            if (cl.ent->obj->get("sessionstate").toString() != "playing" || !isAlive(*cl.ent)) continue;
            const Vec3 o = cl.ent->obj->get("origin").asVector();
            bool overlap = true;
            for (int k = 0; k < 3; ++k)
                if (p[k] + kBoxMaxs[k] < o[k] + kBoxMins[k] || p[k] + kBoxMins[k] > o[k] + kBoxMaxs[k]) overlap = false;
            if (overlap) return Value(int32_t(1));
        }
        return Value(int32_t(0));
    });

    // trigger setTeamForTrigger(team) (0x8117944): only a trigger_use / trigger_use_touch; the team byte limits who can
    // use it (the use scan 0x8121d88 skips a teamed trigger for a client of another team).
    vm_.registerBuiltin("setteamfortrigger", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || (e->classname != "trigger_use" && e->classname != "trigger_use_touch"))
            throw std::runtime_error("setteamfortrigger: trigger entity must be of type trigger_use or trigger_use_touch");
        const int t = teamCode(c.arg(0).toString());
        if (t < 0) throw std::runtime_error("setteamfortrigger: invalid team used must be allies, axis or none");
        e->obj->set("$triggerTeam", Value(int32_t(t)));
        return Value();
    });

    // objective_team lives with the other MP objective built-ins in src/game/mphud.cpp.

    // disableWeapon / enableWeapon (0x80fd286 / 0x80fd306) on a client. TODO(spec): the player state flag they set;
    // here the bot controller does not fire while it is set.
    auto clientEnt = [this](CallContext& c) -> MpClient* {
        Entity* e = entityOf(c.self);
        const int cl = e ? mpClientOf(*e) : -1;
        return cl < 0 ? nullptr : &mp_->clients[size_t(cl)];
    };
    vm_.registerBuiltin("disableweapon", [clientEnt](CallContext& c) {
        if (MpClient* cl = clientEnt(c)) cl->weaponDisabled = true;
        return Value();
    });
    vm_.registerBuiltin("enableweapon", [clientEnt](CallContext& c) {
        if (MpClient* cl = clientEnt(c)) cl->weaponDisabled = false;
        return Value();
    });

    // isOnGround() (0x80facc4) on a client: its player state has a ground entity (not 0x3ff). The local player's comes
    // from the front end's pmove, a bot's from its controller, a network client's from its user-command movement
    // (src/netgame/worldgame.cpp); others count as on the ground.
    vm_.registerBuiltin("isonground", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        const int cl = e ? mpClientOf(*e) : -1;
        if (cl < 0) throw std::runtime_error("entity " + std::to_string(e ? e->number : -1) + " is not a player");
        if (e == player_) return Value(int32_t(playerCmd_.onGround ? 1 : 0));
        if (bots_)
            for (const MpBot& b : bots_->bots)
                if (b.client == cl) return Value(int32_t(!b.psValid || b.ps.onGround ? 1 : 0));
        if (const MpPlayerInput* pi = mpPlayerInput(cl); pi && pi->onGround >= 0) return Value(int32_t(pi->onGround));
        return Value(int32_t(1));
    });

    // client clientClaimTrigger(trigger) (0x8117a72) / clientReleaseTrigger(trigger) (0x8117b5a): a trigger_use(_touch)
    // claimed by a client (+0x1b4, 0x3ff free) is skipped by the use scan of every other client (0x8121db4). A claim of a
    // trigger another client holds does nothing; a release by the holder frees it.
    auto claimArgs = [this](CallContext& c, const char* fn) -> std::pair<int, Entity*> {
        Entity* e = entityOf(c.self);
        const int cl = e ? mpClientOf(*e) : -1;
        if (cl < 0) throw std::runtime_error(std::string(fn) + ": claimer must be a client.");
        Entity* t = entityOf(c.arg(0));
        if (!t) throw std::runtime_error("not an entity");
        if (t->classname != "trigger_use" && t->classname != "trigger_use_touch")
            throw std::runtime_error(std::string(fn) + ": trigger entity must be of type trigger_use or trigger_use_touch");
        return {cl, t};
    };
    vm_.registerBuiltin("clientclaimtrigger", [claimArgs](CallContext& c) {
        auto [cl, t] = claimArgs(c, "clientclaimtrigger");
        const Value h = t->obj->get("$claimedBy");
        if (!h.isNumber() || h.asInt() == cl) t->obj->set("$claimedBy", Value(int32_t(cl)));
        return Value();
    });
    vm_.registerBuiltin("clientreleasetrigger", [claimArgs](CallContext& c) {
        auto [cl, t] = claimArgs(c, "clientreleasetrigger");
        const Value h = t->obj->get("$claimedBy");
        if (h.isNumber() && h.asInt() == cl) t->obj->set("$claimedBy", Value());
        return Value();
    });

    // map_restart(persist) (0x8116a3c): one call per level ("map_restart already called" / "exitlevel already called"),
    // remembers persist and queues the fast_restart command; the restart runs after the server frame.
    vm_.registerBuiltin("map_restart", [this](CallContext& c) {
        if (mp_->restartState == 1) throw std::runtime_error("map_restart already called");
        if (mp_->restartState == 2) throw std::runtime_error("exitlevel already called");
        mp_->restartState = 1;
        mp_->restartPersist = c.argc() > 0 && c.arg(0).truthy();
        return Value();
    });
}

// The client touch and use triggers (G_TouchTriggers / the use scan per client): every playing, living client other
// than the local player (which runTriggers handles) touches trigger_multiple / trigger_radius / trigger_hurt ...
// every frame, and a press of use fires the trigger_use / trigger_use_touch the client's last use search picked
// (mpClientUseSearch); then the search runs again for the next frame.
void World::mpClientTriggers() {
    for (size_t ci = 0; ci < mp_->clients.size(); ++ci) {
        MpClient& cl = mp_->clients[ci];
        Entity* p = cl.ent;
        // Use is +activate or +usereload. Commands arrive several per server frame: what any of them held since the last
        // frame counts, so a tap that is down and up between two frames is still a press.
        const uint32_t seen = cl.buttons | cl.buttonsSinceFrame;
        const bool usePress = (seen & 0x28) && !(cl.oldButtons & 0x28);
        bool press = usePress;
        cl.oldButtons = cl.buttons;
        cl.buttonsSinceFrame = 0;
        // Turrets first (also for a dead client, whose turret is released): a press that gets on or off takes the use.
        if (cl.state == MpClientState::Active && p && p != player_ && mpClientTurret(ci, usePress)) press = false;
        const int picked = cl.useEntity;
        cl.useEntity = -1;
        cl.useHint = 0;
        cl.useHintWeapon.clear();
        cl.useHintText.clear();
        if (cl.state != MpClientState::Active || !p || p == player_ || !isAlive(*p)) continue;
        if (p->obj->get("sessionstate").toString() != "playing") continue;
        mpClientItems(*p, press ? picked : -1); // weapon items (src/game/mpdrop.cpp)
        const Vec3 o = p->obj->get("origin").asVector();
        const Vec3 pmin{o[0] - 15, o[1] - 15, o[2]}, pmax{o[0] + 15, o[1] + 15, o[2] + 70};
        const std::string team = p->obj->get("sessionteam").toString();
        std::vector<Entity*> fired;
        for (const auto& e : entities_) {
            if (e->obj->removed || e->isSpawner || e->classname.rfind("trigger_", 0) != 0) continue;
            if (e->obj->get("$disabled").truthy()) continue;
            const bool use = e->classname == "trigger_use" || e->classname == "trigger_use_touch";
            if (use) {
                // The press uses what the last search picked (+0x5a0); the search already filtered team and claim.
                if (press && e->number == picked) fired.push_back(e.get());
                continue;
            }
            if (e->classname == "trigger_damage" || e->classname == "trigger_lookat" || e->classname == "trigger_disk") continue;
            const Value sf = e->obj->get("spawnflags");
            if (e->classname == "trigger_hurt") {
                if (e->obj->get("$hurtOff").truthy() || !triggerContact(*e, pmin, pmax)) continue;
                const std::string key = "$hurtNext" + std::to_string(ci);
                const Value nt = e->obj->get(key);
                const int flags = sf.isNumber() ? sf.asInt() : 0;
                if (nt.isNumber() && nt.asInt() > timeMs_) continue;
                e->obj->set(key, Value(int32_t(timeMs_ + ((flags & 0x10) ? 1000 : 50))));
                vm_.notify(e->obj, "trigger", {Value(p->obj)});
                const Value dv = e->obj->get("dmg");
                damage(*p, e.get(), dv.isNumber() ? dv.asInt() : 5, Vec3{0, 0, 0}, Vec3{0, 0, 0}, "MOD_TRIGGER_HURT");
                if (flags & 0x20) e->obj->set("$hurtOff", Value(true));
                continue;
            }
            if (sf.isNumber() && (sf.asInt() & 8)) continue;
            if (triggerContact(*e, pmin, pmax)) fired.push_back(e.get());
        }
        for (Entity* e : fired) {
            if (e->obj->removed) continue;
            vm_.notify(e->obj, "trigger", {Value(p->obj)});
            if (e->classname == "trigger_once") e->obj->removed = true;
        }
        if (!p->obj->removed && isAlive(*p) && p->obj->get("sessionstate").toString() == "playing") mpClientUseSearch(ci);
    }
}

// The use search at a client's frame end (0x8121bc6, docs/spec/mp.md "Use search of every client"): hint 0, no hint
// string, no entity; nothing more while dead or in the binocular weapon states. Off a turret the scan (0x8121494, the
// same scoring as the single-player scan, with the team / claim filter of use triggers) is walked in order: a
// trigger_use(_touch) ends the search with its cursor hint and, when that hint is not 0, its hint string (no hint:
// no entity either); any other plain entity ends it with nothing; a weapon item (0x8121aaa: not a grenade, not owned)
// or a usable turret (0x810c358) ends it with the weapon hint (weapon number + 4) and, for a turret, its weapon's
// useHintString; anything else is skipped. pm_flags 4 is taken as the mantle.
void World::mpClientUseSearch(size_t ci) {
    MpClient& cl = mp_->clients[ci];
    Entity* p = cl.ent;
    if (!p) return;
    if ((cl.weaponState >= 0x11 && cl.weaponState <= 0x16) || cl.mantling) return; // the binocular states, a mantle: nothing to use
    // On a turret: its weapon's drop hint (0x8121b36), no entity.
    if (mpClientOnTurret(int(ci))) {
        const std::string& w = cl.turretUse.turret->turret->weapon;
        const std::string drop = weaponField(w, "dropHintString");
        if (!drop.empty()) {
            cl.useHintWeapon = w;
            cl.useHintText = drop;
        }
        return;
    }
    float pitch = cl.viewAngles[0], yaw = cl.viewAngles[1], viewHeight = cl.viewHeight;
    bool onGround = cl.onGround;
    int fuse = cl.grenadeFuseMs;
    if (bots_)
        for (const MpBot& b : bots_->bots)
            if (b.client == int(ci)) {
                pitch = b.pitch;
                yaw = b.yaw;
                if (b.psValid) {
                    viewHeight = b.ps.viewHeight;
                    onGround = b.ps.onGround;
                }
                fuse = 0;
            }
    Entity* const saved = player_;
    struct Restore {
        World& w;
        Entity* saved;
        ~Restore() { w.player_ = saved; }
    } restore{*this, saved};
    player_ = p;
    const Vec3 o = p->obj->get("origin").asVector();
    const Vec3 eye{o[0], o[1], o[2] + viewHeight};
    const float rad = 0.0174532924f;
    const Vec3 fwd{std::cos(pitch * rad) * std::cos(yaw * rad), std::cos(pitch * rad) * std::sin(yaw * rad), -std::sin(pitch * rad)};
    for (const UseCandidate& c : scanUsables(eye, fwd)) {
        Entity& e = *c.e;
        if (e.classname == "trigger_use" || e.classname == "trigger_use_touch") {
            const Value h = e.obj->get("$cursorhint"), str = e.obj->get("$hintstring");
            cl.useHint = h.isNumber() ? h.asInt() : 2;
            if (cl.useHint != 0) {
                cl.useEntity = e.number;
                if (str.isNumber()) cl.useHintText = hintStrings_.at(str.asInt());
            }
            return;
        }
        if (e.classname.rfind("weapon_", 0) == 0) {
            std::string w = e.classname.substr(7);
            for (char& ch : w) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            std::string wt = weaponField(w, "weaponType");
            for (char& ch : wt) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            if (!weapon(w) || wt == "grenade" || inventory(*p).owned.count(w)) continue;
            cl.useEntity = e.number;
            cl.useHintWeapon = w;
            return;
        }
        if (e.turret) {
            if (!turretUsableBy(e, o, fuse, onGround)) continue;
            cl.useEntity = e.number;
            cl.useHintWeapon = e.turret->weapon;
            cl.useHintText = weaponField(e.turret->weapon, "useHintString");
            return;
        }
        if (e.classname == "script_vehicle" || e.isActor) continue;
        return;
    }
}

void World::mpSetClientGround(int client, bool onGround, int grenadeFuseMs, int weaponState, bool mantling) {
    if (!mp_ || client < 0 || size_t(client) >= mp_->clients.size()) return;
    MpClient& c = mp_->clients[size_t(client)];
    c.onGround = onGround;
    c.grenadeFuseMs = grenadeFuseMs;
    c.weaponState = weaponState;
    c.mantling = mantling;
}

} // namespace iw2::game

namespace iw2::game {

namespace {
// A copy of a script value that does not refer to the old VM's objects: arrays are copied, objects, functions and
// animation references become undefined.
Value plainCopy(const Value& v) {
    if (v.isArray()) {
        auto out = std::make_shared<script::Array>();
        for (const auto& [k, item] : std::get<script::ArrayPtr>(v.v)->items) {
            const Value c = plainCopy(item);
            if (c.isDefined()) out->set(k, c);
        }
        return Value(out);
    }
    if (std::holds_alternative<script::ObjectPtr>(v.v) || std::holds_alternative<script::FunctionRef>(v.v) ||
        std::holds_alternative<script::AnimRef>(v.v) || std::holds_alternative<script::AnimTreeRef>(v.v))
        return Value();
    return v;
}
} // namespace

// Fast restart (the fast_restart command, 0x808be3e, after map_restart): the level is rebuilt from the map as at a map
// start, the clients stay connected. Kept: the cvars, the game array (sd.gsc:285-298 reads its round counters and team
// scores back), the client slots with their names and, when persist was set, their pers arrays (else every client gets a
// new one, 0x808efce). Then game init runs the gametype again and every connected client is connected again
// (CodeCallback_PlayerConnect, 0x80f8e7a) and the active ones begin (0x808f628), after three settling server frames of
// 100 ms each. The level time goes on.
// TODO(spec): the snapshot/configstring side and the exact level time after the restart.
void World::mpFastRestart() {
    struct Carried {
        MpClientState state;
        std::string name;
        Value pers;
        std::map<std::string, std::string> cvars;
    };
    const fs::Vfs& vfs = vfs_;
    const std::string mapName = mapName_;
    const std::string gametype = mp_->gametype;
    const bool persist = mp_->restartPersist;
    const int restarts = mp_->restarts + 1;
    auto savedCvars = cvars;
    auto savedPrint = print;
    auto savedPlaySound = playSound;
    auto savedSoundExists = soundExists;
    auto savedStopSound = stopSound;
    auto savedPlayBlend = playBlendSound;
    auto savedSetBlend = setBlendSound;
    auto savedErrors = vm_.errors();
    const size_t loopGuard = vm_.loopGuardIterations();
    const int64_t time = timeMs_;
    const auto meleeSeed = meleeSeed_;
    const std::mt19937 rng = *scriptRng_; // the random sequences go on (a fresh world would repeat the first round)
    std::vector<std::pair<std::string, Value>> game;
    for (const auto& [k, v] : vm_.game()->fields) game.emplace_back(k, plainCopy(v));
    std::vector<Carried> clients;
    for (const MpClient& cl : mp_->clients)
        clients.push_back({cl.state, cl.name, persist && cl.ent ? plainCopy(cl.ent->obj->get("pers")) : Value(), cl.cvars});
    auto log = mp_->log;
    auto prints = mp_->prints;
    std::shared_ptr<BotSystem> bots = bots_;
    const std::shared_ptr<const ScriptNames> scriptNames = scriptNames_;
    const bool dedicated = mpDedicated_;

    this->~World();
    new (this) World(vfs, mapName, true, scriptNames);
    mpDedicated_ = dedicated;

    cvars = savedCvars;
    print = savedPrint;
    playSound = savedPlaySound;
    soundExists = savedSoundExists;
    stopSound = savedStopSound;
    playBlendSound = savedPlayBlend;
    setBlendSound = savedSetBlend;
    vm_.restoreErrors(savedErrors);
    vm_.setLoopGuardIterations(loopGuard);
    timeMs_ = time;
    vm_.setTime(timeMs_);
    meleeSeed_ = meleeSeed;
    *scriptRng_ = rng;
    for (const auto& [k, v] : game) vm_.game()->set(k, v);
    mp_->log = log;
    mp_->prints = prints;
    mp_->restarts = restarts;
    mpStart(gametype);
    // Three server frames of 100 ms run before the clients come back (0x808bf83..0x808bfa3).
    for (int f = 0; f < 3; ++f) {
        timeMs_ += 50;
        frame(true);
    }
    for (size_t i = 0; i < clients.size(); ++i) {
        if (clients[i].state == MpClientState::Free) continue;
        mpConnectSlot(int(i), clients[i].name, clients[i].pers);
        mp_->clients[i].cvars = clients[i].cvars;
    }
    for (size_t i = 0; i < clients.size(); ++i)
        if (clients[i].state == MpClientState::Active) mpBegin(int(i));
    bots_ = bots; // after the reconnect: the bot frame drops bots whose slot is free
    if (bots_ && bots_->nav) paths_ = bots_->nav->toPathGraph();
    if (bots_)
        for (MpBot& b : bots_->bots) {
            const int skill = b.skill, client = b.client;
            b = MpBot{};
            b.client = client;
            b.skill = skill;
            b.joinStep = 2; // the gametype puts a client that already has a team back in the game itself
        }
}

} // namespace iw2::game
