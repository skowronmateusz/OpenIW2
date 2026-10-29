// Skeletons/tags, damage, perception and shooting (docs/spec/combat.md). Most rules here are inferred placeholders
// until the corresponding engine code is analyzed; each is marked in the spec.

#include "game/world.h"
#include "game/axis.h"
#include "game/bulletdamage.h"
#include "game/itempickup.h"
#include "game/vehicles.h"
#include "game/triggerdamage.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <random>

namespace iw2::game {

using namespace script;

namespace {

std::string lowerStr(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 scale(const Vec3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float len(const Vec3& a) { return std::sqrt(dot(a, a)); }

assets::Quat yawQuat(float yawDeg) {
    const float h = yawDeg * float(M_PI) / 360.0f;
    return {0, 0, std::sin(h), std::cos(h)};
}

} // namespace

const assets::XModelParts* World::skeleton(const std::string& model) {
    auto it = skeletons_.find(model);
    if (it == skeletons_.end()) {
        std::optional<assets::XModelParts> parts;
        try {
            if (auto md = vfs_.read(model)) {
                auto xm = assets::parseXModel(*md);
                boneBoxes_[model] = std::move(xm.boneBoxes);
                collSurfs_[model] = std::move(xm.collSurfs);
                modelBounds_[model] = {xm.mins, xm.maxs};
                if (auto pd = vfs_.read("xmodelparts/" + xm.lods.at(0).name)) parts = assets::parseXModelParts(*pd);
            }
        } catch (const std::exception&) {
        }
        it = skeletons_.emplace(model, std::move(parts)).first;
    }
    return it->second ? &*it->second : nullptr;
}

// Body and attachments in entity space: tagless attachments share the body's bones by name, tagged ones hang their
// roots on the tag.
std::vector<HitBoxModel> World::entityModels(Entity& e) {
    std::vector<HitBoxModel> models;
    const Value m = e.obj->get("model");
    if (!m.isString()) return models;
    const assets::XModelParts* body = skeleton(m.asString());
    if (!body) return models;
    // A turret's bone controls (0x51d740) also apply on top of an animation tree (the mg42 animscripts give the turret one).
    const assets::XPose pose =
        e.anim ? (e.turret ? assets::applyControls(*body, e.anim->pose(*body), turretBoneControls(*body, e.turret->pitch, e.turret->yaw, e.turret->drop))
                           : e.anim->pose(*body))
        : e.turret ? assets::controlledPose(*body, turretBoneControls(*body, e.turret->pitch, e.turret->yaw, e.turret->drop))
                   : assets::bindPose(*body);
    // Bone lookup by name; the first model that has a name keeps it.
    std::unordered_map<std::string, std::pair<std::array<float, 3>, assets::Quat>> local;
    std::unordered_map<std::string, int> bodyIndex;
    for (size_t i = 0; i < body->bones.size(); ++i) {
        local.emplace(lowerStr(body->bones[i].name), std::make_pair(pose.position[i], pose.rotation[i]));
        bodyIndex.emplace(lowerStr(body->bones[i].name), int(i));
    }
    auto addModel = [&](const std::string& name, const assets::XModelParts* parts, int attachBone, const assets::XPose& p) {
        HitBoxModel hm;
        hm.parts = parts;
        auto bit = boneBoxes_.find(name);
        hm.boxes = bit == boneBoxes_.end() ? nullptr : &bit->second;
        auto cit = collSurfs_.find(name);
        hm.collSurfs = cit == collSurfs_.end() ? nullptr : &cit->second;
        hm.attachBone = attachBone;
        hm.position = p.position;
        hm.rotation = p.rotation;
        models.push_back(std::move(hm));
    };
    addModel(m.asString(), body, -1, pose);
    for (const auto& at : e.attachments) {
        const assets::XModelParts* ap = skeleton(at.model);
        if (!ap) continue;
        assets::XPose p = assets::bindPose(*ap);
        const auto tagIt = at.tag.empty() ? local.end() : local.find(lowerStr(at.tag));
        for (size_t i = 0; i < ap->bones.size(); ++i) {
            const auto it = at.tag.empty() ? local.find(lowerStr(ap->bones[i].name)) : local.end();
            if (it != local.end()) {
                p.position[i] = it->second.first;
                p.rotation[i] = it->second.second;
            } else if (ap->bones[i].parent < 0 && tagIt != local.end()) {
                p.position[i] = tagIt->second.first;
                p.rotation[i] = tagIt->second.second;
            } else if (ap->bones[i].parent >= 0) {
                const size_t pi = size_t(ap->bones[i].parent);
                p.rotation[i] = assets::multiply(p.rotation[pi], ap->bones[i].localRotation);
                p.position[i] = add(p.position[pi], assets::rotate(p.rotation[pi], ap->bones[i].localOffset));
            }
            local.emplace(lowerStr(ap->bones[i].name), std::make_pair(p.position[i], p.rotation[i]));
        }
        int attachBone = -1;
        if (!at.tag.empty())
            if (auto bi = bodyIndex.find(lowerStr(at.tag)); bi != bodyIndex.end()) attachBone = bi->second;
        addModel(at.model, ap, attachBone, p);
    }
    return models;
}

// World-space bones of an entity's body and attachments (shared skeleton by bone name; tags place roots).
std::unordered_map<std::string, World::BoneXform> World::entityBones(Entity& e) {
    std::unordered_map<std::string, BoneXform> bones;
    const Value o = e.obj->get("origin"), a = e.obj->get("angles");
    if (!o.isVector()) return bones;
    const Vec3 origin = o.asVector();
    const float yaw = a.isVector() ? a.asVector()[1] : 0.0f;
    const assets::Quat er = yawQuat(yaw);
    for (const HitBoxModel& m : entityModels(e))
        for (size_t i = 0; i < m.parts->bones.size(); ++i)
            bones.emplace(lowerStr(m.parts->bones[i].name),
                          BoneXform{add(origin, assets::rotate(er, m.position[i])), assets::multiply(er, m.rotation[i])});
    return bones;
}

Vec3 World::eyePosition(Entity& e) {
    // The player's eye is 0x4e8b70: ps origin + ps view height (+0xd4, the crouch/prone height from pmove) moved by
    // the lean (0x440380), or the turret's tag_player while on one. The player has no anim_pose, so the pose table
    // below would always give a standing eye and a crouched or prone player could never hide behind low cover.
    if (player_ && &e == player_ && e.obj->get("origin").isVector()) return playerEye();
    const Vec3 o = e.obj->get("origin").isVector() ? e.obj->get("origin").asVector() : Vec3{0, 0, 0};
    // Cheap approximation by pose (posing the skeleton for every sight check is too slow for 200+ AI).
    const Value pose = e.obj->get("anim_pose");
    const std::string p = pose.isString() ? pose.asString() : "stand";
    return add(o, {0, 0, p == "prone" ? 12.0f : p == "crouch" ? 40.0f : 60.0f});
}

bool World::canSee(Entity& from, Entity& to) {
    const Vec3 a = eyePosition(from), b = eyePosition(to);
    const TraceResult tr = collision_->trace(a, b, {0, 0, 0}, {0, 0, 0}, Contents::Solid);
    return tr.fraction >= 0.999f;
}

bool World::isAlive(const Entity& e) const {
    const Value h = e.obj->get("health");
    return !e.obj->removed && !e.isSpawner && h.isNumber() && h.asInt() > 0 && (e.isActor || e.isPlayer);
}

// G_Damage (0x528050) acts only on entities that take damage (ent+0x111): living actors and the player here, and
// script_brushmodel / script_model / script_origin entities after setcandamage(true) (0x533eb0).
bool World::takesDamage(const Entity& e) const {
    if (e.isActor || e.isPlayer) return isAlive(e);
    return !e.obj->removed && !e.isSpawner && e.obj->get("$takedamage").truthy();
}

// Applies damage; notifies "damage" (amount, attacker, direction, point, type) and handles death.
// G_Damage (0x5280b8, 0x5280c9) replaces a null inflictor / attacker by the world entity 0x1c13b08 (g_entities[2046]);
// the script callbacks receive that entity (radiusdamage and dodamage pass none), so reading attacker.origin works.
// TODO(spec): the world entity's other fields (zero here, origin 0,0,0).
Value World::attackerValue(Entity* attacker) {
    if (attacker) return Value(attacker->obj);
    if (!worldEntityObj_) {
        worldEntityObj_ = vm_.newObject("worldspawn");
        worldEntityObj_->set("origin", Value(Vec3{0, 0, 0}));
        worldEntityObj_->set("angles", Value(Vec3{0, 0, 0}));
    }
    return Value(worldEntityObj_);
}

// Hit-location damage multiplier (0x528100..0x528143). The attacker's weapon table (weapon +0x5b0 + 4·location, the keys
// locNone, locHelmet, ... of the weapon file) applies when the weapon is a bullet weapon (weaponType +0x78 == 0);
// otherwise, and with no weapon, the default table loaded from info/ai_lochit_dmgtable (0x527b30..0x527ba1:
// "LOCDMGTABLE\name\value\..." for the 19 locations, initialised to 1.0 at 0x527a08). A key a weapon file leaves out
// is taken as 1.0 (TODO(spec): the descriptor default).
float World::hitLocationMultiplier(const Entity* attacker, const std::string& location) {
    static const char* const kLocKeys[19] = {"none", "helmet", "head", "neck", "torso_upper", "torso_lower", "right_arm_upper",
                                             "left_arm_upper", "right_arm_lower", "left_arm_lower", "right_hand", "left_hand",
                                             "right_leg_upper", "left_leg_upper", "right_leg_lower", "left_leg_lower",
                                             "right_foot", "left_foot", "gun"};
    int idx = 0;
    for (int i = 0; i < 19; ++i)
        if (location == kLocKeys[i]) idx = i;
    if (attacker) {
        const std::string w = lowerStr(attacker->obj->get("$weapon").toString());
        const auto* def = w.empty() || w == "none" ? nullptr : weapon(w);
        if (def) {
            auto type = def->find("weapontype");
            if (type == def->end() || type->second.empty() || lowerStr(type->second) == "bullet") {
                std::string key = "loc";
                for (const char c : std::string(kLocKeys[idx]))
                    if (c != '_') key += c;
                auto it = def->find(key);
                return it != def->end() ? float(std::atof(it->second.c_str())) : 1.0f;
            }
        }
    }
    if (!locDmgTableLoaded_) {
        locDmgTableLoaded_ = true;
        locDmgTable_.assign(19, 1.0f);
        if (auto d = vfs_.read("info/ai_lochit_dmgtable")) {
            const std::string text(d->begin(), d->end());
            std::vector<std::string> tok;
            size_t p = 0;
            while (p <= text.size()) {
                const size_t q = text.find('\\', p);
                tok.push_back(text.substr(p, q == std::string::npos ? std::string::npos : q - p));
                if (q == std::string::npos) break;
                p = q + 1;
            }
            if (!tok.empty() && tok[0] == "LOCDMGTABLE")
                for (size_t i = 1; i + 1 < tok.size(); i += 2)
                    for (int k = 0; k < 19; ++k)
                        if (tok[i] == kLocKeys[k]) locDmgTable_[size_t(k)] = float(std::atof(tok[i + 1].c_str()));
        }
    }
    return locDmgTable_[size_t(idx)];
}

void World::damage(Entity& target, Entity* attacker, int amount, const Vec3& dir, const Vec3& point, const std::string& mod,
                   const std::string& location, int dflags) {
    if (!takesDamage(target)) return;
    // 0x528050: an entity with vehicle info (ent+0x108) first asks the vehicle filter (0x5122f0) and leaves when it ignores
    // the means of death. The info is attached when a script_vehicle is initialised (0x50da80).
    if (target.vehicle && target.vehicle->info && vehicleIgnoresDamage(*target.vehicle->info, modNumber(mod), dflags)) return;
    // Entities without a client or actor (0x528050 with ent+0xfc and ent+0x100 zero): "damage" (0x1c7f058: amount,
    // attacker, direction, point, means of death; 0x527fd0) whenever the amount is not 0, health (ent+0x13c) minus the
    // amount, kept above −1000 (0x528466); below 1 "death" (0x1c7f05c) with the attacker and the means-of-death name
    // (0x5a95ac). Script models (handler 0xd) and brush models (0xc) have no pain or die callback (0x5a5744 /
    // 0x5a5748), so they stay damageable. Spawn health: "health" is an int spawn key stored at ent+0x13c (field tables 0x16f518 / 0x170868), the
    // entity memory is zeroed otherwise, so the key or 0 is exact. TODO(spec): the team kill counter (0x82552c) for actor deaths.
    if (!target.isActor && !target.isPlayer) {
        if (amount == 0) return;
        const Value hv = target.obj->get("health");
        int health = (hv.isNumber() ? hv.asInt() : 0) - amount;
        if (health < -999) health = -999;
        target.obj->set("health", Value(int32_t(health)));
        vm_.notify(target.obj, "damage", {Value(int32_t(amount)), attackerValue(attacker), Value(dir), Value(point), Value(mod)});
        if (health < 1) vm_.notify(target.obj, "death", {attackerValue(attacker), Value(mod)});
        if (target.classname == "trigger_damage") triggerDamageHit(target, attacker, amount, mod); // entity type 8 pain / die
        return;
    }
    // 0x528100..0x528143: for a sentient (ent+0x100) the damage is scaled by the hit-location multiplier (attacker weapon
    // table +0x5b0, else the default table 0x1c91328) and truncated; a result <= 0 leaves G_Damage (0x52857f) before any
    // state changes, so no "damage" notify and no pain (a radius-damage edge of 0 gave animscripts/pain.gsc:101 a
    // divide by zero in health / damageTaken). The multiplier tables: hitLocationMultiplier; the product is truncated (_ftol).
    amount = int(float(amount) * hitLocationMultiplier(attacker, location));
    if (amount <= 0) return;
    // FL_GODMODE (entity flags +0x128 bit 1, toggled by the "god" cheat 0x4e9f90): G_Damage skips the damage
    // (0x5282b3). TODO(spec): the knockback computed before that check still applies in the exe.
    if (target.obj->get("$godmode").truthy()) return;
    // Debug: IW2_DMG_DEBUG=1 prints every damage credited to the player (friendly-fire mission fails).
    if (attacker && attacker->isPlayer && std::getenv("IW2_DMG_DEBUG"))
        print("DBG player damage " + std::to_string(amount) + " to " + target.classname + " (" + mod + ") at " + std::to_string(timeMs_));
    // G_Damage (0x52835c): a client accumulates the damage; with a direction the direction is kept, else the
    // target's origin and the world flag (P_DamageFeedback turns that into 255/255).
    if (target.isPlayer && std::getenv("IW2_PLAYER_DMG_LOG"))
        std::fprintf(stderr, "[pdmg %lld] %d from #%d %s (%s)\n", (long long)timeMs_, amount, attacker ? attacker->number : -1, attacker ? attacker->classname.c_str() : "-", mod.c_str());
    if (target.isPlayer) {
        damageBlood_ += amount;
        if (dir[0] != 0 || dir[1] != 0 || dir[2] != 0) {
            damageFrom_ = dir;
            damageFromWorld_ = false;
        } else {
            damageFrom_ = target.obj->get("origin").asVector();
            damageFromWorld_ = true;
        }
    }
    // G_Damage (0x5284d3): the target's sentient remembers its attacker (sentient +0x28); the AI events 5/6
    // reveal it to the actors around.
    if (attacker) target.lastAttacker = attacker;
    // FL_DEMI_GODMODE (flags & 2, toggled by the "demigod" cheat 0x4ea030): a hit that would take health to 0 or below is cut to health - 1
    // (0x5283d2..0x5283ec, after the blood accumulation; the "damage" notify carries the cut amount).
    if (target.obj->get("$demigod").truthy()) {
        const int h0 = target.obj->get("health").asInt();
        if (h0 - amount <= 0) amount = h0 - 1;
    }
    const int health = target.obj->get("health").asInt() - amount;
    target.obj->set("health", Value(int32_t(health)));
    target.obj->set("damagetaken", Value(int32_t(amount)));
    target.obj->set("damagedir", Value(dir));
    target.obj->set("damageyaw", Value(float(std::atan2(-dir[1], -dir[0]) * 180.0 / M_PI)));
    target.obj->set("damagelocation", Value(location));
    // damageweapon is the attacker entity's weapon (pain/die callbacks 0x4ff3b0 / 0x4ff530: entity state weapon
    // index → weapon name). Without an attacker G_Damage (0x528050) credits the world entity, whose weapon is index 0,
    // "none" (0x4dc210). A vehicle's entity weapon is its vehicle file's turretWeapon (set at spawn, 0x50d8a3;
    // index 0 "none" without one).
    if (attacker && attacker->vehicle) {
        const std::string tw = attacker->vehicle->info ? attacker->vehicle->info->turretWeapon : std::string();
        target.obj->set("damageweapon", Value(tw.empty() ? std::string("none") : lowerStr(tw)));
    } else {
        const Value w = attacker ? attacker->obj->get(attacker->isPlayer ? "$weapon" : "weapon") : Value();
        target.obj->set("damageweapon", w.isString() && !w.asString().empty() ? w : Value("none"));
    }
    vm_.notify(target.obj, "damage", {Value(int32_t(amount)), attackerValue(attacker), Value(dir), Value(point), Value(mod)});
    if (health > 0) {
        if (target.isActor) actorPain(target, attacker); // pain callback 0x4ff3b0
        return;
    }
    if (target.isActor) {
        ++stats.kills;
        // G_Damage notifies "death" (attacker, means-of-death name 0x5a95ac; 0x528474), then the die callback
        // (0x4ff530) sets the death state.
        vm_.notify(target.obj, "death", {attackerValue(attacker), Value(mod)});
        actorDie(target, attacker);
    } else if (target.isPlayer) {
        vm_.notify(target.obj, "death", {attackerValue(attacker)});
        print("player killed at " + std::to_string(timeMs_) + " ms by " +
              (attacker ? attacker->classname + " " + attacker->obj->get("targetname").toString() : std::string("world")) + " (" + mod + ")");
        // player_die (0x527d80) sets the client's respawn time to level time + 1700 ms (0x527e85); the client think
        // opens the death screen once the level time passes it (0x4e7eb7, World::runDeathScreen).
        if (playerRespawnMs_ < 0) playerRespawnMs_ = timeMs_ + 1700;
        // player_die (0x527d80): TossClientItems (0x527bd0, unless the origin is in contents 0x80000000): the current
        // weapon (the next one while dropping, ps+0x85cc) is dropped as its weapon_ item (0x521f70) when owned, not
        // "none", with stock ammo of its ammoName (ps+0x118) and not on a turret. The entity's weapon becomes 0
        // (ent+2), so the client draws no view weapon. LookAtKiller (0x527cd0): the dead yaw ps+0x10c faces the
        // inflictor, else the attacker. TODO(spec): the toss velocity built from the view angles, the no-killer yaw.
        Inventory& inv = inventory(target);
        const std::string w = lowerStr(target.obj->get("$weapon").toString());
        const Vec3 po = target.obj->get("origin").asVector();
        const bool noDrop = collision_->trace(po, po, {0, 0, 0}, {0, 0, 0}, 0x80000000u).startSolid; // 0x44c840
        if (!noDrop && !w.empty() && w != "none" && inv.owned.count(w) && inv.ammo[weaponField(w, "ammoName")] != 0 && !playerOnTurret()) {
            Entity& item = spawnEntity("weapon_" + w);
            item.obj->set("origin", target.obj->get("origin"));
            item.obj->set("angles", target.obj->get("angles"));
            const std::string wm = weaponField(w, "worldModel");
            if (!wm.empty()) item.obj->set("model", Value(wm));
            launchItem(item, target); // 0x527bd0 → 0x521f70
        }
        playerDeadWeaponless_ = true;
        if (attacker && attacker != &target) {
            const Vec3 d = sub(attacker->obj->get("origin").asVector(), target.obj->get("origin").asVector());
            playerDeadYaw_ = std::atan2(d[1], d[0]) * 180.0f / float(M_PI);
        }
    }
}

// G_LocationalTrace: G_Trace over the world and the entity clips, except that with a priority map the entities
// with hit boxes (actors, ent+0xa2 & 2) are traced against their bones (0x44b840 → FUN_00473d20) after a pre-test
// against origin + (−64, −64, −32)…(64, 64, 72). Start and end go into the entity's space (yaw, as entityBones).
TraceResult World::locationalTrace(const Vec3& start, const Vec3& end, uint32_t mask, const Entity* pass, const uint8_t* priority,
                                   int& location) {
    location = 0;
    const int passNum = pass ? pass->number : -1;
    TraceResult tr = collision_->trace(start, end, {0, 0, 0}, {0, 0, 0}, mask, passNum, priority != nullptr);
    if (tr.fraction == 0) return tr;
    // Script models (ent+0xa2 & 4, set at spawn 0x533480; contents 0x2080, 0 after notsolid 0x534050) are traced
    // against their xmodel collision surfaces in a locational trace whether or not there is a priority map
    // (0x44b840 → FUN_00474440); the hit names the entity with its contents (ent+0xc0) and the surface's surface
    // flags. TODO(spec): FUN_004746b0 (the DObj check), the DObj bounds pre-test (FUN_00473800; OpenIW2 culls with a
    // sphere around the xmodel bounds), and the box (r.mins / r.maxs) used by traces that are not locational.
    for (const auto& ep : entities_) {
        Entity& e = *ep;
        if (e.obj->removed || e.classname != "script_model" || e.number == passNum) continue;
        if (!(mask & 0x2080u) || e.obj->get("$notsolid").truthy()) continue;
        const Value mv = e.obj->get("model"), ov = e.obj->get("origin");
        if (!mv.isString() || !ov.isVector()) continue;
        if (!skeleton(mv.asString())) continue;
        auto cs = collSurfs_.find(mv.asString());
        if (cs == collSurfs_.end() || cs->second.empty()) continue;
        const Vec3 o = ov.asVector();
        auto bounds = modelBounds_.find(mv.asString());
        if (bounds != modelBounds_.end()) {
            float r2 = 0;
            for (size_t i = 0; i < 3; ++i) {
                const float m = std::max(std::fabs(bounds->second.first[i]), std::fabs(bounds->second.second[i]));
                r2 += m * m;
            }
            // Closest approach of the segment (up to the current fraction) to the origin.
            const Vec3 d = scale(sub(end, start), tr.fraction);
            const float dd = dot(d, d);
            const float t = dd > 0 ? std::clamp(dot(sub(o, start), d) / dd, 0.0f, 1.0f) : 0.0f;
            const Vec3 c = sub(add(start, scale(d, t)), o);
            if (dot(c, c) > r2) continue;
        }
        const Value a = e.obj->get("angles");
        const Vec3 ang = a.isVector() ? a.asVector() : Vec3{0, 0, 0};
        const assets::Quat er = assets::anglesQuat(ang[0], ang[1], ang[2]);
        const assets::Quat inv = {-er.x, -er.y, -er.z, er.w};
        const Vec3 ls = assets::rotate(inv, sub(start, o)), le = assets::rotate(inv, sub(end, o));
        const CollSurfResult h = traceCollSurfs(entityModels(e), ls, le, mask, tr.fraction);
        if (!h.hit || !(h.fraction < tr.fraction)) continue;
        tr.fraction = h.fraction;
        tr.normal = assets::rotate(er, h.normal);
        tr.surfaceFlags = uint32_t(h.surfaceFlags);
        tr.material = -1;
        tr.contents = 0x2080u;
        tr.entity = e.number;
        tr.startSolid = tr.allSolid = false;
        location = 0;
    }
    if (!priority) {
        for (size_t i = 0; i < 3; ++i) tr.endPos[i] = start[i] + (end[i] - start[i]) * tr.fraction;
        return tr;
    }
    const int passOwner = collision_->ownerOf(passNum);
    for (const EntityClip& c : collision_->entityClips()) {
        if (!c.hitBoxes || CollisionWorld::skipClip(c, mask, passNum, passOwner)) continue;
        Entity* e = entityByNumber(c.id);
        if (!e) continue;
        // Segment against the pre-test box up to the current fraction (FUN_00419450).
        float enter = 0, leave = tr.fraction;
        bool miss = false;
        for (size_t i = 0; i < 3 && !miss; ++i) {
            const float lo = c.origin[i] + (i == 2 ? -32.0f : -64.0f), hi = c.origin[i] + (i == 2 ? 72.0f : 64.0f);
            const float d = end[i] - start[i];
            if (d == 0) {
                miss = start[i] < lo || start[i] > hi;
                continue;
            }
            float t0 = (lo - start[i]) / d, t1 = (hi - start[i]) / d;
            if (t0 > t1) std::swap(t0, t1);
            enter = std::max(enter, t0);
            leave = std::min(leave, t1);
            miss = enter > leave;
        }
        if (miss) continue;
        const Value a = e->obj->get("angles");
        const assets::Quat er = yawQuat(a.isVector() ? a.asVector()[1] : 0.0f);
        const assets::Quat inv = {-er.x, -er.y, -er.z, er.w};
        const Vec3 ls = assets::rotate(inv, sub(start, c.origin)), le = assets::rotate(inv, sub(end, c.origin));
        const HitBoxResult h = traceHitBoxes(entityModels(*e), ls, le, priority, tr.fraction);
        if (!h.hit || !(h.fraction < tr.fraction)) continue;
        tr.fraction = h.fraction;
        tr.normal = assets::rotate(er, h.normal);
        tr.surfaceFlags = 0;
        tr.material = -1;
        tr.contents = c.contents;
        tr.entity = c.id;
        tr.startSolid = tr.allSolid = false;
        location = h.location;
    }
    for (size_t i = 0; i < 3; ++i) tr.endPos[i] = start[i] + (end[i] - start[i]) * tr.fraction;
    return tr;
}

// Fires a weapon bullet from `start` towards `end`: world geometry blocks, the first sentient hit takes damage.
// `damageAmount` is the fallback when the weapon (weaponName, else the shooter's own) has no definition.
Entity* World::fireBullet(Entity& shooter, const Vec3& start, const Vec3& end, int damageAmount, Entity* attacker,
                          const std::string& weaponName, int depth) {
    // More than 12 passes: the exe prints an error and fires nothing (0x531343 / 0x531356).
    if (depth > 12) return nullptr;
    // Bullet_Fire_Extended (FUN_00531340): mask 0x280e831; the rifleBullet priority map puts the head first.
    const Value wn = shooter.obj->get(shooter.isPlayer ? "$weapon" : "weapon");
    const auto* def = wn.isString() ? weapon(wn.asString()) : nullptr;
    bool rifle = false;
    if (def) {
        auto it = def->find("riflebullet");
        rifle = it != def->end() && std::atoi(it->second.c_str()) != 0;
    }
    // An AI's bullet that hits a sentient of its own team (an actor or the player: hit entity +0x104 sentient,
    // team at sentient +4) goes on from the hit point ignoring it, up to 12 times ("Ignoring AI shooting teammate"). TODO(spec): penetration (surface flag 0x10), vehicles, the
    // half-damage refire of rifleBullet hits on sentients.
    int location = 0;
    TraceResult tr;
    Entity* hit = nullptr;
    Vec3 from = start;
    const Entity* pass = &shooter;
    for (int depth = 0; depth <= 12; ++depth) {
        tr = locationalTrace(from, end, Contents::BulletMask, pass, rifle ? kHitPriorityRifle : kHitPriorityDefault, location);
        hit = tr.entity >= 0 ? entityByNumber(tr.entity) : nullptr;
        if (hit && shooter.isActor && isSentient(*hit) && hit->obj->get("team").toString() == shooter.obj->get("team").toString()) {
            from = tr.endPos;
            pass = hit;
            hit = nullptr;
            continue;
        }
        break;
    }
    // 0x531443…0x531489: the shot is told to the AI around — a "bullet fired" point event at the muzzle (type 10,
    // 2048²) and the suppression line muzzle → impact (type 16, 128², 0x535860 → 0x54b3b0 adds a danger plane) —
    // both with the broadcast gate forced, when the shooter is a sentient.
    if (attacker && (attacker->isActor || attacker->isPlayer)) {
        aiEventPoint(*attacker, 10, start);
        aiEventLine(*attacker, 16, start, tr.endPos);
    }
    // 0x5314ac: the trigger_damage brushes the shot crosses (start -> impact) are hit with the weapon's damage.
    triggerDamageSegment(attacker ? attacker : &shooter, start, tr.endPos, damageAmount, rifle ? 2 : 1);
    bulletImpact(shooter, def, start, end, tr, hit);
    // Penetration (0x531665): a hit on contents 0x10 goes on from just behind the hit point towards the same end with the
    // next depth and applies no damage here: start = hit + dir · t, t = 0.25 / cosθ (cosθ = −dir·normal) when cosθ >= 0.125,
    // else 0 (0x5316be…0x531729). The impact event above is still sent.
    if (tr.fraction < 1.0f && (tr.contents & 0x10u) && !tr.startSolid) {
        Vec3 d = sub(end, start);
        const float dl = len(d);
        if (dl > 0) {
            d = scale(d, 1.0f / dl);
            const float cosT = -(d[0] * tr.normal[0] + d[1] * tr.normal[1] + d[2] * tr.normal[2]);
            const float t = cosT >= 0.125f ? 0.25f / cosT : 0.0f;
            ++stats.bulletPenetrations;
            return fireBullet(shooter, add(Vec3{tr.endPos[0], tr.endPos[1], tr.endPos[2]}, scale(d, t)), end, damageAmount, attacker,
                              weaponName, depth + 1);
        }
    }
    if (hit && !takesDamage(*hit)) hit = nullptr;
    ++stats.shots;
    if (def) {
        // Weapon sound: fireSoundPlayer without position for the player, fireSound at the shooter otherwise.
        auto it = def->find(shooter.isPlayer ? "firesoundplayer" : "firesound");
        const std::string* snd = it != def->end() && !it->second.empty() ? &it->second : nullptr;
        // The last shot of a clip (event 0xa0, 0x4b7800) overrides it with lastShotSoundPlayer (player) or lastShotSound.
        if (shooter.isPlayer && playerLastShot_) {
            auto ls = def->find("lastshotsoundplayer");
            if (ls == def->end() || ls->second.empty()) ls = def->find("lastshotsound");
            if (ls != def->end() && !ls->second.empty()) snd = &ls->second;
        }
        if (snd) {
            float seconds = 0;
            playSound(*snd, shooter.isPlayer ? nullptr : &start, &seconds);
            // EV_FIRE_WEAPON (0x4b7800): for an actor (entity type 9, 0x4b79ac) whose fire alias resolved (0x4b792a), 0x4c0e20 pings
            // the compass with the length of the started sound (0x4a35c0 -> 0x4423d0, ms) at the sound position (0x4b79b7).
            // TODO(spec): actor projectile weapons send the same event; no actor projectile fire path exists here yet.
            if (!shooter.isPlayer && shooter.obj->kind == "actor") compassPing(shooter, start, int(std::max(0.0f, seconds) * 1000.0f));
        }
    }
    if (hit) {
        ++stats.hits;
        const Vec3 dir = scale(sub(end, start), 1.0f / std::max(1.0f, len(sub(end, start))));
        // Damage by weapon, target and distance start → impact (0x531090, called at 0x531787).
        const auto* dmgDef = weaponName.empty() ? def : weapon(weaponName);
        if (dmgDef) damageAmount = bulletDamageAtDistance(*dmgDef, hit->isPlayer, len(sub(tr.endPos, start)));
        // Bullet_Fire_Extended (0x531379..0x5313a3): rifleBullet (weapon +0x31c) gives MOD_RIFLE_BULLET and dflags 0x40, else
        // MOD_PISTOL_BULLET and 0; armorPiercing (+0x320) adds dflags 2.
        bool ap = false;
        if (def) {
            auto apIt = def->find("armorpiercing");
            ap = apIt != def->end() && std::atoi(apIt->second.c_str()) != 0;
        }
        damage(*hit, attacker, damageAmount, dir, tr.endPos, rifle ? "MOD_RIFLE_BULLET" : "MOD_PISTOL_BULLET", hitLocationName(location),
               (rifle ? 0x40 : 0) | (ap ? 2 : 0));
    }
    return hit;
}

// The impact event of Bullet_Fire_Extended (0x531340, 0x531556…0x531661): sent when the trace hit something
// (fraction < 1, 0x591e20) and the surface has no surface flag 4, except when an AI hits a sentient of its own team.
// Surface type = surface flags >> 20 & 0x1f, flesh (7) for a sentient hit with type 0. On a client (the player):
// event 0xbb, 0xbc with rifleBullet (weapon +0x31c); the client (0x4b82f0) plays bullet_small_<surface> /
// bullet_large_<surface> (sound tables 0xf5b85c / 0xf5b8b8, "%s_%s" by 0x4a27b0) at the point. Otherwise event 0xb8,
// 0xb9 with rifleBullet, 0xba with armorPiercing (+0x320; 0x531310) with the trace normal (+4) and the bullet
// direction reflected off it (+3, d − 2(d·n)n of the normalized direction); the client (0x4b81d0) plays the sound
// of bullet_small / bullet_large / bullet_ap (0xf5b914), the impact table's <type>_normal effect along the normal
// and <type>_reflect along the reflection (bullet_small 0/1, bullet_large 2/3, bullet_ap 6/7; FUN_00485220). On flesh
// with cg_blood (0x1040c58, default 1) the table effects play at most once every 330 ms (0x14a, 0xf7e8c0), the
// other hits play fx/impacts/flesh_hit_noblood.efx (0xf5bcc4) along the normal and no reflect effect.
// TODO(spec): the dir-byte quantization (0x420c30), the tracer (0x4b8140, cg_tracerChance), bullet penetration
// (surface flag 0x10, 0x531665), the hit entity of the event (0x531646).
void World::bulletImpact(Entity& shooter, const std::map<std::string, std::string>* def, const Vec3& start,
                         const Vec3& end, const TraceResult& tr, Entity* hit) {
    if (!(tr.fraction < 1.0f) || (tr.surfaceFlags & 4)) return;
    if (hit && isSentient(*hit) && shooter.isActor && hit->obj->get("team").toString() == shooter.obj->get("team").toString()) return;
    int surface = int((tr.surfaceFlags >> 20) & 0x1f);
    if (hit && isSentient(*hit) && surface == 0) surface = 7;
    static const char* const kSurfaceAlias[] = {"default", "bark", "brick", "carpet", "cloth", "concrete", "dirt", "flesh",
                                                "foliage", "glass", "grass", "gravel", "ice", "metal", "mud", "paper",
                                                "plaster", "rock", "sand", "snow", "water", "wood", "asphalt"};
    if (surface >= 23) return;
    auto flag = [&](const char* key) {
        if (!def) return false;
        auto it = def->find(key);
        return it != def->end() && std::atoi(it->second.c_str()) != 0;
    };
    const bool rifle = flag("riflebullet"), ap = flag("armorpiercing");
    const Vec3 at = tr.endPos;
    if (!shooter.isPlayer) {
        // 0x4b8140 / 0x4b8040: the start is the weapon's tag_flash (0x4abb20), else the entity origin raised by 60 (stand; 40
        // crouched and 11 prone by eFlags 0x4 / 0x8, not tracked here). The player's own shots start at the view origin
        // (the whiz below then always rejects them: closest approach < 64).
        // TODO(spec): the stance heights.
        Vec3 from{};
        if (!entityTagPosition(shooter.number, "tag_flash", from)) {
            from = shooter.obj->get("origin").isVector() ? shooter.obj->get("origin").asVector() : Vec3{0, 0, 0};
            from[2] += 60.0f;
        }
        // No tracer for eType 1 (players, above) nor eType 0xc (live vehicles: the vehicle spawn 0x50d863 stores 0xc, the
        // dead vehicle is 0xb); actors are 9 and draw tracers (after the cg_tracerchance roll in the viewer).
        if (!shooter.vehicle) tracerEvents_.push_back({from, at});
        // 0x4b79f0 (always called from 0x4b8140, also for eType 1 / 0xc): the "whizby" alias (0xf5bc64) plays, with entity
        // number 0x7fe, 16 units back along the path from the closest point to the view origin when that point is at
        // least 64 units from both the start and the end of the path and within 140 units of the view.
        // TODO(spec): the cg_tracerchance > 0 gate (cvar not modelled; default 0.4) and the 0x4b8040 range check.
        if (player_) {
            const Vec3 view = eyePosition(*player_);
            const Vec3 dv = sub(at, from);
            const float total = len(dv);
            if (total > 0.0f) {
                const Vec3 dir = scale(dv, 1.0f / total);
                const float t = dot(sub(view, from), dir);
                if (t >= 64.0f && t + 64.0f <= total) {
                    const Vec3 q = add(from, scale(dir, t));
                    if (len(sub(q, view)) <= 140.0f) {
                        const Vec3 at16 = sub(q, scale(dir, 16.0f));
                        startSound("whizby", &at16);
                    }
                }
            }
        }
    }
    if (hit && hit->isPlayer) {
        startSound(std::string(rifle ? "bullet_large_" : "bullet_small_") + kSurfaceAlias[surface], &at);
        return;
    }
    const int type = ap ? 6 : rifle ? 2 : 0;
    startSound(std::string(ap ? "bullet_ap_" : rifle ? "bullet_large_" : "bullet_small_") + kSurfaceAlias[surface], &at);
    const Vec3 n{tr.normal[0], tr.normal[1], tr.normal[2]};
    Vec3 d = sub(end, start);
    const float l = len(d);
    if (l > 0) d = scale(d, 1.0f / l);
    const Vec3 reflect = add(d, scale(n, -2.0f * dot(d, n)));
    std::string normalFx = impactEffect(type, surface), reflectFx = impactEffect(type + 1, surface);
    if (surface == 7) {
        if (timeMs_ - lastBloodMs_ < 330) {
            normalFx = "fx/impacts/flesh_hit_noblood.efx";
            reflectFx.clear();
        } else {
            lastBloodMs_ = timeMs_;
        }
    }
    if (!normalFx.empty()) fxEvents_.push_back({normalFx, at, n});
    if (!reflectFx.empty()) fxEvents_.push_back({reflectFx, at, reflect});
    ++stats.bulletImpacts;
}

const AccuracyGraph* World::accuracyGraph(const std::string& weaponName, bool targetClient) {
    const auto* def = weapon(weaponName);
    if (!def) return nullptr;
    auto f = def->find(targetClient ? "aivsplayeraccuracygraph" : "aivsaiaccuracygraph");
    if (f == def->end() || f->second.empty()) return nullptr;
    const std::string path = std::string("accuracy/") + (targetClient ? "aivsplayer/" : "aivsai/") + f->second;
    auto it = accuracyGraphs_.find(path);
    if (it == accuracyGraphs_.end()) {
        std::optional<AccuracyGraph> g;
        if (auto d = vfs_.read(path)) {
            AccuracyGraph parsed;
            if (parseAccuracyGraph(std::string(d->begin(), d->end()), parsed)) g = std::move(parsed);
        }
        it = accuracyGraphs_.emplace(path, std::move(g)).first;
    }
    return it->second ? &*it->second : nullptr;
}

int World::weaponDamage(const std::string& weaponName) {
    const auto* w = weapon(weaponName);
    if (!w) return 35;
    auto it = w->find("damage");
    return it == w->end() ? 35 : std::max(1, std::atoi(it->second.c_str()));
}

Vec3 World::playerFire(const Vec3& eye, const Vec3& forward, bool lastShot, float spreadDeg, const Vec3& gunOffset) {
    if (!player_ || !isAlive(*player_)) return {0, 0, 0};
    playerLastShot_ = lastShot;
    const Value w = player_->obj->get("$weapon");
    // FireWeapon (0x531c40) dispatches on weaponType (+0x78): 0 bullet (0x5312b0), 1 grenade (0x531990), 2 projectile
    // (0x531a40 -> G_FireRocket 0x527810). The muzzle point is the eye (0x4e8b70) plus the usercmd gun offset (0x531bff,
    // client +0x864c..0x8654), the axes come from AnglesToAxis of the gun angles (0x531b90): forward, left, up.
    // TODO(spec): the gun angles and offset equal the view angles and zero except while an aimDownSight weapon is between
    // hip and full aim (0x4b6030 / 0x4b60e0 take the view model's sway, bob and kick state, 0x4b4dc0 / 0x4b5d10 etc.).
    const Vec3 origin = add(eye, gunOffset);
    const float pitch = -std::asin(std::clamp(forward[2], -1.0f, 1.0f)) * 180.0f / float(M_PI);
    const float yaw = std::atan2(forward[1], forward[0]) * 180.0f / float(M_PI);
    const Axis axes = anglesAxis({pitch, yaw, 0});
    // 0x531170: the angle uniform over 360 degrees (rand * 0.010986328125), the radius rand / 32768 (0x5a56f4 sequence,
    // angle first), as cos, sin scaled by the radius. Both the bullet (0x531210, tan(spread) * 8192) and the projectile
    // (0x531a40, tan(spread) on the unit forward, normalised afterwards) start from the same disc.
    const float ang = float(nextRand()) * 0.010986328125f * 0.01745329238474369f;
    const float rad = float(nextRand()) * 3.0517578125e-05f;
    const float dx = std::cos(ang) * rad, dy = std::sin(ang) * rad;
    const float t = std::tan(spreadDeg * 0.01745329238474369f);
    const auto offset = [&](float k) { return add(scale(axes[1], dx * t * k), scale(axes[2], dy * t * k)); };
    if (w.isString() && weaponField(w.asString(), "weaponType") == "projectile") {
        Vec3 dir = add(axes[0], offset(1.0f));
        const float l = len(dir);
        if (l > 0) dir = scale(dir, 1.0f / l);
        spawnRocket(*player_, w.asString(), origin, dir);
        playerLastShot_ = false;
        // 0x531b1b: the shooter's velocity loses the unperturbed forward * 64 (0x56f6e0); the caller owns the velocity.
        return scale(axes[0], 64.0f);
    }
    const int dmg = weaponDamage(w.isString() ? w.asString() : "kar98k");
    fireBullet(*player_, origin, add(add(origin, scale(axes[0], 8192.0f)), offset(8192.0f)), dmg, player_);
    playerLastShot_ = false;
    return {0, 0, 0};
}

void World::registerCombatBuiltins() {
    vm_.registerBuiltin("gettagorigin", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e) throw std::runtime_error("gettagorigin on a non-entity");
        const auto bones = entityBones(*e);
        auto it = bones.find(lowerStr(c.arg(0).toString()));
        if (it == bones.end()) throw std::runtime_error("tag " + c.arg(0).toString() + " not found");
        return Value(it->second.position);
    });
    vm_.registerBuiltin("gettagangles", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e) throw std::runtime_error("gettagangles on a non-entity");
        const auto bones = entityBones(*e);
        auto it = bones.find(lowerStr(c.arg(0).toString()));
        if (it == bones.end()) throw std::runtime_error("tag " + c.arg(0).toString() + " not found");
        const auto fwd = assets::rotate(it->second.rotation, {1, 0, 0});
        const float yaw = std::atan2(fwd[1], fwd[0]) * 180.0f / float(M_PI);
        const float pitch = -std::atan2(fwd[2], std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1])) * 180.0f / float(M_PI);
        return Value(Vec3{pitch, yaw, 0});
    });
    vm_.registerBuiltin("geteye", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e) throw std::runtime_error("geteye on a non-entity");
        // 0x4eee60: an actor's eye is its tag_eye (0x500910, actor +0x174; sightEye), the player's 0x4e8b70. TODO(spec):
        // a non-sentient entity is a script error there (string 0x5871b0); here it gets the pose-height eye.
        if (e->isActor) return Value(sightEye(*e, nullptr));
        return Value(eyePosition(*e));
    });
    // cansee(entity [, seconds]) (0x53d930) on an actor: for a sentient target the cached sight of 0x54ce10 no older
    // than the seconds × 1000 (0x591e50, truncated by _ftol2 0x565994), else 250 ms (0x53da00). TODO(spec): a non-sentient
    // target goes straight to 0x54ca60 (0x53da1f; it aims at the centre of the entity's absolute bounds, 0x54cc87),
    // here still the eye-to-eye trace.
    vm_.registerBuiltin("cansee", [this](CallContext& c) {
        Entity* a = entityOf(c.self);
        Entity* b = entityOf(c.arg(0));
        if (!a || !b) return Value(false);
        if (a->isActor && isSentient(*b)) {
            const int64_t within = c.argc() > 1 ? int64_t(c.arg(1).asFloat() * 1000.0f) : 250;
            return Value(seesWithin(*a, *b, within));
        }
        return Value(canSee(*a, *b));
    });
    // canshoot(target [, offset]) (0x53d800): from the muzzle (0x500b40, "Couldn't find tag_flash in entity %d" when the
    // model has none) plus the optional offset vector (second argument) to the target, by 0x54cf40.
    vm_.registerBuiltin("canshoot", [this](CallContext& c) {
        Entity* a = entityOf(c.self);
        if (!a || !a->isActor) throw std::runtime_error("not an actor");
        const Vec3 target = c.arg(0).asVector();
        Vec3 muzzle;
        if (!muzzlePosition(*a, muzzle)) throw std::runtime_error("Couldn't find tag_flash in entity " + std::to_string(a->number));
        if (c.argc() > 1) {
            const Vec3 off = c.arg(1).asVector();
            muzzle = {muzzle[0] + off[0], muzzle[1] + off[1], muzzle[2] + off[2]};
        }
        return Value(muzzleTraceOk(*a, muzzle, target));
    });
    vm_.registerBuiltin("shoot", [this](CallContext& c) {
        Entity* a = entityOf(c.self);
        if (!a || !isAlive(*a)) return Value();
        const float accMod = c.arg(0).isNumber() ? c.arg(0).asFloat() : 1.0f;
        const auto bones = entityBones(*a);
        Vec3 start = eyePosition(*a);
        if (auto it = bones.find("tag_flash"); it != bones.end()) start = it->second.position;
        Entity* enemy = entityOf(a->obj->get("enemy"));
        Vec3 target;
        if (c.arg(1).isVector()) target = c.arg(1).asVector();
        else if (enemy) target = enemy->isPlayer ? eyePosition(*enemy) : sightEye(*enemy, nullptr); // 0x54ff90: 0x4e8b70 / 0x500910
        else return Value();
        // Hit or miss (0x550a70 → 0x54fde0): the shot hits when graph(distance) · accuracy · accMod (clamped to 0..1)
        // beats a random number; then 0x54ff90 aims at a random point of the target's body box, else 0x550390 beside it
        // A shoot() with an explicit position fires straight at it (0x550af6). The shot
        // has spread 0 (0x550bee passes 0 to 0x5312b0). The target point of an actor is its tag_eye (actor +0x174
        // from 0x500910, origin + 64 without the tag; sightEye), the up axis the shooter's own (AngleVectors of its pitch and
        // yaw, actor +0xf8 written at 0x504a00, copied into the shot context by 0x54fee0).
        if (!c.arg(1).isVector()) {
            // Firing at the current enemy is an AI event (type 4) at the shooter's origin, heard by its own team
            // (0x550b5f: EAX 1 << the shooter's team; 0x535ba0 passes the shooter's enemy knowledge on).
            if (enemy) aiEventPoint(*a, 4, a->obj->get("origin").asVector(), 0.0f, true, AiHear::Own);
            const Value accV = a->obj->get("accuracy");
            const float actorAcc = accV.isNumber() ? accV.asFloat() : 0.5f;
            const Value wnv = a->obj->get("weapon");
            const AccuracyGraph* graph = wnv.isString() ? accuracyGraph(wnv.asString(), enemy->isPlayer) : nullptr;
            float accuracy;
            if (graph) {
                const Vec3 ao = a->obj->get("origin").asVector(), eo = enemy->obj->get("origin").asVector();
                accuracy = aiHitChance(*graph, len(sub(eo, ao)), actorAcc, accMod);
            } else {
                accuracy = std::clamp(actorAcc * accMod, 0.0f, 1.0f); // no graph: previous model
            }
            std::uniform_real_distribution<float> u(0.0f, 1.0f);
            const bool hitRoll = accuracy > u(rng_);
            if (static const bool dbg = std::getenv("IW2_ACC_DEBUG") != nullptr; dbg)
                print("ACC #" + std::to_string(a->number) + " " + a->obj->get("origin").toString() + " " + a->obj->get("weapon").toString() + " " + (enemy->isPlayer ? "player" : "ai") + " dist " +
                      std::to_string(int(len(sub(enemy->obj->get("origin").asVector(), a->obj->get("origin").asVector())))) + " chance " +
                      std::to_string(accuracy) + (hitRoll ? " hit" : " miss") + " clip " + a->obj->get("bulletsinclip").toString());
            Vec3 T = target;
            if (enemy->isPlayer) T = eyePosition(*enemy); // the player's eye (0x4e8b70)
            Vec3 v = sub(T, start);
            const float vl = len(v);
            if (vl > 0) v = scale(v, 1.0f / vl);
            const Value shooterAngles = a->obj->get("angles");
            const float pitchR = (shooterAngles.isVector() ? shooterAngles.asVector()[0] : 0.0f) * float(M_PI) / 180.0f;
            const float yawR = (shooterAngles.isVector() ? shooterAngles.asVector()[1] : 0.0f) * float(M_PI) / 180.0f;
            const Vec3f up{std::sin(pitchR) * std::cos(yawR), std::sin(pitchR) * std::sin(yawR), std::cos(pitchR)};
            const Vec3f cs = aiSideAxis({v[0], v[1], v[2]}, up);
            const Vec3f Tf{T[0], T[1], T[2]};
            const Vec3f p = hitRoll ? aiHitPoint(Tf, cs, up, enemy->isPlayer, accuracy, u(rng_), u(rng_))
                                    : aiMissPoint(Tf, {v[0], v[1], v[2]}, cs, up, enemy->isPlayer, u(rng_), u(rng_), u(rng_));
            target = {p.x, p.y, p.z};
        }
        const Vec3 dir = sub(target, start);
        const Value wpn = a->obj->get("weapon");
        fireBullet(*a, start, add(start, scale(dir, 8192.0f / std::max(1.0f, len(dir)))), weaponDamage(wpn.isString() ? wpn.asString() : ""), a);
        // The shot adds EV_FIRE_WEAPON (0x9e, 0xa0 without a bullet: 0x550c46 / 0x550c1b) to the actor; the client handler
        // 0x4b7800 plays, for an entity that is not the local client, the weapon's worldFlashEffect (0xf48e48) via
        // 0x4b6660 at the entity's tag_flash (0x4b78de). TODO(spec): the effect is bolted to the tag (follows it); the
        // effect axes come from the tag, here the shot direction.
        if (wpn.isString()) {
            const std::string flash = weaponField(wpn.asString(), "worldFlashEffect");
            if (!flash.empty() && bones.count("tag_flash")) fxEvents_.push_back({flash, start, dir, a->number, "tag_flash"});
            // The same handler ejects the brass (CG_EjectWeaponBrass 0x4b7730, cg_brass 0xf48cfc default 1): the weapon's
            // shellEjectEffect (+0x160; lastShotEjectEffect +0x164 for event 0xa0) played bolted (0x4853f0) on the
            // weapon model's tag_brass, nothing when the tag is missing (0x47eec0 < 0). TODO(spec): which of 0x9e / 0xa0
            // this shot is, so lastShotEjectEffect is never chosen here.
            const std::string brass = weaponField(wpn.asString(), "shellEjectEffect");
            // 0x4b79d1: no brass when the weapon def has boltAction (+0x328 of the table at 0x5a86c0) set.
            if (!brass.empty() && brass != "none" && std::atoi(weaponField(wpn.asString(), "boltAction").c_str()) == 0 && bones.count("tag_brass")) fxEvents_.push_back({brass, start, dir, a->number, "tag_brass"});
        }
        vm_.notify(a->obj, "weapon_fired");
        return Value();
    });
    vm_.registerBuiltin("dodamage", [this](CallContext& c) {
        Entity* t = entityOf(c.self);
        if (!t) return Value();
        const Vec3 from = c.arg(1).isVector() ? c.arg(1).asVector() : t->obj->get("origin").asVector();
        damage(*t, nullptr, c.arg(0).asInt(), sub(t->obj->get("origin").asVector(), from), from, "MOD_UNKNOWN");
        return Value();
    });
    // setaimanims(aimDown, aimStraight, aimUp, shootDown, shootStraight, shootUp) and aimatpos(pos): the engine
    // blends the three aim animations by the pitch to the target. Returns the time the turn takes (inferred 0.2 s).
    vm_.registerBuiltin("setaimanims", [](CallContext& c) {
        auto o = c.self.asObject();
        const char* keys[] = {"$aim_down", "$aim_straight", "$aim_up", "$shoot_down", "$shoot_straight", "$shoot_up"};
        for (size_t i = 0; i < 6; ++i) o->set(keys[i], c.arg(i));
        return Value();
    });
    vm_.registerBuiltin("aimatpos", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || !e->anim || !c.arg(0).isVector()) return Value(0.2f);
        const Vec3 from = eyePosition(*e), to = c.arg(0).asVector();
        const Vec3 d = sub(to, from);
        const float pitch = std::atan2(d[2], std::sqrt(d[0] * d[0] + d[1] * d[1])) * 180.0f / float(M_PI); // up positive
        const float up = std::clamp(pitch / 45.0f, 0.0f, 1.0f), down = std::clamp(-pitch / 45.0f, 0.0f, 1.0f);
        const float straight = 1.0f - up - down;
        auto setW = [&](const char* key, float w) {
            const Value a = e->obj->get(key);
            if (!std::holds_alternative<script::AnimRef>(a.v)) return;
            const int n = e->anim->tree().find(std::get<script::AnimRef>(a.v).name);
            if (n < 0) return;
            AnimState::SetArgs args;
            args.weight = w;
            args.blendTime = 0.1f; // 0x3dcccccd in all six calls of 0x53cf00
            e->anim->set(n, args);
        };
        setW("$aim_up", up);
        setW("$aim_straight", straight);
        setW("$aim_down", down);
        // 0x53cf00 sets SIX nodes: the three aim nodes (0x4795e0) and the three shoot nodes (0x478f10 + 0x4794b0) with the same
        // three weights (a weight ≤ 0.001 becomes 0), all with blend time 0.1. Without the shoot weights a shooting actor has no
        // weighted leaf under %stand_shoot and shows the bind pose (T-pose). TODO(spec): the pitch → weight scale (0x4210d0).
        auto thin = [](float w) { return w > 0.001f ? w : 0.0f; };
        setW("$shoot_up", thin(up));
        setW("$shoot_straight", thin(straight));
        setW("$shoot_down", thin(down));
        // Face the target.
        const float yaw = std::atan2(d[1], d[0]) * 180.0f / float(M_PI);
        e->obj->set("angles", Value(Vec3{0, yaw, 0}));
        return Value(0.2f);
    });
    // Stances are allowed unless scripts restricted them with allowedstances() (inferred defaults).
    vm_.registerBuiltin("isstanceallowed", [](CallContext& c) {
        const Value allowed = c.self.asObject()->get("$allowedstances");
        if (!allowed.isString()) return Value(true);
        return Value(allowed.asString().find(c.arg(0).toString()) != std::string::npos);
    });
    vm_.registerBuiltin("allowedstances", [](CallContext& c) {
        std::string s;
        for (const auto& a : c.args) s += a.toString() + " ";
        c.self.asObject()->set("$allowedstances", Value(s));
        return Value();
    });
    vm_.registerBuiltin("localtoworldcoords", [](CallContext& c) {
        const auto o = c.self.asObject();
        const Vec3 origin = o->get("origin").asVector();
        const float yaw = o->get("angles").isVector() ? o->get("angles").asVector()[1] * float(M_PI) / 180.0f : 0.0f;
        const Vec3 v = c.arg(0).isVector() ? c.arg(0).asVector() : Vec3{0, 0, 0};
        return Value(Vec3{origin[0] + v[0] * std::cos(yaw) - v[1] * std::sin(yaw), origin[1] + v[0] * std::sin(yaw) + v[1] * std::cos(yaw), origin[2] + v[2]});
    });
    vm_.registerBuiltin("getmovedelta", [this](CallContext& c) {
        if (!std::holds_alternative<script::AnimRef>(c.arg(0).v)) return Value(Vec3{0, 0, 0});
        const assets::XAnim* a = animLib_.anim(std::get<script::AnimRef>(c.arg(0).v).name);
        if (!a || a->deltaTrans.keys.empty()) return Value(Vec3{0, 0, 0});
        const float t0 = c.arg(1).isNumber() ? c.arg(1).asFloat() : 0.0f, t1 = c.arg(2).isNumber() ? c.arg(2).asFloat() : 1.0f;
        const float frames = float(std::max(1, a->numFrames - 1));
        const auto p0 = assets::sampleTrans(a->deltaTrans, t0 * frames, a->keyCount());
        const auto p1 = assets::sampleTrans(a->deltaTrans, t1 * frames, a->keyCount());
        return Value(Vec3{p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]});
    });
    vm_.registerBuiltin("maymovetopoint", [this](CallContext& c) {
        if (!c.arg(0).isVector()) return Value(false);
        const Vec3 from = c.self.asObject()->get("origin").asVector();
        const Vec3 to = c.arg(0).asVector();
        // Step height: 10 while the prone tilt byte +0x288 is set, else 18 (0x53defc). TODO(spec): the rest of 0x53dce0.
        Entity* self = entityOf(c.self);
        const float step = self && self->isActor && self->ai.prone.tilt ? 10.0f : 18.0f;
        const TraceResult tr = collision_->trace(add(from, {0, 0, step}), add(to, {0, 0, step}), {-15, -15, 0}, {15, 15, 52}, Contents::PlayerSolid | Contents::AiClip);
        return Value(tr.fraction >= 0.999f && !tr.startSolid);
    });
    // isingoal(point) (0x540c00 -> 0x5026d0): the point is within the working goal's height (|dz| <= height) and radius (2D).
    // The working goal volume (+0x75c: block +0x748 + 0x14, 0x447830 at 0x50273f) must contain the point too.
    vm_.registerBuiltin("isingoal", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (c.argc() != 1) throw std::runtime_error("isingoal: one argument");
        if (!e || !e->isActor || !c.arg(0).isVector()) return Value(true);
        Vec3 g{};
        float r = 0, h = 0;
        if (!actorGoal(*e, g, r, h)) return Value(true);
        const Vec3 p = c.arg(0).asVector();
        const float dz = p[2] - g[2];
        if (h * h < dz * dz) return Value(false);
        const float dx = g[0] - p[0], dy = g[1] - p[1];
        if (r * r < dx * dx + dy * dy) return Value(false);
        return Value(workGoalVolumeHolds(*e, p));
    });
    // Player-only builtins (0x590ab8 "entity %i is not a player" otherwise).
    auto playerSelf = [this](CallContext& c) -> Entity& {
        Entity* e = entityOf(c.self);
        if (!e || !e->isPlayer) throw std::runtime_error("entity " + std::to_string(e ? e->number : -1) + " is not a player");
        return *e;
    };
    // allowstand/crouch/prone/leanleft/leanright (0x538c90 …): a zero argument sets the pm_flags bit that disables the
    // stance (0x400000 stand, 0x800000 crouch, 0x1000000 prone, 0x2000000 lean left, 0x4000000 lean right), non-zero
    // clears it. The lean bits gate updateLean (0x50ae13); TODO(spec): the stance bits force a stance change
    // (events 0x8c..0x8e, 0x509eea / 0x50a2f3) in the front end's pmove.
    const std::pair<const char*, uint32_t> allows[] = {{"allowstand", 0x400000u}, {"allowcrouch", 0x800000u}, {"allowprone", 0x1000000u},
                                                      {"allowleanleft", 0x2000000u}, {"allowleanright", 0x4000000u}};
    for (const auto& [name, bit] : allows)
        vm_.registerBuiltin(name, [this, playerSelf, bit](CallContext& c) {
            playerSelf(c);
            if (c.arg(0).isNumber() && c.arg(0).asInt() != 0) pmFlags_ &= ~bit;
            else pmFlags_ |= bit;
            return Value();
        });
    // getstance (0x4ee330): player only; client pm_flags (ps+0xc) bit 1 -> "prone", bit 2 -> "crouch", else "stand".
    vm_.registerBuiltin("getstance", [this, playerSelf](CallContext& c) {
        playerSelf(c);
        const int s = playerCmd_.stance;
        return Value(std::string(s == 2 ? "prone" : s == 1 ? "crouch" : "stand"));
    });
    // bullettracer (0x4ee830): reads two vectors and queues a temp entity of event 0xb2 (entity type 0xb2 + 0xf = 0xc1, 0x4d7f40) at the
    // first one, carrying the second at +0x64. The client event switch (0x4cbea9: event - 9, byte table 0x4cc04c, jump table 0x4cc02c)
    // maps 0xc1 (index 0xb8, byte 7) to the default exit 0x4cc023: the event has no handler in the SP exe, so the call is a proven no-op.
    vm_.registerBuiltin("bullettracer", [](CallContext&) { return Value(); });
    // magicbullet (0x4ee880): (weapon, start, end); exactly 3 arguments. Weapon type 0 (bullet): direction = normalize(end - start),
    // bulletspread(0, 8192) then the bullet fire 0x531340 with the world entity (0x7fe) as attacker and ignore entity, then client
    // event 0x9e with the weapon index. Weapon type 1 errors "MagicBullet only handle weapons of type bullet currently."
    vm_.registerBuiltin("magicbullet", [this](CallContext& c) {
        const std::string wn = c.arg(0).toString();
        const auto* w = weapon(wn);
        if (!w) throw std::runtime_error("unknown weapon " + wn);
        auto it = w->find("weapontype");
        const std::string type = it == w->end() ? std::string("bullet") : it->second;
        if (type != "bullet") throw std::runtime_error("MagicBullet only handle weapons of type bullet currently.");
        const Vec3 s = c.arg(1).asVector(), e = c.arg(2).asVector();
        Vec3 d{e[0] - s[0], e[1] - s[1], e[2] - s[2]};
        const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l > 0) d = {d[0] / l, d[1] / l, d[2] / l};
        Entity worldEnt;
        worldEnt.number = 2046;
        worldEnt.obj = attackerValue(nullptr).asObject();
        fireBullet(worldEnt, s, {s[0] + d[0] * 8192.0f, s[1] + d[1] * 8192.0f, s[2] + d[2] * 8192.0f}, weaponDamage(wn), nullptr, wn);
        return Value();
    });
    auto actorSelf = [this](CallContext& c) -> Entity& {
        Entity* e = entityOf(c.self);
        if (!e || !e->isActor) throw std::runtime_error("not an actor");
        return *e;
    };
    // pushplayer (0x5407a0): actor +0x28c bit 0x2000000; a zero argument sets it, non-zero clears it.
    vm_.registerBuiltin("pushplayer", [actorSelf](CallContext& c) {
        Entity& a = actorSelf(c);
        a.ai.noPush = !(c.arg(0).isNumber() && c.arg(0).asInt() != 0);
        return Value();
    });
    // setpotentialthreat (0x540dd0): exactly 1 argument (degrees); actor +0x20b4 = 1, +0x20b8 / +0x20bc = cos / sin.
    vm_.registerBuiltin("setpotentialthreat", [actorSelf](CallContext& c) {
        Entity& a = actorSelf(c);
        if (c.argc() != 1) throw std::runtime_error("setpotentialthreat: usage");
        const float r = c.arg(0).asFloat() * 0.017453292f;
        a.ai.potentialThreat = true;
        a.ai.potentialThreatDir[0] = std::cos(r);
        a.ai.potentialThreatDir[1] = std::sin(r);
        return Value();
    });
    // clearpotentialthreat (0x540e70): no arguments; actor +0x20b4 = 0.
    vm_.registerBuiltin("clearpotentialthreat", [actorSelf](CallContext& c) {
        Entity& a = actorSelf(c);
        if (c.argc() != 0) throw std::runtime_error("clearpotentialthreat: usage");
        a.ai.potentialThreat = false;
        return Value();
    });
    // traversemode (0x53f360): actor +0x210 = 1 "gravity" (script string 0x1c7f080), 2 "nogravity" (0x1c7f0a4), 3 "noclip"
    // (0x1c7f0a2); anything else errors (0x591464). TODO(spec): the mode's effect (negotiation is not implemented).
    vm_.registerBuiltin("traversemode", [actorSelf](CallContext& c) {
        Entity& a = actorSelf(c);
        const std::string m = c.arg(0).toString();
        if (m == "gravity") a.ai.traverseMode = 1;
        else if (m == "nogravity") a.ai.traverseMode = 2;
        else if (m == "noclip") a.ai.traverseMode = 3;
        else throw std::runtime_error("traversemode: illegal mode " + m);
        return Value();
    });
    // animmode (0x53f410): actor +0x21c (the anim mode override, read by setActorAnimScript): "nophysics" 8 and sets gentity
    // flag 0x1000 (+0x128), every other name clears that flag first: "gravity" 4, "nogravity" 6, "angle deltas" 3,
    // "zonly_physics" 7, "none" 0; otherwise the error "illegal call to animmode()".
    vm_.registerBuiltin("animmode", [actorSelf](CallContext& c) {
        Entity& a = actorSelf(c);
        const std::string m = c.arg(0).toString();
        a.ai.noPhysicsFlag = m == "nophysics";
        if (m == "nophysics") a.ai.animModeOverride = 8;
        else if (m == "gravity") a.ai.animModeOverride = 4;
        else if (m == "nogravity") a.ai.animModeOverride = 6;
        else if (m == "angle deltas") a.ai.animModeOverride = 3;
        else if (m == "zonly_physics") a.ai.animModeOverride = 7;
        else if (m == "none") a.ai.animModeOverride = 0;
        else throw std::runtime_error("illegal call to animmode()");
        return Value();
    });
    // setcontents (0x4efd70): entity +0xc0 = the argument (the old value is returned), then the entity is relinked.
    // The entity clips (syncEntityClips) and the vehicle touch scan read the stored value (World::entityContents); 0 unlinks the entity.
    // TODO(spec): the contents of entity classes other than actors, the player and script movers are not modelled.
    vm_.registerBuiltin("setcontents", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e) throw std::runtime_error("setcontents: not an entity");
        const uint32_t old = entityContents(*e);
        e->obj->set("$contents", Value(int32_t(c.arg(0).asInt())));
        return Value(int32_t(old));
    });
    // setsunlight (0x4f5750): 3 arguments, config string 9 = "%g %g %g"; resetsunlight (0x4f57c0): no arguments, config string 9 reset.
    vm_.registerBuiltin("setsunlight", [this](CallContext& c) {
        if (c.argc() != 3) throw std::runtime_error("setsunlight: usage");
        sunlight_ = Vec3{c.arg(0).asFloat(), c.arg(1).asFloat(), c.arg(2).asFloat()};
        return Value();
    });
    vm_.registerBuiltin("resetsunlight", [this](CallContext&) {
        sunlight_.reset();
        return Value();
    });
    // setsoundblend (0x4f7d10): entity of type 6 (script_origin) with two sound aliases and a blend 0..1 (error when outside);
    // stores aliases at +0x8c / +0x8e and the blend at +0x94, then relinks (errors "Lerp must be between 0.0f and 1.0f" and
    // "Entity is not a sound_blend"). TODO(spec): the audio crossfade and the entity type check.
    vm_.registerBuiltin("setsoundblend", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e) throw std::runtime_error("setsoundblend: not an entity");
        const float b = c.arg(2).asFloat();
        if (!(b >= 0.0f && b <= 1.0f)) throw std::runtime_error("Lerp must be between 0.0f and 1.0f");
        e->obj->set("$soundblend", Value(b));
        return Value();
    });
    // isfiring (0x539aa0) weapon state 3, 4, 10, 11; ismeleeing (0x539c10) 10, 11; isthrowinggrenade (0x539b60) 12..16.
    vm_.registerBuiltin("isfiring", [this, playerSelf](CallContext& c) {
        playerSelf(c);
        const int s = playerCmd_.weaponState;
        return Value(s == 3 || s == 4 || s == 10 || s == 11);
    });
    vm_.registerBuiltin("ismeleeing", [this, playerSelf](CallContext& c) {
        playerSelf(c);
        const int s = playerCmd_.weaponState;
        return Value(s == 10 || s == 11);
    });
    vm_.registerBuiltin("isthrowinggrenade", [this, playerSelf](CallContext& c) {
        playerSelf(c);
        const int s = playerCmd_.weaponState;
        return Value(s >= 12 && s <= 16);
    });
    // setfriendlychain (0x4eeb10): src/game/friendlychain.cpp.
    vm_.registerBuiltin("setfriendlychain", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || !e->isPlayer) throw std::runtime_error("Entity must be a player");
        if (c.argc() != 1) throw std::runtime_error("sentient SetFriendlyChain <node>, where <none> is on a friendly chain.");
        int node = -1;
        for (size_t i = 0; i < nodes_.size(); ++i)
            if (c.arg(0).isObject() && nodes_[i].obj == c.arg(0).asObject()) node = int(i);
        if (node < 0) throw std::runtime_error("setfriendlychain: not a pathnode");
        setFriendlyChain(*e, node);
        return Value();
    });
    // bulletspread (start, end, degrees) (0x4ee760 → 0x531210): the point 8192 units along end-start with a random
    // offset in the disc of radius tan(spread)*8192 (angle uniform, radius uniform = rand/32768; the exe's own LCG
    // 0x5a56f4, seed TODO(spec)). The disc axes come from 0x4217f0 (any rotation about the direction is equivalent).
    vm_.registerBuiltin("bulletspread", [](CallContext& c) {
        const Vec3 s = c.arg(0).asVector(), t = c.arg(1).asVector();
        const float deg = c.arg(2).asFloat();
        Vec3 f = sub(t, s);
        const float l = len(f);
        if (l > 0) f = scale(f, 1.0f / l);
        Vec3 right{f[1], -f[0], 0};
        if (len(right) < 1e-4f) right = {1, 0, 0};
        else right = scale(right, 1.0f / len(right));
        const Vec3 up{f[1] * right[2] - f[2] * right[1], f[2] * right[0] - f[0] * right[2], f[0] * right[1] - f[1] * right[0]};
        static uint32_t seed = 0;
        const uint32_t r1 = (seed = seed * 0x343fdu + 0x269ec3u) >> 17;
        const uint32_t r2 = (seed = seed * 0x343fdu + 0x269ec3u) >> 17;
        const float ang = float(r1) * 0.010986328125f * 0.01745329238474369f, rad = float(r2) * 3.0517578125e-05f;
        const float k = std::tan(deg * 0.01745329238474369f) * 8192.0f * rad;
        Vec3 out = add(s, scale(f, 8192.0f));
        out = add(out, add(scale(right, k * std::cos(ang)), scale(up, k * std::sin(ang))));
        return Value(out);
    });
    // isturretready (0x5148e0): only on a player-controlled vehicle ("Must be called on a player controlled vehicle", vehicle
    // +0x174 bit 0); true when the fire timer (+0x184) is <= 0.
    vm_.registerBuiltin("isturretready", [this](CallContext& c) {
        VehicleState& v = vehicleState(c.self);
        if (v.owner < 0) throw std::runtime_error("Must be called on a player controlled vehicle");
        return Value(v.fireTimerMs <= 0);
    });
    // dontinterpolate (0x4edf90): toggles bit 2 (the teleport bit) of the client's eFlags (+0x7c) or the entity's (+0x128) so the
    // renderer skips interpolation for one snapshot. No interpolation state here, so nothing to do.
    vm_.registerBuiltin("dontinterpolate", [](CallContext&) { return Value(); });
    // earthquake(scale, duration, origin, radius) (0x4f4330): scale, duration and radius must be > 0; event 0xc7 goes to the clients.
    vm_.registerBuiltin("earthquake", [this](CallContext& c) {
        const float scale = c.arg(0).asFloat(), dur = c.arg(1).asFloat(), radius = c.arg(3).asFloat();
        if (scale <= 0) throw std::runtime_error("Scale must be greater than 0");
        const int ms = int(dur * 1000.0f);
        if (ms <= 0) throw std::runtime_error("duration must be greater than 0");
        if (radius <= 0) throw std::runtime_error("Radius must be greater than 0");
        earthquakes_.push_back({scale, ms, c.arg(2).asVector(), radius, timeMs_});
        shakeEvents_.push_back({c.arg(2).asVector(), scale, ms, radius, timeMs_}); // event 0xc7 -> cgame 0x4bf5e0
        return Value();
    });
    // viewkick(damage percent, origin) (0x4f7a40): client +0x8658 = (percent * maxhealth + 50) / 100 (error "viewkick: damage %g < 0"),
    // +0x865c = player origin - origin. TODO(spec): the consumer of these fields.
    vm_.registerBuiltin("viewkick", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || !e->isPlayer) throw std::runtime_error("not a player");
        const int pct = c.arg(0).asInt();
        if (pct < 0) throw std::runtime_error("viewkick: damage " + std::to_string(pct) + " < 0");
        const Value mh = e->obj->get("maxhealth");
        const int maxHealth = mh.isNumber() ? mh.asInt() : 100;
        viewKick_.count = (pct * maxHealth + 50) / 100;
        viewKick_.dir = sub(e->obj->get("origin").asVector(), c.arg(1).asVector());
        return Value();
    });
    for (const char* noop : {"trackscriptstate", "setlookattext", "setdefaultdroppitch", "restoredefaultdroppitch"})
        vm_.registerBuiltin(noop, [](CallContext&) { return Value(); });
    // Traces (result keys as used by the shipped scripts and present as strings in the exe: "fraction",
    // "position", "entity", "normal", "surfacetype"). Surface types are not decoded yet ("default").
    // bullettrace (FUN_004f2230): G_Trace, mask 0x280e831 (0x802831 without characters), locational with no priority
    // map, so characters are boxes.
    vm_.registerBuiltin("bullettrace", [this](CallContext& c) {
        const Vec3 a = c.arg(0).asVector(), b = c.arg(1).asVector();
        int location = 0;
        const TraceResult tr = locationalTrace(a, b, c.arg(2).truthy() ? Contents::BulletMask : Contents::BulletMaskNoCharacters,
                                               entityOf(c.arg(3)), nullptr, location);
        auto arr = std::make_shared<Array>();
        auto put = [&](const char* k, Value v) { arr->set(ArrayKey{true, 0, k}, std::move(v)); };
        put("fraction", Value(tr.fraction));
        put("position", Value(Vec3{tr.endPos[0], tr.endPos[1], tr.endPos[2]}));
        put("normal", Value(Vec3{tr.normal[0], tr.normal[1], tr.normal[2]}));
        put("surfacetype", Value(tr.fraction < 1.0f ? "default" : "none"));
        if (tr.entity >= 0)
            if (Entity* e = entityByNumber(tr.entity)) put("entity", Value(e->obj));
        return Value(arr);
    });
    // getfxvisibility(start, end) (0x4f53b0 → 0x48b090).
    vm_.registerBuiltin("getfxvisibility", [this](CallContext& c) {
        const Vec3 start = c.arg(0).asVector();
        return Value(fxVisibility(start, c.arg(1).asVector()));
    });
    // bullettracepassed (FUN_004f24c0 → 0x4db890): a sight trace with the bullet masks.
    vm_.registerBuiltin("bullettracepassed", [this](CallContext& c) {
        const Entity* ignore = entityOf(c.arg(3));
        return Value(!collision_->sightBlocked(c.arg(0).asVector(), c.arg(1).asVector(), {0, 0, 0}, {0, 0, 0},
                                               c.arg(2).truthy() ? Contents::BulletMask : Contents::BulletMaskNoCharacters,
                                               ignore ? ignore->number : -1));
    });
    // sighttracepassed (FUN_004f2580 → 0x44c3c0): a clear trace is still blocked when the visibility through
    // effects (0x48b090) is below 0.2 (0x592020).
    vm_.registerBuiltin("sighttracepassed", [this](CallContext& c) {
        const Entity* ignore = entityOf(c.arg(3));
        const Vec3 start = c.arg(0).asVector(), end = c.arg(1).asVector();
        if (collision_->sightBlocked(start, end, {0, 0, 0}, {0, 0, 0},
                                     c.arg(2).truthy() ? Contents::SightMask : Contents::SightMaskNoCharacters,
                                     ignore ? ignore->number : -1))
            return Value(false);
        return Value(!(fxVisibility(start, end) < 0.2f));
    });
    vm_.registerBuiltin("physicstrace", [this](CallContext& c) {
        const TraceResult tr = collision_->trace(c.arg(0).asVector(), c.arg(1).asVector(), {0, 0, 0}, {0, 0, 0}, Contents::Solid);
        return Value(Vec3{tr.endPos[0], tr.endPos[1], tr.endPos[2]});
    });
    // dropweapon(weapon, hand, throwSpeed) (0x53da50): hand "left"/"right" picks tag_weapon_left/right, other
    // values no tag; the weapon item is spawned there by 0x5220f0. The throw velocity is applied below (exe-checked);
    // the item falls under G_RunItem (World::runItems).
    vm_.registerBuiltin("dropweapon", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e) throw std::runtime_error("not an entity");
        const std::string w = lowerStr(c.arg(0).toString());
        if (w.empty() || w == "none" || !weapon(w)) return Value();
        const std::string hand = lowerStr(c.arg(1).toString());
        Vec3 at = e->obj->get("origin").asVector();
        if (hand == "left" || hand == "right") {
            const auto bones = entityBones(*e);
            if (auto it = bones.find(hand == "left" ? "tag_weapon_left" : "tag_weapon_right"); it != bones.end()) at = it->second.position;
        }
        Entity& item = spawnEntity("weapon_" + w);
        item.obj->set("origin", Value(at));
        item.obj->set("angles", e->obj->get("angles"));
        const auto* def = weapon(w);
        if (auto it = def->find("worldmodel"); it != def->end() && !it->second.empty()) item.obj->set("model", Value(it->second));
        // 0x5220f0 → 0x521f70: the item is launched and falls (G_RunItem). TODO(spec): from the hand tag, as above.
        launchItem(item, *e);
        // A non-player dropper fixes the item's ammunition at drop time (0x5220f0, 0x522185..0x52231f: actorDropAmmo), so the
        // pickup does not resolve it again.
        if (!e->isPlayer) {
            const ItemAmmo a = actorDropAmmo(weaponInt(w, "dropAmmoMin"), weaponInt(w, "dropAmmoMax"), weaponInt(w, "clipSize"), meleeSeed_);
            item.obj->set("$ammoStock", Value(a.stock));
            item.obj->set("$ammoClip", Value(a.clip));
        }
        // 0x53da50 / 0x5220f0: the item starts where a 0x491 trace from the body centre
        // (origin + half the box height) to the hand tag ends (the dropper passes), and flies with dir × throwSpeed, dir =
        // normalize(tag − centre) with 2.0 added to z; throwSpeed 0 gives a zero velocity (it just falls).
        // TODO(spec): the box height is the 72/70 of launchItem; the slide/ease rules on a hit (0x5231b2-0x52327b).
        if (hand == "left" || hand == "right") {
            const float height = e->isPlayer ? 70.0f : e->isActor ? 72.0f : 0.0f;
            const Vec3 o = e->obj->get("origin").asVector();
            const Vec3 centre{o[0], o[1], o[2] + height * 0.5f};
            const TraceResult st = collision_->trace(centre, at, {-1, -1, -1}, {1, 1, 1}, 0x491, e->number);
            const Vec3 start{st.endPos[0], st.endPos[1], st.endPos[2]};
            item.obj->set("origin", Value(start));
            item.obj->set("$trbase", Value(start));
            const float speed = c.arg(2).isNumber() ? c.arg(2).asFloat() : 0.0f;
            Vec3 d{at[0] - centre[0], at[1] - centre[1], at[2] - centre[2] + 2.0f};
            const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (speed != 0 && dl > 0) d = {d[0] / dl * speed, d[1] / dl * speed, d[2] / dl * speed};
            else d = {0, 0, 0};
            item.obj->set("$trdelta", Value(d));
        }
        return Value(item.obj);
    });
    // issuppressed (0x53e6a0): actor only; true when one of the 4 danger plane slots (actor +0x2050, stride 0x18) is in use
    // (+0x0 time != 0) and not short-lived (+0x14 == 0), i.e. a hostile suppression line is currently held.
    // setstablemissile(bool) (0x4efde0): only eType 9 (actor), 0xc (vehicle) or 1 (player), else the script error "Type should be a
    // sentient or a vehicle" (0x586e80); sets (true) or clears entity flag 0x20000 (+0x128), which G_FireRocket copies to the missile
    // (0x527979) and which keeps it from destabilizing (0x527053).
    vm_.registerBuiltin("setstablemissile", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || !(e->isActor || e->isPlayer || e->vehicle)) throw std::runtime_error("Type should be a sentient or a vehicle");
        e->obj->set("$stablemissile", Value(c.arg(0).truthy()));
        return Value();
    });
    // enablegrenadetouchdamage() (0x4f8000): only on a trigger_damage (classname 0x1c7f0ea), else the script error "Currently on
    // supported on damage triggers" (0x58542c); sets +0x128 |= 0x4000, the gate of the grenade path test 0x5297b0.
    vm_.registerBuiltin("enablegrenadetouchdamage", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || e->classname != "trigger_damage") throw std::runtime_error("Currently on supported on damage triggers");
        e->obj->set("$grenadetouchdamage", Value(true));
        return Value();
    });
    vm_.registerBuiltin("issuppressed", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e || !e->isActor) throw std::runtime_error("not an actor");
        for (const DangerPlane& p : e->dangerPlanes)
            if (p.timeMs != 0 && !p.shortLived) return Value(true);
        return Value(false);
    });
    // getclosestenemysqdist (0x53c290): self must be a sentient ("not a sentient", 0x590bd8); the smallest squared distance from self's origin
    // to the origin of an enemy-team sentient (team mask table 0x53c2e7: allies 2, axis 1) that self has a known record of (+0x1784 > 0),
    // skipping notarget (flags & 4, 0x53c37b) and ignoreme (sentient byte +0xc) ones; 1e8 (0x4cbebc20) when there is none.
    vm_.registerBuiltin("getclosestenemysqdist", [this](CallContext& c) {
        Entity* a = entityOf(c.self);
        if (!a || !isSentient(*a)) throw std::runtime_error("not a sentient");
        float best = 1e8f;
        const std::string myTeam = a->obj->get("team").toString(); const std::string other = myTeam == "axis" ? "allies" : myTeam == "allies" ? "axis" : std::string(); // table {0, 2, 1, 0, 0}
        if (!other.empty()) {
            const Vec3 ao = a->obj->get("origin").asVector();
            for (const auto& ep : entities_) {
                Entity& t = *ep;
                if (!isSentient(t) || t.obj->get("team").toString() != other) continue;
                const auto it = a->sentientInfo.find(t.obj.get());
                if (it == a->sentientInfo.end() || it->second.lastKnownMs <= 0) continue;
                if (t.obj->get("$notarget").truthy() || t.obj->get("ignoreme").truthy()) continue;
                const Vec3 d = sub(t.obj->get("origin").asVector(), ao);
                best = std::min(best, dot(d, d));
            }
        }
        return Value(best);
    });
    vm_.registerBuiltin("enablehealthshield", [](CallContext&) { return Value(); });
    vm_.registerBuiltin("setnormalhealth", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (e) {
            const Value mh = e->obj->get("maxhealth");
            const int max = mh.isNumber() ? mh.asInt() : 100;
            e->obj->set("health", Value(int32_t(std::lround(c.arg(0).asFloat() * float(max)))));
        }
        return Value();
    });
    vm_.registerBuiltin("getnormalhealth", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        if (!e) return Value(0.0f);
        const Value mh = e->obj->get("maxhealth");
        const float max = mh.isNumber() ? mh.asFloat() : 100.0f;
        return Value(e->obj->get("health").asFloat() / std::max(1.0f, max));
    });
}

// Visibility through sight-blocking effects (0x48b090): 1 without blockers or when the points are closer than
// fx_visMinTraceDist (0x617eb0: default 80, range 0..1000, 0x40e544). Otherwise each blocker whose projection on the
// segment lies within it (|t − len/2| ≤ len/2) and whose distance² from the segment is below its radius² multiplies
// the result by its visibility.
float World::fxVisibility(const Vec3& start, const Vec3& end) const {
    if (fxSightBlockers_.empty()) return 1.0f;
    Vec3 dir{end[0] - start[0], end[1] - start[1], end[2] - start[2]};
    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len != 0.0f)
        for (float& d : dir) d *= 1.0f / len;
    const auto it = cvars.find("fx_visMinTraceDist");
    const float minDist = it != cvars.end() && !it->second.empty() ? float(std::atof(it->second.c_str())) : 80.0f;
    if (len < minDist) return 1.0f;
    const float half = len * 0.5f;
    float vis = 1.0f;
    for (const FxSightBlocker& b : fxSightBlockers_) {
        const float t = (b.origin[0] - start[0]) * dir[0] + (b.origin[1] - start[1]) * dir[1] + (b.origin[2] - start[2]) * dir[2];
        if (std::fabs(t - half) > half) continue;
        const float dx = dir[0] * t + start[0] - b.origin[0], dy = dir[1] * t + start[1] - b.origin[1],
                    dz = dir[2] * t + start[2] - b.origin[2];
        if (dx * dx + dy * dy + dz * dz < b.radiusSq) vis *= b.visibility;
    }
    return vis;
}

} // namespace iw2::game

namespace iw2::game {

void World::damageFeedback() {
    if (!player_ || !isAlive(*player_)) return;
    damageFeedback_.count = 0;
    const Value mh = player_->obj->get("maxhealth");
    const int maxHealth = mh.isNumber() ? mh.asInt() : 100;
    if (damageBlood_ <= 0 || maxHealth <= 0) return;
    const int count = std::min(127, damageBlood_ * 100 / maxHealth);
    if (damageFromWorld_) {
        damageFeedback_.pitch = damageFeedback_.yaw = 0xff;
        damageFromWorld_ = false;
    } else {
        const Vec3& d = damageFrom_; // vectoangles (0x421b30)
        float yaw = std::atan2(d[1], d[0]) * 180.0f / float(M_PI);
        if (yaw < 0) yaw += 360.0f;
        float pitch = std::atan2(d[2], std::sqrt(d[0] * d[0] + d[1] * d[1])) * 180.0f / float(M_PI);
        pitch = pitch > 0 ? 360.0f - pitch : -pitch;
        damageFeedback_.pitch = int(pitch * 0.7111111283302307f);
        damageFeedback_.yaw = int(yaw * 0.7111111283302307f);
    }
    damageFeedback_.count = count;
    ++damageFeedback_.event;
    damageBlood_ = 0;
}

} // namespace iw2::game
