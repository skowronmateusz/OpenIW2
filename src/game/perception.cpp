// AI perception after the executable's per-frame actor update 0x4fe890: one sight check (0x54d0a0 / 0x54ca60), then
// enemy selection by threat (0x54e230 / 0x54dea0). docs/spec/ai.md, "Perception".
#include "game/world.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <strings.h>

namespace iw2::game {

using namespace script;

namespace {

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

float fieldFloat(const ObjectPtr& o, const char* key, float def) {
    const Value v = o->get(key);
    return v.isNumber() ? v.asFloat() : def;
}

// Team → the team it fights (table {0, 2, 1, 0, 0} in 0x54d0a0 / 0x54e230: axis ↔ allies, others none).
std::string enemyTeam(const std::string& team) {
    if (team == "axis") return "allies";
    if (team == "allies") return "axis";
    return {};
}

constexpr uint32_t kSightMask = 0x280d803; // 0x54c760

} // namespace

bool World::isSentient(const Entity& e) const {
    return (e.isPlayer || e.isActor) && isAlive(e);
}

// Eye position and (horizontal, unit) look direction. Actors: tag_eye (0x500910; origin + 64 without the tag, with
// the facing as direction). Player: origin + view height (0x4e8b70; the view height is not known to the world, so
// the stance approximation of eyePosition() is used, TODO).
// 0x54c7d0 / 0x54c760: a sight trace (mask 0x280d803) from `from` to `to` that ignores the actor and the target and
// must be clear, and the visibility through effects (0x48b090) must be at least 0.2 (0x592020). With a non-zero
// `maxDistSq` (maxsightdistsqrd, +0x1770) the 3D distance² must not exceed it, nor the fog limit 0x1c61038 (FLT_MAX at
// level start, 0x4d9c93; the fog command 0x4e96e0 sets it to ((far - near) * 0.828 + near)² for linear fog, density 1,
// and back to FLT_MAX otherwise); without a maxsightdistsqrd neither limit applies (0x54c7d0 jumps to 0x54c821).
bool World::actorSightClear(Entity& actor, Entity& target, const Vec3& from, const Vec3& to, float maxDistSq) {
    if (maxDistSq != 0.0f) {
        const float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > maxDistSq || d2 > fogSightSq_) return false;
    }
    if (collision_->sightBlocked(from, to, {0, 0, 0}, {0, 0, 0}, kSightMask, actor.number, target.number)) return false;
    return !(fxVisibility(from, to) < 0.2f);
}

Vec3 World::sightEye(Entity& e, Vec3* forward) {
    if (e.eyeFrameMs != timeMs_) {
        e.eyeFrameMs = timeMs_;
        const Vec3 o = e.obj->get("origin").asVector();
        const Value ang = e.obj->get("angles");
        const float yaw = ang.isVector() ? ang.asVector()[1] * float(M_PI) / 180.0f : 0.0f;
        e.eyeForward = {std::cos(yaw), std::sin(yaw), 0};
        e.eyePos = e.isActor ? Vec3{o[0], o[1], o[2] + 64.0f} : eyePosition(e);
        const Value model = e.obj->get("model");
        const assets::XModelParts* parts = e.isActor && model.isString() ? skeleton(model.asString()) : nullptr;
        int eyeBone = -1;
        if (parts)
            for (size_t b = 0; b < parts->bones.size(); ++b)
                if (strcasecmp(parts->bones[b].name.c_str(), "tag_eye") == 0) { eyeBone = int(b); break; }
        if (eyeBone >= 0) {
            const assets::XPose pose = e.anim ? e.anim->pose(*parts) : assets::bindPose(*parts);
            const assets::Quat er{0, 0, std::sin(yaw * 0.5f), std::cos(yaw * 0.5f)};
            const Vec3 p = assets::rotate(er, pose.position[size_t(eyeBone)]);
            e.eyePos = {o[0] + p[0], o[1] + p[1], o[2] + p[2]};
            Vec3 f = assets::rotate(assets::multiply(er, pose.rotation[size_t(eyeBone)]), {1, 0, 0});
            const float l = std::sqrt(f[0] * f[0] + f[1] * f[1]);
            if (l > 0) e.eyeForward = {f[0] / l, f[1] / l, 0};
        }
    }
    if (forward) *forward = e.eyeForward;
    return e.eyePos;
}

// Distance and field-of-view test (0x54c840). maxDistSq 0 means unlimited; fovCos 0 means no cone.
// Like 0x54c7d0, the fog sight limit (0x1c61038, 0x4e96e0) applies only with a non-zero maxDistSq.
bool World::sightTest(Entity&, const Vec3& eye, const Vec3& forward, const Vec3& target, float fovCos, float maxDistSq) {
    const Vec3 d = sub(target, eye);
    const float distSq = dot(d, d);
    if (maxDistSq != 0.0f && (maxDistSq < distSq || distSq > fogSightSq_)) return false;
    if (fovCos != 0.0f) {
        const float f = dot(forward, d);
        if (f < 0.0f) return false;
        if (f * f < distSq * fovCos * fovCos) return false;
    }
    return true;
}

// Sight check of one sentient (0x54ca60) and the target's reciprocal knowledge when it is an actor. Returns
// whether a trace was made.
bool World::sightCheck(Entity& actor, Entity& target) {
    Vec3 fwd;
    const Vec3 eye = sightEye(actor, &fwd);
    Vec3 targetFwd;
    const Vec3 targetEye = sightEye(target, &targetFwd);
    const Value cur = actor.obj->get("enemy"), fav = actor.obj->get("favoriteenemy");
    const bool focus = (cur.isObject() && cur.asObject() == target.obj) || (fav.isObject() && fav.asObject() == target.obj);
    const float fov = focus ? 0.0f : fieldFloat(actor.obj, "fovcosine", 0.0f);
    const float maxd = fieldFloat(actor.obj, "maxsightdistsqrd", 0.0f);
    bool vis = false, traced = false;
    if (sightTest(actor, eye, fwd, targetEye, fov, maxd)) {
        traced = true;
        // 0x54c760 → 0x44c3c0: a sight trace that ignores the actor and the target, then visibility ≥ 0.2 (0x48b090).
        vis = actorSightClear(actor, target, eye, targetEye, 0.0f);
    }
    SentientInfo& info = actor.sentientInfo[target.obj.get()];
    info.visible = vis;
    info.lastCheckMs = timeMs_;
    if (!vis) return traced;
    const bool onTurret = actor.usingTurret != nullptr; // 0x54afd0 (TODO: also check turret ownership)
    info.lastSeenMs = timeMs_;
    if (!onTurret) info.ignoreUntilMs = 0;
    auto remember = [this](Entity& who, Entity& about, SentientInfo& in) { // 0x500c50
        in.lastKnownMs = timeMs_;
        in.lastKnownPos = about.obj->get("origin").asVector();
        in.ownPosThen = who.obj->get("origin").asVector();
    };
    remember(actor, target, info);
    if (target.isActor && !target.usingTurret) {
        const bool tvis = sightTest(target, targetEye, targetFwd, eye, fieldFloat(target.obj, "fovcosine", 0.0f),
                                    fieldFloat(target.obj, "maxsightdistsqrd", 0.0f));
        SentientInfo& back = target.sentientInfo[actor.obj.get()];
        back.visible = tvis;
        back.lastCheckMs = timeMs_;
        if (tvis) {
            back.lastSeenMs = timeMs_;
            back.ignoreUntilMs = 0;
            remember(target, actor, back);
        }
    }
    return traced;
}

int World::nearestNodeWithin(const Vec3& p, float radius) const {
    int best = -1;
    float bestD = radius * radius;
    for (size_t i = 0; i < paths_.size(); ++i) {
        const auto& q = paths_.position(i);
        const float d = (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]);
        if (d <= bestD) { bestD = d; best = int(i); }
    }
    return best;
}

// Threat of a sentient for the actor (0x54dea0). INT_MIN: ignore it.
int World::threatOf(Entity& actor, Entity& target) {
    SentientInfo& info = actor.sentientInfo[target.obj.get()];
    if (actor.obj->get("pacifist").truthy()) {
        const int64_t wait = int64_t(fieldFloat(actor.obj, "pacifistwait", 0.0f) * 1000.0f);
        if (info.lastAttackedMs == 0 || wait <= timeMs_ - info.lastAttackedMs) return std::numeric_limits<int>::min();
    }
    const Vec3 a = actor.obj->get("origin").asVector(), b = target.obj->get("origin").asVector();
    const float dist = std::sqrt(dot(sub(a, b), sub(a, b)));
    const Value cur = actor.obj->get("enemy");
    const bool current = cur.isObject() && cur.asObject() == target.obj;
    const bool vis = info.visible;
    bool aware = vis, recent = false;
    if (!vis) {
        if (current) {
            const Vec3 d = sub(info.lastKnownPos, b);
            int64_t t = timeMs_ - info.lastKnownMs;
            if (dot(d, d) < 4096.0f) {
                // Both near path nodes that see each other (0x515040 within 192 units, 0x517e40 node visibility).
                const int na = nearestNodeWithin(a, 192.0f), nb = nearestNodeWithin(b, 192.0f);
                if (na >= 0 && nb >= 0) {
                    bool nodeVis = na == nb;
                    if (!nodeVis) {
                        const size_t n = paths_.size(), lo = size_t(std::min(na, nb)), hi = size_t(std::max(na, nb));
                        const size_t bit = n * lo + hi;
                        const auto& bits = paths_.visibilityBits();
                        nodeVis = bit / 8 < bits.size() && (bits[bit / 8] >> (bit % 8) & 1);
                    }
                    if (nodeVis) aware = true;
                }
            }
            if (!aware) recent = t < 10000;
        } else {
            aware = timeMs_ - info.lastKnownMs < 5000;
        }
    }
    int score = target.obj->get("threatbias").isNumber() ? target.obj->get("threatbias").asInt() : 0;
    score += vis ? 1000 : aware ? 500 : recent ? 250 : 0;
    // TODO: accuracy term clamp(-100 · (accuracy(actor, dist) − accuracy(target, dist)), −500, 1000) from the
    // weapons' accuracy graphs and the player accuracy cvars (0x550da0); 0 here.
    score += dist >= 2000.0f ? 0 : int(std::lround((2000.0f - dist) * (2000.0f - dist) * 0.00125f)); // 0x54de40
    int count = target.attackers;
    if (current) --count;
    int attackersTerm = count * -100;
    if (attackersTerm + 400 < 0) attackersTerm = -400;
    score += attackersTerm;
    if (current) score += aware ? 1000 : recent ? 500 : 100;
    return score;
}

// Enemy change (0x515150): attacker counts follow, and the actor is notified "enemy".
void World::setEnemy(Entity& actor, Entity* enemy, bool notify) {
    Entity* old = entityOf(actor.obj->get("enemy"));
    if (old == enemy) return;
    if (old) --old->attackers;
    if (enemy) ++enemy->attackers;
    actor.obj->set("enemy", enemy ? Value(enemy->obj) : Value());
    if (notify) vm_.notify(actor.obj, "enemy");
}

// Best enemy by threat (0x54e230).
void World::selectEnemy(Entity& actor) {
    const std::string other = enemyTeam(actor.obj->get("team").toString());
    if (other.empty()) return;
    const bool onTurret = actor.usingTurret != nullptr;
    const Value fav = actor.obj->get("favoriteenemy");
    int bestScore = std::numeric_limits<int>::min() + 1;
    Entity* best = nullptr;
    bool bestAware = onTurret;
    for (const auto& ep : entities_) {
        Entity& t = *ep;
        if (!isSentient(t) || t.obj->get("team").toString() != other || t.obj->get("ignoreme").truthy()) continue;
        auto it = actor.sentientInfo.find(t.obj.get());
        if (it == actor.sentientInfo.end() || it->second.lastKnownMs <= 0) continue;
        const SentientInfo& info = it->second;
        const bool aware = (onTurret || (info.lastSeenMs != 0 && timeMs_ - info.lastSeenMs < 5000)) && info.ignoreUntilMs <= timeMs_;
        if (!aware && bestAware) continue;
        const int threat = threatOf(actor, t);
        if (threat == std::numeric_limits<int>::min()) continue;
        const bool favorite = fav.isObject() && fav.asObject() == t.obj;
        if (bestScore < threat || (!bestAware && aware) || favorite) {
            best = &t;
            bestAware = aware;
            bestScore = favorite ? 0x7ffffffe : threat;
        }
    }
    setEnemy(actor, best, true);
}

// Per-frame perception of one actor (0x4fe890 → 0x54d0a0, 0x54e230).
void World::updatePerception(Entity& e) {
    if (!isAlive(e)) return;
    // A dead or removed enemy is dropped with an "enemy" notify (sentient release 0x4fe170).
    if (Entity* cur = entityOf(e.obj->get("enemy")); cur && !isSentient(*cur)) setEnemy(e, nullptr, true);
    const std::string other = enemyTeam(e.obj->get("team").toString());
    if (other.empty()) return;
    // Candidates by priority (1 / distance) · max(now − last check − 100 ms, 0), highest first; the first one due
    // for a check that passes the distance/field-of-view test gets the one sight trace of this frame.
    const Vec3 o = e.obj->get("origin").asVector();
    std::vector<std::pair<float, Entity*>> cands;
    for (const auto& ep : entities_) {
        Entity& t = *ep;
        if (&t == &e || !isSentient(t) || t.obj->get("team").toString() != other) continue;
        const Vec3 d = sub(t.obj->get("origin").asVector(), o);
        const float distSq = dot(d, d);
        if (distSq == 0.0f) continue;
        const SentientInfo& info = e.sentientInfo[t.obj.get()];
        const int64_t since = std::max<int64_t>(timeMs_ - info.lastCheckMs - 100, 0);
        cands.push_back({float(since) / std::sqrt(distSq), &t});
    }
    std::stable_sort(cands.begin(), cands.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    for (const auto& [prio, t] : cands) {
        const SentientInfo& info = e.sentientInfo[t->obj.get()];
        if (info.lastCheckMs != 0 && info.lastCheckMs >= timeMs_) continue;
        if (sightCheck(e, *t)) break; // one sight trace per frame (trace counter actor +0xd4)
    }
    selectEnemy(e);
}

} // namespace iw2::game
