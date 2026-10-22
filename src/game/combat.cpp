// Skeletons/tags, damage, perception and shooting (docs/spec/combat.md). Most rules here are inferred placeholders
// until the corresponding engine code is analyzed; each is marked in the spec.

#include "game/world.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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
    const assets::XPose pose = e.anim ? e.anim->pose(*body) : assets::bindPose(*body);
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

// Applies damage; notifies "damage" (amount, attacker, direction, point, type) and handles death.
void World::damage(Entity& target, Entity* attacker, int amount, const Vec3& dir, const Vec3& point, const std::string& mod,
                   const std::string& location) {
    if (!isAlive(target)) return;
    if (target.obj->get("$godmode").truthy()) return;
    const int health = target.obj->get("health").asInt() - amount;
    target.obj->set("health", Value(int32_t(health)));
    target.obj->set("damagetaken", Value(int32_t(amount)));
    target.obj->set("damagedir", Value(dir));
    target.obj->set("damageyaw", Value(float(std::atan2(-dir[1], -dir[0]) * 180.0 / M_PI)));
    target.obj->set("damagelocation", Value(location));
    // damageweapon is the attacker entity's weapon (pain/die callbacks 0x4ff3b0 / 0x4ff530: entity state weapon
    // index → weapon name). Without an attacker G_Damage (0x528050) credits the world entity, whose weapon is index 0,
    // "none" (0x4dc210). TODO(spec): the weapon index a vehicle entity carries; vehicles leave the field unchanged.
    if (!attacker || !attacker->vehicle) {
        const Value w = attacker ? attacker->obj->get(attacker->isPlayer ? "$weapon" : "weapon") : Value();
        target.obj->set("damageweapon", w.isString() && !w.asString().empty() ? w : Value("none"));
    }
    vm_.notify(target.obj, "damage", {Value(int32_t(amount)), attacker ? Value(attacker->obj) : Value(), Value(dir), Value(point), Value(mod)});
    if (health > 0) {
        if (target.isActor) actorPain(target, attacker); // pain callback 0x4ff3b0
        return;
    }
    if (target.isActor) {
        ++stats.kills;
        // G_Damage notifies "death" (attacker), then the die callback (0x4ff530) sets the death state.
        vm_.notify(target.obj, "death", {attacker ? Value(attacker->obj) : Value()});
        actorDie(target, attacker);
    } else if (target.isPlayer) {
        vm_.notify(target.obj, "death", {attacker ? Value(attacker->obj) : Value()});
        print("player killed");
        // TODO(spec): the original opens the dead screen and offers the last save (opendeadscreen,
        // loadgame_continue); OpenIW2 restarts the level after 3 s.
        if (!levelChange_.pending) levelChange_ = {true, true, mapName_, false, timeMs_ + 3000};
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
    if (!priority || tr.fraction == 0) return tr;
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
Entity* World::fireBullet(Entity& shooter, const Vec3& start, const Vec3& end, int damageAmount, Entity* attacker) {
    // Bullet_Fire_Extended (FUN_00531340): mask 0x280e831; the rifleBullet priority map puts the head first.
    const Value wn = shooter.obj->get(shooter.isPlayer ? "$weapon" : "weapon");
    const auto* def = wn.isString() ? weapon(wn.asString()) : nullptr;
    bool rifle = false;
    if (def) {
        auto it = def->find("riflebullet");
        rifle = it != def->end() && std::atoi(it->second.c_str()) != 0;
    }
    // An AI's bullet that hits an actor of its own team goes on from the hit point ignoring that actor, up to 12
    // times ("Ignoring AI shooting teammate"). TODO(spec): penetration (surface flag 0x10), vehicles, the
    // half-damage refire of rifleBullet hits on sentients.
    int location = 0;
    TraceResult tr;
    Entity* hit = nullptr;
    Vec3 from = start;
    const Entity* pass = &shooter;
    for (int depth = 0; depth <= 12; ++depth) {
        tr = locationalTrace(from, end, Contents::BulletMask, pass, rifle ? kHitPriorityRifle : kHitPriorityDefault, location);
        hit = tr.entity >= 0 ? entityByNumber(tr.entity) : nullptr;
        if (hit && shooter.isActor && hit->isActor && hit->obj->get("team").toString() == shooter.obj->get("team").toString()) {
            from = tr.endPos;
            pass = hit;
            hit = nullptr;
            continue;
        }
        break;
    }
    if (hit && !isAlive(*hit)) hit = nullptr;
    ++stats.shots;
    if (def) {
        // Weapon sound: fireSoundPlayer without position for the player, fireSound at the shooter otherwise.
        auto it = def->find(shooter.isPlayer ? "firesoundplayer" : "firesound");
        if (it != def->end() && !it->second.empty()) startSound(it->second, shooter.isPlayer ? nullptr : &start);
    }
    if (hit) {
        ++stats.hits;
        const Vec3 dir = scale(sub(end, start), 1.0f / std::max(1.0f, len(sub(end, start))));
        damage(*hit, attacker, damageAmount, dir, tr.endPos, "MOD_RIFLE_BULLET", hitLocationName(location));
    }
    return hit;
}

int World::weaponDamage(const std::string& weaponName) {
    const auto* w = weapon(weaponName);
    if (!w) return 35;
    auto it = w->find("damage");
    return it == w->end() ? 35 : std::max(1, std::atoi(it->second.c_str()));
}

void World::playerFire(const Vec3& eye, const Vec3& forward) {
    if (!player_ || !isAlive(*player_)) return;
    const Value w = player_->obj->get("$weapon");
    const int dmg = weaponDamage(w.isString() ? w.asString() : "kar98k");
    fireBullet(*player_, eye, add(eye, scale(forward, 8192.0f)), dmg, player_);
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
        return Value(eyePosition(*e));
    });
    vm_.registerBuiltin("cansee", [this](CallContext& c) {
        Entity* a = entityOf(c.self);
        Entity* b = entityOf(c.arg(0));
        return Value(a && b && canSee(*a, *b));
    });
    vm_.registerBuiltin("canshoot", [this](CallContext& c) {
        Entity* a = entityOf(c.self);
        if (!a) return Value(false);
        const Vec3 start = eyePosition(*a);
        const Vec3 target = c.arg(0).isVector() ? c.arg(0).asVector() : start;
        return Value(collision_->trace(start, target, {0, 0, 0}, {0, 0, 0}, Contents::Solid).fraction >= 0.999f);
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
        else if (enemy) target = add(enemy->obj->get("origin").asVector(), {0, 0, 40});
        else return Value();
        const Value accV = a->obj->get("accuracy");
        const float accuracy = std::clamp((accV.isNumber() ? accV.asFloat() : 0.5f) * accMod, 0.0f, 1.0f);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        if (u(rng_) > accuracy) {
            // Miss: aim off to the side by up to 40 units (inferred spread).
            std::uniform_real_distribution<float> off(-40.0f, 40.0f);
            target = add(target, {off(rng_), off(rng_), off(rng_) * 0.5f});
        }
        const Vec3 dir = sub(target, start);
        const Value wpn = a->obj->get("weapon");
        fireBullet(*a, start, add(start, scale(dir, 8192.0f / std::max(1.0f, len(dir)))), weaponDamage(wpn.isString() ? wpn.asString() : ""), a);
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
            args.blendTime = 0.2f;
            e->anim->set(n, args);
        };
        setW("$aim_up", up);
        setW("$aim_straight", straight);
        setW("$aim_down", down);
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
        const TraceResult tr = collision_->trace(add(from, {0, 0, 18}), add(to, {0, 0, 18}), {-15, -15, 0}, {15, 15, 52}, Contents::PlayerSolid | Contents::AiClip);
        return Value(tr.fraction >= 0.999f && !tr.startSolid);
    });
    vm_.registerBuiltin("isingoal", [](CallContext& c) {
        const auto o = c.self.asObject();
        const Value g = o->get("$goalpos");
        if (!g.isVector() || !c.arg(0).isVector()) return Value(true);
        const Value r = o->get("goalradius");
        const float radius = r.isNumber() ? r.asFloat() : 64.0f;
        const Vec3 d = sub(c.arg(0).asVector(), g.asVector());
        return Value(d[0] * d[0] + d[1] * d[1] <= radius * radius);
    });
    for (const char* noop : {"exitprone", "enterprone", "orientmode", "animmode", "trackscriptstate", "setproneanimnodes", "updateprone",
                             "setcandamage", "setlookattext", "setdefaultdroppitch", "restoredefaultdroppitch"})
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
    // values no tag; the weapon item is spawned there by 0x5220f0. TODO(spec): the throw velocity (applied when
    // throwSpeed != 0) and the item entity's physics.
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
        return Value(item.obj);
    });
    vm_.registerBuiltin("issuppressed", [](CallContext&) { return Value(false); });
    vm_.registerBuiltin("getclosestenemysqdist", [this](CallContext& c) {
        Entity* a = entityOf(c.self);
        Entity* en = a ? entityOf(a->obj->get("enemy")) : nullptr;
        if (!a || !en) return Value(1e10f);
        const Vec3 d = sub(en->obj->get("origin").asVector(), a->obj->get("origin").asVector());
        return Value(dot(d, d));
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
