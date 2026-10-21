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
                const auto xm = assets::parseXModel(*md);
                if (auto pd = vfs_.read("xmodelparts/" + xm.lods.at(0).name)) parts = assets::parseXModelParts(*pd);
            }
        } catch (const std::exception&) {
        }
        it = skeletons_.emplace(model, std::move(parts)).first;
    }
    return it->second ? &*it->second : nullptr;
}

// World-space bones of an entity's body and attachments (shared skeleton by bone name; tags place roots).
std::unordered_map<std::string, World::BoneXform> World::entityBones(Entity& e) {
    std::unordered_map<std::string, BoneXform> bones;
    const Value m = e.obj->get("model"), o = e.obj->get("origin"), a = e.obj->get("angles");
    if (!m.isString() || !o.isVector()) return bones;
    const Vec3 origin = o.asVector();
    const float yaw = a.isVector() ? a.asVector()[1] : 0.0f;
    const assets::Quat er = yawQuat(yaw);
    auto toWorld = [&](const std::array<float, 3>& p, const assets::Quat& q) {
        return BoneXform{add(origin, assets::rotate(er, p)), assets::multiply(er, q)};
    };
    const assets::XModelParts* body = skeleton(m.asString());
    if (!body) return bones;
    const assets::XPose pose = e.anim ? e.anim->pose(*body) : assets::bindPose(*body);
    std::unordered_map<std::string, std::pair<std::array<float, 3>, assets::Quat>> local;
    for (size_t i = 0; i < body->bones.size(); ++i) local.emplace(lowerStr(body->bones[i].name), std::make_pair(pose.position[i], pose.rotation[i]));
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
    }
    for (const auto& [name, x] : local) bones.emplace(name, toWorld(x.first, x.second));
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
void World::damage(Entity& target, Entity* attacker, int amount, const Vec3& dir, const Vec3& point, const std::string& mod) {
    if (!isAlive(target)) return;
    if (target.obj->get("$godmode").truthy()) return;
    const int health = target.obj->get("health").asInt() - amount;
    target.obj->set("health", Value(int32_t(health)));
    target.obj->set("damagetaken", Value(int32_t(amount)));
    target.obj->set("damagedir", Value(dir));
    target.obj->set("damageyaw", Value(float(std::atan2(-dir[1], -dir[0]) * 180.0 / M_PI)));
    target.obj->set("damagelocation", Value("torso_upper")); // hit locations are not modelled yet
    if (attacker && attacker->obj->get("weapon").isString()) target.obj->set("damageweapon", attacker->obj->get("weapon"));
    vm_.notify(target.obj, "damage", {Value(int32_t(amount)), attacker ? Value(attacker->obj) : Value(), Value(dir), Value(point), Value(mod)});
    if (health > 0) {
        if (target.isActor && target.animScript != "pain" && target.animScript != "death") startAnimScript(target, "pain");
        return;
    }
    if (target.isActor) {
        ++stats.kills;
        target.path.clear();
        startAnimScript(target, "death");
        vm_.notify(target.obj, "death", {attacker ? Value(attacker->obj) : Value()});
    } else if (target.isPlayer) {
        vm_.notify(target.obj, "death", {attacker ? Value(attacker->obj) : Value()});
        print("player killed");
    }
}

// Very simple hit test against sentients: a vertical capsule of radius 15 and height 70 around each origin.
Entity* World::traceSentients(const Vec3& start, const Vec3& end, const Entity* ignore, float& fraction) {
    Entity* best = nullptr;
    const Vec3 d = sub(end, start);
    const float L = len(d);
    if (L <= 0) return nullptr;
    const Vec3 dirN = scale(d, 1.0f / L);
    for (const auto& ep : entities_) {
        Entity& e = *ep;
        if (&e == ignore || !isAlive(e)) continue;
        const Vec3 o = e.obj->get("origin").asVector();
        // Closest approach of the ray to the vertical axis, in 2D.
        const float ox = o[0] - start[0], oy = o[1] - start[1];
        const float dxy = dirN[0] * dirN[0] + dirN[1] * dirN[1];
        float t = dxy > 1e-6f ? (ox * dirN[0] + oy * dirN[1]) / dxy : 0.0f;
        t = std::clamp(t, 0.0f, L);
        const Vec3 p = add(start, scale(dirN, t));
        const float rx = p[0] - o[0], ry = p[1] - o[1];
        if (rx * rx + ry * ry > 15.0f * 15.0f || p[2] < o[2] || p[2] > o[2] + 70.0f) continue;
        if (t / L < fraction) {
            fraction = t / L;
            best = &e;
        }
    }
    return best;
}

// Fires a weapon bullet from `start` towards `end`: world geometry blocks, the first sentient hit takes damage.
Entity* World::fireBullet(Entity& shooter, const Vec3& start, const Vec3& end, int damageAmount) {
    const TraceResult tr = collision_->trace(start, end, {0, 0, 0}, {0, 0, 0}, Contents::Solid);
    float frac = tr.fraction;
    Entity* hit = traceSentients(start, end, &shooter, frac);
    ++stats.shots;
    {
        // Weapon sound: fireSoundPlayer without position for the player, fireSound at the shooter otherwise.
        const Value wn = shooter.obj->get(shooter.isPlayer ? "$weapon" : "weapon");
        if (const auto* def = wn.isString() ? weapon(wn.asString()) : nullptr) {
            auto it = def->find(shooter.isPlayer ? "firesoundplayer" : "firesound");
            if (it != def->end() && !it->second.empty()) startSound(it->second, shooter.isPlayer ? nullptr : &start);
        }
    }
    if (hit) {
        ++stats.hits;
        const Vec3 dir = scale(sub(end, start), 1.0f / std::max(1.0f, len(sub(end, start))));
        damage(*hit, &shooter, damageAmount, dir, add(start, scale(sub(end, start), frac)), "MOD_RIFLE_BULLET");
    }
    return hit;
}

int World::weaponDamage(const std::string& weaponName) {
    const auto* w = weapon(weaponName);
    if (!w) return 35;
    auto it = w->find("damage");
    return it == w->end() ? 35 : std::max(1, std::atoi(it->second.c_str()));
}

// Picks the closest visible opposing sentient as the actor's enemy (inferred placeholder for the engine's
// perception: FOV and sight distance from the actor fields are not applied yet).
void World::updatePerception(Entity& e) {
    if (!isAlive(e) || e.obj->get("ignoreall").truthy()) return;
    const std::string team = e.obj->get("team").toString();
    const std::string other = team == "axis" ? "allies" : team == "allies" ? "axis" : "";
    if (other.empty()) return;
    const Vec3 o = e.obj->get("origin").asVector();
    Entity* best = nullptr;
    float bestD = 2048.0f * 2048.0f;
    for (const auto& cp : entities_) {
        Entity& c = *cp;
        if (!isAlive(c) || c.obj->get("team").toString() != other || c.obj->get("ignoreme").truthy()) continue;
        const Vec3 p = c.obj->get("origin").asVector();
        const float d = dot(sub(p, o), sub(p, o));
        if (d >= bestD) continue;
        if (!canSee(e, c)) continue;
        bestD = d;
        best = &c;
    }
    const Value cur = e.obj->get("enemy");
    if (best) {
        if (!(cur.isObject() && cur.asObject() == best->obj)) {
            e.obj->set("enemy", Value(best->obj));
            vm_.notify(e.obj, "enemy");
        }
    } else if (cur.isObject()) {
        Entity* ce = entityOf(cur);
        if (!ce || !isAlive(*ce)) e.obj->set("enemy", Value());
    }
}

void World::playerFire(const Vec3& eye, const Vec3& forward) {
    if (!player_ || !isAlive(*player_)) return;
    const Value w = player_->obj->get("$weapon");
    const int dmg = weaponDamage(w.isString() ? w.asString() : "kar98k");
    fireBullet(*player_, eye, add(eye, scale(forward, 8192.0f)), dmg);
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
        fireBullet(*a, start, add(start, scale(dir, 8192.0f / std::max(1.0f, len(dir)))), weaponDamage(wpn.isString() ? wpn.asString() : ""));
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
    auto trace = [this](CallContext& c, bool hitCharacters, Entity* ignore, float& fraction, Vec3& pos, Vec3& normal,
                        Entity*& hitEnt) {
        const Vec3 a = c.arg(0).asVector(), b = c.arg(1).asVector();
        const TraceResult tr = collision_->trace(a, b, {0, 0, 0}, {0, 0, 0}, Contents::Solid);
        fraction = tr.fraction;
        pos = tr.endPos;
        normal = tr.normal;
        hitEnt = nullptr;
        if (hitCharacters) {
            float f = fraction;
            if (Entity* e = traceSentients(a, b, ignore, f)) {
                fraction = f;
                pos = add(a, scale(sub(b, a), f));
                normal = scale(sub(b, a), -1.0f / std::max(1.0f, len(sub(b, a))));
                hitEnt = e;
            }
        }
    };
    vm_.registerBuiltin("bullettrace", [this, trace](CallContext& c) {
        float f;
        Vec3 pos, normal;
        Entity* hit;
        trace(c, c.arg(2).truthy(), entityOf(c.arg(3)), f, pos, normal, hit);
        auto arr = std::make_shared<Array>();
        auto put = [&](const char* k, Value v) { arr->set(ArrayKey{true, 0, k}, std::move(v)); };
        put("fraction", Value(f));
        put("position", Value(pos));
        put("normal", Value(normal));
        put("surfacetype", Value(f < 1.0f ? "default" : "none"));
        if (hit) put("entity", Value(hit->obj));
        return Value(arr);
    });
    vm_.registerBuiltin("bullettracepassed", [trace, this](CallContext& c) {
        float f;
        Vec3 pos, normal;
        Entity* hit;
        trace(c, c.arg(2).truthy(), entityOf(c.arg(3)), f, pos, normal, hit);
        return Value(f >= 1.0f);
    });
    vm_.registerBuiltin("sighttracepassed", [trace, this](CallContext& c) {
        float f;
        Vec3 pos, normal;
        Entity* hit;
        trace(c, c.arg(2).truthy(), entityOf(c.arg(3)), f, pos, normal, hit);
        return Value(f >= 1.0f);
    });
    vm_.registerBuiltin("physicstrace", [this](CallContext& c) {
        const TraceResult tr = collision_->trace(c.arg(0).asVector(), c.arg(1).asVector(), {0, 0, 0}, {0, 0, 0}, Contents::Solid);
        return Value(Vec3{tr.endPos[0], tr.endPos[1], tr.endPos[2]});
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

} // namespace iw2::game
