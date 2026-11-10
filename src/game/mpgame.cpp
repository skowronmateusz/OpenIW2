// Multiplayer game layer (offline listen server), implemented from the spec in docs/spec/mp.md.
// VAs in the comments are VAs in the 1.3 Linux dedicated server.
#include "game/mpbots.h"
#include "game/mpgame.h"
#include "game/mpchat.h"
#include "game/world.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace iw2::game {

using script::Builtin;
using script::CallContext;
using script::Value;

namespace {
constexpr const char* kCallbacks = "maps/mp/gametypes/_callbacksetup.gsc";
// sv_maxclients: an int dvar, default 20, range 1..64 (registered at 0x8092d93); the client slots and _teams.gsc:31-40
// (team size limit = half of it) read it.
constexpr int kMaxClientsDefault = 20;

// A localized string's key, else the value as text.
std::string textOf(const Value& v) {
    if (std::holds_alternative<script::LocalizedString>(v.v)) return std::get<script::LocalizedString>(v.v).key;
    return v.toString();
}

// Message text as the 1.3 server sends it (docs/spec/net.md section 10): a localized string's key, then each further
// argument behind 0x15 (observed: "MP_CONNECTED" 0x15 name, "MP_GAIN_POINTS_BY_ELIMINATING" 0x15 "50").
// TODO(spec): further arguments after a plain first argument (openiw2 joins them with spaces).
std::string wireText(CallContext& c, size_t first) {
    if (c.argc() <= first) return {};
    const bool localized = std::holds_alternative<script::LocalizedString>(c.arg(first).v);
    std::string s = textOf(c.arg(first));
    for (size_t i = first + 1; i < c.argc(); ++i) s += (localized ? "\x15" : " ") + messageArgText(c.arg(i));
    return s;
}

std::string lowerStr(std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}
} // namespace

void World::mpStart(const std::string& gametype) {
    if (!mp_) throw std::runtime_error("mpStart: not a multiplayer world");
    mp_->gametype = lowerStr(gametype);
    mp_->localClient = mpDedicated_ ? -1 : 0; // a dedicated server shows no client
    if (cvars.find("sv_maxclients") == cvars.end() || cvars["sv_maxclients"].empty())
        cvars["sv_maxclients"] = std::to_string(kMaxClientsDefault);
    const int maxClients = std::clamp(std::atoi(cvars["sv_maxclients"].c_str()), 1, 64);
    cvars["sv_maxclients"] = std::to_string(maxClients);
    mp_->clients.assign(size_t(maxClients), MpClient{});
    cvars["g_gametype"] = mp_->gametype;
    // g_antilag: a bool dvar, default on (registered at 0x8108b76; docs/spec/mp-hud.md "Antilag").
    if (cvars.find("g_antilag") == cvars.end() || cvars["g_antilag"].empty()) cvars["g_antilag"] = "1";
    // iw2_weaponclasses (an OpenIW2 cvar, docs/debug-flags.md): a list of weapon classes ("rifle,smg") sets the
    // scr_allow_* cvars of the menu weapons before the gametype reads them (docs/spec/mp-bots.md, "Weapon classes").
    if (const auto wc = cvars.find("iw2_weaponclasses"); wc != cvars.end() && !wc->second.empty()) {
        std::vector<std::string> classes;
        std::string cur;
        for (char ch : wc->second + ",") {
            if (ch == ',' || ch == ' ') {
                if (!cur.empty()) classes.push_back(lowerStr(cur));
                cur.clear();
            } else {
                cur += ch;
            }
        }
        mpSetAllowedWeaponClasses(classes);
    }
    prepareStart(); // cvars, map entities (mapname = the bare map name)
    // The client entity is not part of the map: it exists from the connect on (docs/spec/mp.md, "Client lifecycle").
    // The SP spawn made the local player entity; it waits as client 0's entity, out of the game until the connect.
    if (player_) {
        player_->obj->set("health", Value(int32_t(0)));
        player_->obj->set("sessionstate", Value("spectator"));
        player_->obj->set("classname", Value("")); // not found by getentarray("player", "classname") until it connects
        // A dedicated server has no local client entity: the slots 0..maxclients-1 are the only client entities (docs/spec/mp.md
        // "Local client view"). The SP spawn's entity is freed, so no script, trace, trigger or snapshot sees it; player_ stays
        // as an inert, removed object for the code that reads it unguarded.
        if (mpDedicated_) freeEntity(*player_);
    }
    timeMs_ += 50; // the frame start before init, as in runLevelMain (TODO(spec): MP init time)
    vm_.setTime(timeMs_);
    // Game init order (0x8109096): gametype main (0x81182f4, required), level main (0x81101d0, optional),
    // CodeCallback_StartGameType (0x8118322).
    const std::string gt = "maps/mp/gametypes/" + mp_->gametype + ".gsc";
    if (!vfs_.read(gt)) throw std::runtime_error("gametype script not found: " + gt);
    vm_.start(gt, "main", {}, Value(vm_.level()));
    const std::string lvl = "maps/mp/" + mapName_ + ".gsc";
    if (vfs_.read(lvl)) vm_.start(lvl, "main", {}, Value(vm_.level()));
    vm_.start(kCallbacks, "CodeCallback_StartGameType", {}, Value(vm_.level()));
    vm_.drainReady();
    frame(false);
    levelInitializing_ = false;
}

Entity* World::mpClientEntity(int client) {
    if (!mp_ || client < 0 || size_t(client) >= mp_->clients.size()) return nullptr;
    return mp_->clients[size_t(client)].ent;
}

int World::mpClientOf(const Entity& e) const {
    if (!mp_) return -1;
    for (size_t i = 0; i < mp_->clients.size(); ++i)
        if (mp_->clients[i].ent == &e && mp_->clients[i].state != MpClientState::Free) return int(i);
    return -1;
}

// Connect (0x80f8e7a): the slot and its entity are reset, the client is connected, sessionstate spectator,
// spectatorclient -1; then CodeCallback_PlayerConnect with self = the entity and no arguments (0x8118350).
int World::mpConnect(const std::string& name) {
    if (!mp_) return -1;
    for (size_t i = 0; i < mp_->clients.size(); ++i)
        if (mp_->clients[i].state == MpClientState::Free) return mpConnectSlot(int(i), name, Value());
    return -1;
}

int World::mpConnectSlot(int slot, const std::string& name, const Value& pers) {
    MpClient& cl = mp_->clients[size_t(slot)];
    cl = MpClient{};
    // Only a process with a local player gives it this client (a dedicated server has none: localClient -1, every
    // client has its own entity, docs/spec/mp.md "Local client view").
    if (slot == mp_->localClient && player_) {
        cl.ent = player_;
    } else {
        Entity& e = spawnEntity("player");
        e.isPlayer = true;
        cl.ent = &e;
    }
    cl.state = MpClientState::Connected;
    cl.name = mpCleanName(name); // ClientUserinfoChanged name cleaning (docs/spec/mp-chat.md "Player names")
    const auto& o = cl.ent->obj;
    o->set("classname", Value("player"));
    // `pers` is an array on every client from the connect on: dm.gsc:190 indexes it before any script sets it. It is
    // kept by the server's client slot (0x808efce gives every connected client a new one unless the restart persists).
    if (pers.isArray()) o->set("pers", pers);
    else if (!o->get("pers").isArray()) o->set("pers", Value(std::make_shared<script::Array>()));
    o->set("health", Value(int32_t(0)));
    o->set("name", Value(cl.name));
    o->set("sessionstate", Value("spectator"));
    o->set("sessionteam", Value("spectator"));
    o->set("spectatorclient", Value(int32_t(-1)));
    o->set("score", Value(int32_t(0)));
    o->set("deaths", Value(int32_t(0)));
    o->set("statusicon", Value(""));
    o->set("archivetime", Value(0.0f));
    o->set("psoffsettime", Value(int32_t(0)));
    o->set("$client", Value(int32_t(slot)));
    vm_.start(kCallbacks, "CodeCallback_PlayerConnect", {}, Value(o));
    vm_.drainReady();
    return slot;
}

// Begin (0x80f90ae): the client becomes active and its entity is notified "begin".
void World::mpBegin(int client) {
    Entity* e = mpClientEntity(client);
    if (!e) return;
    mp_->clients[size_t(client)].state = MpClientState::Active;
    vm_.notify(e->obj, "begin");
    vm_.drainReady();
}

// Menu response (0x8100d16): notify "menuresponse" with the menu name first and the response second.
void World::mpMenuResponse(int client, const std::string& menu, const std::string& response) {
    Entity* e = mpClientEntity(client);
    if (!e) return;
    vm_.notify(e->obj, "menuresponse", {Value(menu), Value(response)});
    vm_.drainReady();
}

// Disconnect (0x80f94ce): "menuresponse" ("", "disconnect"), the HUD elements owned by the client are freed (0x8103414),
// CodeCallback_PlayerDisconnect, then the entity and the slot are freed. The spectators following this client stop first.
// The scripts make a client's elements at "connecting" and never destroy them (_hud_teamscore.gsc:33-90), so a rejoin
// into the same slot relies on this free. The entity's script object goes too: the waits of its threads end with it
// (docs/spec/gsc-vm.md "Freeing a script object") and the next client of the slot starts without its fields.
void World::mpDisconnect(int client) {
    Entity* e = mpClientEntity(client);
    if (!e || mp_->clients[size_t(client)].state == MpClientState::Free) return;
    vm_.notify(e->obj, "menuresponse", {Value(""), Value("disconnect")});
    vm_.drainReady();
    mpStopFollowersOf(client);
    mpFreeClientHud(client);
    vm_.start(kCallbacks, "CodeCallback_PlayerDisconnect", {}, Value(e->obj));
    vm_.drainReady();
    // Commands the scripts made for this connection and the frame has not sent yet (a menu opened by its last answer)
    // do not reach the next client of the slot, which may connect before the next frame.
    for (auto& [target, text] : mp_->clientCommands)
        if (target == client) target = -1;
    mpClientTurretRelease(size_t(client)); // a turret the client was on is free again
    if (e != player_) {
        freeEntity(*e);
    } else {
        // The local player's entity stays for the next client 0 with a new script object: pers and the script's fields
        // go with the client (a new connect starts with an empty pers, docs/spec/mp.md "Client fields").
        const ObjectPtr old = e->obj;
        byObject_.erase(old.get());
        vm_.freeObject(old);
        inventories_.erase(e->number);
        e->obj = vm_.newObject("player");
        byObject_[e->obj.get()] = e;
        e->obj->set("origin", old->get("origin"));
        e->obj->set("angles", old->get("angles"));
        e->obj->set("health", Value(int32_t(0)));
        e->obj->set("classname", Value(""));
    }
    mp_->clients[size_t(client)] = MpClient{};
}

void World::mpFreeClientHud(int client) {
    std::erase_if(hudElems_, [&](const ObjectPtr& h) {
        const Value owner = h->get("$owner");
        if (!owner.isNumber() || owner.asInt() != client) return false;
        vm_.freeObject(h);
        return true;
    });
}

// Per-frame client upkeep. The killcam's archive and archivetime trimming run at the end of the frame
// (mpKillcamEndFrame, src/game/mpkillcam.cpp).
void World::mpFrame() {
    mpSpectateFrame(); // spectator follow (src/game/mpspectate.cpp, docs/spec/mp-spectate.md)
    mpClientTriggers(); // touch / use triggers of the non-local clients
    mpBotsFrame(); // bot clients (src/game/mpbots.cpp, docs/spec/mp-bots.md)
    mpHudFrame();   // objective_onEntity positions (src/game/mphud.cpp)
    mpPlayersFrame(); // third-person animation of the other clients (src/game/mpplayers.cpp, docs/spec/mp-players.md)
}

void World::mpAutoJoin(int client, const std::string& wanted, const std::string& team) {
    if (!mpClientEntity(client)) return;
    mpMenuResponse(client, vm_.game()->get("menu_team").toString(), team.empty() ? std::string("autoassign") : team);
    frame();
    const std::string menu = mp_->clients[size_t(client)].openMenu;
    // The menu's default (thompson / MP40 / PPSh) when the server allows it, else its first allowed weapon.
    std::string weapon = !wanted.empty() ? wanted : menu == "weapon_german" ? "mp40_mp" : menu == "weapon_russian" ? "ppsh_mp" : "thompson_mp";
    const std::vector<std::string> allowed = mpAllowedWeapons(menu);
    if (!allowed.empty() && std::find(allowed.begin(), allowed.end(), weapon) == allowed.end()) weapon = allowed[0];
    mpMenuResponse(client, menu, weapon);
    frame();
}

const std::string* World::mpLocalCvar(const std::string& name) const {
    if (!mp_ || mp_->localClient < 0) return nullptr;
    const auto& cv = mp_->clients[size_t(mp_->localClient)].cvars;
    const auto it = cv.find(lowerStr(name));
    return it == cv.end() ? nullptr : &it->second;
}

// Client command openscriptmenu (MP client 0x410920): needs exactly two arguments and a connected client; the menu name
// is looked up case-insensitively among the 32 precached menus (-1 when absent) and "cmd mr <sv_serverId> <index>
// <response>" is sent. The server (0x8100d16) turns the index back into the name (empty outside the list).
void World::mpOpenScriptMenu(const std::string& menu, const std::string& response) {
    if (!mp_ || menu.empty() || response.empty()) return;
    std::string name;
    for (const auto& m : menus_)
        if (lowerStr(m) == lowerStr(menu)) name = m;
    mpMenuResponse(mp_->localClient, name, response);
}

// The server puts a client's elements into its player state (0x8104882): an element for every client or for this client,
// and for no team or this client's team; `archived` elements (the default) into the archived part, the others into the
// current part. A killcam spectator gets the followed client's archived player state (0x80f6916) and only its own
// current elements (0x8104882 with flag 2), so its archived elements are those of the followed client.
bool World::mpHudElemVisible(const script::ObjectPtr& h) const {
    if (!mp_) return true;
    int client = mp_->localClient;
    const Value arch = h->get("archived");
    if (client >= 0 && (!arch.isDefined() || arch.truthy())) {
        const MpClient& lc = mp_->clients[size_t(mp_->localClient)];
        if (lc.ent && mpKillcamFrame() >= 0) {
            const Value sc = lc.ent->obj->get("spectatorclient");
            client = mpClientOfNumber(sc.isNumber() ? sc.asInt() : -1);
        }
    }
    return mpHudElemVisibleTo(h, client);
}

bool World::mpHudElemVisibleTo(const script::ObjectPtr& h, int client) const {
    if (client < 0 || size_t(client) >= mp_->clients.size()) return false;
    const Value owner = h->get("$owner");
    if (owner.isNumber()) return owner.asInt() == client;
    const Value team = h->get("$team");
    if (team.isString()) {
        const MpClient& cl = mp_->clients[size_t(client)];
        return cl.ent && cl.ent->obj->get("sessionteam").toString() == team.asString();
    }
    return true;
}

void World::mpSetButtons(int client, uint32_t buttons) {
    if (!mp_ || client < 0 || size_t(client) >= mp_->clients.size()) return;
    mp_->clients[size_t(client)].buttons = buttons;
    mp_->clients[size_t(client)].buttonsSinceFrame |= buttons;
}

// Player damage (0x8101b40): only an active client (state 2) that is playing; the amount has already been scaled by
// the hit-location multiplier (World::damage). CodeCallback_PlayerDamage (0x81183b8) gets the 10 arguments.
std::vector<KnockbackHit> World::mpTakeKnockbacks(int client) {
    if (!mp_ || client < 0 || size_t(client) >= mp_->clients.size()) return {};
    return std::exchange(mp_->clients[size_t(client)].knockbacks, {});
}

void World::mpPlayerDamage(Entity& target, Entity* inflictor, Entity* attacker, int amount, int dflags, const std::string& mod,
                           const std::string& weapon, const Vec3& point, const Vec3& dir, const std::string& location) {
    const int cl = mpClientOf(target);
    if (cl < 0 || mp_->clients[size_t(cl)].state != MpClientState::Active) return;
    if (target.obj->get("sessionstate").toString() != "playing") return;
    if (amount <= 0) return;
    if (attacker) target.lastAttacker = attacker;
    const bool hasDir = dir[0] != 0 || dir[1] != 0 || dir[2] != 0;
    mpBotOnDamage(target, attacker, mod, false); // bot counters and reaction (docs/spec/mp-bots.md)
    vm_.start(kCallbacks, "CodeCallback_PlayerDamage",
              {inflictor ? attackerValue(inflictor) : Value(), attackerValue(attacker), Value(int32_t(amount)), Value(int32_t(dflags)), Value(mod),
               Value(weapon), Value(point), hasDir ? Value(dir) : Value(), Value(location), Value(int32_t(0))},
              Value(target.obj));
}

// Player death (0x810175a): notify "death" with the attacker, then CodeCallback_PlayerKilled (0x8118492) with 9 arguments.
// The last argument is the death animation's duration (0x80d96ee, docs/spec/mp-players.md "Events").
// TODO(spec): the corpse contents and the dead pm_type.
void World::mpPlayerDie(Entity& target, Entity* inflictor, Entity* attacker, int amount, const std::string& mod,
                        const std::string& weapon, const Vec3& dir, const std::string& location) {
    vm_.notify(target.obj, "death", {attackerValue(attacker)});
    mpBotOnDamage(target, attacker, mod, true);
    const int deathAnimMs = mpDeathAnim(target);
    if (&target == player_) playerDeadYaw_ = mpDeadYaw(target, inflictor, attacker);
    const bool hasDir = dir[0] != 0 || dir[1] != 0 || dir[2] != 0;
    vm_.start(kCallbacks, "CodeCallback_PlayerKilled",
              {attackerValue(inflictor), attackerValue(attacker), Value(int32_t(amount)), Value(mod), Value(weapon),
               hasDir ? Value(dir) : Value(), Value(location), Value(int32_t(0)), Value(int32_t(deathAnimMs))},
              Value(target.obj));
}

// LookAtKiller of the MP server (0x810160a, called by player death after the callback): the dead yaw (an int in the
// player state) faces the attacker, else the inflictor (each only when it is not the victim), else it is the victim's own
// yaw; the float is truncated toward zero. Note the order differs from the SP one (inflictor first).
float World::mpDeadYaw(const Entity& target, const Entity* inflictor, const Entity* attacker) const {
    const Entity* by = attacker && attacker != &target ? attacker : inflictor && inflictor != &target ? inflictor : nullptr;
    const Value to = target.obj->get("origin");
    float yaw = 0;
    if (by && to.isVector() && by->obj->get("origin").isVector()) {
        const Vec3 a = by->obj->get("origin").asVector(), t = to.asVector();
        const float dx = a[0] - t[0], dy = a[1] - t[1];
        if (dx != 0 || dy != 0) { // vectoyaw (0x80a4d5a): 0 for no direction, else atan2 in degrees in [0, 360)
            yaw = float(std::atan2(double(dy), double(dx)) * 180.0 / 3.14159265358979323846);
            if (yaw < 0) yaw += 360.0f;
        }
    } else if (const Value an = target.obj->get("angles"); an.isVector()) {
        yaw = an.asVector()[1];
    }
    return std::trunc(yaw);
}

void World::mpDebugDamage(int victim, int attacker, int amount, const std::string& mod, const std::string& location) {
    Entity* v = mpClientEntity(victim);
    if (!v) return;
    Entity* a = mpClientEntity(attacker);
    const Vec3 vo = v->obj->get("origin").asVector();
    Vec3 dir{1, 0, 0};
    if (a) {
        const Vec3 ao = a->obj->get("origin").asVector();
        dir = {vo[0] - ao[0], vo[1] - ao[1], vo[2] - ao[2]};
    }
    damage(*v, a, amount, dir, vo, mod, location);
    vm_.drainReady();
}

void World::registerMpBuiltins() {
    // sessionteam / sessionstate are validated client fields (setters 0x80f7db6 / 0x80f7efc; error texts 0x8157ae0 /
    // 0x8157b40). Only on client entities.
    vm_.fieldStoreHook = [this](const script::ObjectPtr& o, const std::string& field, const Value& v) {
        if (field != "sessionteam" && field != "sessionstate") return;
        const auto it = byObject_.find(o.get());
        if (it == byObject_.end() || !it->second->isPlayer) return;
        const std::string s = v.toString();
        if (field == "sessionteam" && s != "allies" && s != "axis" && s != "none" && s != "spectator")
            throw std::runtime_error("'" + s + "' is an illegal sessionteam string. Must be allies, axis, none, or spectator.");
        if (field == "sessionstate" && s != "playing" && s != "dead" && s != "spectator" && s != "intermission")
            throw std::runtime_error("'" + s + "' is an illegal sessionstate string. Must be playing, dead, spectator, or intermission.");
    };
    auto clientOf = [this](CallContext& c) -> MpClient& {
        Entity* e = entityOf(c.self);
        const int cl = e ? mpClientOf(*e) : -1;
        if (cl < 0) throw std::runtime_error("entity " + std::to_string(e ? e->number : -1) + " is not a player");
        return mp_->clients[size_t(cl)];
    };
    // self spawn(origin, angles) on a client places the player (dm.gsc:419); any other self is the entity spawn.
    // The client spawn runs no kill box (the kill-box routine 0x811f0b4 has no direct caller, docs/spec/mp.md "Client spawn").
    Builtin entitySpawn = vm_.builtin("spawn");
    vm_.registerBuiltin("spawn", [this, entitySpawn](CallContext& c) -> Value {
        Entity* e = entityOf(c.self);
        if (!e || !e->isPlayer || mpClientOf(*e) < 0) return entitySpawn(c);
        const Vec3 o = c.arg(0).asVector();
        const Vec3 a = c.arg(1).isVector() ? c.arg(1).asVector() : Vec3{0, 0, 0};
        e->obj->set("origin", Value(o));
        e->obj->set("angles", Value(Vec3{0, a[1], 0}));
        e->velocity = {0, 0, 0};
        // The client spawn starts from an empty player state: the gametype hands out the weapons again
        // (_weapons.gsc:279 gives the pistol only when the primaryb slot is "none").
        inventory(*e) = Inventory{};
        e->obj->set("$weapon", Value("none"));
        // ClientSpawn (1.3 Linux dedicated server 0x80f910e) clears the whole client but its persistent part: the shell
        // shock fields with it, and the movement state restarts (docs/spec/mp.md "Client spawn").
        MpClient& cl = mp_->clients[size_t(mpClientOf(*e))];
        cl.shockIndex = 0;
        cl.shockStartMs = 0;
        cl.shockDurationMs = 0;
        cl.controlsFrozen = false;
        cl.weaponDisabled = false;
        cl.knockbacks.clear();
        cl.damageTimerMs = cl.damageDurationMs = 0;
        ++cl.spawnCount;
        cl.setViewAngles = {a[0], a[1], a[2]};  // ClientSpawn sets the view (0x80f940e -> SetClientViewAngle)
        ++cl.viewAngleSets;
        if (e == player_) {
            playerTeleport_ = o;
            playerViewAngles_ = a;
            playerSpawn_ = o;
            playerDeadWeaponless_ = false;
            controlsFrozen_ = false;
            // The cleared client holds the accumulated damage too: the fatal hit of the previous life (not reported while
            // dead) must not show as a hit direction icon after the respawn.
            damageBlood_ = 0;
            damageFromWorld_ = false;
        }
        return Value();
    });
    // finishPlayerDamage(inflictor, attacker, damage, dflags, mod, weapon, point, dir, hitloc, timeOffset): the
    // gametype's application of the damage (dm.gsc:268, docs/spec/mp.md "finishPlayerDamage"): the push on the player, the god
    // flags, the health and the notify; at 0 or below the player dies. The game has no armor.
    vm_.registerBuiltin("finishplayerdamage", [this, clientOf](CallContext& c) {
        MpClient& cl = clientOf(c);
        Entity& t = *cl.ent;
        int amount = c.arg(2).isNumber() ? c.arg(2).asInt() : 0;
        if (amount <= 0 || t.obj->get("sessionstate").toString() != "playing") return Value();
        const int h = t.obj->get("health").asInt();
        if (h <= 0) return Value();
        const int dflags = c.arg(3).isNumber() ? c.arg(3).asInt() : 0;
        if (!(dflags & 4) && !mpClientOnTurret(int(&cl - mp_->clients.data()))) {
            KnockbackHit hit;
            hit.amount = amount;
            hit.mp = true;
            if (c.arg(7).isVector()) {
                const Vec3 d = c.arg(7).asVector();
                hit.dir = {d[0], d[1], d[2]};
            }
            if (&t == player_) playerKnockbacks_.push_back(hit);
            else cl.knockbacks.push_back(hit);
        }
        if (t.obj->get("$godmode").truthy()) return Value();
        if (t.obj->get("$demigod").truthy() && h - amount <= 0) amount = h - 1;
        const float timePerPoint = cvarFloat("player_dmgtimer_timePerPoint", 100.0f);
        const int maxTime = int(cvarFloat("player_dmgtimer_maxTime", 750.0f));
        cl.damageTimerMs = std::min(cl.damageTimerMs + int(float(amount) * timePerPoint), maxTime);
        cl.damageDurationMs = cl.damageTimerMs;
        float hitYaw = 0;
        if (c.arg(7).isVector()) {
            const Vec3 d = c.arg(7).asVector();
            hitYaw = std::trunc(std::atan2(d[1], d[0]) * 180.0f / 3.14159265f);
        }
        float viewYaw = t.obj->get("angles").isVector() ? t.obj->get("angles").asVector()[1] : 0.0f;
        if (viewYaw < 0) viewYaw += 360.0f;
        cl.flinchYaw = hitYaw - std::trunc(viewYaw);
        const int health = std::max(h - amount, -999);
        t.obj->set("health", Value(int32_t(health)));
        vm_.notify(t.obj, "damage", {Value(int32_t(amount)), attackerValue(entityOf(c.arg(1)))});
        if (&t == player_) {
            // The engine's player damage (0x80fb316 -> 0x8102886) accumulates the damage and its direction as SP G_Damage
            // does; the hit direction icons read it (docs/spec/mp-hud.md).
            damageBlood_ += amount;
            const Vec3 d = c.arg(7).isVector() ? c.arg(7).asVector() : Vec3{0, 0, 0};
            if (d[0] != 0 || d[1] != 0 || d[2] != 0) {
                damageFrom_ = d;
                damageFromWorld_ = false;
            } else {
                damageFrom_ = t.obj->get("origin").asVector();
                damageFromWorld_ = true;
            }
        }
        if (health <= 0) {
            Entity* inflictor = entityOf(c.arg(0));
            Entity* attacker = entityOf(c.arg(1));
            const Vec3 dir = c.arg(7).isVector() ? c.arg(7).asVector() : Vec3{0, 0, 0};
            // The weapon is the one the damage carried (argument 5), not what the attacker holds now.
            const std::string weapon = c.arg(5).isString() && !c.arg(5).asString().empty() ? c.arg(5).asString() : std::string("none");
            mpPlayerDie(t, inflictor, attacker, amount, c.arg(4).toString(), weapon, dir, c.arg(8).isDefined() ? c.arg(8).toString() : "none");
        }
        return Value();
    });
    // suicide (0x80fbbd2..0x80fbc8d): the god flags and health are cleared, then the death path runs with the player as its own
    // inflictor and attacker, 100000 damage and MOD_SUICIDE (12).
    vm_.registerBuiltin("suicide", [this, clientOf](CallContext& c) {
        MpClient& cl = clientOf(c);
        Entity& t = *cl.ent;
        if (t.obj->get("health").asInt() <= 0) return Value();
        t.obj->set("health", Value(int32_t(0)));
        mpPlayerDie(t, &t, &t, 100000, "MOD_SUICIDE", "none", Vec3{0, 0, 0}, "none");
        return Value();
    });
    vm_.registerBuiltin("getguid", [clientOf](CallContext& c) {
        clientOf(c);
        return Value(int32_t(0)); // a local client has no key GUID
    });
    // makeCvarServerInfo(name, default) (VAs in the 1.3 Linux dedicated server: 0x8116bfa): the cvar keeps its value or
    // takes the default when it has none, and becomes a server-info cvar that the clients see (configstrings 142..).
    vm_.registerBuiltin("makecvarserverinfo", [this](CallContext& c) {
        if (c.argc() < 1) return Value();
        const std::string name = c.arg(0).toString();
        auto it = cvars.find(name);
        if ((it == cvars.end() || it->second.empty()) && c.argc() >= 2) cvars[name] = textOf(c.arg(1));
        auto& list = mp_->serverInfoCvars;
        if (std::find(list.begin(), list.end(), name) == list.end()) list.push_back(name);
        return Value();
    });
    vm_.registerBuiltin("setclientcvar", [this, clientOf](CallContext& c) {
        MpClient& cl = clientOf(c);
        const std::string v = wireText(c, 1);
        cl.cvars[lowerStr(c.arg(0).toString())] = v;
        const int n = int(&cl - mp_->clients.data());
        mp_->clientCommands.push_back({n, "v " + c.arg(0).toString() + " \"" + v + "\""});
        return Value();
    });
    // openMenu / openMenuNoMouse / closeMenu (docs/spec/mp.md "Script menus"): the server sends the client the menu's
    // precache index (popupopen) or a close (popupclose). Every client keeps its open menu here (bots and mpAutoJoin read
    // it); only the local client's commands reach this process's front end.
    auto menuIndex = [this](const std::string& name) {
        for (size_t i = 0; i < menus_.size(); ++i)
            if (lowerStr(menus_[i]) == lowerStr(name)) return int(i);
        return -1;
    };
    auto openMenu = [this, clientOf, menuIndex](CallContext& c, bool mouse) {
        MpClient& cl = clientOf(c);
        const std::string name = c.arg(0).toString();
        const int index = menuIndex(name);
        // TODO(spec): the error text of the name lookup for a menu that was not precached (as in SP, 0x4f89f0).
        if (index < 0) throw std::runtime_error("openmenu: menu " + name + " was not precached");
        cl.openMenu = name;
        // Every call is a server command to that client, in call order with its other commands, so a closeMenu followed by
        // an openMenu in the same frame reaches the client as both (a per-frame diff of openMenu lost the close and left
        // the previous menu open under the new one).
        mp_->clientCommands.push_back({int(&cl - mp_->clients.data()), "t " + std::to_string(index) + (mouse ? "" : " 1")});
        if (int(&cl - mp_->clients.data()) == mp_->localClient)
            clientCommands_.push_back({"popupopen", std::to_string(index) + (mouse ? "" : " 1")});
        return Value();
    };
    vm_.registerBuiltin("openmenu", [openMenu](CallContext& c) { return openMenu(c, true); });
    vm_.registerBuiltin("openmenunomouse", [openMenu](CallContext& c) { return openMenu(c, false); });
    vm_.registerBuiltin("closemenu", [this, clientOf](CallContext& c) {
        MpClient& cl = clientOf(c);
        cl.openMenu.clear();
        mp_->clientCommands.push_back({int(&cl - mp_->clients.data()), "u"}); // TODO(spec): the close-menu letter (net.md section 10)
        if (int(&cl - mp_->clients.data()) == mp_->localClient) clientCommands_.push_back({"popupclose", ""});
        return Value();
    });
    // closeInGameMenu: TODO(spec) its server command; the script menus opened by the ESC key (g_scriptMainMenu) are
    // script menus too, so closeMenu already closes them here.
    // closeInGameMenu (VAs in the 1.3 Linux dedicated server: 0x80fbed2) sends the client the command K.
    vm_.registerBuiltin("closeingamemenu", [this, clientOf](CallContext& c) {
        MpClient& cl = clientOf(c);
        mp_->clientCommands.push_back({int(&cl - mp_->clients.data()), "K"});
        return Value();
    });
    // iprintln / iprintlnbold exist twice (lnxded function table 0x8110ad2 / 0x8110afe, method table 0x80fca18 /
    // 0x80fca9e): the function sends the message to every client (client -1), the method only to the client `self`.
    for (const char* fn : {"iprintln", "iprintlnbold"}) {
        Builtin base = vm_.builtin(fn);
        vm_.registerBuiltin(fn, [this, base, clientOf, name = std::string(fn)](CallContext& c) -> Value {
            int target = -1;
            if (c.self.isDefined()) {
                MpClient& cl = clientOf(c);
                target = int(&cl - mp_->clients.data());
            }
            mp_->prints.push_back(name + "[" + std::to_string(target) + "] " + wireText(c, 0));
            if (target >= 0 && target != mp_->localClient) return Value();
            return base(c);
        });
    }
    // newClientHudElem(player) (0x8103c9e): the element belongs to that client (the argument must be a client) and is
    // drawn only on its screen. newTeamHudElem(team) (0x8103d0a): drawn for the clients of that team.
    // TODO(spec): the team element's exact team test (the team string is mapped to a team number at 0x8103d0a).
    Builtin newHud = vm_.builtin("newhudelem");
    vm_.registerBuiltin("newclienthudelem", [this, newHud](CallContext& c) -> Value {
        Entity* e = entityOf(c.arg(0));
        const int cl = e ? mpClientOf(*e) : -1;
        if (cl < 0) throw std::runtime_error("newClientHudElem: the argument is not a client");
        Value h = newHud(c);
        h.asObject()->set("$owner", Value(int32_t(cl)));
        return h;
    });
    vm_.registerBuiltin("newteamhudelem", [newHud](CallContext& c) -> Value {
        Value h = newHud(c);
        h.asObject()->set("$team", Value(c.arg(0).toString()));
        return h;
    });
    vm_.registerBuiltin("logprint", [this](CallContext& c) {
        mp_->log.push_back(c.arg(0).toString());
        return Value();
    });
    vm_.registerBuiltin("issplitscreen", [](CallContext&) { return Value(int32_t(0)); });
    // exitLevel (0x8116ab4): one-shot like map_restart (state 2: a later map_restart is "exitlevel already called").
    vm_.registerBuiltin("exitlevel", [this](CallContext&) {
        if (mp_->restartState == 0) mp_->restartState = 2;
        mp_->exitRequested = true;
        return Value();
    });
    vm_.registerBuiltin("setspawnweapon", [clientOf](CallContext& c) {
        clientOf(c).ent->obj->set("$weapon", Value(lowerStr(c.arg(0).toString())));
        return Value();
    });
    // cloneplayer(deathAnimDuration): a body entity left where the player died (dm.gsc:381; _deathicons.gsc reads its
    // origin). The bodies use a queue of 8 (VAs in the 1.3 Linux dedicated server: 0x811eaa4 takes entity
    // 64 + (counter % 8), the body that held the slot disappears); docs/spec/net.md section 11.
    // A body is only ever replaced when its slot is reused (no timer); docs/spec/mp-players.md "The body".
    vm_.registerBuiltin("cloneplayer", [this, clientOf](CallContext& c) {
        Entity& p = *clientOf(c).ent;
        const int slot = mp_->bodyQueueNext % 8;
        const int flip = (mp_->bodyQueueNext / 8 + 1) % 2 * 2; // eFlags bit 2, toggled on each reuse of the slot
        ++mp_->bodyQueueNext;
        for (const auto& e : entities_)
            if (e->classname == "playercorpse" && !e->obj->removed && e->obj->get("$bodyslot").isNumber() &&
                e->obj->get("$bodyslot").asInt() == slot)
                e->obj->removed = true;
        Entity& body = spawnEntity("playercorpse");
        body.obj->set("$bodyslot", Value(int32_t(slot)));
        body.obj->set("$bodyflip", Value(int32_t(flip)));
        body.obj->set("$bodyborn", Value(int32_t(timeMs_)));
        body.obj->set("$bodythink", Value(int32_t(timeMs_ + (c.arg(0).isNumber() ? std::max(c.arg(0).asInt(), 0) : 0))));
        body.obj->set("$bodyclient", Value(int32_t(mpClientOf(p))));
        body.obj->set("origin", p.obj->get("origin"));
        body.obj->set("angles", p.obj->get("angles"));
        // The body falls from where the player died with the victim's velocity (docs/spec/mp-players.md "The body").
        {
            const float cap = cvarFloat("g_clonePlayerMaxVelocity", 80.0f);
            Vec3 v{p.velocity[0], p.velocity[1], p.velocity[2]};
            for (int k = 0; k < 2; ++k) v[size_t(k)] = std::min(v[size_t(k)], cap);
            body.obj->set("$trtype", Value(int32_t(5)));
            body.obj->set("$trtime", Value(float(timeMs_)));
            body.obj->set("$trduration", Value(int32_t(0)));
            body.obj->set("$trbase", p.obj->get("origin"));
            body.obj->set("$trdelta", Value(v));
            const int vh = mpClientOf(p) >= 0 ? mp_->clients[size_t(mpClientOf(p))].animInput.move.viewHeightTarget : 60;
            body.obj->set("$bodyheight", Value(vh == 11 ? 30.0f : vh == 40 ? 50.0f : 70.0f));
        }
        if (p.obj->get("model").isDefined()) body.obj->set("model", p.obj->get("model"));
        // The body wears the player's attachments and plays the death animation (docs/spec/mp-players.md, "The body").
        body.attachments = p.attachments;
        const int cl = mpClientOf(p);
        const std::string anim = cl >= 0 ? mp_->clients[size_t(cl)].anim.deathAnim : std::string();
        if (const AnimTree* tree = animLib_.tree("multiplayer"); tree && tree->find(anim) >= 0) {
            body.anim = std::make_unique<AnimState>(*tree, animLib_);
            AnimState::SetArgs a;
            a.blendTime = 0;
            a.knob = true;
            body.anim->set(tree->find(anim), a);
            // The node number goes out as the body's legs / torso animation (docs/spec/net.md section 11).
            body.obj->set("$deathanim", Value(int32_t(tree->find(anim))));
        }
        return Value(body.obj);
    });
    // useButtonPressed of a non-local client reads its own buttons (use 0x28 as for the local player).
    Builtin localUse = vm_.builtin("usebuttonpressed");
    vm_.registerBuiltin("usebuttonpressed", [this, localUse](CallContext& c) -> Value {
        Entity* e = entityOf(c.self);
        const int cl = e ? mpClientOf(*e) : -1;
        if (cl < 0 || e == player_) return localUse(c);
        return Value(int32_t((mp_->clients[size_t(cl)].buttons & 0x28u) ? 1 : 0));
    });
    // attackbuttonpressed (bit 1) and meleebuttonpressed (bit 4) read the same usercmd buttons of a non-local client.
    for (const auto& [name, bitInit] : {std::pair<const char*, uint32_t>{"attackbuttonpressed", 0x1u}, {"meleebuttonpressed", 0x4u}}) {
        Builtin local = vm_.builtin(name);
        vm_.registerBuiltin(name, [this, local, bit = bitInit](CallContext& c) -> Value {
            Entity* e = entityOf(c.self);
            const int cl = e ? mpClientOf(*e) : -1;
            if (cl < 0 || e == player_) return local(c);
            return Value(int32_t((mp_->clients[size_t(cl)].buttons & bit) ? 1 : 0));
        });
    }
    registerMpDropBuiltins();     // dropItem (src/game/mpdrop.cpp)
    // Player methods whose single-player version changes this process's own player (its view, input, sounds or the
    // front end's movement): on any other client they must not reach the local player (docs/spec/mp.md "Per-client
    // player state"). The single-player builtin still runs for its argument checks and its entity-side effects; the
    // local-player state it touched is restored afterwards. TODO(spec): the per-client effect of these on a network
    // client (the original keeps them in that client's player state; this port has no wire field for them yet).
    auto nonLocalClient = [this](CallContext& c) -> int {
        Entity* e = c.self.isObject() ? entityOf(c.self) : nullptr;
        const int cl = e ? mpClientOf(*e) : -1;
        return cl >= 0 && e != player_ ? cl : -1;
    };
    for (const char* name : {"viewkick", "setorigin", "setplayerangles", "freezecontrols", "allowstand", "allowcrouch", "allowprone",
                             "allowleanleft", "allowleanright", "setreverb", "deactivatereverb", "playerlinktodelta",
                             "playerlinktoabsolute", "unlink", "setblur"}) {
        Builtin base = vm_.builtin(name);
        if (!base) continue;
        vm_.registerBuiltin(name, [this, base, nonLocalClient, n = std::string(name)](CallContext& c) -> Value {
            const int cl = nonLocalClient(c);
            if (cl < 0) return base(c);
            const int blood = damageBlood_; // viewkick writes the damage feedback fields (client +0x8658..)
            const Vec3 from = damageFrom_;
            const bool fromWorld = damageFromWorld_;
            const auto teleport = playerTeleport_, angles = playerViewAngles_;
            const bool frozen = controlsFrozen_;
            const uint32_t flags = pmFlags_;
            const auto link = playerLink_;
            const size_t commands = clientCommands_.size();
            Value r = base(c);
            damageBlood_ = blood;
            damageFrom_ = from;
            damageFromWorld_ = fromWorld;
            playerTeleport_ = teleport;
            playerViewAngles_ = angles;
            controlsFrozen_ = frozen;
            pmFlags_ = flags;
            playerLink_ = link;
            clientCommands_.resize(std::min(commands, clientCommands_.size()));
            MpClient& m = mp_->clients[size_t(cl)];
            if (n == "freezecontrols") m.controlsFrozen = c.arg(0).truthy(); // 0x80fd204: the client field +0x27b4
            if (n == "setplayerangles" && c.arg(0).isVector()) { // 0x80fa964 -> SetClientViewAngle: the client's delta_angles
                const Vec3 a = c.arg(0).asVector();
                m.setViewAngles = {a[0], a[1], a[2]};
                ++m.viewAngleSets;
            }
            if (n == "setplayerangles" && bots_)
                for (MpBot& b : bots_->bots)
                    if (b.client == cl) {
                        const Vec3 a = c.arg(0).asVector();
                        b.pitch = a[0];
                        b.yaw = a[1];
                    }
            return r;
        });
    }
    // Queries of the player's own input read this process's commands in single player; another client answers from its
    // own state: the view angles and stance of its movement. A bot has no aim, fire or movement input to answer with; that is its design (bots are not in the original).
    vm_.registerBuiltin("getplayerangles", [this, nonLocalClient, base = vm_.builtin("getplayerangles")](CallContext& c) -> Value {
        const int cl = nonLocalClient(c);
        if (cl < 0) return base(c);
        Entity* e = entityOf(c.self);
        Vec3 a = e->obj->get("angles").isVector() ? e->obj->get("angles").asVector() : Vec3{0, 0, 0};
        if (bots_)
            for (const MpBot& b : bots_->bots)
                if (b.client == cl) a = {b.pitch, b.yaw, 0};
        return Value(a);
    });
    vm_.registerBuiltin("getstance", [this, nonLocalClient, base = vm_.builtin("getstance")](CallContext& c) -> Value {
        const int cl = nonLocalClient(c);
        if (cl < 0) return base(c);
        int vh = 60;
        if (const MpPlayerInput* pi = mpPlayerInput(cl); pi && pi->valid) vh = pi->move.viewHeightTarget;
        if (bots_)
            for (const MpBot& b : bots_->bots)
                if (b.client == cl && b.psValid) vh = b.ps.stance == Stance::Prone ? 11 : b.ps.stance == Stance::Crouch ? 40 : 60;
        return Value(std::string(vh <= 11 ? "prone" : vh <= 40 ? "crouch" : "stand"));
    });
    vm_.registerBuiltin("playerads", [this, nonLocalClient, base = vm_.builtin("playerads")](CallContext& c) -> Value {
        const int cl = nonLocalClient(c);
        if (cl < 0) return base(c);
        const MpPlayerInput* pi = mpPlayerInput(cl);
        return Value(pi && pi->ads ? 1.0f : 0.0f);
    });
    for (const char* name : {"isfiring", "ismeleeing", "isthrowinggrenade", "getnormalizedmovement"}) {
        Builtin base = vm_.builtin(name);
        if (!base) continue;
        const bool vec = std::string(name) == "getnormalizedmovement";
        vm_.registerBuiltin(name, [nonLocalClient, base, vec](CallContext& c) -> Value {
            if (nonLocalClient(c) < 0) return base(c);
            return vec ? Value(Vec3{0, 0, 0}) : Value(int32_t(0));
        });
    }
    registerMpGametypeBuiltins(); // team scores, spawn placement, map_restart (src/game/mpgametypes.cpp)
    registerBotGoalBuiltins(); // bot goals and roles (src/game/mpbotgoals.cpp)
    registerBotBuiltins(); // addtestclient (src/game/mpbots.cpp)
    registerMpHudBuiltins(); // obituary, playLocalSound, MP objectives, setWayPoint (src/game/mphud.cpp)
    registerChatBuiltins(); // sayall / sayteam (src/game/mpchat.cpp)
    registerSpectateBuiltins(); // allowSpectateTeam, precacheStatusIcon (src/game/mpspectate.cpp)
    registerKillcamBuiltins(); // setarchive (src/game/mpkillcam.cpp)
}

} // namespace iw2::game
