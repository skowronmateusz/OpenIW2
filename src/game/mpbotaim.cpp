// Multiplayer bot aim (docs/spec/mp-bots.md "Aim", "Shooting"). An OpenIW2 extension, implemented from the spec. The
// bot's view is moved like a hand moves a mouse: a damped spring toward the point the bot wants to look at, limited by
// the skill's turn rate, which overshoots and settles; the point it aims at wanders around the target (the hand's
// error) and trails a moving target. Shots leave along the view with the weapon's own spread and kick the view like a
// player's; whether they hit follows from where the view pointed.
#include "game/mpbots.h"
#include "game/world.h"

#include <algorithm>
#include <cmath>

namespace iw2::game {

namespace {
constexpr float kDeg = 3.14159265358979f / 180.0f;
constexpr float kDt = 0.05f;
float wrap180(float a) {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}
} // namespace

void botAimStep(BotAim& a, float& yaw, float& pitch, float wantYaw, float wantPitch, float omega, float damping, float maxRate, float dt) {
    // Two half steps: the spring is stiff against a 50 ms frame.
    for (int i = 0; i < 2; ++i) {
        const float h = dt * 0.5f;
        const float ey = wrap180(wantYaw - yaw), ep = wantPitch - pitch;
        a.yawVel += (omega * omega * ey - 2.0f * damping * omega * a.yawVel) * h;
        a.pitchVel += (omega * omega * ep - 2.0f * damping * omega * a.pitchVel) * h;
        a.yawVel = std::clamp(a.yawVel, -maxRate, maxRate);
        a.pitchVel = std::clamp(a.pitchVel, -maxRate, maxRate);
        yaw = wrap180(yaw + a.yawVel * h);
        pitch = std::clamp(pitch + a.pitchVel * h, -85.0f, 85.0f);
    }
}

float botAimNoiseScale(const BotAimNoiseIn& in) {
    float s = 1.0f;
    if (in.moving) s *= 1.5f;
    if (in.stance == Stance::Crouch) s *= 0.8f;
    if (in.stance == Stance::Prone) s *= 0.6f;
    s *= 1.0f - 0.4f * std::clamp(in.adsFrac, 0.0f, 1.0f);       // the sights steady the hand
    if (in.hurtMs < 600) s *= 2.0f;                              // flinching from a hit
    if (in.turnRate > 100.0f) s *= 1.5f;                         // still swinging
    // Settling: 1.5 times the error when the target is new, half of it after a second and more on it.
    s *= 0.5f + std::exp(-float(std::max(0, in.onTargetMs)) / 700.0f);
    return s;
}

float botFireTolerance(float dist, float skillFactor, bool sprayClose) {
    const float half = std::atan2(13.0f, std::max(1.0f, dist)) / kDeg; // half the width of a body at that distance
    float tol = std::max(half * 1.3f, 0.5f) * skillFactor;
    if (sprayClose) tol *= 1.5f;
    return tol;
}

// The weapon file fields the bots read every frame, looked up once per weapon.
const BotWeaponInfo& World::mpBotWeaponInfo(const std::string& weapon) {
    auto it = bots_->weaponInfo.find(weapon);
    if (it != bots_->weaponInfo.end()) return it->second;
    BotWeaponInfo w;
    w.cls = mpWeaponClass(weapon);
    w.clipName = weaponField(weapon, "clipName");
    w.ammoName = weaponField(weapon, "ammoName");
    w.clipSize = std::max(1, weaponInt(weapon, "clipSize"));
    w.reloadTime = weaponFloat(weapon, "reloadEmptyTime") > 0 ? weaponFloat(weapon, "reloadEmptyTime") : weaponFloat(weapon, "reloadTime");
    w.semi = weaponField(weapon, "semiAuto") == "1" || weaponField(weapon, "boltAction") == "1";
    w.scope = !weaponField(weapon, "adsOverlayShader").empty();
    const float zf = weaponFloat(weapon, "adsZoomFov");
    w.adsZoomFov = zf > 0 ? zf : 80.0f;
    w.adsZoomInFrac = weaponFloat(weapon, "adsZoomInFrac");
    w.adsZoomOutFrac = weaponFloat(weapon, "adsZoomOutFrac");
    w.adsSpread = weaponFloat(weapon, "adsSpread");
    w.moveSpeedScale = weaponField(weapon, "moveSpeedScale").empty() ? 0.0f : weaponFloat(weapon, "moveSpeedScale");
    w.hip.standMin = weaponFloat(weapon, "hipSpreadStandMin");
    w.hip.duckedMin = weaponFloat(weapon, "hipSpreadDuckedMin");
    w.hip.proneMin = weaponFloat(weapon, "hipSpreadProneMin");
    w.hip.max = weaponFloat(weapon, "hipSpreadMax");
    w.hip.duckedMax = weaponFloat(weapon, "hipSpreadDuckedMax");
    w.hip.proneMax = weaponFloat(weapon, "hipSpreadProneMax");
    w.hip.decayRate = weaponFloat(weapon, "hipSpreadDecayRate");
    w.hip.fireAdd = weaponFloat(weapon, "hipSpreadFireAdd");
    w.hip.turnAdd = weaponFloat(weapon, "hipSpreadTurnAdd");
    w.hip.moveAdd = weaponFloat(weapon, "hipSpreadMoveAdd");
    w.hip.duckedDecay = weaponFloat(weapon, "hipSpreadDuckedDecay");
    w.hip.proneDecay = weaponFloat(weapon, "hipSpreadProneDecay");
    return bots_->weaponInfo.emplace(weapon, std::move(w)).first->second;
}

// Turns the bot's view toward the wanted angles (botAimStep with the skill's spring). The view is the angles the bot
// steers (b.aim.yaw / pitch) plus what it does not steer: the hand's wandering error and the view kick of its shots.
void World::mpBotTurnView(MpBot& b, float wantYaw, float wantPitch, bool tracking) {
    const BotSkill& sk = botSkill(b.skill);
    // The steered angles: the view without last frame's offsets (so a view set from outside is taken over).
    float yaw = wrap180(b.yaw - b.aim.offYaw), pitch = b.pitch - b.aim.offPitch;
    // Turning to an enemy known in the last 3 s (heard, hit by, just lost) or to a throw is a little calmer than following a
    // target in sight; looking around and along the walk is calmer still and at most 60% of the turn rate.
    const bool alert = tracking || b.throwPending.valid || (b.enemy >= 0 && timeMs_ - b.enemySeenMs < 3000);
    botAimStep(b.aim, yaw, pitch, wantYaw, wantPitch, sk.aimOmega * (tracking ? 1.0f : alert ? 0.8f : 0.6f), sk.aimDamping,
               sk.turnRate * (alert ? 1.0f : 0.6f), kDt);
    b.aim.yaw = yaw;
    b.aim.pitch = pitch;
    if (!tracking) { // no target: the hand's error fades
        b.aim.noiseYaw *= 1.0f - kDt / 0.35f;
        b.aim.noisePitch *= 1.0f - kDt / 0.35f;
    }
    const auto kick = b.kick.viewKick();
    b.aim.offYaw = b.aim.noiseYaw + kick[1];
    b.aim.offPitch = b.aim.noisePitch + kick[0];
    b.yaw = wrap180(yaw + b.aim.offYaw);
    b.pitch = std::clamp(pitch + b.aim.offPitch, -85.0f, 85.0f);
}

// The point of the seen enemy the bot aims at: the chest when a line to it is clear, else the head, else the legs; on
// the posed body (clients are hit on their bone boxes), read from the pose every fourth frame.
Vec3 World::mpBotAimPoint(MpBot& b, Entity& enemy) {
    const Vec3 o = enemy.obj->get("origin").asVector();
    const int part = (b.enemyParts & 2) ? 2 : (b.enemyParts & 1) ? 1 : (b.enemyParts & 4) ? 4 : 2;
    if (b.aimBoneEnt != enemy.number || b.aimBonePart != part || ((bots_->frameCount + b.client) & 3) == 0) {
        const Vec3 head = eyePosition(enemy);
        Vec3 p = part == 1 ? head : part == 4 ? Vec3{o[0], o[1], o[2] + (head[2] - o[2]) * 0.3f} : Vec3{head[0], head[1], head[2] - 14.0f};
        const auto bones = entityBones(enemy);
        const char* name = part == 1 ? "j_head" : part == 4 ? "j_mainroot" : "j_spine4";
        if (auto it = bones.find(name); it != bones.end()) p = it->second.position;
        b.aimBoneOffset = {p[0] - o[0], p[1] - o[1], p[2] - o[2]};
        b.aimBoneEnt = enemy.number;
        b.aimBonePart = part;
    }
    return {o[0] + b.aimBoneOffset[0], o[1] + b.aimBoneOffset[1], o[2] + b.aimBoneOffset[2]};
}

// The angles the bot steers its view to while it fights the seen enemy: toward a point that follows the aim point with
// the skill's lag, led by the enemy's velocity as far as the skill does, plus the hand's wandering error.
void World::mpBotAimAt(MpBot& b, Entity& enemy, const Vec3& eye, float& wantYaw, float& wantPitch, const BotAimNoiseIn& noiseIn) {
    const BotSkill& sk = botSkill(b.skill);
    const int64_t now = timeMs_;
    const Vec3 target = mpBotAimPoint(b, enemy);
    if (!b.aimValid) {
        // The followed point starts where the view points, at the enemy's distance.
        const float fy = b.yaw * kDeg, fp = b.pitch * kDeg;
        const float d = std::sqrt((target[0] - eye[0]) * (target[0] - eye[0]) + (target[1] - eye[1]) * (target[1] - eye[1]) +
                                  (target[2] - eye[2]) * (target[2] - eye[2]));
        b.aimPos = {eye[0] + std::cos(fp) * std::cos(fy) * d, eye[1] + std::cos(fp) * std::sin(fy) * d, eye[2] - std::sin(fp) * d};
        b.aimValid = true;
        b.aim.onTargetSinceMs = now;
        b.aim.noiseYaw = b.aim.noisePitch = 0;
    }
    const float lag = float(std::max(1, sk.aimLagMs));
    const float k = std::min(1.0f, 50.0f / lag);
    for (int i = 0; i < 3; ++i) b.aimPos[size_t(i)] += (target[size_t(i)] - b.aimPos[size_t(i)]) * k;
    // The lead: the lagging point trails a moving enemy by velocity x lag; the skill makes up part of it.
    const float lead = sk.aimLead * lag * 0.001f;
    const Vec3 at{b.aimPos[0] + b.enemyVel[0] * lead, b.aimPos[1] + b.enemyVel[1] * lead, b.aimPos[2] + b.enemyVel[2] * lead};
    const float dx = at[0] - eye[0], dy = at[1] - eye[1], dz = at[2] - eye[2];
    wantYaw = std::atan2(dy, dx) / kDeg;
    wantPitch = -std::atan2(dz, std::sqrt(dx * dx + dy * dy)) / kDeg;
    mpBotAimNoise(b, noiseIn, 1.0f);
}

// The hand's error: the view wanders around where the bot steers it (time constant 0.35 s), wider for a low skill,
// while moving, swinging or flinching, narrower crouched, prone, at the sights and the longer the bot is on the
// target. It is added to the view in mpBotTurnView.
void World::mpBotAimNoise(MpBot& b, const BotAimNoiseIn& noiseIn, float scale) {
    BotAimNoiseIn in = noiseIn;
    in.onTargetMs = int(timeMs_ - b.aim.onTargetSinceMs);
    in.turnRate = std::hypot(b.aim.yawVel, b.aim.pitchVel);
    const float sigma = botSkill(b.skill).aimNoiseDeg * b.trait.aim * botAimNoiseScale(in) * scale;
    auto gauss = [&] { return (float(nextRand() % 1000) + float(nextRand() % 1000) + float(nextRand() % 1000)) / 1000.0f * 2.0f - 3.0f; };
    const float decay = kDt / 0.35f, push = std::sqrt(2.0f * decay);
    b.aim.noiseYaw += -b.aim.noiseYaw * decay + sigma * push * gauss();
    b.aim.noisePitch += -b.aim.noisePitch * decay + sigma * 0.6f * push * gauss(); // less up and down than sideways
}

// A shot of the bot's hand weapon: along its view with the weapon's spread at the player state's aim spread, as a
// client's shot (docs/spec/net.md section 13); the view takes the weapon's kick.
void World::mpBotFireShot(MpBot& b, Entity& self, const Vec3& eye, const std::string& weapon, bool lastShot) {
    const BotWeaponInfo& wi = mpBotWeaponInfo(weapon);
    const HipSpread& hs = wi.hip;
    const float adsFrac = b.weapon.adsFrac();
    const float spread = fireSpreadDegrees(hs, b.ps.viewHeight, b.ps.aimSpread, adsFrac, wi.adsSpread);
    addFireSpread(b.ps, hs, adsFrac);
    const float pr = b.pitch * kDeg, yr = b.yaw * kDeg;
    const Vec3 fwd{std::cos(pr) * std::cos(yr), std::cos(pr) * std::sin(yr), -std::sin(pr)};
    const Vec3 kick = fireWeaponShot(self, weapon, eye, fwd, lastShot, spread);
    for (int k = 0; k < 3; ++k) b.ps.velocity[size_t(k)] -= kick[size_t(k)];
    // The view kick of the shot (docs/spec/weapons.md "Recoil"): the same spring a player's view has.
    const RecoilKick::Field field = [&](const char* key) { return weaponFloat(weapon, key); };
    b.kick.fire(field, adsFrac, b.weapon.reducedKick());
    b.kickActive = true;
    b.kickFrames = 2;
}

// The view kick's spring for this frame (its offset is added to the bot's view in mpBotTurnView).
void World::mpBotKickStep(MpBot& b, const std::string& weapon, bool hasWeapon) {
    if (!b.kickActive) return;
    const RecoilKick::Field field = [&](const char* key) { return weaponFloat(weapon, key); };
    b.kick.update(field, 50, hasWeapon, b.weapon.adsFrac());
    const auto after = b.kick.viewKick();
    b.kickActive = after[0] != 0 || after[1] != 0 || b.kickFrames-- > 0;
}

} // namespace iw2::game
