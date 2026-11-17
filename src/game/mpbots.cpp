// Multiplayer bots (docs/spec/mp-bots.md). OpenIW2 extension: the original's addtestclient clients never move or shoot;
// here a controller produces their player input every server frame. Reused engine parts: pmove (docs/spec/pmove.md),
// the weapon state machine (docs/spec/weapons.md), bullet fire and damage (FireWeapon / Bullet_Fire path, combat.cpp),
// the server's item touch / use and turret use of every client. Perception is in mpbotsense.cpp, the aim and the shots
// in mpbotaim.cpp, weapons from the ground and cover in mpbotitems.cpp, turrets in mpbotturret.cpp.
#include "game/mpbots.h"
#include "game/playerevents.h"
#include "game/viewweapon.h"
#include "game/world.h"

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace iw2::game {

using script::CallContext;
using script::Value;

namespace {

constexpr float kDt = 0.05f; // one server frame
constexpr float kDeg = 3.14159265358979f / 180.0f;
constexpr uint32_t kNavGenVersion = 9; // part of the cache key: bump when the generator changes

const BotSkill kSkills[4] = {
    // name       react  turn  noise omega damp  lead  tol   notice sight  burst pause  aimLag memory hear  steps  grenade corners
    {"recruit", 450, 180.0f, 5.5f, 5.0f, 0.55f, 0.3f, 2.2f, 500, 1500.0f, 3, 700, 300, 4000, 1200.0f, 300.0f, 0.25f, false},
    {"regular", 320, 260.0f, 3.4f, 7.0f, 0.65f, 0.5f, 1.6f, 350, 2500.0f, 4, 500, 200, 6000, 1800.0f, 400.0f, 0.45f, false},
    {"hardened", 230, 360.0f, 2.0f, 9.5f, 0.75f, 0.75f, 1.25f, 250, 3500.0f, 6, 350, 130, 8000, 2500.0f, 500.0f, 0.65f, true},
    {"veteran", 160, 480.0f, 1.25f, 12.0f, 0.85f, 0.9f, 1.0f, 160, 5000.0f, 8, 250, 70, 10000, 3500.0f, 600.0f, 0.85f, true},
};

float wrap180(float a) {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

Vec3 v3(const Value& v) { return v.isVector() ? v.asVector() : Vec3{0, 0, 0}; }

float dist2(const Vec3& a, const Vec3& b) {
    const float x = a[0] - b[0], y = a[1] - b[1];
    return std::sqrt(x * x + y * y);
}

float dist3(const Vec3& a, const Vec3& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// How near its start node a bot must stand to begin a ladder, mantle or leap link.
float botTraverseReach(int flags) { return (flags & NavLinkFlag::Leap) ? 8.0f : 20.0f; }

} // namespace

const BotSkill& botSkill(int skill) { return kSkills[std::clamp(skill, 0, 3)]; }

std::array<float, 2> botMoveInput(const std::array<float, 2>& dir, float yawDeg) {
    const float c = std::cos(yawDeg * kDeg), s = std::sin(yawDeg * kDeg);
    float f = dir[0] * c + dir[1] * s;
    float r = dir[0] * s - dir[1] * c; // the right axis of yaw is (sin, -cos)
    const float m = std::max(std::fabs(f), std::fabs(r));
    if (m < 1e-4f) return {0, 0};
    return {f / m, r / m};
}

float botTurn(float from, float to, float maxStep) {
    const float d = wrap180(to - from);
    return wrap180(from + std::clamp(d, -maxStep, maxStep));
}

const MantleAnims* World::mpMantleAnims() {
    if (!bots_) bots_ = std::make_shared<BotSystem>();
    if (!bots_->mantleLoaded) { // xanim/player_mantle_* (tree indices 1-10, pmove.h MantleAnims)
        bots_->mantleLoaded = true;
        for (int i = 1; i < 11; ++i) {
            try {
                if (const auto d = vfs_.read(std::string("xanim/") + mantleAnimName(i))) bots_->mantleAnims[size_t(i)] = assets::parseXAnim(*d);
            } catch (const std::exception&) {
            }
            if (bots_->mantleAnims[size_t(i)]) bots_->mantle.anims[size_t(i)] = &*bots_->mantleAnims[size_t(i)];
        }
    }
    return bots_->mantle.loaded() ? &bots_->mantle : nullptr;
}

// The map's navigation graph: from the cache when its key matches this map's BSP data and the generator version,
// else generated from the collision world (seeded with the MP spawn points) and written to the cache.
const NavGraph& World::mpNav() {
    mpMantleAnims();
    if (bots_->nav) return *bots_->nav;
    // The flood fill starts at the spawn points left by the game type, so they are part of the cache key: two game
    // types on one map (parallel processes sharing the cache) must not read each other's graph.
    std::vector<std::array<float, 3>> seeds;
    for (const auto& e : entities_)
        if (!e->obj->removed && e->classname.rfind("mp_", 0) == 0 && e->classname.find("spawn") != std::string::npos)
            seeds.push_back(v3(e->obj->get("origin")));
    uLong crc = crc32(0L, bspData_.data(), uInt(bspData_.size()));
    if (!seeds.empty()) crc = crc32(crc, reinterpret_cast<const Bytef*>(seeds.data()), uInt(seeds.size() * sizeof(seeds[0])));
    const uint64_t key = (uint64_t(kNavGenVersion) << 32) | uint64_t(crc);
    char hexKey[16];
    std::snprintf(hexKey, sizeof hexKey, "%08x", unsigned(crc));  // one file per key: no rebuilds between game types
    const std::string dir = navCacheDir();
    const std::string path = dir.empty() ? std::string() : dir + "/" + mapName_ + "_" + std::string(hexKey) + ".nav";
    if (!path.empty()) bots_->nav = loadNavGraph(path, key);
    if (!bots_->nav) {
        std::array<float, 3> mins{-4096, -4096, -1024}, maxs{4096, 4096, 1024};
        const auto models = bsp_->lump(assets::BspLump::Models); // lump 35 row 0: the world's bounds (docs/spec/d3dbsp.md)
        if (models.size() >= 24) {
            std::memcpy(mins.data(), models.data(), 12);
            std::memcpy(maxs.data(), models.data() + 12, 12);
        }
        // Spawn points outside the world model's bounds (mp_dawnville has some) widen the scanned box.
        for (const auto& sp : seeds)
            for (int k = 0; k < 3; ++k) mins[size_t(k)] = std::min(mins[size_t(k)], sp[size_t(k)] - 256.0f), maxs[size_t(k)] = std::max(maxs[size_t(k)], sp[size_t(k)] + 256.0f);
        // PathGraph links hold 16-bit node numbers: a coarser grid when a large map gives more nodes.
        NavParams np;
        np.mantle = &bots_->mantle;
        const auto genStart = std::chrono::steady_clock::now();
        for (;;) {
            bots_->nav = generateNavGraph(*collision_, mins, maxs, seeds, np);
            if (bots_->nav->size() < 65536 || np.cell > 64.0f) break;
            np.cell *= 1.5f;
        }
        if (!path.empty()) saveNavGraph(path, *bots_->nav, key);
        // A miss costs seconds (tens on a busy machine): say so, with the file, so a slow map load can be told apart.
        std::printf("navigation graph %s: generated in %.1f s, %zu nodes\n", path.empty() ? mapName_.c_str() : path.c_str(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - genStart).count(), bots_->nav->size());
    }
    paths_ = bots_->nav->toPathGraph(); // A* (PathGraph::findPath) runs on it
    return *bots_->nav;
}

void World::mpSetBotSkill(int skill) {
    if (!bots_) bots_ = std::make_shared<BotSystem>();
    bots_->skill = std::clamp(skill, 0, 3);
}

// addtestclient (VAs in the 1.3 Linux dedicated server: 0x08116bd8 -> 0x080913da): a client slot is connected with the
// name bot<N>; here the slot is also given a controller.
int World::mpAddBot(const std::string& name) {
    if (!mp_) return -1;
    if (!bots_) bots_ = std::make_shared<BotSystem>();
    if (int(bots_->bots.size()) >= kMaxBots) return -1; // the bot cap (kMaxBots)
    const std::string n = name.empty() ? "bot" + std::to_string(bots_->counter) : name;
    const int slot = mpConnect(n);
    if (slot < 0) return -1;
    ++bots_->counter;
    MpBot b;
    b.client = slot;
    b.skill = bots_->skill;
    // The bot's own traits (docs/spec/mp-bots.md "Skill presets"): factors of 0.85..1.2 from the bot's number, so that the
    // bots of one skill differ; drawn without the game's random numbers.
    {
        uint32_t h = uint32_t(bots_->counter) * 2654435761u + 0x9e3779b9u;
        auto next = [&] { h ^= h << 13, h ^= h >> 17, h ^= h << 5; return 0.85f + float(h % 1000u) / 1000.0f * 0.35f; };
        b.trait.reaction = next();
        b.trait.aim = next();
        b.trait.aggression = next();
    }
    if (bots_->bots.capacity() < 64) bots_->bots.reserve(64); // mpBotsFrame holds references while scripts may add bots
    bots_->bots.push_back(b);
    return slot;
}

int World::mpBotCount() const { return bots_ ? int(bots_->bots.size()) : 0; }

void World::mpKickBot(int client) {
    if (!bots_ || !mpIsBot(client)) return;
    mpDisconnect(client);
    auto& v = bots_->bots;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const MpBot& b) { return b.client == client; }), v.end());
}

bool World::mpIsBot(int client) const {
    if (!bots_) return false;
    return std::any_of(bots_->bots.begin(), bots_->bots.end(), [&](const MpBot& b) { return b.client == client; });
}

void World::mpBotSetCombat(int client, bool enabled) {
    if (!bots_) return;
    for (MpBot& b : bots_->bots)
        if (b.client == client) b.combat = enabled;
}

std::string World::mpBotDescribe(int client) const {
    if (!bots_) return {};
    for (const MpBot& b : bots_->bots)
        if (b.client == client) {
            std::ostringstream s;
            s << "bot " << client << " skill " << botSkill(b.skill).name << " enemy " << b.enemy << (b.enemyVisible ? " visible" : "")
              << " path " << b.pathIdx << "/" << b.path.size() << " clip " << b.clip << " shots " << b.shots << " hits " << b.stats.hits
              << " kills " << b.stats.kills << " grenades " << b.stats.grenades << " stuck " << b.stuck
              << (b.objective.empty() ? "" : " objective " + b.objective) << (b.goalReached ? " goal reached" : "");
            return s.str();
        }
    return {};
}

void World::registerBotBuiltins() {
    // addtestclient(): connects a test client and returns its entity (undefined when the slots are full).
    vm_.registerBuiltin("addtestclient", [this](CallContext&) -> Value {
        const int slot = mpAddBot();
        Entity* e = slot >= 0 ? mpClientEntity(slot) : nullptr;
        return e ? Value(e->obj) : Value();
    });
}

void World::mpBotsFrame() {
    if (!bots_ || bots_->bots.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();
    // Disconnected bots leave the list.
    bots_->bots.erase(std::remove_if(bots_->bots.begin(), bots_->bots.end(),
                                     [&](const MpBot& b) { return mp_->clients[size_t(b.client)].state == MpClientState::Free; }),
                      bots_->bots.end());
    bots_->plansThisFrame = 0;
    ++bots_->frameCount;
    // What every bot's perception reads of the clients, once for all of them.
    bots_->clients.assign(mp_->clients.size(), BotClientInfo{});
    for (size_t c = 0; c < mp_->clients.size(); ++c) {
        const MpClient& oc = mp_->clients[c];
        BotClientInfo& ci = bots_->clients[c];
        ci.ent = oc.ent;
        if (oc.state != MpClientState::Active || !oc.ent || !isAlive(*oc.ent) || oc.ent->obj->get("sessionstate").toString() != "playing") continue;
        ci.playing = true;
        const std::string team = oc.ent->obj->get("sessionteam").toString();
        ci.team = team == "axis" ? 1 : team == "allies" ? 2 : 0;
        ci.origin = v3(oc.ent->obj->get("origin"));
        ci.eye = eyePosition(*oc.ent);
        const MpPlayerInput* pi = mpPlayerInput(int(c));
        ci.speed = std::max(pi && pi->valid ? pi->move.xySpeed : 0.0f, std::hypot(oc.ent->velocity[0], oc.ent->velocity[1]));
        ci.viewHeight = pi && pi->valid ? pi->move.viewHeightTarget : 60;
        ci.ads = pi && pi->valid && pi->ads;
        if (oc.ent == player_) { // the listen server's own player: its stance and aim are the command's (its move runs in the front end)
            ci.viewHeight = playerCmd_.stance == 2 ? 11 : playerCmd_.stance == 1 ? 40 : 60;
            ci.ads = playerCmd_.adsFrac >= 0.5f;
        }
    }
    for (size_t bi = 0; bi < bots_->bots.size(); ++bi) mpBotFrame(bots_->bots[bi]);
    bots_->maxPlansInAFrame = std::max(bots_->maxPlansInAFrame, bots_->plansThisFrame);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    bots_->lastFrameUs = us;
    bots_->maxFrameUs = std::max(bots_->maxFrameUs, us);
    bots_->sumFrameUs += us;
}

void World::mpBotFrame(MpBot& b) {
    const int64_t now = timeMs_;
    MpClient& cl = mp_->clients[size_t(b.client)];
    Entity* e = cl.ent;
    if (!e) return;
    if (b.usePressed) { // the use button is held for one frame
        mpSetButtons(b.client, 0);
        b.usePressed = false;
    }
    b.buttons = 0; // this frame's buttons, sent once at the end of the frame (mpBotCommand)
    // Begin the frame after the connect (a real client sends it when it has loaded the map).
    if (cl.state == MpClientState::Connected) {
        mpBegin(b.client);
        b.joinAtMs = now + 100;
        return;
    }
    const std::string state = e->obj->get("sessionstate").toString();
    // Join through the gametype's menus like a player: team menu "autoassign", then the weapon menu it opens. A
    // spectator that already has a team and a weapon (pers) waits to respawn instead: ctf / sd / hq keep the dead as
    // spectators until the use button (ctf.gsc:701-735) or the next round.
    const Value pers = e->obj->get("pers");
    auto persOf = [&](const char* k) -> std::string {
        if (!pers.isArray()) return {};
        const Value* v = std::get<script::ArrayPtr>(pers.v)->get(script::ArrayKey{true, 0, k});
        return v && v->isDefined() ? v->toString() : std::string();
    };
    const std::string persTeam = persOf("team");
    const bool hasTeam = persTeam == "allies" || persTeam == "axis";
    const bool waitingRespawn = state == "spectator" && hasTeam && !persOf("weapon").empty();
    if (!waitingRespawn && (state == "spectator" || (state != "playing" && state != "dead" && state != "intermission"))) {
        if (now < b.joinAtMs) return;
        if (cl.openMenu.rfind("weapon", 0) == 0) {
            // An allowed weapon of the bot's class (scr_allow_* cvars, docs/spec/mp-bots.md "Weapon classes").
            mpMenuResponse(b.client, cl.openMenu, mpBotPickWeapon(b, cl.openMenu));
            b.joinStep = 2;
        } else if (!hasTeam || b.joinStep == 0) {
            mpMenuResponse(b.client, vm_.game()->get("menu_team").toString(), "autoassign");
            b.joinStep = 1;
        } else {
            b.joinStep = 0; // a team but no weapon menu open: ask again
        }
        b.joinAtMs = now + 250;
        return;
    }
    if (state == "dead" || waitingRespawn) {
        b.psValid = false;
        b.stallSinceMs = -1;
        b.holdingUse = false;
        b.enemy = -1;
        b.path.clear();
        if (b.deadSinceMs < 0) b.deadSinceMs = now;
        // dm.gsc respawns on the use button after the death delay (docs/spec/mp.md "Player death").
        if (now - b.deadSinceMs >= 2500 && (now / 50) % 10 == 0) {
            mpSetButtons(b.client, 0x8);
            b.usePressed = true;
        }
        return;
    }
    b.deadSinceMs = -1;
    if (state != "playing") return;
    const NavGraph& nav = mpNav();
    const BotSkill& sk = botSkill(b.skill);

    // (Re)spawned or moved by script: restart the movement state at the entity.
    const Vec3 origin = v3(e->obj->get("origin"));
    const int spawns = mp_->clients[size_t(b.client)].spawnCount;
    // On a turret the turret code holds the bot behind the gun, and puts it back where it got on when it gets off
    // (docs/spec/turrets.md "Multiplayer"): that place is the bot's, without a restart of its movement.
    const bool onTurret = mpClientOnTurret(b.client);
    if (b.psValid && b.spawnCount == spawns && (onTurret || b.onTurret)) {
        b.ps.origin = origin;
        b.ps.velocity = {0, 0, 0};
    }
    if (!b.psValid || b.spawnCount != spawns || dist3(origin, b.ps.origin) > 64.0f) {
        b.spawnCount = spawns; // a client spawn clears the player state (docs/spec/mp.md "Client spawn")
        b.ps = PlayerState{};
        b.ps.origin = origin;
        b.ps.timeMs = now;
        markSpawned(b.ps);
        b.yaw = v3(e->obj->get("angles"))[1];
        b.pitch = 0;
        b.psValid = true;
        b.path.clear();
        b.stallSinceMs = -1;
        b.enemy = -1;
        b.progressPos = origin;
        b.progressMs = now;
        b.stuck = 0;
        b.anchorPos = origin;
        b.anchorMs = now;
        b.stillSinceMs = now;
        b.noReachSpells = 0;
        b.aimValid = false;
        b.smokeUsed = false;
        b.pistolSinceMs = -1;
        b.nextGrenadeMs = now + 3000;
        // A new life starts with a fresh weapon state (raised from nothing), as a network player's (netcombat).
        b.weapon = PlayerWeapon{};
        b.weaponWired = false;
        b.firePrev = b.reloadPrev = b.meleePrev = false;
        b.weaponStatePrev = 0;
        b.throwPending = {};
        b.burstShots = 0;
        b.lastSlots.clear();
        b.wasDefenceless = false;
        b.spawnWeapon.clear();
        b.detour = {};
        b.ignoreItems.clear();
        b.inCover = false;
        b.coverCooldownMs = 0;
        b.scanYaw = b.yaw;
        b.scanNextMs = now + 1000;
        b.aim = {};
        b.kick.reset();
        b.kickActive = false;
        b.aimBoneEnt = -1;
        b.notice.clear();
        b.alert = {};
        b.focusUntilMs = 0;
        b.enemyVisible = false;
        b.enemyParts = 0;
        b.hurtMs = -100000;
        b.onTurret = false;
        b.turretCooldownMs = now + 5000;
        b.killMs = -100000;
        b.weaponSwitchMs = -100000;
        b.wantReload = false;
    }
    // Ammunition from the inventory (docs/spec/mp-bots.md "Combat behaviour"): the gametype's loadout.
    Inventory& inv = inventory(*e);
    std::string weaponName = e->obj->get("$weapon").toString();
    auto clipOf = [&](const std::string& w) -> int& { return inv.clip[mpBotWeaponInfo(w).clipName]; };
    auto reserveOf = [&](const std::string& w) -> int& { return inv.ammo[mpBotWeaponInfo(w).ammoName]; };
    const bool hasWeapon = weaponName != "none" && !weaponName.empty();
    if (weaponName != b.clipWeapon) {
        b.clipWeapon = weaponName;
        b.burstShots = 0;
    }
    // The weapon the bot chose in the menu: the slot weapon that is no pistol, remembered at the spawn.
    if (b.spawnWeapon.empty())
        for (int sl = 1; sl <= 2; ++sl)
            if (!inv.slot[size_t(sl)].empty() && mpBotWeaponInfo(inv.slot[size_t(sl)]).cls != "pistol") b.spawnWeapon = inv.slot[size_t(sl)];
    // The weapon state machine reads and writes the clip and the reserve of the inventory (owned weapons only).
    if (!b.weaponWired) {
        b.weaponWired = true;
        World* wp = this;
        const int client = b.client;
        auto inventoryOf = [wp, client](const std::string& n) -> Inventory* {
            Entity* ent = size_t(client) < wp->mp_->clients.size() ? wp->mp_->clients[size_t(client)].ent : nullptr;
            if (!ent) return nullptr;
            Inventory& iv = wp->inventory(*ent);
            return iv.owned.count(n) ? &iv : nullptr;
        };
        b.weapon.lookup = [wp](const std::string& n) { return wp->weapon(n); };
        // The offhand grenade of a throw the bot solved (mpBotGrenade): the machine's +frag / +smoke picks it.
        b.weapon.findOffhand = [wp, client](const std::string&) -> std::string {
            const MpBot* ob = wp->mpBot(client);
            return ob && ob->throwPending.valid ? ob->throwPending.weapon : std::string();
        };
        b.weapon.readAmmo = [wp, inventoryOf](const std::string& n) -> std::optional<std::pair<int, int>> {
            Inventory* iv = inventoryOf(n);
            if (!iv) return std::nullopt;
            const BotWeaponInfo& info = wp->mpBotWeaponInfo(n);
            return std::make_pair(iv->clip[info.clipName], iv->ammo[info.ammoName]);
        };
        b.weapon.writeAmmo = [wp, inventoryOf](const std::string& n, int clip, int stock) {
            Inventory* iv = inventoryOf(n);
            if (!iv) return;
            const BotWeaponInfo& info = wp->mpBotWeaponInfo(n);
            iv->clip[info.clipName] = clip;
            iv->ammo[info.ammoName] = stock;
        };
    }
    b.clip = hasWeapon ? clipOf(weaponName) : 0;
    // The held weapon was taken away (a swap with a weapon on the ground): nothing is in the hand, the next weapon is
    // raised without a put-away, as for a network client (docs/spec/net.md section 13).
    if (e->obj->get("$weapontaken").truthy()) {
        e->obj->set("$weapontaken", Value());
        b.weapon.weaponTaken();
    }
    const Vec3 eye{b.ps.origin[0], b.ps.origin[1], b.ps.origin[2] + b.ps.viewHeight};

    // Perception (src/game/mpbotsense.cpp): what is inside the bot's view with a clear line, noticed after a while; sounds
    // and hits as believed places.
    const std::string myTeam = e->obj->get("sessionteam").toString();
    const bool teams = mp_->gametype != "dm";
    Entity* enemy = mpBotSense(b, *e, eye, weaponName);
    const bool fighting = enemy && b.combat;
    if (fighting) b.noEnemySinceMs = now;
    if (onTurret) { // behind a mounted machine gun (src/game/mpbotturret.cpp)
        mpBotTurretFrame(b, *e, enemy, eye);
        mpBotMeasure(b, *e, false);
        mpSetViewState(b.client, mpViewStateOf(b.weapon, b.ps));
        mpBotCommand(b);
        return;
    }
    b.onTurret = false;
    // A bot without a round in either slot weapon does not stand and fight (docs/spec/mp-bots.md "Ammunition and weapon
    // choice"): it goes on its way (its objective, a weapon on the ground) unless the enemy is within 300 units, where it
    // charges for the melee blow.
    int slotRounds = 0;
    for (int sl = 1; sl <= 2; ++sl)
        if (!inv.slot[size_t(sl)].empty()) slotRounds += clipOf(inv.slot[size_t(sl)]) + reserveOf(inv.slot[size_t(sl)]);
    const bool unarmed = slotRounds == 0;
    const bool charge = fighting && unarmed && dist3(eye, eyePosition(*enemy)) < 300.0f;
    const bool engage = fighting && (!unarmed || charge);

    // Objective (docs/spec/mp-bots.md, "Objectives"): the API goal or the gametype's objective for the bot's role.
    BotObjective obj = mpBotObjective(b, *e);
    const bool carrying = e->obj->get("flag").isDefined(); // ctf: the carrier runs home even under fire
    // Detours (docs/spec/mp-bots.md "Detours"): a weapon on the ground. Not for a flag carrier, not on a traversal, not
    // while planting or defusing or close to doing so; a fight comes first unless the bot has no round left.
    {
        const bool committed = carrying || b.holdingUse || (obj.valid && obj.use && dist3(obj.pos, b.ps.origin) < 600.0f) || b.goal.active;
        b.noNewDetour = b.traverseSinceMs >= 0; // on a ladder, mantle or leap a detour goes on, but none is begun
        if (committed) {
            b.detour = {};
            b.inCover = false;
        } else {
            // Cover first (a hurt bot, an empty clip); else a weapon on the ground; else a turret.
            const std::string held = e->obj->get("$weapon").toString();
            const bool heldOk = !held.empty() && held != "none";
            const bool emptyClip = heldOk && clipOf(held) == 0 && reserveOf(held) > 0;
            const int ws0 = b.weapon.stateNumber();
            const float ed = enemy ? dist3(eye, eyePosition(*enemy)) : dist3(b.enemyLastPos, b.ps.origin);
            if (!mpBotCoverDetour(b, *e, fighting, ed, emptyClip, ws0 >= 5 && ws0 <= 9, obj) &&
                (b.detour.kind == BotDetour::Turret || !mpBotItemDetour(b, *e, eye, engage, unarmed, obj)))
                mpBotTurretDetour(b, *e, eye, engage, unarmed, obj);
        }
    }
    b.objective = obj.valid ? obj.what : "";
    const Vec3 bmin{b.ps.origin[0] - 15, b.ps.origin[1] - 15, b.ps.origin[2]}, bmax{b.ps.origin[0] + 15, b.ps.origin[1] + 15, b.ps.origin[2] + 70};
    const bool atUse = obj.valid && obj.use && obj.useTarget && triggerContact(*obj.useTarget, bmin, bmax);
    // Holding use inside the target: the first frame presses (the use scan fires on a press), then it stays held.
    if (atUse) {
        if (b.holdingUse) b.buttons |= 0x8u;
        b.holdingUse = true;
    } else {
        b.holdingUse = false;
    }
    const bool objReached = obj.valid && !obj.use && dist2(obj.pos, b.ps.origin) <= obj.radius &&
                            std::fabs(obj.pos[2] - b.ps.origin[2]) < 72.0f;
    if (obj.valid && b.goal.active) b.goalReached = atUse || objReached;

    // Movement goal: the objective, else the enemy's last known position, else a random node.
    std::array<float, 2> moveDir{0, 0};
    bool linkCrouch = false;
    bool wantJump = b.wantJump;
    b.wantJump = false;
    // A fight stops the walk, except for a flag carrier and a bot on its last steps to a turret.
    const bool walkObjective = obj.valid && !atUse && !objReached &&
                               (!engage || carrying || b.detour.kind == BotDetour::Turret || b.detour.kind == BotDetour::Cover);
    if (walkObjective && (dist3(obj.pos, b.planGoal) > 128.0f || now >= b.replanMs)) {
        b.path.clear(); // the objective moved (a carrier, a new radio) or the periodic replan is due
        b.pathIdx = 0;
    }
    const size_t mainCount = nav.mainCount ? nav.mainCount : nav.size();
    if (!engage || walkObjective) {
        if (!(obj.valid && (atUse || objReached)) && nav.size() > 0) {
            // At most 2 plans per frame for all bots (docs/spec/mp-bots.md "Cost limits"); the others wait.
            if (b.pathIdx >= b.path.size() && bots_->plansThisFrame < 2) {
                ++bots_->plansThisFrame;
                b.planMs = now;
                b.path.clear();
                // The start: the nearest of the close nodes (160 units) that the body reaches in a straight line (at
                // most 8 tried), else the nearest within 512 (docs/spec/mp-bots.md "Stuck watch").
                int start = -1;
                {
                    const std::vector<int> close = nav.within(b.ps.origin, 160.0f);
                    for (size_t k = 0; k < close.size() && k < 8 && start < 0; ++k) {
                        if (std::find(b.badStarts.begin(), b.badStarts.end(), close[k]) != b.badStarts.end()) continue;
                        const auto& q = nav.nodes[size_t(close[k])];
                        if (std::fabs(q[2] - b.ps.origin[2]) > 40.0f) continue;
                        const TraceResult tr = collision_->trace(b.ps.origin, Vec3{q[0], q[1], std::max(q[2], b.ps.origin[2])}, {-15, -15, 18},
                                                                 {15, 15, 40}, Contents::PlayerSolid, e->number);
                        if (tr.fraction >= 1.0f && !tr.startSolid) start = close[k];
                    }
                    for (size_t k = 0; k < close.size() && start < 0 && !b.badStarts.empty(); ++k)
                        if (std::find(b.badStarts.begin(), b.badStarts.end(), close[k]) == b.badStarts.end()) start = close[k];
                    if (start < 0) start = nav.nearest(b.ps.origin, 512.0f);
                }
                int goal = -1;
                if (obj.valid) goal = nav.nearest(obj.pos, 512.0f, true);
                else if (b.enemy >= 0 && b.combat) goal = nav.nearest(b.enemyLastPos, 256.0f, true);
                if (goal < 0 || (goal == start && !obj.valid)) goal = nextRand() % int(mainCount);
                if (start >= 0 && goal != start) b.path = paths_.findPath(start, goal);
                b.pathIdx = b.path.size() > 1 ? 1 : 0;
                if (b.pathIdx == 1) { // the start node first when the way to the second one is not straight
                    const auto& q = nav.nodes[size_t(b.path[1])];
                    const TraceResult tr = collision_->trace(b.ps.origin, Vec3{q[0], q[1], std::max(q[2], b.ps.origin[2])}, {-15, -15, 18},
                                                             {15, 15, 70}, Contents::PlayerSolid, e->number);
                    if (tr.fraction < 1.0f || tr.startSolid) b.pathIdx = 0;
                }
                b.planGoal = obj.valid ? obj.pos : Vec3{0, 0, 0};
                b.replanMs = now + 3000 + (b.client * 137) % 1000; // spread over the bots
                if (b.path.empty() && !(obj.valid && goal == start)) b.stuck++;
            }
            auto linkFlags = [&](int from, int to) -> int {
                for (const NavLink& lk : nav.links[size_t(from)])
                    if (int(lk.target) == to) return lk.flags;
                return 0;
            };
            // Nodes passed (within 24 units) are dropped from the front of the path; the start node of a ladder, mantle
            // or leap link only when the bot stands where the traversal starts.
            while (b.pathIdx < b.path.size()) {
                const auto& tgt = nav.nodes[size_t(b.path[b.pathIdx])];
                const int out = b.pathIdx + 1 < b.path.size() ? linkFlags(b.path[b.pathIdx], b.path[b.pathIdx + 1]) : 0;
                const float reach = (out & NavLinkFlag::Special) ? botTraverseReach(out) : 24.0f;
                if (dist2(Vec3{tgt[0], tgt[1], tgt[2]}, b.ps.origin) < reach && std::fabs(tgt[2] - b.ps.origin[2]) < 48.0f) ++b.pathIdx;
                else break;
            }
            // Off the path (fallen from its floor, knocked under it or to the far side of a wall): the next node is out of a
            // step's and a jump's reach, or farther than a link and not in straight reach; walking at it would press the
            // bot against the wall in between: plan again (at most once a second).
            if (b.pathIdx < b.path.size() && b.ps.onGround && now - b.planMs >= 1000) {
                const auto& q = nav.nodes[size_t(b.path[b.pathIdx])];
                const float rise = q[2] - b.ps.origin[2];
                const int in = b.pathIdx > 0 ? linkFlags(b.path[b.pathIdx - 1], b.path[b.pathIdx]) : 0;
                bool lost = !(in & NavLinkFlag::Special) && (rise > ((in & NavLinkFlag::Jump) ? 40.0f : 30.0f) || (rise < -40.0f && !(in & NavLinkFlag::Drop)));
                if (!lost && !in && dist2(q, b.ps.origin) > 48.0f) {
                    const TraceResult tr = collision_->trace(b.ps.origin, Vec3{q[0], q[1], b.ps.origin[2]}, {-15, -15, 18}, {15, 15, 70}, Contents::PlayerSolid, e->number);
                    lost = tr.fraction < 1.0f || tr.startSolid;
                }
                if (lost) {
                    b.path.clear();
                    b.pathIdx = 0;
                    ++b.stats.offPathReplans;
                }
            }
            if (b.pathIdx < b.path.size()) {
                // Steer at the farthest of the next few nodes reached over plain links (no jump or drop, little height
                // change), so that the walk does not zigzag over the grid.
                size_t look = b.pathIdx;
                while (look + 1 < b.path.size() && look < b.pathIdx + 4 && !linkFlags(b.path[look], b.path[look + 1]) &&
                       std::fabs(nav.nodes[size_t(b.path[look + 1])][2] - b.ps.origin[2]) < 20.0f)
                    ++look;
                // Only a node the body can walk at straight (a box from knee to chest height clears the way): no
                // corner cutting into walls.
                while (look > b.pathIdx) {
                    const auto& q = nav.nodes[size_t(b.path[look])];
                    const TraceResult tr = collision_->trace(b.ps.origin, Vec3{q[0], q[1], b.ps.origin[2]}, {-15, -15, 18}, {15, 15, 70},
                                                             Contents::PlayerSolid, e->number);
                    if (tr.fraction >= 1.0f && !tr.startSolid) break;
                    --look;
                }
                // The nodes before it are passed for good: were they kept, the first one the box misses by a hair would
                // turn the bot back to a node behind it, and the next frame forward again (running on the spot).
                b.pathIdx = look;
                const auto& tgt = nav.nodes[size_t(b.path[look])];
                const float dx = tgt[0] - b.ps.origin[0], dy = tgt[1] - b.ps.origin[1];
                const float l = std::max(1e-3f, std::sqrt(dx * dx + dy * dy));
                moveDir = {dx / l, dy / l};
                // A Crouch link (a low door or passage, navgen): crouched from its start node to its end node, and
                // when the bot stands on a crouch-only floor (the link it came by, or the next one, is a Crouch link).
                if ((b.pathIdx > 0 && (linkFlags(b.path[b.pathIdx - 1], b.path[b.pathIdx]) & NavLinkFlag::Crouch)) ||
                    (b.pathIdx + 1 < b.path.size() && (linkFlags(b.path[b.pathIdx], b.path[b.pathIdx + 1]) & NavLinkFlag::Crouch) &&
                     dist2(Vec3{nav.nodes[size_t(b.path[b.pathIdx])][0], nav.nodes[size_t(b.path[b.pathIdx])][1], 0}, b.ps.origin) < 48.0f))
                    linkCrouch = true;
                // A jump link ahead: jump when close to the ledge.
                const auto& nxt = nav.nodes[size_t(b.path[b.pathIdx])];
                if (b.pathIdx > 0 && (linkFlags(b.path[b.pathIdx - 1], b.path[b.pathIdx]) & NavLinkFlag::Jump) &&
                    dist2(Vec3{nxt[0], nxt[1], nxt[2]}, b.ps.origin) < 48.0f)
                    wantJump = true;
            } else if (obj.valid && dist2(obj.pos, b.ps.origin) < 256.0f) {
                // The last stretch off the graph: straight at the objective (a flag, the middle of a bomb zone).
                const float dx = obj.pos[0] - b.ps.origin[0], dy = obj.pos[1] - b.ps.origin[1];
                const float l = std::max(1e-3f, std::sqrt(dx * dx + dy * dy));
                if (l > 4.0f) moveDir = {dx / l, dy / l};
            }
        }
    }
    // A ladder / mantle link of the path (navgen): from its start node on, its traversal input replaces the walk.
    const NavLink* traverse = nullptr;
    float traverseFromZ = 0, traverseToZ = 0;
    const float xySpeed = std::hypot(b.ps.velocity[0], b.ps.velocity[1]);
    if (!engage && b.pathIdx > 0 && b.pathIdx < b.path.size()) {
        const auto& from = nav.nodes[size_t(b.path[b.pathIdx - 1])];
        for (const NavLink& lk : nav.links[size_t(b.path[b.pathIdx - 1])])
            if (int(lk.target) == b.path[b.pathIdx] && (lk.flags & NavLinkFlag::Special)) {
                const float d = dist2(Vec3{from[0], from[1], from[2]}, b.ps.origin);
                if (b.traverseSinceMs >= 0 || (d < botTraverseReach(lk.flags) && b.ps.onGround)) {
                    traverse = &lk, traverseFromZ = from[2], traverseToZ = nav.nodes[lk.target][2];
                } else {
                    // Not on the start node (run past it, or pushed off): back to it, never at the far end of the link,
                    // which is behind the wall or the gap. Coasting the last stretch: the friction stops a run in speed / 5.5.
                    const float dx = from[0] - b.ps.origin[0], dy = from[1] - b.ps.origin[1];
                    if (d > xySpeed / 5.5f && d > 1e-3f) moveDir = {dx / d, dy / d};
                    else moveDir = {0, 0};
                }
            }
    }
    if (traverse && b.traverseSinceMs < 0 && xySpeed > 40.0f) { // stop on the start node first, as the generator's run
        traverse = nullptr;
        moveDir = {0, 0};
    }
    // A traversal that does not move the bot (8 units in 1.5 s: its way is blocked) has failed like one that takes too long.
    if (traverse && b.traverseSinceMs >= 0 && dist3(b.traverseProgressPos, b.ps.origin) > 8.0f) {
        b.traverseProgressPos = b.ps.origin;
        b.traverseProgressMs = now;
    }
    if (!traverse) b.traverseSinceMs = -1;
    else if (b.traverseSinceMs < 0) b.traverseSinceMs = now, b.traverseProgressPos = b.ps.origin, b.traverseProgressMs = now;
    else if (now - b.traverseSinceMs > 8000 || now - b.traverseProgressMs > 1500 ||
             dist2(b.ps.origin, Vec3{nav.nodes[traverse->target][0], nav.nodes[traverse->target][1], 0}) >
                 traverse->distance + 64.0f) { // failed (too long, or carried off): plan again
        // A link that failed twice is taken out of the graph for this map (docs/spec/mp-bots.md "Stuck watch").
        const uint64_t key = (uint64_t(uint32_t(b.path[b.pathIdx - 1])) << 32) | traverse->target;
        if (++bots_->linkFailures[key] >= 2) {
            auto& ls = bots_->nav->links[size_t(b.path[b.pathIdx - 1])];
            const uint32_t to = traverse->target;
            ls.erase(std::remove_if(ls.begin(), ls.end(), [&](const NavLink& l) { return l.target == to; }), ls.end());
            paths_ = bots_->nav->toPathGraph();
            ++bots_->linksRemoved;
            for (MpBot& o : bots_->bots) o.path.clear(), o.pathIdx = 0, o.traverseSinceMs = -1; // paths may use it
        }
        b.path.clear();
        b.pathIdx = 0;
        b.traverseSinceMs = -1;
        traverse = nullptr;
    }

    // Weapon choice (docs/spec/mp-bots.md "Ammunition and weapon choice"): the two slot weapons, whatever their classes
    // (a weapon taken from the ground may have replaced the pistol).
    const float enemyDist = enemy ? dist3(eye, eyePosition(*enemy)) : 1e9f;
    auto roundsOf = [&](const std::string& w) { return w.empty() ? 0 : clipOf(w) + reserveOf(w); };
    if (!hasWeapon && !cl.weaponDisabled) { // nothing in hand (the held weapon was taken): a slot weapon
        const std::string pick = !inv.slot[1].empty() ? inv.slot[1] : inv.slot[2];
        if (!pick.empty()) {
            weaponName = pick;
            e->obj->set("$weapon", Value(pick));
            b.clipWeapon = pick;
        }
    }
    const bool holding = weaponName != "none" && !weaponName.empty();
    const std::string other = !holding ? std::string() : weaponName == inv.slot[1] ? inv.slot[2] : weaponName == inv.slot[2] ? inv.slot[1] : std::string();
    {
        // The weapon the bot prefers in its hands: one with rounds, a main weapon before a pistol, the one it chose.
        auto prefer = [&](const std::string& w) {
            if (w.empty()) return -1;
            return (roundsOf(w) > 0 ? 100 : 0) + (mpBotWeaponInfo(w).cls == "pistol" ? 0 : 10) + (w == b.spawnWeapon ? 1 : 0);
        };
        BotWeaponIn wi;
        wi.heldClass = holding ? mpBotWeaponInfo(weaponName).cls : std::string();
        wi.otherClass = other.empty() ? std::string() : mpBotWeaponInfo(other).cls;
        wi.heldClip = holding ? clipOf(weaponName) : 0;
        wi.heldReserve = holding ? reserveOf(weaponName) : 0;
        wi.heldClipSize = holding ? mpBotWeaponInfo(weaponName).clipSize : 1;
        wi.otherClip = other.empty() ? 0 : clipOf(other);
        wi.otherReserve = other.empty() ? 0 : reserveOf(other);
        wi.heldReloadTime = holding ? mpBotWeaponInfo(weaponName).reloadTime : 2.0f;
        wi.fighting = fighting;
        wi.enemyDist = enemyDist;
        wi.enemyKnown = b.enemy >= 0;
        wi.quietMs = int(std::min<int64_t>(now - b.noEnemySinceMs, 1000000));
        wi.heldPreferred = prefer(weaponName) >= prefer(other);
        wi.sinceSwitchMs = int(std::min<int64_t>(now - b.weaponSwitchMs, 1000000));
        const int ws = b.weapon.stateNumber();
        const bool changing = ws == 1 || ws == 2 || (ws >= 10 && ws <= 16) || b.weapon.name() != weaponName;
        // At a weapon on the ground the bot holds the weapon it gives up and presses use (mpBotItemUse).
        const bool atItem = b.detour.kind == BotDetour::Item && b.detour.use && dist3(b.detour.pos, b.ps.origin) <= 120.0f && !engage;
        if (atItem && mpBotItemUse(b, *e, weaponName)) b.buttons |= 0x8u;
        const BotWeaponAction act = holding && !cl.weaponDisabled && !changing && !atItem ? botWeaponAction(wi) : BotWeaponAction::None;
        if (act == BotWeaponAction::Switch) {
            e->obj->set("$weapon", Value(other)); // the weapon state drops the held weapon and raises this one (an empty one reloads)
            if (mpBotWeaponInfo(other).cls == "pistol") b.pistolSinceMs = now, ++b.stats.pistolSwitches;
            else b.pistolSinceMs = -1;
            weaponName = other;
            b.clipWeapon = other;
            b.weaponSwitchMs = now;
            ++b.stats.weaponSwitches;
        }
        b.wantReload = act == BotWeaponAction::Reload;
    }
    const std::string wclass = holding ? mpBotWeaponInfo(weaponName).cls : std::string();
    b.clip = holding ? clipOf(weaponName) : 0;

    // Cover: does crouching hide the bot from the enemy (every 500 ms)?
    if (fighting && now >= b.coverCheckMs) {
        b.coverCheckMs = now + 500;
        const Vec3 chest{eyePosition(*enemy)[0], eyePosition(*enemy)[1], eyePosition(*enemy)[2] - 14.0f};
        const Vec3 low{b.ps.origin[0], b.ps.origin[1], b.ps.origin[2] + 40.0f};
        const Vec3 high{b.ps.origin[0], b.ps.origin[1], b.ps.origin[2] + 60.0f};
        const bool lowBlocked = collision_->trace(low, chest, {0, 0, 0}, {0, 0, 0}, Contents::SightMaskNoCharacters, e->number).fraction < 0.999f;
        const bool highSees = collision_->trace(high, chest, {0, 0, 0}, {0, 0, 0}, Contents::SightMaskNoCharacters, e->number).fraction >= 0.999f;
        b.coverCrouch = lowBlocked && highSees;
    } else if (!fighting) {
        b.coverCrouch = false;
    }
    const int wstate = b.weapon.stateNumber();
    const bool reloading = wstate >= 5 && wstate <= 9; // reload states of PM_Weapon (docs/spec/weapons.md)
    const bool holdFire = reloading || now < b.nextFireMs;
    BotStanceIn si;
    si.weaponClass = wclass;
    si.dist = enemyDist;
    si.fighting = engage && !atUse && !charge;
    si.moving = traverse != nullptr || walkObjective;
    si.coverCrouch = b.coverCrouch;
    si.holdFire = holdFire;
    // Behind cover the bot crouches (docs/spec/mp-bots.md "Cover").
    const Stance stance = b.detour.kind == BotDetour::Cover && b.inCover ? Stance::Crouch : botChooseStance(si);
    if (stance == Stance::Crouch) ++b.stats.crouchFrames;
    if (stance == Stance::Prone) ++b.stats.proneFrames;
    // Debug: IW2_MP_BOT_ADS=1 makes every bot hold its aim whenever it holds a weapon (scoped killcam shots, docs/debug-flags.md).
    static const bool forceAds = std::getenv("IW2_MP_BOT_ADS") && std::atoi(std::getenv("IW2_MP_BOT_ADS")) != 0;
    // An aiming bot that loses its enemy from sight (out of the scope's circle, behind a post) keeps its aim for 0.7 s
    // while its view follows to where the enemy was; only then it lowers the weapon to look around.
    const bool adsGrace = !fighting && b.adsSinceMs >= 0 && b.enemy >= 0 && !b.enemyHeardOnly && now - b.enemySeenMs < 700 && b.combat && !atUse &&
                          botWantAds(wclass, dist3(b.enemyLastPos, b.ps.origin));
    const bool ads = ((si.fighting || adsGrace) && !si.moving && botWantAds(wclass, fighting ? enemyDist : dist3(b.enemyLastPos, b.ps.origin))) ||
                     ((forceAds || b.forceAds) && holding);
    if (ads && b.adsSinceMs < 0) b.adsSinceMs = now;
    if (!ads) b.adsSinceMs = -1;

    // Combat movement: a standing bot strafes, closes in from far and backs off when close.
    if (engage && !walkObjective && !atUse && now >= b.reactAtMs - 200) {
        if (now >= b.strafeFlipMs) {
            b.strafe = (nextRand() & 1) ? 1.0f : -1.0f;
            b.strafeFlipMs = now + 600 + nextRand() % 900;
        }
        const Vec3 eo = v3(enemy->obj->get("origin"));
        const float dx = eo[0] - b.ps.origin[0], dy = eo[1] - b.ps.origin[1];
        const float l = std::max(1e-3f, std::sqrt(dx * dx + dy * dy));
        const float toward = charge ? 1.0f : l > 900.0f ? 1.0f : l < 250.0f ? -0.6f : 0.0f;
        if (charge) b.strafe = 0.0f; // straight at the enemy for the melee blow
        // toward the enemy plus the strafe (the right axis of the direction to the enemy is (dy, -dx)/l)
        moveDir = {0, 0};
        // The combat step stays on the graph (a main node within 40 units of the point 40 units ahead): strafing must
        // not drop the bot into a pit the graph has no way out of. Else the other side, else no step.
        for (int side = 0; side < 2 && stance == Stance::Stand; ++side) {
            const std::array<float, 2> m{dx / l * toward + dy / l * b.strafe * 0.8f, dy / l * toward - dx / l * b.strafe * 0.8f};
            const float ml = std::max(1e-3f, std::hypot(m[0], m[1]));
            if (nav.size() == 0 || nav.nearest(Vec3{b.ps.origin[0] + m[0] / ml * 40.0f, b.ps.origin[1] + m[1] / ml * 40.0f, b.ps.origin[2]},
                                               40.0f, true) >= 0) {
                moveDir = m;
                break;
            }
            b.strafe = -b.strafe;
        }
        b.path.clear();
    }

    // Aim: a point that follows the enemy's chest with the skill's lag; a hidden enemy seen in the last 2 s is
    // pre-aimed at its last position; corner checks while roaming; else along the walk direction.
    float wantYaw = b.yaw, wantPitch = 0;
    Vec3 chestNow{};
    auto lookAt = [&](const Vec3& p) {
        const float dx = p[0] - eye[0], dy = p[1] - eye[1], dz = p[2] - eye[2];
        wantYaw = std::atan2(dy, dx) / kDeg;
        wantPitch = -std::atan2(dz, std::sqrt(dx * dx + dy * dy)) / kDeg;
    };
    // The view kick of the bot's own shots moves the view first (src/game/mpbotaim.cpp).
    mpBotKickStep(b, weaponName, holding);
    const bool throwing = b.throwPending.valid && !traverse;
    if (throwing) {
        // A grenade in the hand: the view goes to the throw's angles (it leaves along the view).
        wantYaw = b.throwPending.yaw;
        wantPitch = b.throwPending.pitch;
        b.aimValid = false;
    } else if (enemy && engage) {
        // The aimed point of the enemy's body with the skill's lag, lead and hand error (mpBotAimAt).
        chestNow = mpBotAimPoint(b, *enemy);
        BotAimNoiseIn ni;
        ni.moving = xySpeed > 60.0f;
        ni.stance = b.ps.stance;
        ni.adsFrac = b.weapon.adsFrac();
        ni.hurtMs = int(std::min<int64_t>(now - b.hurtMs, 100000));
        mpBotAimAt(b, *enemy, eye, wantYaw, wantPitch, ni);
    } else {
        b.aimValid = false;
        if (b.detour.kind == BotDetour::Item && b.detour.use && dist3(b.detour.pos, b.ps.origin) < 100.0f && !traverse) {
            lookAt(b.detour.pos); // the weapon on the ground the bot is about to take
        } else if (b.detour.kind == BotDetour::Turret && dist3(b.detour.pos, b.ps.origin) < 100.0f && !traverse) {
            const BotTurret& bt = bots_->turrets[size_t(b.detour.turret)];
            lookAt(Vec3{bt.origin[0], bt.origin[1], bt.origin[2] + 28.0f}); // the gun the bot is about to use
        } else if (b.enemy >= 0 && (now - b.enemySeenMs < 2000 || b.inCover) && b.combat && !traverse) {
            lookAt(Vec3{b.enemyLastPos[0], b.enemyLastPos[1], b.enemyLastPos[2] + 48.0f}); // where the enemy was (behind cover: kept)
        } else if (moveDir[0] != 0 || moveDir[1] != 0) {
            wantYaw = std::atan2(moveDir[1], moveDir[0]) / kDeg;
            // Corner checks (hardened, veteran): now and then a look along the most open side direction.
            if (sk.cornerChecks && b.enemy < 0 && !traverse) {
                if (now >= b.nextGlanceMs) {
                    b.nextGlanceMs = now + 1500 + nextRand() % 1500;
                    float bestLen = 0;
                    for (float off : {-90.0f, -45.0f, 45.0f, 90.0f}) {
                        const float a = (wantYaw + off) * kDeg;
                        const Vec3 to{eye[0] + std::cos(a) * 512.0f, eye[1] + std::sin(a) * 512.0f, eye[2]};
                        const float f = collision_->trace(eye, to, {0, 0, 0}, {0, 0, 0}, Contents::SightMaskNoCharacters, e->number).fraction;
                        if (f > bestLen + 0.05f) bestLen = f, b.glanceYaw = wantYaw + off;
                    }
                    if (bestLen > 0.5f) {
                        b.glanceUntilMs = now + 700;
                        ++b.stats.cornerChecks;
                    }
                }
                if (now < b.glanceUntilMs) wantYaw = b.glanceYaw;
            }
        } else if (!traverse && b.combat) {
            // Standing with nobody known (a guard on its objective, a bot planting): it looks around, every 1.5..4 s along
            // another of the open directions about it (sight traces of 600 units in twelve directions; one at least 30%
            // free, not the one it just looked along).
            if (now >= b.scanNextMs) {
                b.scanNextMs = now + 1500 + nextRand() % 2500;
                const float base = float(nextRand() % 30);
                float pick = b.scanYaw, total = 0;
                for (int k = 0; k < 12; ++k) {
                    const float yawK = base + float(k) * 30.0f;
                    if (std::fabs(wrap180(yawK - b.scanYaw)) < 40.0f) continue;
                    const float a = yawK * kDeg;
                    const Vec3 to{eye[0] + std::cos(a) * 600.0f, eye[1] + std::sin(a) * 600.0f, eye[2]};
                    const float f = collision_->trace(eye, to, {0, 0, 0}, {0, 0, 0}, Contents::SightMaskNoCharacters, e->number).fraction;
                    if (f < 0.3f) continue;
                    // One of them at random, the more open the likelier (reservoir sampling by the free length).
                    total += f;
                    if (float(nextRand() % 1000) / 1000.0f * total < f) pick = yawK;
                }
                b.scanYaw = wrap180(pick);
            }
            wantYaw = b.scanYaw;
        }
    }
    const float maxTurn = sk.turnRate * kDt;
    const float yaw0 = b.yaw, pitch0 = b.pitch; // the view at the frame start: one turn step per frame, whatever sets the goal
    mpBotTurnView(b, wantYaw, wantPitch, enemy && engage && !throwing);
    // At a turret the bot wants: use once the use search of its client offers it (behind the gun, on the ground).
    if (b.detour.kind == BotDetour::Turret && cl.useEntity == b.detour.entity && !b.usePrev && b.ps.onGround) b.buttons |= 0x8u;

    // Reload (botWeaponAction): the weapon state reloads an empty clip by itself; the reload button is pressed once for
    // a tactical reload and for an empty clip the state did not reload (prone and moving).
    const bool wantReload = holding && b.wantReload && !reloading && reserveOf(weaponName) > 0;

    // Grenades (docs/spec/mp-bots.md "Grenades"): every 2 s, by client number.
    if (b.combat && !traverse && b.ps.onGround && !cl.weaponDisabled && now >= b.grenadeCheckMs) {
        b.grenadeCheckMs = now + 2000 + (b.client * 61) % 400;
        Entity* tgt = b.enemy >= 0 ? entityByNumber(b.enemy) : nullptr;
        if (now >= b.nextGrenadeMs && tgt && isAlive(*tgt) && now - b.enemySeenMs < 3000) {
            const Vec3 tp = b.enemyVisible ? v3(tgt->obj->get("origin")) : b.enemyLastPos;
            const float d = dist2(tp, b.ps.origin);
            bool grouped = false;
            bool friendNear = d < 256.0f;
            for (size_t c = 0; c < mp_->clients.size(); ++c) {
                const MpClient& oc = mp_->clients[c];
                Entity* o = oc.ent;
                if (int(c) == b.client || oc.state != MpClientState::Active || !o || o == tgt || !isAlive(*o)) continue;
                if (o->obj->get("sessionstate").toString() != "playing") continue;
                const bool enemyOf = !(teams && o->obj->get("sessionteam").toString() == myTeam);
                const float od = dist3(v3(o->obj->get("origin")), tp);
                // Another enemy beside the target counts only when the bot sees it too (noticed, in sight the last 500 ms).
                if (enemyOf && od < 200.0f && c < b.notice.size() && b.notice[c] >= 1.0f && now - b.noticeSeenMs[c] < 500) grouped = true;
                if (!enemyOf && od < 256.0f) friendNear = true;
            }
            if ((!b.enemyVisible || grouped) && !friendNear && d >= 250.0f && d <= 1400.0f &&
                float(nextRand()) / 32768.0f < sk.grenadeChance && mpBotGrenade(b, *e, eye, false, Vec3{tp[0], tp[1], tp[2] + 8.0f}))
                b.nextGrenadeMs = now + 6000;
        }
        // Smoke: sd attackers before the plant, ctf carriers and attackers, once per life. Toward an enemy in sight, or
        // one known in the last 3 s at its believed place, 400..1500 units away: a third of the way there. An sd attacker
        // on its way to a bomb zone 500..1200 units ahead also covers its approach: two thirds of the way to the zone.
        if (!b.smokeUsed) {
            bool smokeRole = false, sdAttack = false;
            if (mp_->gametype == "ctf") smokeRole = carrying || b.objective == "take flag" || b.objective == "escort";
            if (mp_->gametype == "sd") {
                const Value att = vm_.game()->get("attackers");
                const Value planted = vm_.level()->get("bombplanted");
                smokeRole = sdAttack = att.isString() && att.asString() == myTeam && !planted.truthy();
            }
            const bool smokeTarget = fighting || (b.enemy >= 0 && now - b.enemySeenMs < 3000);
            const float smokeDist = fighting ? enemyDist : dist3(b.enemyLastPos, b.ps.origin);
            auto toward = [&](const Vec3& to, float part) {
                return Vec3{b.ps.origin[0] + (to[0] - b.ps.origin[0]) * part, b.ps.origin[1] + (to[1] - b.ps.origin[1]) * part,
                            b.ps.origin[2] + (to[2] - b.ps.origin[2]) * part + 8.0f};
            };
            if (smokeRole && smokeTarget && smokeDist > 400.0f && smokeDist < 1500.0f) {
                mpBotGrenade(b, *e, eye, true, toward(fighting ? v3(enemy->obj->get("origin")) : b.enemyLastPos, 1.0f / 3.0f));
            } else if (sdAttack && obj.valid && (b.objective == "plant" || b.objective == "push zone")) {
                const float zd = dist3(obj.pos, b.ps.origin);
                if (zd > 500.0f && zd < 1200.0f) mpBotGrenade(b, *e, eye, true, toward(obj.pos, 2.0f / 3.0f));
            }
        }
    }

    // Fire (docs/spec/mp-bots.md "Shooting"): after the reaction time, once the view is on the point the bot aims at
    // (which is off the enemy by the hand's error and the lag), at the weapon's fire rate, in bursts. Where the shot goes
    // follows from the view.
    // The error the bot sees: of the angles it steers plus the view kick of its last shots (it waits for the sight to
    // come back); the hand's own error it does not see.
    const float aimErr = std::hypot(wrap180(wantYaw - b.aim.yaw - b.kick.viewKick()[1]), wantPitch - b.aim.pitch - b.kick.viewKick()[0]);
    const bool adsReady = ads && b.weapon.adsFrac() >= 1.0f;
    // A sniper rifle is fired aimed only (beyond its ADS distance).
    const bool sniperWait = wclass == "sniper" && ads && !adsReady;
    const bool automatic = wclass == "smg" || wclass == "mg";
    const float fireTol = botFireTolerance(enemyDist, sk.aimTolerance, automatic && enemyDist < 500.0f);
    // The trigger: the weapon state machine decides when a shot leaves (fireTime, rechamber, reload, raise / drop); a
    // semi-automatic or bolt-action trigger is released between shots (docs/spec/mp-bots.md "Weapon state").
    const bool wantFire = engage && !cl.weaponDisabled && now >= b.reactAtMs && now >= b.nextFireMs && b.clip > 0 && aimErr < fireTol &&
                          holding && !sniperWait && !throwing && b.weapon.name() == weaponName;
    // The melee blow (docs/spec/mp-bots.md "Melee"): a press, released the next frame.
    const bool wantMelee = engage && holding && !cl.weaponDisabled && now >= b.reactAtMs &&
                           botWantMelee(dist3(eye, chestNow), aimErr, wclass, b.clip, reloading || wstate == 1 || wstate == 2);
    PlayerWeapon::Input wi;
    const bool semi = holding && mpBotWeaponInfo(weaponName).semi;
    wi.melee = wantMelee && !b.meleePrev;
    b.meleePrev = wi.melee;
    wi.fire = wantFire && !wantMelee && !(semi && b.firePrev);
    b.firePrev = wi.fire;
    wi.reload = wantReload && !b.reloadPrev;
    b.reloadPrev = wantReload;
    wi.ads = ads;
    wi.weapon = holding ? weaponName : std::string();
    wi.holster = cl.weaponDisabled;
    wi.proneMoving = b.ps.stance == Stance::Prone && moveDir != std::array<float, 2>{0, 0};
    // A solved throw: the offhand button is held until the machine has left the ready states (it then pulls the
    // grenade back and throws it on the release), as a player's press and release would.
    // The grenade is held back a little longer while the view is not on the throw's angles yet (at most 1.2 s after the
    // decision: a held frag grenade cooks).
    if (b.throwPending.valid) {
        const bool aligned = aimErr < 4.0f || now - b.throwPending.sinceMs > 1200;
        if (b.weapon.stateNumber() < 12 || (b.weapon.stateNumber() <= 14 && !aligned)) (b.throwPending.smoke ? wi.smoke : wi.frag) = true;
        if (now - b.throwPending.sinceMs > 4000) b.throwPending = {}; // never thrown (the weapon state did not get to it)
    }
    if (holding && !b.weapon.equipped() && !b.weapon.holstered())
        if (const auto* def = weapon(weaponName)) b.weapon.equip(weaponName, *def);
    const int shotsNow = holding ? b.weapon.update(kDt, wi) : 0;
    if (b.weapon.stateNumber() == 13 && b.weaponStatePrev != 13) mpPushClientEvent(b.client, mpev::kPrepOffhand, b.weapon.viewWeaponName());
    b.weaponStatePrev = b.weapon.stateNumber();
    for (const auto& ev : b.weapon.takeEvents()) {
        if (ev.kind == PlayerWeapon::Event::ReloadBegin) {
            if (!ev.weapon.empty()) cl.animInput.reloaded = true, ++b.stats.reloads;
        } else if (ev.kind == PlayerWeapon::Event::StateEvent) {
            mpPushClientEvent(b.client, ev.code, ev.weapon); // reload, rechamber, raise, put away (docs/spec/net.md section 13)
        } else if (ev.kind == PlayerWeapon::Event::MeleeSwing) {
            mpPushClientEvent(b.client, mpev::kMeleeSwipe, b.weapon.name());
            ++b.stats.melees;
        } else if (ev.kind == PlayerWeapon::Event::MeleeHit) {
            // The blow lands (the client's melee trace of the weapon event, as a network client's): along the bot's view.
            mpPushClientEvent(b.client, mpev::kFireMelee, b.weapon.name());
            clientMelee(*e, eye, Vec3{b.pitch, b.yaw, 0});
        } else if (ev.kind == PlayerWeapon::Event::OffhandThrow) {
            mpPushClientEvent(b.client, mpev::kUseOffhand, ev.weapon);
            mpBotThrowPending(b, *e, eye, b.weapon.takeFuseMs());
        } else if (ev.kind == PlayerWeapon::Event::NoOffhand) {
            b.throwPending = {}; // the grenade is gone (no ammunition)
        }
    }
    (void)b.weapon.takeSounds();
    for (int shot = 0; shot < shotsNow; ++shot) {
        const std::string firedWeapon = b.weapon.name();
        // The shot leaves along the bot's view with the weapon's spread (mpBotFireShot): a spread weapon fires its pellets,
        // a projectile weapon a rocket, through the clients' shared fire path (World::fireWeaponShot).
        mpBotFireShot(b, *e, eye, firedWeapon, b.weapon.clip() == 0 && shot == shotsNow - 1);
        cl.animInput.fired = true;
        ++b.shots;
        ++b.stats.shots;
        if (adsReady) ++b.stats.adsShots;
        b.clip = b.weapon.clip();
        const BotBurst burst = botBurst(wclass, enemyDist, sk.burst, sk.burstPauseMs);
        if (++b.burstShots >= burst.shots) {
            b.burstShots = 0;
            if (burst.pauseMs > 0) b.nextFireMs = now + burst.pauseMs + nextRand() % 200;
        }
    }

    mpBotMeasure(b, *e, fighting && !atUse);

    // Stuck watch: no progress for a second while walking -> jump; twice -> a sidestep in a random direction for
    // 600 ms; three times -> new goal.
    if (now < b.escapeUntilMs && !engage && !traverse) moveDir = b.escapeDir;
    const bool walking = moveDir[0] != 0 || moveDir[1] != 0;
    if (traverse) { // climbing or mantling: no stuck jump (a jump on a ladder pushes off it)
        b.progressPos = b.ps.origin;
        b.progressMs = now;
        wantJump = false;
    }
    if (now - b.progressMs >= 1000) {
        if (walking && !engage && dist2(b.progressPos, b.ps.origin) < 24.0f) {
            wantJump = true;
            // No way to a detour: it is given up at once (docs/spec/mp-bots.md "Detours").
            if (b.detour.kind != BotDetour::None && !objReached) mpBotDetourStuck(b);
            if (++b.stuck >= 2) {
                // A random direction that stays on the graph when one of four tries does (as the combat step).
                for (int t = 0; t < 4; ++t) {
                    const float a = float(nextRand() % 360) * kDeg;
                    b.escapeDir = {std::cos(a), std::sin(a)};
                    if (nav.nearest(Vec3{b.ps.origin[0] + b.escapeDir[0] * 40.0f, b.ps.origin[1] + b.escapeDir[1] * 40.0f, b.ps.origin[2]}, 40.0f,
                                    true) >= 0)
                        break;
                }
                b.escapeCrouch = !b.escapeCrouch;
                b.escapeUntilMs = now + (b.escapeCrouch ? 1200 : 600);
                if (b.escapeCrouch && b.pathIdx < b.path.size()) { // crouched: towards the path again
                    const auto& q = nav.nodes[size_t(b.path[b.pathIdx > 0 ? b.pathIdx - 1 : 0])];
                    const float dx = q[0] - b.ps.origin[0], dy = q[1] - b.ps.origin[1], l = std::max(1e-3f, std::hypot(dx, dy));
                    if (nextRand() & 1) b.escapeDir = {dx / l, dy / l};
                }
            }
            if (b.stuck >= 3) {
                // The walk link the bot failed on (stuck three times while on it): after two failures it is taken out
                // of the map's graph as a failed traversal is (docs/spec/mp-bots.md "Stuck watch").
                // Stuck on the way to the start node: that node is not taken as a start again until the bot has
                // moved 64 units.
                if (b.pathIdx == 0 && !b.path.empty()) b.badStarts.push_back(b.path[0]);
                if (b.pathIdx > 0 && b.pathIdx < b.path.size()) {
                    const uint32_t fromN = uint32_t(b.path[b.pathIdx - 1]), to = uint32_t(b.path[b.pathIdx]);
                    const auto& fq = nav.nodes[fromN];
                    if (dist2(Vec3{fq[0], fq[1], fq[2]}, b.ps.origin) < 96.0f && ++bots_->linkFailures[(uint64_t(fromN) << 32) | to] >= 2) {
                        auto& ls = bots_->nav->links[fromN];
                        ls.erase(std::remove_if(ls.begin(), ls.end(), [&](const NavLink& l) { return l.target == to; }), ls.end());
                        paths_ = bots_->nav->toPathGraph();
                        ++bots_->linksRemoved;
                        for (MpBot& o : bots_->bots) o.path.clear(), o.pathIdx = 0, o.traverseSinceMs = -1;
                    }
                }
                b.path.clear();
                b.pathIdx = 0;
                b.stuck = 0;
            }
        } else if (walking) {
            b.stuck = 0;
        }
        b.progressPos = b.ps.origin;
        b.progressMs = now;
    }
    // Long stuck watch (docs/spec/mp-bots.md "Stuck watch"): 20 s within 64 units while not fighting and not on the
    // objective: counted, and a new random goal.
    // Off the graph (no node within 32 units) for 3 s while not fighting: the path is planned again, from the nearest
    // node the body reaches in a straight line (docs/spec/mp-bots.md "Stuck watch"), long before the 20 s watch.
    if (engage || nav.nearest(b.ps.origin, 32.0f) >= 0) b.offGraphSinceMs = now;
    else if (now - b.offGraphSinceMs >= 3000) {
        b.offGraphSinceMs = now;
        b.path.clear();
        b.pathIdx = 0;
        ++b.stats.offGraphReplans;
    }
    if (b.noReachSpells && nav.nearest(b.ps.origin, 24.0f) >= 0) b.noReachSpells = 0; // back on the graph
    if (engage || (obj.valid && (atUse || objReached)) || dist3(b.anchorPos, b.ps.origin) > 64.0f) {
        b.stillSinceMs = now;
        if (dist3(b.anchorPos, b.ps.origin) > 64.0f) b.badStarts.clear();
        b.anchorPos = b.ps.origin;
        b.anchorMs = now;
    }
    b.stats.longestStillMs = std::max(b.stats.longestStillMs, int(now - b.stillSinceMs));
    if (now - b.anchorMs > 20000) {
        ++b.stats.stuckEvents;
        // Trapped: the second time in a row at the same spot, with no node within 160 units that the body reaches in a
        // straight line (a pit the graph has no way out of), the bot dies as a player's console kill does and respawns.
        bool reach = false;
        for (int n : nav.within(b.ps.origin, 160.0f)) {
            const auto& q = nav.nodes[size_t(n)];
            if (std::fabs(q[2] - b.ps.origin[2]) > 40.0f) continue;
            if (std::find(b.badStarts.begin(), b.badStarts.end(), n) != b.badStarts.end()) continue; // tried, not reached
            const TraceResult tr = collision_->trace(b.ps.origin, Vec3{q[0], q[1], std::max(q[2], b.ps.origin[2])}, {-15, -15, 18},
                                                     {15, 15, 40}, Contents::PlayerSolid, e->number);
            if (tr.fraction >= 1.0f && !tr.startSolid) {
                reach = true;
                break;
            }
        }
        b.noReachSpells = reach ? 0 : b.noReachSpells + 1;
        if (b.noReachSpells >= 2 && isAlive(*e)) {
            ++b.stats.trapped;
            const int h = e->obj->get("health").asInt();
            e->obj->set("health", Value(int32_t(0)));
            mpPlayerDie(*e, e, e, h, "MOD_SUICIDE", "none", Vec3{0, 0, 0}, "none");
            return;
        }
        if (std::getenv("IW2_BOT_STUCKDBG")) {
            std::fprintf(stderr, "STUCK bot %d at %.0f %.0f %.0f stance %d ground %d vel %.0f %.0f %.0f move %.2f %.2f path %zu/%zu enemy %d vis %d obj %d (%s, %.0f away) trav %d detour %d",
                         b.client, b.ps.origin[0], b.ps.origin[1], b.ps.origin[2], int(b.ps.stance), int(b.ps.onGround), b.ps.velocity[0],
                         b.ps.velocity[1], b.ps.velocity[2], moveDir[0], moveDir[1], b.pathIdx, b.path.size(), b.enemy, int(b.enemyVisible),
                         int(obj.valid), obj.what, obj.valid ? dist3(obj.pos, b.ps.origin) : 0.0f, int(traverse != nullptr), int(b.detour.kind));
            for (size_t k = b.pathIdx > 0 ? b.pathIdx - 1 : 0; k < b.path.size() && k < b.pathIdx + 3; ++k)
                std::fprintf(stderr, " n(%.0f %.0f %.0f)", nav.nodes[size_t(b.path[k])][0], nav.nodes[size_t(b.path[k])][1], nav.nodes[size_t(b.path[k])][2]);
            std::fprintf(stderr, "\n");
        }
        b.anchorPos = b.ps.origin;
        b.anchorMs = now;
        b.path.clear();
        b.pathIdx = 0;
        if (!obj.valid && mainCount > 0 && bots_->plansThisFrame < 2) {
            ++bots_->plansThisFrame;
            const int start = nav.nearest(b.ps.origin, 512.0f);
            if (start >= 0) b.path = paths_.findPath(start, nextRand() % int(mainCount));
            b.pathIdx = b.path.size() > 1 ? 1 : 0;
            b.replanMs = now + 3000;
        }
    }

    PlayerInput in;
    const auto fr = botMoveInput(moveDir, b.yaw);
    in.forward = fr[0];
    in.right = fr[1];
    // A jump is a press: held on one frame, released on the next.
    in.jump = wantJump && !b.jumpLatch && b.ps.onGround && stance == Stance::Stand;
    b.jumpLatch = in.jump;
    if (wantJump && !in.jump) b.wantJump = true;
    in.yaw = b.yaw;
    in.pitch = b.pitch;
    // A sidestep crouches every other time (a low ceiling: under a wagon, a crouch-only gap).
    in.stance = now < b.escapeUntilMs && b.escapeCrouch && !engage ? Stance::Crouch : stance;
    if (linkCrouch && in.stance == Stance::Stand) in.stance = Stance::Crouch; // a Crouch link of the path
    in.adsHeld = ads;
    in.adsAllowed = ads;
    // The weapon side of the move, as for a network client (docs/spec/net.md section 13): the aim fraction (no spread
    // gain while fully aimed) and the weapon state.
    in.adsFrac = b.weapon.adsFrac();
    in.weaponState = b.weapon.stateNumber();
    if (traverse) {
        const PlayerInput t = navTraverseInput(*traverse, traverseFromZ, traverseToZ, b.ps, now == b.traverseSinceMs);
        // The move takes the link's angles at once (a ladder climbs along the command pitch); the bot's own view, which
        // the killcam shows (docs/spec/mp-hud.md "Killcam"), turns toward them at the skill's turn rate like anywhere else.
        in.yaw = t.yaw;
        in.pitch = t.pitch;
        b.yaw = botTurn(yaw0, t.yaw, maxTurn);
        b.pitch = std::clamp(pitch0 + std::clamp(wrap180(t.pitch - pitch0), -maxTurn, maxTurn), -85.0f, 85.0f);
        b.aim.yawVel = b.aim.pitchVel = 0;
        b.aim.offYaw = b.aim.offPitch = 0;
        in.forward = t.forward;
        in.right = 0;
        in.jump = t.jump;
        in.stance = Stance::Stand;
        in.adsHeld = in.adsAllowed = false;
        b.jumpLatch = in.jump;
    }
    MoveParams mp = multiplayerMoveParams(); // the MP dvar defaults (docs/spec/mp-pmove.md)
    mp.passEntity = e->number;
    if (bots_->mantle.loaded()) mp.mantle = &bots_->mantle;
    // The held weapon's move speed scale and hip spread: the aim spread of the player state grows with the bot's moves
    // and turns and decays at rest, as a player's (docs/spec/pmove.md); a shot reads it (mpBotFireShot).
    if (holding) {
        const BotWeaponInfo& info = mpBotWeaponInfo(weaponName);
        if (info.moveSpeedScale != 0) mp.weaponSpeedScale = info.moveSpeedScale;
        mp.hipSpread = info.hip;
    }
    const Vec3 originBefore = b.ps.origin;
    for (const KnockbackHit& hit : mpTakeKnockbacks(b.client)) applyKnockback(b.ps, hit, cvarFloat("g_knockback", 1000.0f));
    playerMove(*collision_, b.ps, in, kDt, mp);
    mpBotMeasureStall(b, !engage && !traverse && (in.forward != 0 || in.right != 0), dist3(originBefore, b.ps.origin), moveDir);
    // The step's landing events give fall damage like any client's (ClientEvents, docs/spec/mp-pmove.md).
    if (b.ps.eventSeq - b.eventsDone > 4) b.eventsDone = b.ps.eventSeq - 4;
    // The same events go to the bot's entity event ring (the player state to entity state copy, docs/spec/net.md
    // section 13) and are heard here as another player's steps.
    for (; b.eventsDone < b.ps.eventSeq; ++b.eventsDone) {
        const int ev = b.ps.events[b.eventsDone & 3];
        clientFallEvent(*e, ev, b.ps.eventParms[b.eventsDone & 3]);
        if (playerStateEventSentToEntity(ev)) mpPushClientEvent(b.client, ev, std::string());
        otherPlayerMoveEventSound(*e, ev);
    }
    e->obj->set("origin", Value(Vec3{b.ps.origin[0], b.ps.origin[1], b.ps.origin[2]}));
    e->obj->set("angles", Value(Vec3{0, b.yaw, 0}));
    e->velocity = {b.ps.velocity[0], b.ps.velocity[1], b.ps.velocity[2]};
    // Movement for the third-person animation (docs/spec/mp-players.md).
    cl.animInput.valid = true;
    cl.animInput.move.viewHeightTarget = int(b.ps.viewHeightTarget);
    cl.animInput.move.xySpeed = std::sqrt(b.ps.velocity[0] * b.ps.velocity[0] + b.ps.velocity[1] * b.ps.velocity[1]);
    cl.animInput.move.forwardMove = in.forward;
    cl.animInput.move.rightMove = in.right;
    cl.animInput.move.slow = b.ps.adsSlow || b.ps.lean != 0;
    cl.animInput.ads = b.ps.ads;
    mpSetViewState(b.client, mpViewStateOf(b.weapon, b.ps)); // the recorded view state (the killcam view of this bot), its ground and legs event
    mpBotCommand(b);
}

// The bot's command of this frame as the server reads a client's (docs/spec/mp-bots.md "Client side of a bot"): the
// buttons (use, attack on a turret) and the view the use search and the turret code read.
void World::mpBotCommand(MpBot& b) {
    mpSetButtons(b.client, b.buttons);
    mpSetView(b.client, Vec3{b.pitch, b.yaw, 0}, b.psValid ? b.ps.viewHeight : 60.0f);
    b.usePrev = (b.buttons & 0x28u) != 0;
}

// The measurement counters of a playing bot (docs/spec/mp-bots.md "Measurement"): time without anything to fight with,
// time with every slot weapon empty, and the rounds and weapons gained from items.
void World::mpBotMeasure(MpBot& b, Entity& self, bool fighting) {
    const MpClient& cl = mp_->clients[size_t(b.client)];
    Inventory& inv = inventory(self);
    const std::string held = self.obj->get("$weapon").toString();
    const bool hasWeapon = !held.empty() && held != "none";
    ++b.stats.aliveFrames;
    int rounds = 0;
    for (int sl = 1; sl <= 2; ++sl)
        if (!inv.slot[size_t(sl)].empty()) {
            const BotWeaponInfo& info = mpBotWeaponInfo(inv.slot[size_t(sl)]);
            rounds += inv.clip[info.clipName] + inv.ammo[info.ammoName];
        }
    if (rounds == 0) ++b.stats.emptyFrames;
    const int ws = b.weapon.stateNumber();
    // Ready (state 0) with an empty clip: not the fire time after the last shot (3, 4), not a reload (5-9), a weapon
    // change (1, 2), a melee (10, 11) or a throw (12-16).
    const bool idleEmpty = ws == 0 && b.weapon.clip() == 0 && b.weapon.name() == held;
    const bool defenceless = fighting && !cl.weaponDisabled && !mpClientOnTurret(b.client) && (!hasWeapon || idleEmpty);
    if (defenceless) ++b.stats.defencelessFrames;
    if (defenceless != b.wasDefenceless && std::getenv("IW2_BOT_AMMODBG"))
        std::fprintf(stderr, "AMMO t=%lld bot %d %s: held %s clip %d reserve %d state %d machine %s slots [%s %d+%d] [%s %d+%d]\n",
                     static_cast<long long>(timeMs_), b.client, defenceless ? "DEFENCELESS" : "ok", held.c_str(), b.weapon.clip(),
                     hasWeapon ? inv.ammo[weaponField(held, "ammoName")] : 0, ws, b.weapon.name().c_str(), inv.slot[1].c_str(),
                     inv.slot[1].empty() ? 0 : inv.clip[weaponField(inv.slot[1], "clipName")],
                     inv.slot[1].empty() ? 0 : inv.ammo[weaponField(inv.slot[1], "ammoName")], inv.slot[2].c_str(),
                     inv.slot[2].empty() ? 0 : inv.clip[weaponField(inv.slot[2], "clipName")],
                     inv.slot[2].empty() ? 0 : inv.ammo[weaponField(inv.slot[2], "ammoName")]);
    b.wasDefenceless = defenceless;
    // Rounds gained between two frames without a change of the slot weapons: ammunition taken from an item.
    const std::string slots = inv.slot[1] + "/" + inv.slot[2];
    if (b.lastSlots == slots && rounds > b.lastRounds) ++b.stats.ammoPickups;
    if (!b.lastSlots.empty() && b.lastSlots != slots) ++b.stats.weaponPickups; // a weapon taken from the ground
    b.lastSlots = slots;
    b.lastRounds = rounds;
}

// Measures how long a bot that wants to walk stays on the spot (docs/spec/mp-bots.md "Stuck watch").
void World::mpBotMeasureStall(MpBot& b, bool wantsWalk, float step, const std::array<float, 2>& dir) {
    const int64_t now = timeMs_;
    if (wantsWalk) bots_->walkMs += 50;
    const bool left = b.stallSinceMs >= 0 && dist3(b.stallPos, b.ps.origin) > 48.0f;
    if (b.stallSinceMs >= 0 && (!wantsWalk || left)) {
        const int ms = int(now - b.stallSinceMs);
        if (ms > 1000 && bots_->stalls.size() < 65536) {
            BotStall s = b.stallSample;
            s.client = b.client;
            s.pos = b.stallPos;
            s.ms = ms;
            s.travelled = b.stallTravelled;
            for (const MpClient& oc : mp_->clients)
                if (oc.state == MpClientState::Active && oc.ent && oc.ent != mp_->clients[size_t(b.client)].ent &&
                    oc.ent->obj->get("sessionstate").toString() == "playing" && dist3(v3(oc.ent->obj->get("origin")), b.ps.origin) < 48.0f)
                    s.blockedByPlayer = true;
            bots_->stalls.push_back(s);
        }
        b.stallSinceMs = -1;
    }
    if (wantsWalk && b.stallSinceMs < 0) {
        b.stallSinceMs = now;
        b.stallPos = b.ps.origin;
        b.stallTravelled = 0;
        b.stallSample = {};
    }
    if (b.stallSinceMs >= 0) b.stallTravelled += step;
    if (b.stallSinceMs >= 0 && now - b.stallSinceMs == 5000 && std::getenv("IW2_BOT_STUCKDBG")) {
        std::fprintf(stderr, "STALL bot %d 5 s at %.0f %.0f %.0f dir %.2f %.2f path %zu/%zu objective '%s' detour %d enemy %d vis %d unarmed-walk stuck %d escape %d",
                     b.client, b.ps.origin[0], b.ps.origin[1], b.ps.origin[2], dir[0], dir[1], b.pathIdx, b.path.size(), b.objective.c_str(), int(b.detour.kind),
                     b.enemy, int(b.enemyVisible), b.stuck, int(now < b.escapeUntilMs));
        for (size_t k = b.pathIdx > 0 ? b.pathIdx - 1 : 0; k < b.path.size() && k < b.pathIdx + 3; ++k)
            std::fprintf(stderr, " n(%.0f %.0f %.0f)", bots_->nav->nodes[size_t(b.path[k])][0], bots_->nav->nodes[size_t(b.path[k])][1], bots_->nav->nodes[size_t(b.path[k])][2]);
        std::fprintf(stderr, "\n");
    }
    if (b.stallSinceMs >= 0 && !b.stallSample.sampled && now - b.stallSinceMs >= 1000) {
        BotStall& s = b.stallSample; // what the bot is doing a second into the spell
        s.sampled = true;
        s.what = b.objective;
        if (b.pathIdx > 0 && b.pathIdx < b.path.size()) {
            s.from = b.path[b.pathIdx - 1];
            s.to = b.path[b.pathIdx];
            s.fromDist = dist2(bots_->nav->nodes[size_t(s.from)], b.ps.origin);
            for (const NavLink& l : bots_->nav->links[size_t(s.from)])
                if (int(l.target) == s.to) s.flags = l.flags;
        }
        const Vec3 o{b.ps.origin[0], b.ps.origin[1], b.ps.origin[2] + 1};
        s.blocked = collision_->trace(o, Vec3{o[0] + dir[0] * 24, o[1] + dir[1] * 24, o[2]}, {-15, -15, 0}, {15, 15, 70}, Contents::PlayerSolid,
                                      mp_->clients[size_t(b.client)].ent->number).fraction < 0.5f;
    }
}

const std::vector<BotStall>& World::mpBotStalls() const {
    static const std::vector<BotStall> none;
    return bots_ ? bots_->stalls : none;
}

int64_t World::mpBotWalkMs() const { return bots_ ? bots_->walkMs : 0; }

const MpBotStats* World::mpBotStats(int client) const {
    if (!bots_) return nullptr;
    for (const MpBot& b : bots_->bots)
        if (b.client == client) return &b.stats;
    return nullptr;
}

MpBot* World::mpBot(int client) {
    if (!bots_) return nullptr;
    for (MpBot& b : bots_->bots)
        if (b.client == client) return &b;
    return nullptr;
}

void World::mpBotSetSkill(int client, int skill) {
    if (!bots_) return;
    for (MpBot& b : bots_->bots)
        if (b.client == client) b.skill = std::clamp(skill, 0, 3);
}

World::MpBotCost World::mpBotCost() const {
    MpBotCost c;
    if (!bots_) return c;
    c.lastUs = bots_->lastFrameUs;
    c.maxUs = bots_->maxFrameUs;
    c.frames = bots_->frameCount;
    c.meanUs = bots_->frameCount ? bots_->sumFrameUs / double(bots_->frameCount) : 0;
    c.maxPlans = bots_->maxPlansInAFrame;
    return c;
}

} // namespace iw2::game
