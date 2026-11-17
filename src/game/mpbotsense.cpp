// Multiplayer bot perception (docs/spec/mp-bots.md "Perception", "Hearing"). An OpenIW2 extension, implemented from the
// spec. A bot knows what a player at its place could: what is inside the view of its screen at the current zoom with a
// clear line (noticed after a time that grows towards the edge of the view and with the distance), and what it hears or
// is hit by, as a delayed, imprecise place it has to look at.
#include "game/mpbots.h"
#include "game/viewweapon.h"
#include "game/world.h"

#include <algorithm>
#include <cmath>

namespace iw2::game {

using script::Value;

namespace {
constexpr float kDeg = 3.14159265358979f / 180.0f;
Vec3 v3(const Value& v) { return v.isVector() ? v.asVector() : Vec3{0, 0, 0}; }
float dist3(const Vec3& a, const Vec3& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}
} // namespace

float botNoticeMs(const BotNoticeIn& in) {
    float t = float(in.baseMs) * (1.0f + 2.0f * in.ecc * in.ecc);       // three times as long at the edge of the view
    const float eff = in.dist * std::clamp(in.fov43, 1.0f, 160.0f) / 80.0f; // a zoomed view brings the target closer
    t *= 1.0f + (eff / 1200.0f) * (eff / 1200.0f);
    if (in.targetFired) t *= 0.4f;       // a muzzle flash
    else if (in.targetMoving) t *= 0.7f; // movement catches the eye
    else if (in.targetProne) t *= 2.0f;
    else if (in.targetCrouched) t *= 1.3f;
    if (in.alerted) t *= 0.5f;           // looking for somebody there
    return std::max(50.0f, t);
}

std::array<float, 3> botHeardPosition(const std::array<float, 3>& listener, const std::array<float, 3>& source, float yawErrDeg,
                                      float distScale) {
    const float dx = source[0] - listener[0], dy = source[1] - listener[1];
    const float c = std::cos(yawErrDeg * kDeg), s = std::sin(yawErrDeg * kDeg);
    return {listener[0] + (dx * c - dy * s) * distScale, listener[1] + (dx * s + dy * c) * distScale,
            listener[2] + (source[2] - listener[2]) * distScale};
}

// The bot's view now: the field of view of its screen (the weapon's zoom by the aim fraction, the turret view on a
// turret) and, through a scope (a weapon with an aim overlay, aimed), only the scope's circle.
BotView World::mpBotView(const MpBot& b, const std::string& weaponName, float& fov43) {
    fov43 = 80.0f;
    bool scoped = false;
    if (mpClientOnTurret(b.client)) {
        fov43 = kTurretViewFov;
    } else if (!weaponName.empty() && weaponName != "none" && b.weapon.name() == weaponName) {
        const BotWeaponInfo& wi = mpBotWeaponInfo(weaponName);
        fov43 = adsFov(80.0f, wi.adsZoomFov, wi.adsZoomInFrac, wi.adsZoomOutFrac, b.weapon.adsFrac(), b.weapon.adsZoomingIn());
        scoped = wi.scope && b.weapon.adsFrac() >= 0.5f;
    }
    BotView v = botViewAngles(fov43);
    if (scoped) v.halfYawDeg = v.halfPitchDeg; // the overlay is black outside the scope's circle
    return v;
}

// Whether smoke stands between two points: the line passes within 150 units of the middle of a smoke grenade's cloud
// (from 1.5 s to 20 s after it went off).
bool World::mpBotSmokeBlocks(const Vec3& a, const Vec3& b) const {
    if (!bots_ || bots_->smokes.empty()) return false;
    for (const auto& s : bots_->smokes) {
        const int64_t age = timeMs_ - s.second;
        if (age < 1500 || age > 20000) continue;
        const Vec3 c{s.first[0], s.first[1], s.first[2] + 60.0f};
        const Vec3 ab{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
        float t = l2 > 0 ? ((c[0] - a[0]) * ab[0] + (c[1] - a[1]) * ab[1] + (c[2] - a[2]) * ab[2]) / l2 : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        if (dist3(Vec3{a[0] + ab[0] * t, a[1] + ab[1] * t, a[2] + ab[2] * t}, c) < 150.0f) return true;
    }
    return false;
}

void World::mpBotSmoke(const Vec3& at) {
    if (!bots_) return;
    auto& v = bots_->smokes;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& s) { return timeMs_ - s.second > 20000; }), v.end());
    v.emplace_back(at, timeMs_);
}

// Something heard or felt: the place the bot believes the enemy at, known to it after `delayMs`. A later one replaces a
// pending one only when it is more urgent (damage before gunfire before steps).
void World::mpBotAlert(MpBot& b, int entity, const Vec3& believed, int delayMs, int urgency) {
    if (b.alert.valid && b.alert.urgency > urgency && timeMs_ < b.alert.atMs) return;
    b.alert.valid = true;
    b.alert.entity = entity;
    b.alert.pos = believed;
    b.alert.atMs = timeMs_ + delayMs;
    b.alert.urgency = urgency;
}

// Perception of one frame. Returns the enemy the bot sees and fights (nullptr none) and keeps b.enemy, b.enemyVisible,
// b.enemySeenMs, b.enemyLastPos (the believed place), b.enemyParts and b.reactAtMs.
Entity* World::mpBotSense(MpBot& b, Entity& self, const Vec3& eye, const std::string& weaponName) {
    const int64_t now = timeMs_;
    const BotSkill& sk = botSkill(b.skill);
    const bool teams = mp_->gametype != "dm";
    const std::vector<BotClientInfo>& infos = bots_->clients; // this frame's clients (mpBotsFrame)
    const int myTeam = size_t(b.client) < infos.size() ? infos[size_t(b.client)].team : 0;
    float fov43 = 80.0f;
    const BotView view = mpBotView(b, weaponName, fov43);
    // The search for new enemies runs every other frame (half of the bots each frame, docs/spec/mp-bots.md "Cost
    // limits"); the enemy the bot fights is looked at every frame.
    const bool fullScan = ((bots_->frameCount + b.client) & 1) == 0;
    const float frameMs = fullScan ? 100.0f : 50.0f;
    if (b.notice.size() != mp_->clients.size()) b.notice.assign(mp_->clients.size(), 0.0f), b.noticeSeenMs.assign(mp_->clients.size(), -100000);
    Entity* best = nullptr;
    float bestScore = 1e9f;
    int bestParts = 0;
    // A search looks at the five nearest enemies inside the view (and the fought one): attention is limited, and so is the
    // number of sight traces (docs/spec/mp-bots.md "Cost limits").
    float nearLimit = 1e9f;
    if (fullScan) {
        float near[5] = {1e9f, 1e9f, 1e9f, 1e9f, 1e9f};
        for (size_t c = 0; c < infos.size(); ++c) {
            const BotClientInfo& ci = infos[c];
            if (int(c) == b.client || !ci.playing || (teams && ci.team == myTeam)) continue;
            const Vec3 d{ci.eye[0] - eye[0], ci.eye[1] - eye[1], ci.eye[2] - eye[2]};
            const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (dl > sk.sightDist || dl >= near[4]) continue;
            const auto off = botViewOffset(d, b.yaw, b.pitch);
            if (std::fabs(off[0]) > view.halfYawDeg + 10.0f || std::fabs(off[1]) > view.halfPitchDeg + 10.0f) continue;
            int k = 4;
            for (; k > 0 && near[k - 1] > dl; --k) near[k] = near[k - 1];
            near[k] = dl;
        }
        nearLimit = near[4];
    }
    for (size_t c = 0; c < infos.size(); ++c) {
        const BotClientInfo& ci = infos[c];
        const MpClient& oc = mp_->clients[c];
        Entity* t = ci.ent;
        const bool isCurrent = t && t->number == b.enemy;
        if (!fullScan && !isCurrent) continue;
        if (int(c) == b.client || !ci.playing || (teams && ci.team == myTeam)) {
            b.notice[c] = 0;
            continue;
        }
        if (!isCurrent && dist3(ci.eye, eye) > nearLimit) continue; // beyond the five nearest in view: not looked at now
        // The fought enemy's place is read now (it may have moved this frame); the others' as gathered.
        const Vec3 to = isCurrent ? v3(t->obj->get("origin")) : ci.origin;
        const Vec3 head = isCurrent ? eyePosition(*t) : ci.eye;
        const Vec3 d{head[0] - eye[0], head[1] - eye[1], head[2] - eye[2]};
        const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        int parts = 0;
        float ecc = 1.0f;
        if (dl <= sk.sightDist && dl > 1.0f) {
            // Inside the view (widened by the body's half width at that distance)?
            const Vec3 mid{(head[0] + to[0]) * 0.5f - eye[0], (head[1] + to[1]) * 0.5f - eye[1], (head[2] + to[2]) * 0.5f - eye[2]};
            const auto off = botViewOffset(mid, b.yaw, b.pitch);
            const float body = std::atan2(16.0f, dl) / kDeg;
            const float ex = std::max(0.0f, std::fabs(off[0]) - body) / view.halfYawDeg, ey = std::max(0.0f, std::fabs(off[1]) - body * 2.0f) / view.halfPitchDeg;
            ecc = std::max(ex, ey);
            if (ecc <= 1.0f && !mpBotSmokeBlocks(eye, head)) {
                // Clear lines to the head, the chest and the legs (the enemy the bot fights: all three, for its aim).
                const Vec3 pts[3] = {head, Vec3{head[0], head[1], to[2] + (head[2] - to[2]) * 0.7f}, Vec3{to[0], to[1], to[2] + (head[2] - to[2]) * 0.25f}};
                for (int k = 0; k < 3; ++k) {
                    if (collision_->trace(eye, pts[k], {0, 0, 0}, {0, 0, 0}, Contents::SightMaskNoCharacters, self.number).fraction >= 0.999f) parts |= 1 << k;
                    if (parts && !isCurrent) break;
                }
            }
        }
        if (!parts) {
            // Out of sight: an enemy the bot had noticed stays known for 3 s (seen again, it is noticed at once).
            if (!(b.notice[c] >= 1.0f && now - b.noticeSeenMs[c] < 3000)) b.notice[c] = std::max(0.0f, b.notice[c] - frameMs / 1500.0f);
            continue;
        }
        if (b.notice[c] < 1.0f) {
            BotNoticeIn in;
            in.baseMs = int(float(sk.noticeMs) * b.trait.reaction);
            in.ecc = ecc;
            in.dist = dl;
            in.fov43 = fov43;
            in.targetMoving = ci.speed > 50.0f;
            in.targetProne = ci.viewHeight <= 11;
            in.targetCrouched = ci.viewHeight > 11 && ci.viewHeight <= 40;
            in.targetFired = now - oc.lastShotMs < 300;
            in.alerted = (b.enemy == t->number && now - b.enemySeenMs < 5000) || (b.alert.valid && b.alert.entity == t->number);
            b.notice[c] += frameMs / botNoticeMs(in);
            if (b.notice[c] < 1.0f) continue;
            b.notice[c] = 1.0f;
            ++b.stats.acquired;
            if (ecc > 1.0f) ++b.stats.acquiredOutsideView; // cannot happen: the measure of the old model
        }
        b.noticeSeenMs[c] = now;
        // The nearest noticed enemy in sight; the one the bot fights is kept unless another is much closer.
        const float score = dl * (isCurrent ? 0.7f : 1.0f);
        if (score < bestScore) bestScore = score, best = t, bestParts = parts;
    }
    // Turned to an attacker by a hit (mpBotOnDamage): until it is seen, or 1.5 s have passed, nobody else is fought.
    if (now < b.focusUntilMs && best && best->number != b.focusEnt) best = nullptr;
    if (best && best->number == b.focusEnt) b.focusUntilMs = 0;
    if (best) {
        if (best->number != b.enemy || !b.enemyVisible) {
            // The reaction: from noticing to acting; half of it for an enemy seen in the last 1.5 s (back from cover).
            const bool again = best->number == b.enemy && now - b.enemySeenMs < 1500;
            const int react = int(float(sk.reactionMs) * b.trait.reaction) + int(nextRand() % 150);
            b.reactAtMs = std::max(b.reactAtMs, now + (again ? react / 2 : react));
        }
        if (best->number != b.enemy) b.aimValid = false;
        b.enemy = best->number;
        b.enemyVisible = true;
        b.enemySeenMs = now;
        b.enemyLastPos = v3(best->obj->get("origin"));
        b.enemyVel = {best->velocity[0], best->velocity[1], best->velocity[2]};
        b.enemyParts = bestParts;
        b.enemyHeardOnly = false;
        if (b.alert.valid && b.alert.entity == best->number) b.alert.valid = false;
    } else if (fullScan || b.enemyVisible) {
        b.enemyVisible = false;
        Entity* old = b.enemy >= 0 ? entityByNumber(b.enemy) : nullptr;
        // A dead enemy is forgotten (the obituary names it), a lost one after the skill's memory time.
        if (!old || !isAlive(*old) || now - b.enemySeenMs > sk.memoryMs) b.enemy = -1;
    }

    // Hearing (docs/spec/mp-bots.md "Hearing"): enemy weapon fire, and running steps close by, give a believed place after a
    // delay; the place is off by a random angle and distance. A bot that fights an enemy in sight does not turn to them.
    if (b.combat && !b.enemyVisible) {
        const bool stepsFrame = ((bots_->frameCount + b.client) & 3) == 0;
        for (size_t c = 0; c < infos.size(); ++c) {
            const BotClientInfo& ci = infos[c];
            const MpClient& oc = mp_->clients[c];
            Entity* t = ci.ent;
            const bool shot = now - oc.lastShotMs < 100; // a shot of the last two frames
            if (!shot && !stepsFrame) continue;
            if (int(c) == b.client || !ci.playing || (teams && ci.team == myTeam)) continue;
            int urgency = 0;
            Vec3 at{};
            if (shot) {
                at = {oc.lastShotPos[0], oc.lastShotPos[1], oc.lastShotPos[2]};
                if (dist3(at, eye) > sk.hearDist) continue;
                at = ci.origin;
                urgency = 2;
            } else {
                // Steps: a standing enemy running on the ground, not aiming (docs/spec/mp-pmove.md: quiet when crouched).
                at = ci.origin;
                if (ci.speed < 130.0f || ci.viewHeight < 60 || ci.ads || dist3(at, eye) > sk.stepDist) continue;
                urgency = 1;
            }
            // The sound leaves the direction and the distance uncertain: gunfire by up to 12..25 degrees, steps by up to
            // 30..45, the distance by a quarter.
            const float maxErr = (urgency == 2 ? 12.0f : 30.0f) + float(3 - b.skill) * (urgency == 2 ? 4.3f : 5.0f);
            const float err = (float(nextRand() % 2001) / 1000.0f - 1.0f) * maxErr;
            const float scale = 0.75f + float(nextRand() % 501) / 1000.0f;
            const Vec3 believed = botHeardPosition(b.ps.origin, at, err, scale);
            const bool scoped = view.halfYawDeg < 15.0f; // through a scope the sound takes longer to get the bot's attention
            const int delay = int(float(sk.reactionMs) * b.trait.reaction * (scoped ? 2.0f : 1.2f)) + int(nextRand() % 200);
            // Not again for the same enemy while the last sound's place is fresh (a burst is one sound).
            if (b.enemy == t->number && now - b.enemySeenMs < 1500 && urgency < 2) continue;
            if (b.alert.valid && b.alert.entity == t->number && now < b.alert.atMs) continue;
            mpBotAlert(b, t->number, believed, delay, urgency);
        }
    }
    // A sound or a hit the bot has become aware of: the enemy is known at the believed place.
    if (b.alert.valid && now >= b.alert.atMs) {
        b.alert.valid = false;
        Entity* src = entityByNumber(b.alert.entity);
        const bool fightingOther = b.enemyVisible && b.enemy != b.alert.entity;
        if (src && isAlive(*src) && b.combat && !(fightingOther && b.alert.urgency < 3) && !(b.enemyVisible && b.enemy == b.alert.entity)) {
            const bool steps = b.alert.urgency == 1;
            // Steps of somebody while another enemy is known: not followed.
            if (!(steps && b.enemy >= 0 && b.enemy != b.alert.entity && now - b.enemySeenMs < 4000)) {
                if (b.enemy != b.alert.entity) b.aimValid = false;
                if (b.alert.urgency >= 3) b.focusEnt = b.alert.entity, b.focusUntilMs = now + 1500;
                b.enemy = b.alert.entity;
                b.enemyVisible = false;
                b.enemySeenMs = now;
                b.enemyLastPos = b.alert.pos;
                b.enemyHeardOnly = true;
                b.path.clear();
                b.pathIdx = 0;
                ++b.stats.heard;
            }
        }
    }
    return b.enemyVisible ? entityByNumber(b.enemy) : nullptr;
}

} // namespace iw2::game
