// Multiplayer bots on the map's mounted machine guns (docs/spec/mp-bots.md "Turrets"). An OpenIW2 extension, implemented
// from the spec. A bot uses a turret as a player does (docs/spec/turrets.md "Multiplayer"): it walks behind the gun,
// looks at it, presses use when the use search of its client offers the turret, aims with its view angles, fires with
// the attack button and gets off with another press of use.
#include "game/mpbots.h"
#include "game/world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace iw2::game {

using script::Value;

namespace {
constexpr float kDeg = 3.14159265358979f / 180.0f;
float wrap180(float a) {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}
float dist3(const Vec3& a, const Vec3& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}
float dist2(const Vec3& a, const Vec3& b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }
} // namespace

bool botTurretCovers(const BotTurret& t, const std::array<float, 3>& p, float margin) {
    const float yaw = std::atan2(p[1] - t.origin[1], p[0] - t.origin[0]) / kDeg;
    return std::fabs(wrap180(yaw - t.centreYaw)) <= t.halfArc + margin;
}

BotTurretLeave botTurretLeave(const BotTurretIn& in) {
    if (in.flanked) return BotTurretLeave::Flanked;
    if (in.health < 35) return BotTurretLeave::Hurt;
    if (in.enemyOutsideArcMs > 1000) return BotTurretLeave::OutOfArc;
    if (in.quietMs > in.holdMs || in.onMs > in.maxMs) return BotTurretLeave::Quiet;
    return BotTurretLeave::Stay;
}

// The map's usable turrets with the place to use each from and how much of the map each overlooks (once per map).
void World::mpBotBuildTurrets() {
    if (bots_->turretsBuilt) return;
    bots_->turretsBuilt = true;
    const NavGraph& nav = mpNav();
    const size_t mainCount = nav.mainCount ? nav.mainCount : nav.size();
    const size_t stride = std::max<size_t>(1, mainCount / 600);
    for (const auto& ep : entities_) {
        Entity& e = *ep;
        if (!e.turret || e.obj->removed || e.isSpawner || !(e.turret->flags & 0x1000u)) continue;
        const TurretState& ts = *e.turret;
        BotTurret t;
        t.entity = e.number;
        t.origin = e.obj->get("origin").asVector();
        const Vec3 ta = e.obj->get("angles").isVector() ? e.obj->get("angles").asVector() : Vec3{0, 0, 0};
        // The middle of the arcs a user gets (docs/spec/turrets.md: the setup arcs are restored at the use).
        t.halfArc = (std::fabs(ts.setupRightArc) + std::fabs(ts.setupLeftArc)) * 0.5f;
        t.centreYaw = wrap180(ta[1] + ts.setupRightArc + t.halfArc);
        const Vec3 dir{std::cos(t.centreYaw * kDeg), std::sin(t.centreYaw * kDeg), 0};
        // The place to use it from: the graph node nearest the point 40 units behind the gun that is behind the gun (the
        // use's arc test), whose standing eye is within the use reach of the turret's box centre with a clear line.
        const Vec3 ideal{t.origin[0] - dir[0] * 40.0f, t.origin[1] - dir[1] * 40.0f, t.origin[2]};
        const Vec3 centre{t.origin[0], t.origin[1], t.origin[2] + 28.0f};
        float best = 1e9f;
        for (int n : nav.within(Vec3{ideal[0], ideal[1], ideal[2] - 30.0f}, 140.0f)) {
            if (size_t(n) >= mainCount) continue;
            const auto& q = nav.nodes[size_t(n)];
            if (q[2] > t.origin[2] + 20.0f || q[2] < t.origin[2] - 75.0f) continue;
            const float fd = dist2(q, t.origin);
            if (fd < 1.0f) continue;
            const float cosA = ((t.origin[0] - q[0]) * dir[0] + (t.origin[1] - q[1]) * dir[1]) / fd;
            if (std::acos(std::clamp(cosA, -1.0f, 1.0f)) / kDeg > t.halfArc) continue;
            const Vec3 eye{q[0], q[1], q[2] + 60.0f};
            if (dist3(eye, centre) > 68.0f) continue;
            if (collision_->sightBlocked(eye, centre, {0, 0, 0}, {0, 0, 0}, 0x11, e.number, 0x7ff)) continue;
            const float d = dist2(q, ideal);
            if (d < best) best = d, t.node = n, t.usePos = {q[0], q[1], q[2]};
        }
        // What the gun overlooks: sampled graph nodes 200..3000 units away inside the arcs with a clear line from the gun.
        const Vec3 gun{t.origin[0], t.origin[1], t.origin[2] + 16.0f};
        for (size_t i = 0; i < mainCount; i += stride) {
            const auto& q = nav.nodes[i];
            const float d = dist2(q, t.origin);
            if (d < 200.0f || d > 3000.0f || !botTurretCovers(t, q, 0.0f)) continue;
            if (collision_->trace(gun, Vec3{q[0], q[1], q[2] + 40.0f}, {0, 0, 0}, {0, 0, 0}, Contents::SightMaskNoCharacters, e.number).fraction >= 0.999f)
                ++t.coverage;
        }
        bots_->turrets.push_back(t);
        if (std::getenv("IW2_BOT_STUCKDBG"))
            std::fprintf(stderr, "TURRET %d at %.0f %.0f %.0f arcs %.0f +- %.0f use node %d (%.0f %.0f %.0f) coverage %d\n", t.entity, t.origin[0], t.origin[1],
                         t.origin[2], t.centreYaw, t.halfArc, t.node, t.usePos[0], t.usePos[1], t.usePos[2], t.coverage);
    }
}

const std::vector<BotTurret>& World::mpBotTurrets() {
    if (!bots_) bots_ = std::make_shared<BotSystem>();
    mpBotBuildTurrets();
    return bots_->turrets;
}

// Chooses a turret to walk to (once a second) and follows that detour; `obj` becomes the place behind the gun. Returns
// true while such a detour is on.
bool World::mpBotTurretDetour(MpBot& b, Entity& self, const Vec3& eye, bool engage, bool unarmed, BotObjective& obj) {
    const int64_t now = timeMs_;
    BotDetour& d = b.detour;
    if (b.turretIgnoreUntilMs.size() != bots_->turrets.size()) b.turretIgnoreUntilMs.assign(bots_->turrets.size(), 0);
    if (d.kind == BotDetour::Turret) {
        const BotTurret& t = bots_->turrets[size_t(d.turret)];
        Entity* te = entityByNumber(t.entity);
        const bool gone = !te || te->obj->removed || !te->turret || te->turret->inUse;
        const bool timedOut = now >= d.untilMs;
        if (timedOut) { // not offered by the use search from here: not again for a while, and after two tries never
            if (size_t(d.turret) < b.turretIgnoreUntilMs.size()) b.turretIgnoreUntilMs[size_t(d.turret)] = now + 60000;
            if (dist2(d.pos, b.ps.origin) < 40.0f && ++bots_->turrets[size_t(d.turret)].failures >= 2) bots_->turrets[size_t(d.turret)].node = -1;
        }
        // A fight on the way: only a bot close to the gun (or without a round) goes on.
        if (gone || timedOut || (engage && !unarmed && dist2(d.pos, b.ps.origin) > 220.0f)) d = {};
    }
    // In a fight only against an enemy more than 500 units away, for a gun within 200 units that overlooks it.
    const bool fightFar = engage && !unarmed && dist3(b.enemyLastPos, b.ps.origin) > 500.0f;
    if (d.kind == BotDetour::None && b.combat && !b.noNewDetour && now >= b.turretCheckMs && now >= b.turretCooldownMs && (!engage || unarmed || fightFar)) {
        b.turretCheckMs = now + 1000 + (b.client * 53) % 200;
        mpBotBuildTurrets();
        if (b.turretIgnoreUntilMs.size() != bots_->turrets.size()) b.turretIgnoreUntilMs.assign(bots_->turrets.size(), 0);
        const std::string role = obj.valid ? obj.what : "";
        const bool guard = role.rfind("guard", 0) == 0 || role == "hold radio";
        float bestScore = 0;
        int best = -1;
        for (size_t i = 0; i < bots_->turrets.size(); ++i) {
            const BotTurret& t = bots_->turrets[i];
            if (t.node < 0 || t.coverage < 12) continue;
            if (now < b.turretIgnoreUntilMs[i]) continue;
            Entity* te = entityByNumber(t.entity);
            if (!te || te->obj->removed || !te->turret || te->turret->inUse) continue;
            const float dist = dist3(t.usePos, b.ps.origin);
            if (dist > (unarmed ? 1200.0f : fightFar ? 200.0f : 600.0f)) continue;
            // A team mate already on its way to this gun (in a team game): left to that one.
            bool taken = false;
            if (mp_->gametype != "dm")
                for (const MpBot& o : bots_->bots)
                    if (o.client != b.client && o.detour.kind == BotDetour::Turret && o.detour.turret == int(i) &&
                        size_t(o.client) < bots_->clients.size() && bots_->clients[size_t(o.client)].team == bots_->clients[size_t(b.client)].team)
                        taken = true;
            if (taken) continue;
            const Vec3 gun{t.origin[0], t.origin[1], t.origin[2] + 16.0f};
            auto overlooks = [&](const Vec3& p) {
                const float pd = dist2(p, t.origin);
                if (pd < 150.0f || pd > 3000.0f || !botTurretCovers(t, p, -5.0f)) return false;
                return collision_->trace(gun, Vec3{p[0], p[1], p[2] + 40.0f}, {0, 0, 0}, {0, 0, 0}, Contents::SightMaskNoCharacters, t.entity).fraction >= 0.999f;
            };
            float score = 0;
            // An enemy known in the last 8 s where the gun overlooks it, and not close to the bot itself.
            if (b.enemy >= 0 && now - b.enemySeenMs < 8000 && dist3(b.enemyLastPos, b.ps.origin) > 400.0f && overlooks(b.enemyLastPos)) score = 1.0f;
            // A guard whose objective the gun overlooks.
            else if (fightFar) continue;
            else if (guard && dist3(obj.pos, b.ps.origin) < 700.0f && overlooks(obj.pos)) score = 0.7f;
            // Nothing known: now and then the gun on the bot's way, when it overlooks much.
            else if (b.enemy < 0 && dist < 350.0f && t.coverage >= 25 && nextRand() % 100 < (unarmed ? 90 : 30)) score = 0.4f;
            if (unarmed && score < 0.5f && b.enemy < 0) score = 0.5f; // without a round a gun with endless ammunition is welcome
            score *= 1.0f - dist / 2400.0f;
            if (score > bestScore) bestScore = score, best = int(i);
        }
        if (best >= 0) {
            const BotTurret& t = bots_->turrets[size_t(best)];
            d = {};
            d.kind = BotDetour::Turret;
            d.entity = t.entity;
            d.turret = best;
            d.pos = t.usePos;
            d.radius = 12.0f;
            d.use = true;
            d.sinceMs = now;
            d.untilMs = now + 5000 + int64_t(dist3(t.usePos, b.ps.origin) * 10.0f);
            b.path.clear();
            b.pathIdx = 0;
        }
    }
    (void)eye;
    (void)self;
    if (d.kind != BotDetour::Turret) return false;
    obj = BotObjective{};
    obj.valid = true;
    obj.pos = d.pos;
    obj.radius = d.radius;
    obj.what = "turret";
    return true;
}

// One frame of a bot on a turret: the view (kept inside the arcs) follows the enemy in sight or sweeps the arcs, attack
// fires in bursts, use gets off (botTurretLeave). The turret code holds the bot behind the gun; its own movement and
// hand weapon rest.
void World::mpBotTurretFrame(MpBot& b, Entity& self, Entity* enemy, const Vec3& eye) {
    const int64_t now = timeMs_;
    MpClient& cl = mp_->clients[size_t(b.client)];
    const BotSkill& sk = botSkill(b.skill);
    Entity* te = cl.turretUse.turret;
    if (!te || !te->turret) return;
    int idx = -1;
    for (size_t i = 0; i < bots_->turrets.size(); ++i)
        if (bots_->turrets[i].entity == te->number) idx = int(i);
    if (!b.onTurret) { // just got on
        b.onTurret = true;
        b.detour = {};
        b.turretSinceMs = now;
        b.turretShotBase = te->turret->fireCount;
        b.turretFlanked = false;
        b.turretOutSinceMs = -1;
        // How long the bot stays without seeing anybody: 12..25 s, a guard 30..60 s.
        const bool guard = b.objective.rfind("guard", 0) == 0 || b.objective == "hold radio";
        b.turretHoldMs = guard ? 30000 + int(nextRand() % 30000) : 12000 + int(nextRand() % 13000);
        b.turretBurstUntilMs = 0;
        b.turretPauseUntilMs = 0;
        ++b.stats.turretMounts;
    }
    ++b.stats.turretFrames;
    b.stats.turretShots += te->turret->fireCount - b.turretShotBase;
    b.turretShotBase = te->turret->fireCount;
    b.path.clear();
    b.pathIdx = 0;
    b.traverseSinceMs = -1;
    // The stuck watches rest: the bot sits behind the gun on purpose.
    b.progressPos = b.anchorPos = b.ps.origin;
    b.progressMs = b.anchorMs = b.stillSinceMs = b.offGraphSinceMs = now;
    b.stuck = 0;
    b.stallSinceMs = -1; // not a bot that wants to walk and stays on the spot

    // The view's limits: the turret's arcs as the use set them (the view clamp of the player state).
    const float baseYaw = cl.turretUse.clampBase[1], rangeYaw = cl.turretUse.clampRange[1];
    const float basePitch = wrap180(cl.turretUse.clampBase[0]), rangePitch = cl.turretUse.clampRange[0];
    float wantYaw = baseYaw, wantPitch = basePitch;
    bool onTarget = false;
    const bool fighting = enemy && b.combat;
    float outside = 0;
    if (fighting || (b.enemy >= 0 && now - b.enemySeenMs < 3000)) {
        Vec3 at = b.enemyLastPos;
        at[2] += 40.0f;
        if (fighting) {
            at = mpBotAimPoint(b, *enemy);
            // The hand's error on the gun's grips: half of the hand weapon's (the gun rests on its mount).
            if (!b.aimValid) b.aimValid = true, b.aim.onTargetSinceMs = now;
            BotAimNoiseIn ni;
            ni.hurtMs = int(std::min<int64_t>(now - b.hurtMs, 100000));
            mpBotAimNoise(b, ni, 0.5f);
        }
        const float dx = at[0] - eye[0], dy = at[1] - eye[1], dz = at[2] - eye[2];
        wantYaw = std::atan2(dy, dx) / kDeg;
        wantPitch = -std::atan2(dz, std::sqrt(dx * dx + dy * dy)) / kDeg;
        outside = std::max(0.0f, std::fabs(wrap180(wantYaw - baseYaw)) - rangeYaw);
    } else {
        // Nobody known: a slow sweep over the arcs.
        wantYaw = baseYaw + rangeYaw * 0.75f * std::sin(float(now) * 0.0007f + float(b.client));
    }
    wantYaw = baseYaw + std::clamp(wrap180(wantYaw - baseYaw), -rangeYaw, rangeYaw);
    wantPitch = basePitch + std::clamp(wrap180(wantPitch - basePitch), -rangePitch, rangePitch);
    if (!fighting) b.aimValid = false;
    mpBotTurnView(b, wantYaw, wantPitch, fighting);
    const float err = std::fabs(wrap180(wantYaw - b.aim.yaw)) + std::fabs(wantPitch - b.aim.pitch); // of the angles the bot steers
    onTarget = fighting && outside <= 0 && now >= b.reactAtMs && err < 4.0f;
    // Bursts of 0.6..1.6 s with pauses by the skill; a lost enemy is fired at for a second more (the place it left).
    const bool suppress = !fighting && b.combat && b.enemy >= 0 && now - b.enemySeenMs < 1000 && err < 4.0f;
    if ((onTarget || suppress) && now >= b.turretPauseUntilMs) {
        if (now >= b.turretBurstUntilMs && b.turretBurstUntilMs != 0) {
            b.turretPauseUntilMs = now + sk.burstPauseMs + int(nextRand() % 300);
            b.turretBurstUntilMs = 0;
        } else {
            if (b.turretBurstUntilMs == 0) b.turretBurstUntilMs = now + 600 + int(nextRand() % 1000);
            b.buttons |= 0x1u;
        }
    } else if (!onTarget && !suppress) {
        b.turretBurstUntilMs = 0;
    }

    // Getting off (botTurretLeave).
    if (fighting && outside > 0) {
        if (b.turretOutSinceMs < 0) b.turretOutSinceMs = now;
    } else {
        b.turretOutSinceMs = -1;
    }
    BotTurretIn in;
    in.flanked = b.turretFlanked;
    in.health = self.obj->get("health").isNumber() ? self.obj->get("health").asInt() : 100;
    in.enemyOutsideArcMs = b.turretOutSinceMs < 0 ? 0 : int(now - b.turretOutSinceMs);
    in.quietMs = int(std::min<int64_t>(now - std::max(b.turretSinceMs, b.enemySeenMs), 10000000));
    in.holdMs = b.turretHoldMs;
    in.onMs = int(std::min<int64_t>(now - b.turretSinceMs, 10000000));
    in.maxMs = b.turretHoldMs * 3; // 36..75 s on the gun at most (a guard 90..180 s)
    const BotTurretLeave leave = botTurretLeave(in);
    if (leave != BotTurretLeave::Stay && !b.usePrev) {
        b.buttons = 0x8u; // use: off the gun
        b.turretCooldownMs = now + (leave == BotTurretLeave::Quiet ? 20000 : 30000);
        if (idx >= 0 && size_t(idx) < b.turretIgnoreUntilMs.size()) b.turretIgnoreUntilMs[size_t(idx)] = now + 60000;
        if (leave == BotTurretLeave::Flanked || leave == BotTurretLeave::Hurt) b.strafeFlipMs = 0;
    }
    self.obj->set("angles", Value(Vec3{0, b.yaw, 0}));
    self.velocity = {0, 0, 0};
    cl.animInput.valid = true;
    cl.animInput.move.xySpeed = 0;
    cl.animInput.move.forwardMove = cl.animInput.move.rightMove = 0;
}

// Damage to a bot on a turret from a direction the gun cannot turn to: flanked.
void World::mpBotTurretDamaged(MpBot& b, const Vec3& from) {
    const MpClient& cl = mp_->clients[size_t(b.client)];
    if (!b.onTurret || !cl.turretUse.turret) return;
    const float yaw = std::atan2(from[1] - b.ps.origin[1], from[0] - b.ps.origin[0]) / kDeg;
    if (std::fabs(wrap180(yaw - cl.turretUse.clampBase[1])) > cl.turretUse.clampRange[1] + 5.0f) b.turretFlanked = true;
}

} // namespace iw2::game
