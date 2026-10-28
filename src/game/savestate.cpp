// openiw2 savegame body: VM + World state (docs/spec/savegame.md section 5).
//
// Saved: the script object graph (every entity's script object carries origin, angles, health, targetname, ... as fields), all
// script threads, the level time, cvars, entity numbering and per-entity engine fields listed in World::saveState (attachments,
// anim tree and per-node playback, scripted animation block, actor path, AI state stack, velocity, node cache and claim, friendly chain, working goal, attackers, missile states, vehicle / turret use),
// movers, links, inventories, hud elements, the script and engine random generators, pending notifies, objectives, looped effects, earthquakes, bad places, path
// disconnects, node claims, the random generator states.
//
// TODO(spec) (not restored; rebuilt or reset on load; the compass ping table is cleared on load, as CG_Init 0x4a5c10 does):
//  - the rest of ActorAi (requests, cover lists, timers; the state stack and depth are saved), danger planes, perception (sentientInfo), eye cache;
//  - sound handles of vehicles and turrets; FriendlyChains runtime beyond the per-entity chain node;
//  - the player's turret pointer (playerTurret_) / shell shock / view kick / damage feedback / use hints and the front end's pmove state;
//  - precached shell shocks, fx sight blockers and other caches (rebuilt by the level's precache code that ran in the save's past:
//    they are NOT rebuilt, because main() does not run again).
#include "game/savestate.h"

#include "game/world.h"
#include "script/stateio.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <type_traits>
#include <zlib.h>

namespace iw2::game {

using script::ObjectTable;
using script::StateReader;
using script::StateWriter;

namespace {

// A vehicle path node copy of a setswitchnode replacement (VehiclePathPos::switched / original). The node's script object is one of
// the collected getallvehiclenodes objects.
void writeVNode(StateWriter& w, const ObjectTable& t, const VehicleNode& n) {
    w.i32(t.indexOf(n.obj));
    w.str(n.name);
    w.str(n.target);
    w.b(n.rotate);
    w.f32(n.speed);
    w.f32(n.lookahead);
    w.vec3(n.origin);
    w.vec3(n.dir);
    w.vec3(n.angles);
    w.f32(n.length);
    w.i32(n.next);
    w.i32(n.prev);
}

VehicleNode readVNode(StateReader& r, const ObjectTable& t) {
    VehicleNode n;
    n.obj = t.object(r.i32());
    n.name = r.str();
    n.target = r.str();
    n.rotate = r.b();
    n.speed = r.f32();
    n.lookahead = r.f32();
    n.origin = r.vec3();
    n.dir = r.vec3();
    n.angles = r.vec3();
    n.length = r.f32();
    n.next = r.i32();
    n.prev = r.i32();
    return n;
}

void writeTraj(StateWriter& w, const Trajectory& t) {
    w.i32(t.type);
    w.i64(t.startMs);
    w.i32(t.durationMs);
    w.vec3(t.base);
    w.vec3(t.delta);
}
Trajectory readTraj(StateReader& r) {
    Trajectory t;
    t.type = r.i32();
    t.startMs = r.i64();
    t.durationMs = r.i32();
    t.base = r.vec3();
    t.delta = r.vec3();
    return t;
}
void writeChannel(StateWriter& w, const MoverChannel& c) {
    w.b(c.active);
    w.b(c.angles);
    writeTraj(w, c.tr);
    w.vec3(c.target);
    w.i32(c.linearMs);
    w.i32(c.decelMs);
    w.f32(c.speed);
    w.vec3(c.velocity);
    w.vec3(c.phase1End);
    w.vec3(c.phase2End);
}
MoverChannel readChannel(StateReader& r) {
    MoverChannel c;
    c.active = r.b();
    c.angles = r.b();
    c.tr = readTraj(r);
    c.target = r.vec3();
    c.linearMs = r.i32();
    c.decelMs = r.i32();
    c.speed = r.f32();
    c.velocity = r.vec3();
    c.phase1End = r.vec3();
    c.phase2End = r.vec3();
    return c;
}
int num(const Entity* e) { return e ? e->number : -1; }

} // namespace

std::vector<script::Value> World::stateRoots() const {
    std::vector<script::Value> roots;
    roots.emplace_back(vm_.level());
    roots.emplace_back(vm_.game());
    roots.emplace_back(vm_.anim());
    for (const auto& e : entities_) {
        roots.emplace_back(e->obj);
        if (e->usingTurret) roots.emplace_back(e->usingTurret);
        if (e->turret) {
            for (const auto* o : {&e->turret->target, &e->turret->engaged, &e->turret->owner})
                if (*o) roots.emplace_back(*o);
        }
        if (e->vehicle && e->vehicle->targetEnt) roots.emplace_back(e->vehicle->targetEnt);
    }
    for (const auto& n : nodes_) roots.emplace_back(n.obj);
    for (const auto& o : vehicleNodes_) roots.emplace_back(o);
    for (const auto& o : hudElems_) roots.emplace_back(o);
    for (const auto& p : pendingNotifies_) roots.emplace_back(p.obj);
    if (worldEntityObj_) roots.emplace_back(worldEntityObj_);
    return roots;
}

void World::saveState(StateWriter& w) {
    ObjectTable t;
    t.writeFunction = [this](StateWriter& sw, int f) { vm_.writeFunctionName(sw, f); };
    for (const auto& v : stateRoots()) t.collect(v);
    vm_.collectState(t);
    w.str(mapName_);
    t.write(w);
    vm_.saveState(w, t);

    // --- world scalars ---
    w.i64(timeMs_);
    w.i64(realMs_);
    w.i32(nextEntityNumber_);
    w.i32(nextFx_);
    w.u32(meleeSeed_);
    {
        std::ostringstream os;
        os << rng_;
        w.str(os.str());
        std::ostringstream ss;
        if (scriptRng_) ss << *scriptRng_;
        w.str(ss.str());
    }
    w.i32(perceptionTick_);
    w.vec3(playerPrev_);
    w.b(havePlayerPrev_);
    w.b(controlsFrozen_);
    w.b(autoPickup_);
    w.b(ignorePlayerRadiusDamage_);
    w.i64(playerRespawnMs_);
    w.f32(cullDist_);
    w.str(northYaw_);
    w.b(drawCompassFriendlies_);
    w.b(sunlight_.has_value());
    if (sunlight_) w.vec3(*sunlight_);
    w.str(cinematic_);
    w.i32(exitMs_);
    w.b(levelChange_.pending);
    w.b(levelChange_.failed);
    w.str(levelChange_.map);
    w.b(levelChange_.persistent);
    w.i64(levelChange_.atMs);
    w.b(moviePending_);

    w.b(playerLink_.has_value());
    if (playerLink_) {
        w.i32(playerLink_->parent);
        w.str(playerLink_->tag);
        w.f32(playerLink_->fraction);
        w.b(playerLink_->absolute);
        w.b(playerLink_->haveRef);
        w.raw(&playerLink_->ref, sizeof playerLink_->ref);
    }
    w.i32(saveCounter_);
    w.i32(playerUseState_);
    w.u32(playerEFlags_);
    w.u32(pmFlags_);

    w.u32(uint32_t(cvars.size()));
    for (const auto& [k, v] : cvars) {
        w.str(k);
        w.str(v);
    }
    w.u32(uint32_t(fxNames_.size()));
    for (const auto& f : fxNames_) w.str(f);
    w.u32(uint32_t(loopedFx_.size()));
    for (const auto& l : loopedFx_) {
        w.i32(l.id);
        w.f32(l.delayS);
        w.vec3(l.origin);
        w.i64(l.nextMs);
    }
    w.u32(uint32_t(hudElems_.size()));
    for (const auto& h : hudElems_) w.i32(t.indexOf(h));
    w.u32(uint32_t(pendingNotifies_.size()));
    for (const auto& p : pendingNotifies_) {
        w.i64(p.atMs);
        w.i32(t.indexOf(p.obj));
        w.str(p.name);
    }
    w.u32(uint32_t(menus_.size()));
    for (const auto& m : menus_) w.str(m);
    for (const auto& o : objectives_) {
        w.i32(o.state);
        w.str(o.text);
        for (const auto& p : o.positions) w.vec3(p);
        w.str(o.icon);
        w.b(o.ring);
        w.i64(o.ringStartMs);
    }
    w.u32(uint32_t(earthquakes_.size()));
    for (const auto& q : earthquakes_) {
        w.f32(q.scale);
        w.i32(q.durationMs);
        w.vec3(q.origin);
        w.f32(q.radius);
        w.i64(q.startMs);
    }
    for (const auto& b : badPlaces_) {
        w.i64(b.expireMs);
        w.i64(b.eventMs);
        w.str(b.name);
        w.i32(b.type);
        w.u32(b.teamMask);
        w.vec3(b.origin);
        w.f32(b.radius);
        w.f32(b.halfHeight);
        w.f32(b.left);
        w.f32(b.right);
    }
    // links blocked by bad places, per link and team (+8); restored as is, so a bad place expiring after the load gives back
    // exactly the counts it took
    w.u32(uint32_t(linkBlock_.size()));
    for (const auto& lb : linkBlock_) {
        w.u32(uint32_t(lb.size()));
        if (!lb.empty()) w.raw(lb.data(), lb.size() * sizeof lb[0]);
    }
    w.u32(uint32_t(movers_.size()));
    for (const auto& [n, m] : movers_) {
        w.i32(n);
        writeChannel(w, m.pos);
        writeChannel(w, m.apos);
    }
    w.u32(uint32_t(links_.size()));
    for (const auto& [n, l] : links_) {
        w.i32(n);
        w.i32(l.parent);
        w.str(l.tag);
        w.vec3(l.originOffset);
        w.vec3(l.anglesOffset);
        w.b(l.fullFrame);
        w.b(l.hasStartFrame);
        w.vec3(l.startOrigin);
        for (const auto& a : l.startAxis) w.vec3(a);
    }
    w.u32(uint32_t(inventories_.size()));
    for (const auto& [n, inv] : inventories_) {
        w.i32(n);
        w.u32(uint32_t(inv.owned.size()));
        for (const auto& s : inv.owned) w.str(s);
        for (const auto& s : inv.slot) w.str(s);
        w.str(inv.offhand);
        w.u32(uint32_t(inv.ammo.size()));
        for (const auto& [k, v] : inv.ammo) { w.str(k); w.i32(v); }
        w.u32(uint32_t(inv.clip.size()));
        for (const auto& [k, v] : inv.clip) { w.str(k); w.i32(v); }
    }
    // path disconnects: the entity numbers holding disabled links (re-applied by disconnectpaths on load)
    w.u32(uint32_t(pathRecs_.size()));
    for (const auto& [n, recs] : pathRecs_) w.i32(n);
    // node claims
    w.u32(uint32_t(nodes_.size()));
    for (const auto& n : nodes_) {
        w.i32(num(n.owner));
        w.i64(n.ownerUntilMs);
        for (int64_t v : n.teamBlockedMs) w.i64(v);
    }

    // --- entities ---
    w.i32(num(player_));
    w.u32(uint32_t(entities_.size()));
    for (const auto& ep : entities_) {
        const Entity& e = *ep;
        w.i32(e.number);
        w.str(e.classname);
        w.b(e.isPlayer);
        w.b(e.isSpawner);
        w.b(e.isActor);
        w.i32(t.indexOf(e.obj));
        w.str(e.animTree);
        w.u32(e.anim ? uint32_t(e.anim->nodeStates().size()) : 0u);
        if (e.anim)
            for (const AnimNodeState& n : e.anim->nodeStates()) {
                for (float f : {n.weight, n.goalWeight, n.blendLeft, n.time, n.oldTime, n.rate}) w.f32(f);
                w.i32(n.cycle);
                w.i32(n.oldCycle);
                w.b(n.ended);
                w.str(n.notify);
            }
        w.u32(uint32_t(e.attachments.size()));
        for (const auto& a : e.attachments) { w.str(a.model); w.str(a.tag); }
        w.b(e.scripted != nullptr);
        if (const ScriptedAnim* sa = e.scripted.get()) {
            static_assert(std::is_trivially_copyable_v<Axis>);
            w.raw(&sa->axis, sizeof sa->axis);
            w.vec3(sa->origin);
            w.vec3(sa->originOffset);
            w.vec3(sa->anglesOffset);
            w.i32(sa->anim);
            w.i32(sa->root);
            w.b(sa->started);
            w.b(sa->deathplant);
        }
        static_assert(std::is_trivially_copyable_v<ActorPath>);
        w.raw(&e.path, sizeof e.path); // plain data (points, counts, lookahead); openiw2 layout
        w.u32(uint32_t(e.ai.stack.size()));
        for (int st : e.ai.stack) w.i32(st);
        w.i32(e.ai.depth);
        w.u64(e.animScriptThread);
        w.str(e.animScript);
        w.i64(e.animScriptStartMs);
        w.vec3(e.velocity);
        w.i32(e.cachedNode);
        w.b(e.nodeCached);
        w.b(e.nodeUnreachable);
        w.i64(e.nodeCacheMs);
        w.i32(e.usingVehicle);
        w.i32(t.indexOf(e.usingTurret));
        w.i32(num(e.lastAttacker));
        w.i32(e.claimedNode);
        w.i32(e.friendlyChain);
        w.i64(e.friendlyChainMs);
        w.i32(e.chainNode);
        w.b(e.workGoal.set);
        w.vec3(e.workGoal.pos);
        w.f32(e.workGoal.radius);
        w.f32(e.workGoal.height);
        w.i32(e.workGoal.mode);
        w.i32(e.attackers);
        w.b(e.grenade != nullptr);
        if (const GrenadeState* g = e.grenade.get()) {
            w.str(g->weapon);
            w.i32(num(g->parent));
            w.i64(g->explodeMs);
            w.i32(g->trType);
            w.i64(g->trTime);
            w.vec3(g->trBase);
            w.vec3(g->trDelta);
            w.b(g->bounce);
            w.b(g->onGround);
            w.i32(num(g->holder));
            w.b(g->inHand);
            w.vec3(g->landing);
            w.i64(g->landingMs);
            w.i64(g->pingMs);
            w.vec3(g->aBase);
            w.vec3(g->aDelta);
            w.i64(g->aTime);
        }
        w.b(e.rocket != nullptr);
        if (const RocketState* rk = e.rocket.get()) {
            w.str(rk->weapon);
            w.i32(num(rk->parent));
            w.i64(rk->explodeMs);
            w.i64(rk->trTime);
            w.vec3(rk->trBase);
            w.vec3(rk->trDelta);
        }
        w.b(e.turret != nullptr);
        if (const TurretState* tu = e.turret.get()) {
            w.str(tu->weapon);
            w.u32(tu->flags);
            w.i32(t.indexOf(tu->target));
            w.vec3(tu->targetOffset);
            for (float f : {tu->topArc, tu->rightArc, tu->bottomArc, tu->leftArc, tu->setupRightArc, tu->setupLeftArc, tu->dropPitch,
                            tu->yawConvergenceTime, tu->pitchConvergenceTime, tu->suppressionTime, tu->aiSpread, tu->playerSpread,
                            tu->maxRangeSq, tu->pitch, tu->yaw})
                w.f32(f);
            w.b(tu->dropPitchDone);
            w.i32(tu->team);
            w.i32(tu->state);
            w.i32(t.indexOf(tu->engaged));
            w.i64(tu->engageStartMs);
            w.vec3(tu->aimPoint);
            w.vec3(tu->aimOffset);
            w.vec3(tu->wantAngles);
            w.i32(tu->scanIndex);
            w.i32(tu->fireCountdownMs);
            w.str(tu->loopSound);
            w.b(tu->inUse);
            w.i32(t.indexOf(tu->owner));
            w.i32(tu->stance);
            w.i32(tu->savedStance);
            w.vec3(tu->savedOrigin);
            w.i32(tu->fireTimerMs);
        }
        w.b(e.vehicle != nullptr);
        if (const VehicleState* v = e.vehicle.get()) {
            const VehiclePathPos& p = v->path;
            w.i32(p.node);
            w.b(p.end);
            for (float f : {p.frac, p.speed, p.lookahead, p.rotateFrac}) w.f32(f);
            w.vec3(p.origin);
            w.vec3(p.angles);
            w.vec3(p.lookPoint);
            w.b(p.hasSwitch); // setswitchnode replacement in progress (VehiclePathPos::switched / original)
            if (p.hasSwitch) {
                writeVNode(w, t, p.switched);
                writeVNode(w, t, p.original);
            }
            w.b(v->started);
            w.i32(v->waitNode);
            w.f32(v->waitSpeed);
            w.i32(v->speedMode);
            for (float f : {v->targetSpeed, v->accel, v->speed, v->stepFrac, v->forwardSpeed, v->turretPitch, v->turretYaw, v->steerAngle})
                w.f32(f);
            w.vec3(v->velocity);
            w.vec3(v->localVelocity);
            w.vec3(v->angularVelocity);
            w.i32(v->fireTimerMs);
            w.vec3(v->aimPoint);
            w.b(v->hasTarget);
            w.i32(t.indexOf(v->targetEnt));
            w.vec3(v->targetVec);
            w.vec3(v->targetOffset);
            w.b(v->altBarrel);
            w.i32(v->team);
            w.b(v->usable);
            w.i32(v->texScroll);
            w.b(v->onCompass);
            w.str(v->lookAtText);
            w.str(v->lookAtText2);
            w.b(v->engineOn);
            w.i32(v->owner);
            for (int i = 0; i < 6; ++i) { w.f32(v->wheelVel[i]); w.f32(v->wheelHeight[i]); w.i32(v->wheelSurface[i]); }
            for (float f : {v->joltDirF, v->joltDirR, v->joltAmp, v->joltPhase, v->joltPitch, v->joltRoll, v->joltSpeed, v->joltDecel}) w.f32(f);
            w.b(v->sightBlocked);
            w.i32(v->turretSnd);
            w.b(v->turretMoved);
            w.f32(v->idleFrac);
            w.f32(v->engFrac);
        }
    }
}

void World::loadState(StateReader& r) {
    const std::string map = r.str();
    if (map != mapName_) throw std::runtime_error("savegame: map " + map + " does not match the loaded map " + mapName_);
    std::unordered_map<uint64_t, script::ObjectPtr> live;
    script::collectLive(stateRoots(), live);
    ObjectTable t;
    t.readFunction = [this](StateReader& sr) { return vm_.readFunctionName(sr); };
    t.read(r, live, [this](const std::string& kind, uint64_t id) { return vm_.newObjectWithId(kind, id); });
    vm_.loadState(r, t);

    // CG_Init (0x4a5c10, run on every level / savegame load) zeroes the cgame block holding the compass ping table (0xf79a88); the
    // table is not saved, so it starts empty after a restore. The saved layout is unchanged (no kStateVersion bump).
    compassPings_.clear();
    timeMs_ = r.i64();
    realMs_ = r.i64();
    nextEntityNumber_ = r.i32();
    nextFx_ = r.i32();
    meleeSeed_ = r.u32();
    {
        std::istringstream is(r.str());
        is >> rng_;
        std::istringstream ss(r.str());
        if (scriptRng_ && ss.rdbuf()->in_avail() > 0) ss >> *scriptRng_;
    }
    perceptionTick_ = r.i32();
    playerPrev_ = r.vec3();
    havePlayerPrev_ = r.b();
    controlsFrozen_ = r.b();
    autoPickup_ = r.b();
    ignorePlayerRadiusDamage_ = r.b();
    playerRespawnMs_ = r.i64();
    cullDist_ = r.f32();
    northYaw_ = r.str();
    drawCompassFriendlies_ = r.b();
    sunlight_.reset();
    if (r.b()) sunlight_ = r.vec3();
    cinematic_ = r.str();
    exitMs_ = r.i32();
    levelChange_.pending = r.b();
    levelChange_.failed = r.b();
    levelChange_.map = r.str();
    levelChange_.persistent = r.b();
    levelChange_.atMs = r.i64();
    moviePending_ = r.version >= 2 ? r.b() : false;

    playerLink_.reset();
    if (r.b()) {
        PlayerLink l;
        l.parent = r.i32();
        l.tag = r.str();
        l.fraction = r.f32();
        l.absolute = r.b();
        l.haveRef = r.b();
        r.raw(&l.ref, sizeof l.ref);
        playerLink_ = l;
    }
    saveCounter_ = r.i32();
    playerUseState_ = r.i32();
    playerEFlags_ = r.u32();
    pmFlags_ = r.u32();

    // The exe writes only the dvars with flag 0x400 (0x4e2e00 -> 0x433200(buffer, 0x400) tests dvar flags & mask), so campaign
    // progress (mis_01..mis_10 / mis_difficulty, flags 0x1001) is never taken back from a save. openiw2 keeps no dvar flags; the set
    // with bit 0x400 is read from the register calls (0x431bf0 int / 0x431d00 string / 0x431f60 bool / 0x432020 float, 0x432260 for
    // string dvars; the flag word is the first pushed argument) and has these members: sv_saveOnStartMap 0x449db0 (0x1404),
    // cg_fov 0x4a11b3, cg_viewVehicleInfluence 0x4a121b, cg_drawHUD 0x4a1247, cg_hudCompassMaxRange 0x4a1347, ui_campaign
    // 0x446a61 / 0x4c3222, ui_nextMission 0x4c3267, g_speed 0x4d87bf, g_friendlyNameDist 0x4d8a30, g_friendlyfireDist 0x4d8a50,
    // g_friendlyfireUseDist 0x4d8a70, g_friendlyfireUseTime 0x4d8aac. Dvars made by the script builtin setcvar (0x4ec4c0, create call
    // 0x431bf0 with flag word 0) are outside the set, so the exe would not take them back from a save either; this port still
    // restores them, because its world round trip assumes main() does not run again on a load. TODO(spec): the load sequence that
    // refills those dvars (needs a decode of the level start after G_ReadGame); until then every cvar except the known-outside ones
    // below is restored. g_gameskill is registered with 0x1064 / 0x1066, so it is outside the set too.
    {
        auto keepLive = [](const std::string& k) {
            std::string l = k;
            for (auto& ch : l) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            return l.rfind("mis_", 0) == 0 || l == "sv_lastsavegame" || l == "com_playerprofile";
        };
        std::map<std::string, std::string, CvarNameLess> live;
        for (const auto& [k, v] : cvars)
            if (keepLive(k)) live[k] = v;
        cvars.clear();
        for (uint32_t i = 0, n = r.u32(); i < n; ++i) {
            std::string k = r.str();
            std::string v = r.str();
            if (!keepLive(k)) cvars[k] = std::move(v);
        }
        for (auto& [k, v] : live) cvars[k] = v;
    }
    fxNames_.clear();
    for (uint32_t i = 0, n = r.u32(); i < n; ++i) fxNames_.push_back(r.str());
    loopedFx_.clear();
    for (uint32_t i = 0, n = r.u32(); i < n; ++i) {
        LoopedFx l;
        l.id = r.i32();
        l.delayS = r.f32();
        l.origin = r.vec3();
        l.nextMs = r.i64();
        loopedFx_.push_back(l);
    }
    hudElems_.clear();
    for (uint32_t i = 0, n = r.u32(); i < n; ++i) hudElems_.push_back(t.object(r.i32()));
    pendingNotifies_.clear();
    for (uint32_t i = 0, n = r.u32(); i < n; ++i) {
        PendingNotify p;
        p.atMs = r.i64();
        p.obj = t.object(r.i32());
        p.name = r.str();
        pendingNotifies_.push_back(std::move(p));
    }
    menus_.clear();
    for (uint32_t i = 0, n = r.u32(); i < n; ++i) menus_.push_back(r.str());
    for (auto& o : objectives_) {
        o.state = r.i32();
        o.text = r.str();
        for (auto& p : o.positions) p = r.vec3();
        o.icon = r.str();
        o.ring = r.b();
        o.ringStartMs = r.i64();
    }
    earthquakes_.clear();
    for (uint32_t i = 0, n = r.u32(); i < n; ++i) {
        Earthquake q;
        q.scale = r.f32();
        q.durationMs = r.i32();
        q.origin = r.vec3();
        q.radius = r.f32();
        q.startMs = r.i64();
        earthquakes_.push_back(q);
    }
    for (auto& b : badPlaces_) {
        b.expireMs = r.i64();
        b.eventMs = r.i64();
        b.name = r.str();
        b.type = r.i32();
        b.teamMask = r.u32();
        b.origin = r.vec3();
        b.radius = r.f32();
        b.halfHeight = r.f32();
        b.left = r.f32();
        b.right = r.f32();
    }
    {
        linkBlock_.resize(r.u32()); // allocated on first use (badplace.cpp), so the fresh world may not have it yet
        for (auto& lb : linkBlock_) {
            lb.resize(r.u32());
            if (!lb.empty()) r.raw(lb.data(), lb.size() * sizeof lb[0]);
        }
    }
    movers_.clear();
    for (uint32_t i = 0, n = r.u32(); i < n; ++i) {
        const int k = r.i32();
        MoverState m;
        m.pos = readChannel(r);
        m.apos = readChannel(r);
        movers_[k] = m;
    }
    links_.clear();
    for (uint32_t i = 0, n = r.u32(); i < n; ++i) {
        const int k = r.i32();
        Link l;
        l.parent = r.i32();
        l.tag = r.str();
        l.originOffset = r.vec3();
        l.anglesOffset = r.vec3();
        l.fullFrame = r.b();
        l.hasStartFrame = r.b();
        l.startOrigin = r.vec3();
        for (auto& a : l.startAxis) a = r.vec3();
        links_[k] = l;
    }
    inventories_.clear();
    for (uint32_t i = 0, n = r.u32(); i < n; ++i) {
        const int k = r.i32();
        Inventory inv;
        for (uint32_t j = 0, m = r.u32(); j < m; ++j) inv.owned.insert(r.str());
        for (auto& s : inv.slot) s = r.str();
        inv.offhand = r.str();
        for (uint32_t j = 0, m = r.u32(); j < m; ++j) { std::string a = r.str(); inv.ammo[a] = r.i32(); }
        for (uint32_t j = 0, m = r.u32(); j < m; ++j) { std::string a = r.str(); inv.clip[a] = r.i32(); }
        inventories_[k] = std::move(inv);
    }
    std::vector<int> disconnected;
    for (uint32_t i = 0, n = r.u32(); i < n; ++i) disconnected.push_back(r.i32());
    const uint32_t nNodes = r.u32();
    if (nNodes != nodes_.size()) throw std::runtime_error("savegame: path node count differs from the map");
    std::vector<int> nodeOwners(nNodes);
    for (uint32_t i = 0; i < nNodes; ++i) {
        nodeOwners[i] = r.i32();
        nodes_[i].ownerUntilMs = r.i64();
        for (int64_t& v : nodes_[i].teamBlockedMs) v = r.i64();
    }

    // --- entities ---
    const int playerNum = r.i32();
    struct Fix { Entity* e; int lastAttacker, grenadeParent, grenadeHolder, rocketParent; };
    std::vector<Fix> fixes;
    std::vector<int> savedNumbers;
    for (uint32_t i = 0, n = r.u32(); i < n; ++i) {
        const int number = r.i32();
        savedNumbers.push_back(number);
        Entity* e = entityByNumber(number);
        if (!e) {
            entities_.push_back(std::make_unique<Entity>());
            e = entities_.back().get();
            e->number = number;
        }
        e->classname = r.str();
        e->isPlayer = r.b();
        e->isSpawner = r.b();
        e->isActor = r.b();
        script::ObjectPtr obj = t.object(r.i32());
        if (e->obj != obj) {
            if (e->obj) byObject_.erase(e->obj.get());
            e->obj = obj;
        }
        byObject_[e->obj.get()] = e;
        const std::string tree = r.str();
        if (tree.empty()) {
            e->anim.reset();
            e->animTree.clear();
        } else if (e->animTree != tree || !e->anim) {
            useAnimTree(*e, tree);
        }
        const uint32_t nAnim = r.u32();
        if (nAnim && (!e->anim || e->anim->nodeStates().size() != nAnim))
            throw std::runtime_error("savegame: anim tree " + tree + " node count differs");
        for (uint32_t j = 0; j < nAnim; ++j) {
            AnimNodeState& n = e->anim->nodeStates()[j];
            for (float* f : {&n.weight, &n.goalWeight, &n.blendLeft, &n.time, &n.oldTime, &n.rate}) *f = r.f32();
            n.cycle = r.i32();
            n.oldCycle = r.i32();
            n.ended = r.b();
            n.notify = r.str();
        }
        e->attachments.clear();
        for (uint32_t j = 0, m = r.u32(); j < m; ++j) {
            Attachment a;
            a.model = r.str();
            a.tag = r.str();
            e->attachments.push_back(std::move(a));
        }
        e->scripted.reset();
        if (r.b()) {
            auto sa = std::make_unique<ScriptedAnim>();
            r.raw(&sa->axis, sizeof sa->axis);
            sa->origin = r.vec3();
            sa->originOffset = r.vec3();
            sa->anglesOffset = r.vec3();
            sa->anim = r.i32();
            sa->root = r.i32();
            sa->started = r.b();
            sa->deathplant = r.b();
            e->scripted = std::move(sa);
        }
        r.raw(&e->path, sizeof e->path);
        const uint32_t nStack = r.u32();
        if (nStack != e->ai.stack.size()) throw std::runtime_error("savegame: AI state stack size differs");
        for (int& st : e->ai.stack) st = r.i32();
        e->ai.depth = r.i32();
        e->animScriptThread = r.u64();
        e->animScript = r.str();
        e->animScriptStartMs = r.i64();
        e->velocity = r.vec3();
        e->cachedNode = r.i32();
        e->nodeCached = r.b();
        e->nodeUnreachable = r.b();
        e->nodeCacheMs = r.i64();
        e->usingVehicle = r.i32();
        e->usingTurret = t.object(r.i32());
        Fix fx{e, r.i32(), -1, -1, -1};
        e->claimedNode = r.i32();
        e->friendlyChain = r.i32();
        e->friendlyChainMs = r.i64();
        e->chainNode = r.i32();
        e->workGoal.set = r.b();
        e->workGoal.pos = r.vec3();
        e->workGoal.radius = r.f32();
        e->workGoal.height = r.f32();
        e->workGoal.mode = r.i32();
        e->attackers = r.i32();
        e->grenade.reset();
        if (r.b()) {
            auto g = std::make_unique<GrenadeState>();
            g->weapon = r.str();
            fx.grenadeParent = r.i32();
            g->explodeMs = r.i64();
            g->trType = r.i32();
            g->trTime = r.i64();
            g->trBase = r.vec3();
            g->trDelta = r.vec3();
            g->bounce = r.b();
            g->onGround = r.b();
            fx.grenadeHolder = r.i32();
            g->inHand = r.b();
            g->landing = r.vec3();
            g->landingMs = r.i64();
            g->pingMs = r.i64();
            g->aBase = r.vec3();
            g->aDelta = r.vec3();
            g->aTime = r.i64();
            e->grenade = std::move(g);
        }
        e->rocket.reset();
        if (r.b()) {
            auto rk = std::make_unique<RocketState>();
            rk->weapon = r.str();
            fx.rocketParent = r.i32();
            rk->explodeMs = r.i64();
            rk->trTime = r.i64();
            rk->trBase = r.vec3();
            rk->trDelta = r.vec3();
            e->rocket = std::move(rk);
        }
        if (r.b()) {
            const std::string weaponInfo = r.str();
            if (!e->turret) initTurret(*e, weaponInfo); // a turret spawned during play (spawnturret): set up, then overwritten
            TurretState& tu = *e->turret;
            tu.weapon = weaponInfo;
            tu.flags = r.u32();
            tu.target = t.object(r.i32());
            tu.targetOffset = r.vec3();
            for (float* f : {&tu.topArc, &tu.rightArc, &tu.bottomArc, &tu.leftArc, &tu.setupRightArc, &tu.setupLeftArc, &tu.dropPitch,
                             &tu.yawConvergenceTime, &tu.pitchConvergenceTime, &tu.suppressionTime, &tu.aiSpread, &tu.playerSpread,
                             &tu.maxRangeSq, &tu.pitch, &tu.yaw})
                *f = r.f32();
            tu.dropPitchDone = r.b();
            tu.team = r.i32();
            tu.state = r.i32();
            tu.engaged = t.object(r.i32());
            tu.engageStartMs = r.i64();
            tu.aimPoint = r.vec3();
            tu.aimOffset = r.vec3();
            tu.wantAngles = r.vec3();
            tu.scanIndex = r.i32();
            tu.fireCountdownMs = r.i32();
            tu.loopSound = r.str();
            tu.loopHandle = -1; // sounds restart from the front end
            tu.inUse = r.b();
            tu.owner = t.object(r.i32());
            tu.stance = r.i32();
            tu.savedStance = r.i32();
            tu.savedOrigin = r.vec3();
            tu.fireTimerMs = r.i32();
        }
        if (r.b()) {
            VehicleState& v = vehicleState(script::Value(e->obj)); // created from vehicletype / spawnflags for one spawned during play
            VehiclePathPos& p = v.path;
            p.node = r.i32();
            p.end = r.b();
            for (float* f : {&p.frac, &p.speed, &p.lookahead, &p.rotateFrac}) *f = r.f32();
            p.origin = r.vec3();
            p.angles = r.vec3();
            p.lookPoint = r.vec3();
            p.hasSwitch = r.b();
            if (p.hasSwitch) {
                p.switched = readVNode(r, t);
                p.original = readVNode(r, t);
            }
            v.started = r.b();
            v.waitNode = r.i32();
            v.waitSpeed = r.f32();
            v.speedMode = r.i32();
            for (float* f : {&v.targetSpeed, &v.accel, &v.speed, &v.stepFrac, &v.forwardSpeed, &v.turretPitch, &v.turretYaw, &v.steerAngle})
                *f = r.f32();
            v.velocity = r.vec3();
            v.localVelocity = r.vec3();
            v.angularVelocity = r.vec3();
            v.fireTimerMs = r.i32();
            v.aimPoint = r.vec3();
            v.hasTarget = r.b();
            v.targetEnt = t.object(r.i32());
            v.targetVec = r.vec3();
            v.targetOffset = r.vec3();
            v.altBarrel = r.b();
            v.team = r.i32();
            v.usable = r.b();
            v.texScroll = r.i32();
            v.onCompass = r.b();
            v.lookAtText = r.str();
            v.lookAtText2 = r.str();
            v.engineOn = r.b();
            v.owner = r.i32();
            for (int i = 0; i < 6; ++i) { v.wheelVel[i] = r.f32(); v.wheelHeight[i] = r.f32(); v.wheelSurface[i] = r.i32(); }
            for (float* f : {&v.joltDirF, &v.joltDirR, &v.joltAmp, &v.joltPhase, &v.joltPitch, &v.joltRoll, &v.joltSpeed, &v.joltDecel}) *f = r.f32();
            v.sightBlocked = r.b();
            v.turretSnd = r.i32();
            v.turretMoved = r.b();
            v.idleFrac = r.f32();
            v.engFrac = r.f32();
            v.spinHandle = v.idleHandle = v.engHandle = 0; // sound handles: the front end restarts the loops
        }
        // runtime that is not saved starts over (see the TODO(spec) list at the top)
        e->sentientInfo.clear();
        e->eyeFrameMs = -1;
        fixes.push_back(fx);
    }
    if (!r.atEnd()) throw std::runtime_error("savegame: trailing bytes in the state");
    // entities the fresh world has that the save does not (cannot happen with an unchanged map: numbers are never reused)
    for (const auto& ep : entities_)
        if (std::find(savedNumbers.begin(), savedNumbers.end(), ep->number) == savedNumbers.end())
            throw std::runtime_error("savegame: entity " + std::to_string(ep->number) + " of the map is missing from the save");
    std::sort(entities_.begin(), entities_.end(), [](const auto& a, const auto& b) { return a->number < b->number; });
    for (const Fix& f : fixes) {
        f.e->lastAttacker = entityByNumber(f.lastAttacker);
        if (f.e->grenade) {
            f.e->grenade->parent = entityByNumber(f.grenadeParent);
            f.e->grenade->holder = entityByNumber(f.grenadeHolder);
        }
        if (f.e->rocket) f.e->rocket->parent = entityByNumber(f.rocketParent);
    }
    grenades_.clear();
    rockets_.clear();
    for (const auto& ep : entities_) {
        if (ep->grenade && !ep->obj->removed) grenades_.push_back(ep.get());
        if (ep->rocket && !ep->obj->removed) rockets_.push_back(ep.get());
    }
    for (uint32_t i = 0; i < nNodes; ++i) nodes_[i].owner = entityByNumber(nodeOwners[i]);
    player_ = entityByNumber(playerNum);
    // path disconnects: the fresh world disconnected its DYNAMICPATH brush models; bring the set in line with the save
    std::vector<int> current;
    for (const auto& [n, recs] : pathRecs_) current.push_back(n);
    for (int n : current)
        if (std::find(disconnected.begin(), disconnected.end(), n) == disconnected.end())
            if (Entity* e = entityByNumber(n)) connectEntityPaths(*e);
    for (int n : disconnected)
        if (!pathRecs_.count(n))
            if (Entity* e = entityByNumber(n)) disconnectEntityPaths(*e);
    levelInitializing_ = false; // 0x4da540 clears 0x1c61048 (2 during a savegame load) once the level is loaded
}

std::string World::stateDigest() const {
    std::ostringstream os;
    os << "map " << mapName_ << " time " << timeMs_ << " vmtime " << vm_.time() << " entities " << entities_.size() << "\n";
    auto val = [](const script::Value& v) {
        if (v.isObject()) return std::string("obj#") + std::to_string(v.asObject() ? v.asObject()->id : 0);
        if (v.isArray()) return std::string("array[") + std::to_string(std::get<script::ArrayPtr>(v.v)->size()) + "]";
        return v.typeName() + ":" + v.toString();
    };
    auto fields = [&](const script::Object& o) {
        std::vector<std::string> f;
        for (const auto& [k, v] : o.fields) f.push_back(k + "=" + val(v));
        std::sort(f.begin(), f.end());
        std::string s;
        for (const auto& x : f) s += " " + x;
        return s;
    };
    os << "level" << fields(*vm_.level()) << "\n";
    for (const auto& e : entities_) {
        os << "ent " << e->number << " " << e->classname << " obj#" << e->obj->id << (e->obj->removed ? " removed" : "")
           << " tree " << e->animTree << " att " << e->attachments.size() << fields(*e->obj) << "\n";
        if (e->vehicle) {
            const VehiclePathPos& p = e->vehicle->path;
            os << "vehicle " << e->number << " node " << p.node << " switch " << p.hasSwitch;
            if (p.hasSwitch) os << " " << p.switched.name << "->" << p.switched.next << " len " << p.switched.length << " orig " << p.original.next;
            os << "\n";
        }
    }
    for (const auto& d : vm_.describeThreads()) os << "thread " << d << "\n";
    for (const auto& [k, v] : cvars) os << "cvar " << k << "=" << v << "\n";
    os << "hud " << hudElems_.size() << " notifies " << pendingNotifies_.size() << " movers " << movers_.size() << " links " << links_.size()
       << " inventories " << inventories_.size() << " paths " << pathRecs_.size() << "\n";
    return os.str();
}

// ---- file container ----

std::vector<uint8_t> buildSaveFile(World& world, const SaveRequest& req) {
    world.clearSaveRecentlyLoaded(); // G_WriteGame 0x4e3f2a
    StateWriter w;
    w.raw(kStateMagic, sizeof kStateMagic);
    w.u32(kStateVersion);
    world.saveState(w);
    const std::vector<uint8_t>& body = w.bytes;
    uLongf zsize = compressBound(uLong(body.size()));
    std::vector<uint8_t> z(zsize);
    // level 1 like the exe's compressor call (0x4d51e5)
    if (compress2(z.data(), &zsize, body.data(), uLong(body.size()), 1) != Z_OK) throw std::runtime_error("savegame: deflate failed");
    z.resize(zsize);

    ui::SaveHeaderFields f;
    f.bspChecksum = uint32_t(crc32(0, world.bspData().data(), uInt(world.bspData().size())));
    f.bodyChecksum = uint32_t(crc32(0, body.data(), uInt(body.size())));
    f.saveId = req.saveId;
    f.mapName = world.mapName();
    f.campaign = world.cvars.count("ui_campaign") ? world.cvars.at("ui_campaign") : "american"; // registered default (0x4d5087)
    f.image = req.image;
    f.description = req.description;
    f.name = req.name;
    int health = 0, maxHealth = 0;
    if (Entity* p = world.player()) {
        const auto h = p->obj->get("health"), m = p->obj->get("maxhealth");
        health = h.isNumber() ? h.asInt() : 0;
        maxHealth = m.isNumber() ? m.asInt() : 0;
    }
    f.healthPercent = ui::saveHealthPercent(health, maxHealth);
    f.difficulty = world.cvars.count("g_gameskill") ? std::atoi(world.cvars.at("g_gameskill").c_str()) : 0;
    f.manual = req.manual;
    f.tm = ui::saveTimeNow();
    f.bodySize = uint32_t(body.size());
    f.compressedSize = uint32_t(z.size());
    std::vector<uint8_t> file = ui::buildSaveHeader(f);
    file.insert(file.end(), z.begin(), z.end());
    return file;
}

std::string parseSaveFile(const std::vector<uint8_t>& file, LoadedSave& out) {
    constexpr uint32_t kMax = 0x280000;      // G_LoadGame's limit of the compressed size (0x4d5270)
    constexpr uint32_t kMaxRaw = 0x4000000;  // openiw2: its body is larger than the original's 0x280000 raw dump; own cap 64 MB
    if (file.size() < ui::kSaveHeaderSize) return "G_LoadGame: bad read";
    if (!ui::saveHeaderLoadable(file)) return "G_LoadGame: wrong version";
    if (!ui::saveHeaderInfo(file, out.info)) return "G_LoadGame: bad read";
    uint32_t raw = 0, comp = 0;
    std::memcpy(&raw, file.data() + 0x4d0, 4);
    std::memcpy(&comp, file.data() + 0x4d4, 4);
    out.name = std::string(reinterpret_cast<const char*>(file.data() + 0x460), strnlen(reinterpret_cast<const char*>(file.data() + 0x460), 0x40));
    if (comp > kMax || raw > kMaxRaw) return "G_LoadGame: savegame is larger than the maximum allowed save file";
    if (file.size() < ui::kSaveHeaderSize + comp) return "G_LoadGame: bad read";
    out.body.assign(raw, 0);
    uLongf n = raw;
    if (uncompress(out.body.data(), &n, file.data() + ui::kSaveHeaderSize, comp) != Z_OK || n != raw)
        return "SaveMemory_DecompressLoad: decompress failed.";
    if (raw < sizeof kStateMagic + 4 || std::memcmp(out.body.data(), kStateMagic, sizeof kStateMagic) != 0) return "not an openiw2 save";
    uint32_t ver = 0;
    std::memcpy(&ver, out.body.data() + sizeof kStateMagic, 4);
    if (ver != kStateVersion && ver != 1) return "openiw2 save format " + std::to_string(ver) + ", expected " + std::to_string(kStateVersion);
    return {};
}

void restoreSave(World& world, const LoadedSave& save) {
    StateReader r(save.body.data() + sizeof kStateMagic + 4, save.body.size() - sizeof kStateMagic - 4);
    std::memcpy(&r.version, save.body.data() + sizeof kStateMagic, 4); // version 1 (before the movie-pending flag) still loads
    world.loadState(r);
    world.markSaveLoaded();
}

} // namespace iw2::game

// ---- save requests and script builtins (docs/spec/savegame.md section 3) ----

namespace iw2::game {

int World::queueSave(const std::string& name, const std::string& description, const std::string& image, int type, int flags) {
    if (reloading_) { // g_reloading (0x44a140)
        print("savegame request ignored");
        return -1;
    }
    if (type == 1) {
        if (autosaveThisFrame_) {
            print("^3Warning: Multiple Autosaves attempted in same frame. Save " + name + " ignored");
            return -2;
        }
        autosaveThisFrame_ = true;
    }
    if (pendingSaves_.size() >= 3) {
        print("^3Warning: Pending Saves limit exceeded. Save " + name + " ignored");
        return -3;
    }
    PendingSave p;
    p.name = name.substr(0, 63);
    p.description = description.substr(0, 255);
    p.image = image.substr(0, 63);
    p.type = type;
    p.flags = flags;
    p.saveId = type == 1 ? ++saveCounter_ : 0; // 0x44a11c
    pendingSaves_.push_back(p);
    return p.saveId;
}

void World::processSaves() {
    if (pendingSaves_.empty()) {
        autosaveThisFrame_ = false;
        return;
    }
    if (reloading_) { // 0x44a239: the queue stays
        print("savegame request ignored");
        return;
    }
    auto pending = std::exchange(pendingSaves_, {});
    autosaveThisFrame_ = false;
    for (const PendingSave& p : pending) {
        SaveRequest req;
        // 0x4e02b0 builds the file name with the name rule of 0x4e02e0 (the '\' of "autosave\<map>" becomes '/')
        const std::string file = ui::saveFileName(p.name);
        if (file.empty()) {
            print("savegame: invalid name '" + p.name + "'");
            saveFailed_ = true;
            continue;
        }
        req.name = file.substr(5, file.size() - 9); // between "save/" and ".svg"
        req.description = p.description;
        req.image = p.image;
        req.saveId = p.saveId;
        req.manual = p.type == 0;
        SaveFile f;
        f.name = req.name;
        f.manual = req.manual;
        try {
            f.bytes = buildSaveFile(*this, req);
            saveFailed_ = false;
        } catch (const std::exception& e) {
            print(std::string("savegame failed: ") + e.what());
            saveFailed_ = true;
            continue;
        }
        if (p.flags & 6) saveFiles_.push_back(std::move(f));
        else uncommitted_ = std::make_pair(p.saveId, std::move(f)); // savegamenocommit: kept until commitsave
    }
}

void World::registerSaveBuiltins() {
    using script::CallContext;
    using script::Value;
    // savegame(name [, description [, image]]) (0x4f3a30) and savegamenocommit (0x4f3bd0, flags 1, returns the id).
    auto request = [this](CallContext& c, int flags) -> int {
        if (player_ && !isAlive(*player_)) throw std::runtime_error("Attempting to save while dead");
        std::string name = c.argc() > 0 ? c.arg(0).toString() : std::string();
        if (name.empty()) name = "auto";
        const std::string description = c.argc() > 1 ? c.arg(1).toString() : std::string();
        const std::string image = c.argc() > 2 ? c.arg(2).toString() : std::string("$default");
        std::string lower = name;
        for (auto& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        const std::string file = (lower == "levelstart" ? "autosave\\" + mapName_ : "autosave\\" + mapName_ + "-" + name).substr(0, 63);
        commitRequested_ = false; // 0x4f3b7a: byte 0x1b15ac1 = 0
        const int id = queueSave(file, description, image, 1, flags);
        if (id == -1) throw std::runtime_error("Attempting to save during a restart.");
        if (id == -2) throw std::runtime_error("Attempting to save multiple times in a single frame.");
        return id;
    };
    vm_.registerBuiltin("savegame", [request](CallContext& c) {
        request(c, 2);
        return Value();
    });
    vm_.registerBuiltin("savegamenocommit", [request](CallContext& c) { return Value(int32_t(request(c, 1))); });
    // commitsave(id) (0x4f3db0): writes the uncommitted save of that id. Byte 0x1b15ac1 (set by 0x4d5830 and G_WriteGame 0x4e3ffc..0x4e40c0): when set, the exe clears it and returns without committing. TODO(spec): model it (needs the G_WriteGame caller flow).
    vm_.registerBuiltin("commitsave", [this](CallContext& c) {
        const int id = c.arg(0).asInt();
        commitRequested_ = false;
        if (!uncommitted_) throw std::runtime_error("Save Commit attempted with no valid committable save available");
        if (uncommitted_->first != id)
            throw std::runtime_error("Save Commit attempted on incorrect save currentsave:" + std::to_string(uncommitted_->first) +
                                     ", commit:" + std::to_string(id));
        saveFiles_.push_back(std::move(uncommitted_->second));
        uncommitted_.reset();
        return Value();
    });
    // issavesuccessful (0x4f3d80): the last save did not fail. issaverecentlyloaded (0x4f3da0): byte 0x1b15ac0, set by a load,
    // cleared by G_WriteGame (0x4e3f2a).
    vm_.registerBuiltin("issavesuccessful", [this](CallContext&) { return Value(!saveFailed_); });
    vm_.registerBuiltin("issaverecentlyloaded", [this](CallContext&) { return Value(saveRecentlyLoaded_); });
}

} // namespace iw2::game
