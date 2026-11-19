// Multiplayer bot combat (docs/spec/mp-bots.md "Combat behaviour", "Cost limits", "Stuck watch"): the pure decisions,
// fights per skill (shots, hits, kills, grenades), the pistol switch, hearing, the cost of 15 bots and the stuck watch
// on every map. Needs the game files (IW2_MAIN); the graph cache goes to IW2_NAV_CACHE.
//   test_mpbotcombat                 pure tests, the fights on mp_toujane, sd rounds, hearing
//   test_mpbotcombat perf <map>      15 bots: frame cost and A* plans per frame
//   test_mpbotcombat stall <map> [gametype]  "wants to walk, stays on the spot" spells by hot spot (IW2_BOT_SOAK_SECONDS)
//   test_mpbotcombat stall-check <map>       the same with limits
//   test_mpbotcombat soak <map>      8 bots for IW2_BOT_SOAK_SECONDS (default 60) game seconds: no bot stuck
//   test_mpbotcombat behaviour       the humanlike behaviour scenarios on mp_toujane (ammunition, melee, pickups, turrets,
//                                    perception, aim), each a bot against a client that stands where the test puts it;
//                                    giving way on mp_dawnville, the loft gun of mp_farmhouse
//   test_mpbotcombat giveway | crevice | hearing | turretloft   one of those scenarios (hearing: the more sounds of the fights)
//   test_mpbotcombat stats <map> <gametype> <bots> <skill> <seconds> [scripted]   measurement run (docs/spec/mp-bots.md
//                                    "Measurement"): bots against each other, with `scripted` also against a scripted player

#include "game/mpbots.h"
#include "game/world.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using iw2::game::Entity;
using iw2::game::MpBotStats;
using iw2::game::NavGraph;
using iw2::game::Stance;
using iw2::game::World;
using iw2::script::Value;
using V3 = std::array<float, 3>;

void run(World& w, int frames) {
    for (int i = 0; i < frames; ++i) w.frame();
}

void pureTests() {
    using iw2::game::BotStanceIn;
    using iw2::game::botChooseStance;
    BotStanceIn in;
    in.weaponClass = "sniper";
    in.dist = 1200;
    CHECK(botChooseStance(in) == Stance::Stand); // not fighting
    in.fighting = true;
    CHECK(botChooseStance(in) == Stance::Prone);
    in.moving = true;
    CHECK(botChooseStance(in) == Stance::Stand);
    in.moving = false;
    in.weaponClass = "rifle";
    CHECK(botChooseStance(in) == Stance::Crouch);
    in.dist = 400;
    CHECK(botChooseStance(in) == Stance::Stand);
    in.coverCrouch = true;
    in.holdFire = true;
    CHECK(botChooseStance(in) == Stance::Crouch); // hides behind the cover while reloading
    in.holdFire = false;
    CHECK(botChooseStance(in) == Stance::Stand); // stands up to fire over it
    in.coverCrouch = false;
    in.weaponClass = "smg";
    in.dist = 1500;
    CHECK(botChooseStance(in) == Stance::Stand);

    using iw2::game::botWantAds;
    CHECK(botWantAds("sniper", 450) && !botWantAds("sniper", 300));
    CHECK(botWantAds("rifle", 600) && !botWantAds("rifle", 300));
    CHECK(botWantAds("smg", 500) && !botWantAds("smg", 400));
    CHECK(!botWantAds("shotgun", 2000) && !botWantAds("pistol", 2000));

    using iw2::game::botBurst;
    CHECK(botBurst("smg", 300, 6, 350).shots == 6 && botBurst("smg", 300, 6, 350).pauseMs == 350);
    CHECK(botBurst("mg", 1000, 6, 350).shots == 3 && botBurst("mg", 1000, 6, 350).pauseMs == 525);
    CHECK(botBurst("mg", 1000, 3, 700).shots == 2);
    CHECK(botBurst("rifle", 300, 6, 350).shots == 1 && botBurst("rifle", 300, 6, 350).pauseMs == 0);
    CHECK(botBurst("pistol", 300, 8, 250).shots == 3);

    const float g = 800;
    // The view angles of a player's throw (the grenade leaves along the view at the speed, plus a speed upwards): the
    // arc lands on the target, the high arc looks further up, out of reach gives none.
    using iw2::game::botGrenadeAngles;
    const float up = 120;
    for (int high = 0; high < 2; ++high)
        for (const V3& to : {V3{500, 0, 0}, V3{300, 200, -100}, V3{-250, 100, 60}, V3{0, -700, 30}}) {
            float yaw = 0, pitch = 0;
            V3 vel{};
            CHECK(botGrenadeAngles({0, 0, 60}, {to[0], to[1], to[2] + 60}, 940, up, g, high != 0, yaw, pitch, vel));
            const float y = yaw * 0.0174532925f, p = pitch * 0.0174532925f;
            // the velocity is the view's forward times the speed plus the upward speed
            CHECK(std::fabs(vel[0] - std::cos(p) * std::cos(y) * 940) < 0.5f && std::fabs(vel[1] - std::cos(p) * std::sin(y) * 940) < 0.5f);
            CHECK(std::fabs(vel[2] - (-std::sin(p) * 940 + up)) < 0.5f);
            const float x = std::hypot(to[0], to[1]), t = x / std::hypot(vel[0], vel[1]);
            CHECK(std::fabs(vel[0] * t - to[0]) < 1.0f && std::fabs(vel[1] * t - to[1]) < 1.0f);
            CHECK(std::fabs(vel[2] * t - 0.5f * g * t * t - to[2]) < 2.0f);
        }
    float yl = 0, pl = 0, yh = 0, ph = 0;
    V3 vl{}, vh{};
    CHECK(botGrenadeAngles({0, 0, 0}, {600, 0, 0}, 940, up, g, false, yl, pl, vl) && botGrenadeAngles({0, 0, 0}, {600, 0, 0}, 940, up, g, true, yh, ph, vh));
    CHECK(ph < pl - 20); // the high arc: the view much further up (a view pitch is negative upwards)
    CHECK(!botGrenadeAngles({0, 0, 0}, {5000, 0, 0}, 940, up, g, false, yl, pl, vl));

    // Giving way: a mate 30 units away walking at the bot presses into it; one walking away, standing, too far or on
    // another floor does not.
    using iw2::game::botPushedBy;
    CHECK(botPushedBy({30, 0}, 0, {1, 0}));
    CHECK(botPushedBy({30, 10}, 0, {1, 0.2f}));
    CHECK(!botPushedBy({30, 0}, 0, {-1, 0}));
    CHECK(!botPushedBy({30, 0}, 0, {0, 1}));
    CHECK(!botPushedBy({30, 0}, 0, {0, 0}));
    CHECK(!botPushedBy({60, 0}, 0, {1, 0}));
    CHECK(!botPushedBy({30, 0}, 90, {1, 0}));
}

// The pure decisions of the humanlike behaviour (docs/spec/mp-bots.md).
void purePlayerTests() {
    using iw2::game::BotWeaponAction;
    using iw2::game::BotWeaponIn;
    using iw2::game::botWeaponAction;
    BotWeaponIn w;
    w.heldClass = "smg", w.otherClass = "pistol";
    w.heldClip = 20, w.heldReserve = 100, w.heldClipSize = 30, w.otherClip = 8, w.otherReserve = 16;
    CHECK(botWeaponAction(w) == BotWeaponAction::None);
    // Out of rounds: the other weapon, also while fighting and at any distance; with none left in either, nothing.
    w.heldClip = 0, w.heldReserve = 0;
    CHECK(botWeaponAction(w) == BotWeaponAction::Switch);
    w.fighting = true, w.enemyDist = 1500;
    CHECK(botWeaponAction(w) == BotWeaponAction::Switch);
    w.otherClip = 0, w.otherReserve = 0;
    CHECK(botWeaponAction(w) == BotWeaponAction::None);
    // An empty pistol in the hand and a main weapon with rounds (the case that left bots defenceless): the main weapon.
    w = BotWeaponIn{};
    w.heldClass = "pistol", w.otherClass = "rifle", w.heldClipSize = 8, w.otherClip = 0, w.otherReserve = 40, w.fighting = true, w.enemyDist = 700;
    CHECK(botWeaponAction(w) == BotWeaponAction::Switch);
    // An empty clip in a fight: reload; with an enemy close and a loaded second weapon, that weapon.
    w = BotWeaponIn{};
    w.heldClass = "smg", w.otherClass = "pistol", w.heldClip = 0, w.heldReserve = 90, w.heldClipSize = 30, w.otherClip = 8, w.fighting = true;
    w.enemyDist = 900;
    CHECK(botWeaponAction(w) == BotWeaponAction::Reload);
    w.enemyDist = 300;
    CHECK(botWeaponAction(w) == BotWeaponAction::Switch);
    w.heldReloadTime = 0.6f; // a quick reload beats the change
    CHECK(botWeaponAction(w) == BotWeaponAction::Reload);
    // Two loaded weapons: the pistol against an enemy that got close to a bolt-action rifle, the rifle at a distance.
    w = BotWeaponIn{};
    w.heldClass = "bolt", w.otherClass = "pistol", w.heldClip = 5, w.heldClipSize = 5, w.otherClip = 8, w.fighting = true, w.enemyDist = 200;
    CHECK(botWeaponAction(w) == BotWeaponAction::Switch);
    w.sinceSwitchMs = 1000;
    CHECK(botWeaponAction(w) == BotWeaponAction::None);
    w = BotWeaponIn{};
    w.heldClass = "pistol", w.otherClass = "bolt", w.heldClip = 5, w.heldClipSize = 8, w.otherClip = 5, w.fighting = true, w.enemyDist = 1400;
    CHECK(botWeaponAction(w) == BotWeaponAction::Switch);
    // Quiet: back to the preferred weapon after 2 s; tactical reloads.
    w = BotWeaponIn{};
    w.heldClass = "pistol", w.otherClass = "smg", w.heldClip = 5, w.heldClipSize = 8, w.otherClip = 0, w.otherReserve = 60, w.heldPreferred = false;
    w.quietMs = 1000;
    CHECK(botWeaponAction(w) == BotWeaponAction::None);
    w.quietMs = 2500;
    CHECK(botWeaponAction(w) == BotWeaponAction::Switch);
    w = BotWeaponIn{};
    w.heldClass = "smg", w.otherClass = "pistol", w.heldClip = 12, w.heldReserve = 60, w.heldClipSize = 30, w.otherClip = 8;
    w.enemyKnown = true, w.quietMs = 1000;
    CHECK(botWeaponAction(w) == BotWeaponAction::None); // the enemy just left the sight
    w.quietMs = 2000;
    CHECK(botWeaponAction(w) == BotWeaponAction::Reload); // under half a clip
    w.heldClip = 20;
    CHECK(botWeaponAction(w) == BotWeaponAction::None);
    w.enemyKnown = false, w.quietMs = 4000;
    CHECK(botWeaponAction(w) == BotWeaponAction::Reload); // nothing known: under 70%
    w.heldClip = 25;
    CHECK(botWeaponAction(w) == BotWeaponAction::None);
    w.heldReserve = 0, w.heldClip = 3;
    CHECK(botWeaponAction(w) == BotWeaponAction::None); // no reserve: nothing to reload
    // Quiet for long: the second weapon's empty clip is filled too.
    w = BotWeaponIn{};
    w.heldClass = "smg", w.otherClass = "pistol", w.heldClip = 30, w.heldReserve = 60, w.heldClipSize = 30, w.otherClip = 0, w.otherReserve = 16;
    w.quietMs = 7000;
    CHECK(botWeaponAction(w) == BotWeaponAction::Switch);

    using iw2::game::botWantMelee;
    CHECK(botWantMelee(50, 5, "smg", 0, false));   // empty
    CHECK(botWantMelee(50, 5, "smg", 10, true));   // reloading
    CHECK(botWantMelee(50, 5, "bolt", 5, false));  // a slow weapon
    CHECK(!botWantMelee(50, 5, "smg", 10, false)); // a loaded automatic shoots
    CHECK(botWantMelee(30, 5, "smg", 10, false));  // unless the enemy is at arm's length
    CHECK(!botWantMelee(90, 5, "bolt", 0, false)); // out of reach
    CHECK(!botWantMelee(50, 40, "bolt", 0, false)); // not looking at it

    // Cover: a hurt bot breaks off (not a recruit; a bold one later), a hardened one also to reload.
    using iw2::game::BotRetreatIn;
    using iw2::game::botWantRetreat;
    BotRetreatIn r;
    r.skill = 1, r.enemyKnown = true, r.health = 25, r.hurtMs = 500;
    CHECK(botWantRetreat(r));
    r.skill = 0;
    CHECK(!botWantRetreat(r));
    r.skill = 1, r.health = 35;
    CHECK(!botWantRetreat(r));
    r.skill = 3;
    CHECK(botWantRetreat(r)); // a veteran breaks off sooner (45)
    r.aggression = 1.2f, r.health = 40;
    CHECK(!botWantRetreat(r)); // a bold one at 36
    r.aggression = 1.0f, r.hurtMs = 5000;
    CHECK(!botWantRetreat(r)); // not hurt lately
    r.hurtMs = 500, r.enemyKnown = false;
    CHECK(!botWantRetreat(r));
    r = BotRetreatIn{};
    r.skill = 2, r.enemyKnown = true, r.emptyClip = true, r.enemyDist = 600;
    CHECK(botWantRetreat(r));
    r.enemyDist = 1200;
    CHECK(!botWantRetreat(r));
    r.enemyDist = 600, r.skill = 1;
    CHECK(!botWantRetreat(r));

    // Weapons on the ground.
    using iw2::game::BotItemIn;
    using iw2::game::botItemWant;
    BotItemIn it;
    it.itemClass = "smg";
    it.slotClass[0] = "pistol", it.slotClass[1] = "bolt";
    it.slotRounds[0] = 20, it.slotRounds[1] = 50, it.slotClipSize[0] = 8, it.slotClipSize[1] = 5;
    auto want = botItemWant(it);
    CHECK(want.use && want.replace == 0 && std::fabs(want.desire - 0.7f) < 1e-3f); // the pistol gives way to the other distance's weapon
    it.itemClass = "rifle";
    CHECK(std::fabs(botItemWant(it).desire - 0.5f) < 1e-3f);
    it.itemClass = "grenade";
    CHECK(botItemWant(it).desire == 0);
    it.itemClass = "smg", it.slotRounds[1] = 0; // the rifle has no round: it is replaced
    want = botItemWant(it);
    CHECK(want.use && want.replace == 1 && std::fabs(want.desire - 0.9f) < 1e-3f);
    it.slotRounds[0] = 0;
    CHECK(botItemWant(it).desire == 1.0f); // nothing left at all
    it = BotItemIn{};
    it.itemClass = "mg", it.slotClass[0] = "bolt", it.slotClass[1] = "smg";
    it.slotRounds[0] = 50, it.slotRounds[1] = 100, it.slotClipSize[0] = 5, it.slotClipSize[1] = 30;
    want = botItemWant(it);
    CHECK(want.use && want.replace == 0 && want.desire > 0); // two classes better than the bolt-action rifle
    it.itemClass = "shotgun";
    CHECK(botItemWant(it).desire == 0);
    it.slotRounds[1] = 12; // under a clip
    want = botItemWant(it);
    CHECK(want.replace == 1 && want.desire > 0);
    it = BotItemIn{};
    it.itemClass = "smg", it.owned = true, it.ownedReserve = 30, it.ownedMaxAmmo = 180, it.ownedRounds = 50;
    want = botItemWant(it);
    CHECK(!want.use && want.desire > 0); // ammunition, by walking over it
    it.ownedReserve = 150;
    CHECK(botItemWant(it).desire == 0);
    it = BotItemIn{};
    it.itemClass = "smg", it.freeSlot = true, it.slotClass[0] = "pistol", it.slotRounds[0] = 10;
    want = botItemWant(it);
    CHECK(want.use && want.replace == -1 && want.desire > 0);

    // Turrets.
    iw2::game::BotTurret bt;
    bt.origin = {100, 100, 0};
    bt.centreYaw = 90, bt.halfArc = 45;
    CHECK(iw2::game::botTurretCovers(bt, {100, 900, 0}, 0));
    CHECK(iw2::game::botTurretCovers(bt, {600, 700, 0}, 0));   // 40 degrees off the middle
    CHECK(!iw2::game::botTurretCovers(bt, {900, 500, 0}, 0));  // 63 degrees off
    CHECK(!iw2::game::botTurretCovers(bt, {100, -500, 0}, 0)); // behind the gun
    using iw2::game::BotTurretIn;
    using iw2::game::BotTurretLeave;
    using iw2::game::botTurretLeave;
    BotTurretIn ti;
    CHECK(botTurretLeave(ti) == BotTurretLeave::Stay);
    ti.quietMs = 16000;
    CHECK(botTurretLeave(ti) == BotTurretLeave::Quiet);
    ti.holdMs = 40000;
    CHECK(botTurretLeave(ti) == BotTurretLeave::Stay);
    ti.enemyOutsideArcMs = 1500;
    CHECK(botTurretLeave(ti) == BotTurretLeave::OutOfArc);
    ti.health = 20;
    CHECK(botTurretLeave(ti) == BotTurretLeave::Hurt);
    ti.flanked = true;
    CHECK(botTurretLeave(ti) == BotTurretLeave::Flanked);
    ti = BotTurretIn{};
    ti.onMs = 50000, ti.maxMs = 45000; // long enough on the gun, even with enemies about
    CHECK(botTurretLeave(ti) == BotTurretLeave::Quiet);

    // The view: a 16:9 screen. At the hip (fov 80) 48 degrees to either side and 32 up and down; through a rifle's sights
    // (50) and a scope (15) less.
    using iw2::game::botViewAngles;
    const auto hip = botViewAngles(80), sights = botViewAngles(50), scope = botViewAngles(15);
    CHECK(std::fabs(hip.halfYawDeg - 48.2f) < 0.3f && std::fabs(hip.halfPitchDeg - 32.2f) < 0.3f);
    CHECK(sights.halfYawDeg < 33 && sights.halfYawDeg > 30);
    CHECK(scope.halfYawDeg < 10.1f && scope.halfPitchDeg < 5.8f);
    using iw2::game::botInView;
    auto dirOf = [](float yawDeg, float pitchDeg) {
        const float y = yawDeg * 0.0174532925f, p = pitchDeg * 0.0174532925f;
        return V3{std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p)};
    };
    CHECK(botInView(hip, dirOf(30, 0), 0, 0) && botInView(hip, dirOf(-45, 10), 0, 0));
    CHECK(!botInView(hip, dirOf(60, 0), 0, 0));   // 2 o'clock
    CHECK(!botInView(hip, dirOf(90, 0), 0, 0));   // 3 o'clock
    CHECK(!botInView(hip, dirOf(180, 0), 0, 0));  // behind
    CHECK(!botInView(hip, dirOf(0, 40), 0, 0));   // at the feet
    CHECK(botInView(hip, dirOf(170, 0), 150, 0)); // relative to the view's own yaw, over the wrap
    CHECK(botInView(hip, dirOf(-170, 0), 170, 0));
    CHECK(!botInView(scope, dirOf(15, 0), 0, 0) && botInView(scope, dirOf(4, 2), 0, 0));
    // Noticing: slower towards the edge, with the distance, for a still or prone target; faster through a zoom, for a
    // target that fires or one the bot expects.
    using iw2::game::BotNoticeIn;
    using iw2::game::botNoticeMs;
    BotNoticeIn n;
    n.baseMs = 300, n.dist = 300;
    const float centre = botNoticeMs(n);
    CHECK(centre > 300 && centre < 330);
    n.ecc = 1;
    CHECK(std::fabs(botNoticeMs(n) / centre - 3.0f) < 0.01f);
    n.ecc = 0, n.dist = 2400;
    const float farHip = botNoticeMs(n);
    CHECK(farHip > 4 * centre);
    n.fov43 = 15; // through a scope the far target is as large as one at 450 units
    CHECK(botNoticeMs(n) < 1.2f * centre);
    n = BotNoticeIn{};
    n.baseMs = 300, n.dist = 600;
    const float still = botNoticeMs(n);
    n.targetMoving = true;
    CHECK(botNoticeMs(n) < still);
    n.targetMoving = false, n.targetProne = true;
    CHECK(botNoticeMs(n) > 1.9f * still);
    n.targetFired = true;
    CHECK(botNoticeMs(n) < 0.5f * still);
    n = BotNoticeIn{};
    n.baseMs = 300, n.dist = 600, n.alerted = true;
    CHECK(std::fabs(botNoticeMs(n) / still - 0.5f) < 0.01f);
    // The view's motion: a turn of 90 degrees gets there, overshoots (more with less damping), settles, and never turns
    // faster than the turn rate.
    {
        using iw2::game::BotAim;
        using iw2::game::botAimStep;
        float over[2] = {0, 0}, reached[2] = {0, 0};
        const float damp[2] = {0.55f, 0.85f}, omega[2] = {5.0f, 12.0f}, rate[2] = {180.0f, 480.0f};
        for (int k = 0; k < 2; ++k) {
            BotAim a;
            float yaw = 170, pitch = 0, maxStep = 0;
            for (int f = 0; f < 80; ++f) {
                const float before = yaw;
                botAimStep(a, yaw, pitch, -100, 10, omega[k], damp[k], rate[k], 0.05f); // 170 -> -100: 90 degrees over the wrap
                float step = yaw - before;
                while (step > 180) step -= 360;
                while (step < -180) step += 360;
                maxStep = std::max(maxStep, std::fabs(step));
                float past = yaw + 100; // degrees past the target (the turn goes up through 180 to -100)
                while (past > 180) past -= 360;
                while (past < -180) past += 360;
                over[k] = std::max(over[k], past);
                if (reached[k] == 0 && std::fabs(past) < 1.0f) reached[k] = float(f + 1) * 0.05f;
            }
            CHECK(maxStep <= rate[k] * 0.05f + 0.01f);
            CHECK(std::fabs(yaw + 100) < 0.2f && std::fabs(pitch - 10) < 0.2f); // settled on the target
            CHECK(reached[k] > 0);
        }
        std::cout << "view turn of 90 degrees: recruit there after " << reached[0] << " s, overshoot " << over[0] << " deg; veteran " << reached[1] << " s, "
                  << over[1] << " deg\n";
        CHECK(over[0] > 1.0f);          // the loose hand swings past the target
        CHECK(over[1] < over[0]);       // the firm one less
        CHECK(reached[1] < reached[0]); // and is there sooner
    }
    using iw2::game::BotAimNoiseIn;
    using iw2::game::botAimNoiseScale;
    BotAimNoiseIn an;
    const float fresh = botAimNoiseScale(an);
    CHECK(std::fabs(fresh - 1.5f) < 1e-3f); // a new target: one and a half times the skill's error
    an.onTargetMs = 3000;
    const float settled = botAimNoiseScale(an);
    CHECK(settled < 0.55f && settled > 0.5f);
    an.moving = true;
    CHECK(botAimNoiseScale(an) > settled);
    an.moving = false, an.stance = Stance::Crouch;
    const float crouched = botAimNoiseScale(an);
    an.stance = Stance::Prone;
    CHECK(crouched < settled && botAimNoiseScale(an) < crouched);
    an.stance = Stance::Stand, an.adsFrac = 1;
    CHECK(std::fabs(botAimNoiseScale(an) / settled - 0.6f) < 1e-3f);
    an.adsFrac = 0, an.hurtMs = 200;
    CHECK(std::fabs(botAimNoiseScale(an) / settled - 2.0f) < 1e-3f);
    an.hurtMs = 100000, an.turnRate = 200;
    CHECK(botAimNoiseScale(an) > settled);
    using iw2::game::botFireTolerance;
    CHECK(std::fabs(botFireTolerance(500, 1.0f, false) - 1.3f * std::atan2(13.0f, 500.0f) * 57.29578f) < 1e-3f);
    CHECK(botFireTolerance(5000, 1.0f, false) == 0.5f);                      // never under half a degree
    CHECK(botFireTolerance(500, 2.2f, false) > botFireTolerance(500, 1.0f, false));
    CHECK(std::fabs(botFireTolerance(300, 1.0f, true) / botFireTolerance(300, 1.0f, false) - 1.5f) < 1e-3f);
    // A heard place: turned around the listener and scaled.
    const V3 heard = iw2::game::botHeardPosition({100, 100, 0}, {1100, 100, 50}, 90, 0.5f);
    CHECK(std::fabs(heard[0] - 100) < 0.01f && std::fabs(heard[1] - 600) < 0.01f && std::fabs(heard[2] - 25) < 0.01f);
}

// A world with the local client as a spectator (it never spawns) and `bots` bots at `skill`.
struct Game {
    World w;
    std::vector<int> bots;
    Game(iw2::fs::Vfs& vfs, const std::string& map, const std::string& gt, int nbots, int skill) : w(vfs, map, true) {
        w.mpStart(gt);
        const int c0 = w.mpConnect("player");
        w.mpBegin(c0);
        run(w, 1);
        w.mpSetBotSkill(skill);
        for (int i = 0; i < nbots; ++i) bots.push_back(w.mpAddBot());
    }
    MpBotStats sum() const {
        MpBotStats s;
        for (int c : bots)
            if (const MpBotStats* b = w.mpBotStats(c)) {
                s.shots += b->shots, s.hits += b->hits, s.kills += b->kills, s.deaths += b->deaths;
                for (int k = 0; k < 7; ++k) s.classShots[k] += b->classShots[k], s.classHits[k] += b->classHits[k];
                for (int k = 0; k < 3; ++k) s.bandShots[k] += b->bandShots[k], s.bandHits[k] += b->bandHits[k];
                s.grenades += b->grenades, s.grenadeHits += b->grenadeHits, s.smokes += b->smokes;
                s.pistolSwitches += b->pistolSwitches, s.adsShots += b->adsShots, s.crouchFrames += b->crouchFrames;
                s.proneFrames += b->proneFrames, s.heard += b->heard, s.cornerChecks += b->cornerChecks, s.stuckEvents += b->stuckEvents, s.trapped += b->trapped;
                s.longestStillMs = std::max(s.longestStillMs, b->longestStillMs);
                s.aliveFrames += b->aliveFrames, s.defencelessFrames += b->defencelessFrames, s.emptyFrames += b->emptyFrames;
                s.reloads += b->reloads, s.weaponSwitches += b->weaponSwitches, s.weaponPickups += b->weaponPickups;
                s.ammoPickups += b->ammoPickups, s.melees += b->melees, s.meleeHits += b->meleeHits;
                s.turretMounts += b->turretMounts, s.turretFrames += b->turretFrames, s.turretShots += b->turretShots;
                s.turretKills += b->turretKills, s.acquired += b->acquired, s.acquiredOutsideView += b->acquiredOutsideView;
                s.turretHits += b->turretHits;
                s.retreats += b->retreats;
            }
        return s;
    }
};

// The fields of a kill line of the game log (K;guid;victim;team;name;guid;attacker;team;name;weapon;damage;mod;hitloc).
std::vector<std::string> logFields(const std::string& l) {
    std::vector<std::string> f;
    std::string cur;
    for (char ch : l) {
        if (ch == ';') f.push_back(cur), cur.clear();
        else cur += ch;
    }
    f.push_back(cur);
    return f;
}

// A scripted player for the measurement runs (docs/spec/mp-bots.md "Measurement"): a client that is no bot, walks the
// graph from node to node at a run and, when a bot is within 1500 units in front of it (60 degrees to either side) with a
// clear line between the eyes, stops, turns to it and half a second later fires its weapon at the bot's chest every
// 150 ms with an aim error of up to 1.5 degrees. It respawns with the use button. It is the same before and after a
// change of the bots, so the bots' numbers against it compare.
struct Scripted {
    World& w;
    int client = -1;
    std::vector<int> path;
    size_t idx = 0;
    uint32_t seed = 12345;
    int target = -1;
    int64_t seenFrame = 0, frame = 0, lastShot = 0, deadSince = -1;
    int shots = 0;
    float yaw = 0;
    uint32_t rnd() { return (seed = seed * 1103515245u + 12345u) >> 16 & 0x7fff; }
    explicit Scripted(World& world) : w(world) {
        client = w.mpConnect("scripted");
        w.mpBegin(client);
        w.frame();
        w.mpAutoJoin(client);
    }
    void step(const std::vector<int>& bots) {
        ++frame;
        Entity* e = w.mpClientEntity(client);
        if (!e) return;
        w.mpSetButtons(client, 0);
        if (e->obj->get("sessionstate").toString() != "playing") {
            path.clear();
            target = -1;
            if (deadSince < 0) deadSince = frame;
            if (frame - deadSince > 50 && frame % 10 == 0) w.mpSetButtons(client, 0x8);
            return;
        }
        deadSince = -1;
        const NavGraph& g = w.mpNav();
        V3 o = e->obj->get("origin").asVector();
        const V3 eye{o[0], o[1], o[2] + 60};
        // The nearest bot in front with a clear line.
        int best = -1;
        float bestD = 1500.0f;
        V3 bestChest{};
        for (int c : bots) {
            Entity* t = w.mpClientEntity(c);
            if (!t || t->obj->get("sessionstate").toString() != "playing") continue;
            const V3 to = t->obj->get("origin").asVector();
            const V3 chest{to[0], to[1], to[2] + 44};
            const float d = std::sqrt((chest[0] - eye[0]) * (chest[0] - eye[0]) + (chest[1] - eye[1]) * (chest[1] - eye[1]) + (chest[2] - eye[2]) * (chest[2] - eye[2]));
            if (d >= bestD) continue;
            float dy = std::atan2(chest[1] - eye[1], chest[0] - eye[0]) * 57.29578f - yaw;
            while (dy > 180) dy -= 360;
            while (dy < -180) dy += 360;
            if (std::fabs(dy) > 60.0f && c != target) continue;
            if (w.collision().trace(eye, chest, {0, 0, 0}, {0, 0, 0}, iw2::game::Contents::SightMaskNoCharacters, e->number).fraction < 0.999f) continue;
            best = c, bestD = d, bestChest = chest;
        }
        if (best != target) target = best, seenFrame = frame;
        if (target >= 0) {
            yaw = std::atan2(bestChest[1] - eye[1], bestChest[0] - eye[0]) * 57.29578f;
            e->obj->set("angles", Value(iw2::script::Vec3{0, yaw, 0}));
            if (frame - seenFrame >= 10 && frame - lastShot >= 3) {
                lastShot = frame;
                ++shots;
                V3 d{bestChest[0] - eye[0], bestChest[1] - eye[1], bestChest[2] - eye[2]};
                const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                const float err = 1.5f * 0.01745f * l; // up to 1.5 degrees off, in units at the target
                for (int k = 0; k < 3; ++k) d[size_t(k)] += (float(rnd()) / 16384.0f - 1.0f) * err * 0.7f;
                const float l2 = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                w.clientFire(*e, eye, {d[0] / l2, d[1] / l2, d[2] / l2});
            }
            return;
        }
        // Walk the graph: 9.5 units a frame (190 units/s) from node to node of a path to a random node.
        if (idx >= path.size()) {
            const int start = g.nearest(o, 512.0f);
            path.clear();
            idx = 0;
            if (start >= 0 && g.mainCount > 0) path = w.paths().findPath(start, int(rnd() % uint32_t(g.mainCount)));
            if (path.empty()) return;
        }
        float left = 9.5f;
        while (left > 0 && idx < path.size()) {
            const V3 q = g.nodes[size_t(path[idx])];
            const float dx = q[0] - o[0], dy = q[1] - o[1], dz = q[2] - o[2];
            const float l = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (l <= left) {
                o = q, left -= l, ++idx;
                continue;
            }
            yaw = std::atan2(dy, dx) * 57.29578f;
            o = {o[0] + dx / l * left, o[1] + dy / l * left, o[2] + dz / l * left};
            left = 0;
        }
        e->obj->set("origin", Value(iw2::script::Vec3{o[0], o[1], o[2]}));
        e->obj->set("angles", Value(iw2::script::Vec3{0, yaw, 0}));
    }
};

void print(const std::string& what, const MpBotStats& s) {
    std::cout << what << ": shots " << s.shots << " hits " << s.hits << " kills " << s.kills << " deaths " << s.deaths << " grenades "
              << s.grenades << " grenade hits " << s.grenadeHits << " smokes " << s.smokes << " pistol " << s.pistolSwitches << " ads shots "
              << s.adsShots << " crouch " << s.crouchFrames << " prone " << s.proneFrames << " heard " << s.heard << " corners "
              << s.cornerChecks << " stuck " << s.stuckEvents << " trapped " << s.trapped << " longest still " << s.longestStillMs / 1000.0 << " s\n";
}

// Two graph nodes `lo`..`hi` apart (flat), with the sight between their eyes clear (`visible`) or blocked.
bool nodePair(World& w, float lo, float hi, bool visible, V3& a, V3& b) {
    const NavGraph& g = w.mpNav();
    for (size_t i = 0; i < g.mainCount; i += 97)
        for (size_t j = 0; j < g.mainCount; j += 13) {
            const V3 p = g.nodes[i], q = g.nodes[j];
            const float d = std::hypot(p[0] - q[0], p[1] - q[1]);
            if (d < lo || d > hi || std::fabs(p[2] - q[2]) > 32) continue;
            const auto tr = w.collision().trace({p[0], p[1], p[2] + 60}, {q[0], q[1], q[2] + 60}, {0, 0, 0}, {0, 0, 0},
                                                iw2::game::Contents::SightMaskNoCharacters, -1);
            if ((tr.fraction >= 0.999f) != visible) continue;
            a = p, b = q;
            return true;
        }
    return false;
}

std::string field(World& w, const std::string& weapon, const char* key) {
    const auto* d = w.weapon(weapon);
    if (!d) return {};
    const auto it = d->find(key);
    return it == d->end() ? std::string() : it->second;
}

void place(Entity* e, const V3& at, const V3& faceTo) {
    e->obj->set("origin", Value(iw2::script::Vec3{at[0], at[1], at[2]}));
    const float yaw = std::atan2(faceTo[1] - at[1], faceTo[0] - at[0]) * 57.29578f;
    e->obj->set("angles", Value(iw2::script::Vec3{0, yaw, 0}));
}

// Every bullet weapon of weapons/mp: a bot fights an enemy that does not fire back and cannot die; its shots follow the
// player's weapon state machine (docs/spec/mp-bots.md "Weapon state"): at most one shot per frame, shots at least
// fireTime apart (plus rechamberTime for a bolt-action weapon), a reload from an empty clip of at least its reload time.
int weaponTests(iw2::fs::Vfs& vfs) {
    std::vector<std::string> names;
    for (const auto& path : vfs.list("weapons/mp/")) {
        const std::string n = path.substr(path.rfind('/') + 1);
        if (!n.empty()) names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    Game g(vfs, "mp_toujane", "dm", 2, 3);
    run(g.w, 40);
    V3 pa{}, pb{};
    CHECK(nodePair(g.w, 550, 900, true, pa, pb));
    // A goal at its place keeps the bot from leaving it for a turret or a weapon on the ground (the test puts it back
    // there every frame; a move of more than 64 units restarts its weapon state).
    g.w.mpBotSetGoal(g.bots[0], pa, 64.0f);
    int tested = 0;
    std::map<std::string, int> perClass;
    for (const std::string& wn : names) {
        const std::string cls = g.w.mpWeaponClass(wn);
        if (field(g.w, wn, "weapontype") != "bullet" || cls.empty() || cls == "grenade" || wn.find("bipod") != std::string::npos ||
            wn.find("30cal") != std::string::npos)
            continue;
        Entity* a = g.w.mpClientEntity(g.bots[0]);
        Entity* b = g.w.mpClientEntity(g.bots[1]);
        if (!a || !b) break;
        g.w.mpBotSetCombat(g.bots[1], false);
        auto& inv = g.w.inventory(*a);
        const std::string clipName = field(g.w, wn, "clipname"), ammoName = field(g.w, wn, "ammoname");
        const int clipSize = std::atoi(field(g.w, wn, "clipsize").c_str());
        inv.owned.insert(wn);
        inv.slot[1] = wn, inv.slot[2].clear();
        inv.clip[clipName] = clipSize;
        inv.ammo[ammoName] = clipSize * 3;
        a->obj->set("$weapon", Value(wn));
        auto num = [&](const char* k) { return float(std::atof(field(g.w, wn, k).c_str())); };
        const bool bolt = field(g.w, wn, "boltaction") == "1";
        const float minGap = num("firetime") + (bolt ? num("rechambertime") : 0.0f);
        const float reloadMin = std::min(num("reloadtime") + num("reloadstarttime"),
                                         num("reloademptytime") > 0 ? num("reloademptytime") : 1e9f);
        const MpBotStats* s = g.w.mpBotStats(g.bots[0]);
        int last = s->shots, lastShotFrame = -1000, shots = 0, multi = 0, reloads = 0;
        float minSeen = 1e9f, minReload = 1e9f;
        bool emptied = false;
        for (int f = 0; f < 20 * 14; ++f) {
            place(a, pa, pb), place(b, pb, pa);
            b->obj->set("health", Value(100000));
            g.w.frame();
            const int n = s->shots - last;
            last = s->shots;
            if (n <= 0) continue;
            if (n > 1) ++multi;
            const float gap = float(f - lastShotFrame) * 0.05f;
            if (lastShotFrame >= 0) {
                if (emptied) minReload = std::min(minReload, gap), ++reloads;
                else minSeen = std::min(minSeen, gap);
            }
            emptied = inv.clip[clipName] == 0;
            lastShotFrame = f;
            shots += n;
        }
        std::cout << wn << " (" << cls << (bolt ? ", bolt" : "") << "): shots " << shots << ", min gap " << minSeen << " s (>= " << minGap
                  << "), reloads " << reloads << ", min reload gap " << minReload << " s (>= " << reloadMin << ")\n";
        CHECK(shots >= 3);
        CHECK(multi == 0);
        CHECK(minSeen + 1e-3f >= minGap);
        CHECK(reloads == 0 || minReload + 1e-3f >= reloadMin);
        ++perClass[cls];
        ++tested;
    }
    std::cout << "weapons tested: " << tested << "\n";
    CHECK(tested >= 25);
    for (const char* c : {"rifle", "bolt", "sniper", "smg", "mg", "shotgun", "pistol"}) CHECK(perClass[c] > 0);
    return 0;
}

float flat(const V3& a, const V3& b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }

void hearingTests(iw2::fs::Vfs& vfs);

int combatTests(iw2::fs::Vfs& vfs) {
    // 1. Fights per skill: 6 bots of one skill in DM for 90 s on mp_toujane.
    MpBotStats st[4];
    for (int skill : {0, 3}) {
        Game g(vfs, "mp_toujane", "dm", 6, skill);
        run(g.w, 20 * 90);
        st[skill] = g.sum();
        print(std::string("skill ") + iw2::game::botSkill(skill).name, st[skill]);
        CHECK(st[skill].shots > 50);
        CHECK(st[skill].kills > 0);
        CHECK(g.w.vm().errors().empty());
    }
    const float r0 = float(st[0].hits) / float(std::max(1, st[0].shots)), r3 = float(st[3].hits) / float(std::max(1, st[3].shots));
    std::cout << "hit rate recruit " << r0 << " veteran " << r3 << "; kills " << st[0].kills << " / " << st[3].kills << "\n";
    CHECK(r3 > r0);
    CHECK(st[3].kills > st[0].kills);
    CHECK(st[0].grenades + st[3].grenades > 0);
    CHECK(st[3].cornerChecks > 0 && st[0].cornerChecks == 0);
    CHECK(st[3].crouchFrames + st[3].proneFrames > 0);

    // 1b. Smoke grenades: sd attackers before the plant throw them (6 bots with smgs, which the gametype gives smoke
    //     grenades, 3 minutes on mp_toujane).
    {
        Game g(vfs, "mp_toujane", "sd", 6, 2);
        for (int c : g.bots) g.w.mpBotSetWeaponClass(c, "smg");
        // A round restart (sd's map_restart, a fast restart of the level) keeps what belongs to the bots: their counters
        // and their weapon class (docs/spec/mp-bots.md "Measurement").
        int lastShots = 0, lastAlive = 0;
        bool monotonic = true;
        for (int f = 0; f < 20 * 180; ++f) {
            g.w.frame();
            const MpBotStats now = g.sum();
            if (now.shots < lastShots || now.aliveFrames < lastAlive) monotonic = false;
            lastShots = now.shots, lastAlive = now.aliveFrames;
        }
        const MpBotStats s = g.sum();
        print("sd hardened", s);
        std::cout << "sd rounds restarted " << g.w.mpState().restarts << "\n";
        CHECK(s.smokes > 0);
        CHECK(g.w.mpState().restarts >= 1);
        CHECK(monotonic);
        for (int c : g.bots) CHECK(g.w.mpBot(c) && g.w.mpBot(c)->weaponClass == "smg");
        CHECK(g.w.vm().errors().empty());
    }

    // 2. Pistol: a bot whose main weapon has no round left switches to the pistol of its other slot and fires it at an
    //    enemy 250-450 units away in sight.
    {
        Game g(vfs, "mp_toujane", "dm", 2, 3);
        run(g.w, 40);
        Entity* a = g.w.mpClientEntity(g.bots[0]);
        Entity* b = g.w.mpClientEntity(g.bots[1]);
        CHECK(a->obj->get("sessionstate").toString() == "playing" && b->obj->get("sessionstate").toString() == "playing");
        auto& inv = g.w.inventory(*a);
        // givePistol's slot weapon lands in the first slot (both slots empty), the chosen weapon in the second.
        const bool p1 = g.w.mpWeaponClass(inv.slot[1]) == "pistol";
        const std::string main = p1 ? inv.slot[2] : inv.slot[1], pistol = p1 ? inv.slot[1] : inv.slot[2];
        std::cout << "loadout " << main << " / " << pistol << " clip " << inv.clip[field(g.w, main, "clipname")] << "\n";
        CHECK(g.w.mpWeaponClass(pistol) == "pistol");
        CHECK(inv.ammo[field(g.w, pistol, "ammoname")] + inv.clip[field(g.w, pistol, "clipname")] > 0);
        CHECK(inv.clip[field(g.w, main, "clipname")] > 0); // the gametype's loadout is the bot's ammunition
        inv.clip[field(g.w, main, "clipname")] = 0;
        inv.ammo[field(g.w, main, "ammoname")] = 0;
        g.w.mpBotSetCombat(g.bots[1], false);
        V3 pa{}, pb{};
        CHECK(nodePair(g.w, 250, 450, true, pa, pb));
        int f = 0;
        for (; f < 20 * 10 && a->obj->get("$weapon").toString() != pistol; ++f) {
            place(a, pa, pb), place(b, pb, pa);
            g.w.frame();
        }
        const MpBotStats* s = g.w.mpBotStats(g.bots[0]);
        std::cout << "pistol after " << f / 20.0f << " s; " << g.w.mpBotDescribe(g.bots[0]) << "\n";
        CHECK(a->obj->get("$weapon").toString() == pistol);
        CHECK(s->pistolSwitches == 1);
        // The enemy is held in the bot's sight: the main weapon without a round is changed for the pistol at once, before
        // the fight (it used to be changed only with the enemy in sight).
        for (int k = 0; k < 20 * 3; ++k) {
            place(b, pb, pa);
            g.w.frame();
        }
        CHECK(s->shots > 0);
    }

    // 3. Hearing: an enemy out of sight 600-1500 units away fires. The bot does not know it at once: after its reaction
    //    time it takes the shooter as its enemy at a place it believes, off the true one in direction (up to 12 degrees
    //    for a veteran) and distance (up to a quarter), not at the shooter's exact place.
    {
        Game g(vfs, "mp_toujane", "dm", 2, 3);
        run(g.w, 40);
        Entity* a = g.w.mpClientEntity(g.bots[0]);
        Entity* b = g.w.mpClientEntity(g.bots[1]);
        g.w.mpBotSetCombat(g.bots[1], false);
        V3 pa{}, pb{};
        CHECK(nodePair(g.w, 600, 1500, false, pa, pb));
        place(a, pa, pb), place(b, pb, pa);
        run(g.w, 2);
        CHECK(g.w.mpBotStats(g.bots[0])->heard == 0);
        const auto bo = b->obj->get("origin").asVector();
        const iw2::script::Vec3 eye{bo[0], bo[1], bo[2] + 60};
        place(a, pa, pb), place(b, pb, pa);
        g.w.debugFireBulletFrom(*b, eye, {eye[0], eye[1], eye[2] + 100});
        g.w.frame();
        CHECK(g.w.mpBotStats(g.bots[0])->heard == 0); // not within the same frame
        int f = 1;
        for (; f < 20 && g.w.mpBotStats(g.bots[0])->heard == 0; ++f) g.w.frame();
        const iw2::game::MpBot* mb = g.w.mpBot(g.bots[0]);
        const V3 ao = a->obj->get("origin").asVector();
        const V3 believed = mb->enemyLastPos;
        const float trueYaw = std::atan2(bo[1] - ao[1], bo[0] - ao[0]) * 57.29578f, gotYaw = std::atan2(believed[1] - ao[1], believed[0] - ao[0]) * 57.29578f;
        float dyaw = gotYaw - trueYaw;
        while (dyaw > 180) dyaw -= 360;
        while (dyaw < -180) dyaw += 360;
        const float ratio = flat(believed, ao) / std::max(1.0f, flat(V3{bo[0], bo[1], bo[2]}, ao));
        std::cout << "hearing: aware after " << f * 50 << " ms, believed place " << dyaw << " degrees off, distance x " << ratio << "; "
                  << g.w.mpBotDescribe(g.bots[0]) << "\n";
        CHECK(g.w.mpBotStats(g.bots[0])->heard == 1);
        CHECK(f * 50 >= 150 && f * 50 <= 700);
        CHECK(mb->enemy == b->number && !mb->enemyVisible && mb->enemyHeardOnly);
        CHECK(std::fabs(dyaw) <= 13.0f);
        CHECK(ratio > 0.7f && ratio < 1.3f);
        CHECK(flat(believed, V3{bo[0], bo[1], bo[2]}) > 1.0f); // a guess, not the shooter's place
    }
    hearingTests(vfs);
    return 0;
}

// 4. More sounds (docs/spec/mp-bots.md "Hearing"): a reload close by, a near miss from beyond the hearing distance, a
//    grenade that lands close by (the bot also runs from it) and one that goes off farther away. Each is known after a
//    delay, never in the same frame, at a place off the true one; a team mate's near miss is not taken for an enemy; the
//    bullets of a burst are one cue. e. An enemy out of sight that fires every half second: the bot walks towards the
//    sounds and gets there, it does not go to and fro on each new believed place.
void hearingTests(iw2::fs::Vfs& vfs) {
    using iw2::game::BotCue;
    auto yawOff = [](const V3& from, const V3& truth, const V3& got) {
        float d = (std::atan2(got[1] - from[1], got[0] - from[0]) - std::atan2(truth[1] - from[1], truth[0] - from[0])) * 57.29578f;
        while (d > 180) d -= 360;
        while (d < -180) d += 360;
        return d;
    };
    // a. Reload: an enemy out of sight 250..450 units away reloads (its clip emptied: the weapon state reloads by itself).
    {
        Game g(vfs, "mp_toujane", "dm", 2, 3);
        run(g.w, 40);
        Entity* a = g.w.mpClientEntity(g.bots[0]);
        Entity* b = g.w.mpClientEntity(g.bots[1]);
        g.w.mpBotSetCombat(g.bots[1], false);
        V3 pa{}, pb{};
        CHECK(nodePair(g.w, 250, 450, false, pa, pb));
        place(a, pa, pb), place(b, pb, pa);
        g.w.mpBotSetGoal(g.bots[0], pa, 64.0f);
        g.w.mpBotSetGoal(g.bots[1], pb, 64.0f);
        run(g.w, 20 * 2);
        const MpBotStats* s = g.w.mpBotStats(g.bots[0]);
        const int heard0 = s->heard;
        auto& inv = g.w.inventory(*b);
        const std::string held = b->obj->get("$weapon").toString();
        inv.clip[field(g.w, held, "clipname")] = 0;
        int f = 0, reloadFrame = -1;
        for (; f < 20 * 3 && s->heardCue[int(BotCue::Reload)] == 0; ++f) {
            g.w.frame();
            if (reloadFrame < 0 && g.w.mpState().clients[size_t(g.bots[1])].lastReloadMs == g.w.debugTimeMs()) reloadFrame = f;
        }
        const iw2::game::MpBot* mb = g.w.mpBot(g.bots[0]);
        const V3 ao = a->obj->get("origin").asVector(), bo = b->obj->get("origin").asVector();
        std::cout << "hearing a reload: began at frame " << reloadFrame << ", known at " << f << ", " << yawOff(ao, bo, mb->enemyLastPos) << " degrees off\n";
        CHECK(s->heard == heard0 + 1 && s->heardCue[int(BotCue::Reload)] == 1);
        CHECK(reloadFrame >= 0 && f > reloadFrame + 1);
        CHECK(mb->enemy == b->number && mb->enemyHeardOnly);
        CHECK(std::fabs(yawOff(ao, bo, mb->enemyLastPos)) <= 31.0f); // a veteran: up to 30 degrees
        CHECK(flat(mb->enemyLastPos, bo) > 1.0f);
    }
    // b. Near miss: an enemy (and in the second run a team mate) 1300..2000 units away, beyond a recruit's hearing of
    //    gunfire, fires a bullet that passes 30 units beside the bot's head while the bot looks away.
    for (const bool mate : {false, true}) {
        Game g(vfs, "mp_toujane", mate ? "tdm" : "dm", mate ? 3 : 2, 0);
        run(g.w, 60);
        int ia = g.bots[0], ib = g.bots[1];
        if (mate)
            for (int i : g.bots)
                for (int j : g.bots)
                    if (i < j && g.w.mpClientEntity(i)->obj->get("sessionteam").toString() == g.w.mpClientEntity(j)->obj->get("sessionteam").toString())
                        ia = i, ib = j;
        for (int c : g.bots)
            if (c != ia) g.w.mpBotSetCombat(c, false);
        Entity* a = g.w.mpClientEntity(ia);
        Entity* b = g.w.mpClientEntity(ib);
        V3 pa{}, pb{};
        CHECK(nodePair(g.w, 1300, 2000, true, pa, pb));
        g.w.mpBotSetGoal(ia, pa, 64.0f);
        g.w.mpBotSetGoal(ib, pb, 64.0f);
        for (int c : g.bots)
            if (c != ia && c != ib) g.w.mpBotSetGoal(c, pb, 400.0f);
        const MpBotStats* s = g.w.mpBotStats(ia);
        iw2::game::MpBot* mb = g.w.mpBot(ia);
        const float away = std::atan2(pa[1] - pb[1], pa[0] - pb[0]) * 57.29578f;
        for (int f = 0; f < 20 * 2; ++f) {
            place(a, pa, pb), place(b, pb, pa);
            mb->yaw = away, mb->pitch = 0;
            g.w.frame();
        }
        const int heard0 = s->heard;
        const V3 ao = a->obj->get("origin").asVector(), bo = b->obj->get("origin").asVector();
        const V3 eyeA{ao[0], ao[1], ao[2] + 60}, eyeB{bo[0], bo[1], bo[2] + 60};
        const float dx = eyeA[0] - eyeB[0], dy = eyeA[1] - eyeB[1], l = std::hypot(dx, dy);
        const V3 past{eyeA[0] + dx / l * 400 - dy / l * 30, eyeA[1] + dy / l * 400 + dx / l * 30, eyeA[2]};
        const int health = a->obj->get("health").asInt();
        // Three bullets 100 ms apart (a burst): one cue, known as soon after the first as after a single one.
        bool sameFrame = false;
        int f = 0;
        for (; f < 20 && s->heard == heard0; ++f) {
            if (f == 0 || f == 2 || f == 4) g.w.debugFireBulletFrom(*b, eyeB, past);
            mb->yaw = away;
            g.w.frame();
            if (f == 0) sameFrame = s->heard != heard0;
        }
        std::cout << "near misses (" << (mate ? "team mate" : "enemy") << " " << l << " units away): heard " << s->heard - heard0 << " after " << f * 50 << " ms";
        if (!mate) std::cout << ", " << yawOff(ao, bo, mb->enemyLastPos) << " degrees off, distance x " << flat(mb->enemyLastPos, ao) / flat(bo, ao);
        std::cout << "\n";
        CHECK(!sameFrame);
        CHECK(a->obj->get("health").asInt() == health); // not hit
        if (mate) {
            CHECK(s->heard == heard0);
        } else {
            CHECK(s->heardCue[int(BotCue::NearMiss)] == 1 && s->heardCue[int(BotCue::Gunfire)] == 0);
            CHECK(f * 50 >= 150 && f * 50 <= 700);
            CHECK(std::fabs(yawOff(ao, bo, mb->enemyLastPos)) <= 26.0f);
            const float ratio = flat(mb->enemyLastPos, ao) / flat(bo, ao);
            CHECK(ratio > 0.65f && ratio < 1.35f);
        }
    }
    // c. A grenade lands 100 units in front of the bot (thrown by an enemy 450..700 units away on open ground); d. one goes
    //    off 650..950 units away, by its thrower 800..1100 units away.
    for (const bool far : {false, true}) {
        Game g(vfs, "mp_toujane", "dm", 2, 3);
        run(g.w, 40);
        Entity* a = g.w.mpClientEntity(g.bots[0]);
        Entity* b = g.w.mpClientEntity(g.bots[1]);
        g.w.mpBotSetCombat(g.bots[1], false);
        // Two nodes on open, level ground: graph nodes all along the line between them at about the same height (the
        // grenade must not fall off a ledge on the way).
        V3 pa{}, pb{};
        {
            const NavGraph& nav = g.w.mpNav();
            bool found = false;
            for (size_t i = 0; i < nav.mainCount && !found; i += 53)
                for (size_t j = 0; j < nav.mainCount && !found; j += 29) {
                    const V3 p = nav.nodes[i], q = nav.nodes[j];
                    const float d = flat(p, q);
                    if (d < (far ? 800 : 450) || d > (far ? 1100 : 700) || std::fabs(p[2] - q[2]) > 8) continue;
                    bool level = true;
                    for (float t = 0; t <= 1.0f && level; t += 24.0f / d) {
                        const int n = nav.nearest({p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t, p[2]}, 20.0f, true);
                        level = n >= 0 && std::fabs(nav.nodes[size_t(n)][2] - p[2]) < 8;
                    }
                    const bool sees = g.w.collision().trace({p[0], p[1], p[2] + 60}, {q[0], q[1], q[2] + 60}, {0, 0, 0}, {0, 0, 0},
                                                            iw2::game::Contents::SightMaskNoCharacters, -1).fraction >= 0.999f;
                    if (level && sees) pa = p, pb = q, found = true;
                }
            CHECK(found);
        }
        g.w.mpBotSetGoal(g.bots[0], pa, 64.0f);
        g.w.mpBotSetGoal(g.bots[1], pb, 64.0f);
        place(a, pa, pb), place(b, pb, pa);
        run(g.w, 20);
        const MpBotStats* s = g.w.mpBotStats(g.bots[0]);
        const int heard0 = s->heard;
        std::string frag;
        for (const std::string& w : g.w.inventory(*b).owned)
            if (field(g.w, w, "offhandclass") == "Frag Grenade") frag = w;
        CHECK(!frag.empty());
        const V3 ao = a->obj->get("origin").asVector(), bo = b->obj->get("origin").asVector();
        const V3 eyeB{bo[0], bo[1], bo[2] + 60};
        // Where it lands: 100 units from the bot towards the thrower, or (far) next to the thrower itself.
        const float l = flat(ao, bo);
        const V3 at = far ? V3{bo[0] + (ao[0] - bo[0]) / l * 150, bo[1] + (ao[1] - bo[1]) / l * 150, bo[2]}
                          : V3{ao[0] + (bo[0] - ao[0]) / l * 100, ao[1] + (bo[1] - ao[1]) / l * 100, ao[2]};
        float yaw = 0, pitch = 0;
        V3 vel{};
        CHECK(iw2::game::botGrenadeAngles(eyeB, {at[0], at[1], at[2] + 4}, float(std::atof(field(g.w, frag, "projectilespeed").c_str())),
                                          float(std::atof(field(g.w, frag, "projectilespeedup").c_str())), 800.0f, false, yaw, pitch, vel));
        g.w.clientThrowGrenade(*b, frag, eyeB, {pitch, yaw, 0}, {0, 0, 0});
        float landDist = -1, endDist = -1;
        V3 lastGrenade{};
        bool heardSameFrame = false, live = true;
        iw2::game::MpBot* mb = g.w.mpBot(g.bots[0]);
        const float away = std::atan2(ao[1] - bo[1], ao[0] - bo[0]) * 57.29578f;
        for (int f = 0; f < 20 * 6; ++f) {
            a->obj->set("health", Value(100000));
            // Looking away from the thrower (close: until it hears something; far: all along, so it does not see the thrower).
            if (s->heard == heard0 || far) mb->yaw = away, mb->pitch = 0;
            const int before = s->heard;
            g.w.frame();
            if (s->heard != before && f == 0) heardSameFrame = true;
            bool found = false;
            for (const auto& ep : g.w.entities())
                if (ep->grenade && !ep->obj->removed) {
                    found = true;
                    if (ep->grenade->bounceEvents.empty()) continue;
                    lastGrenade = ep->obj->get("origin").asVector();
                    if (landDist < 0) landDist = flat(a->obj->get("origin").asVector(), lastGrenade);
                }
            if (live && !found && landDist >= 0) live = false, endDist = flat(a->obj->get("origin").asVector(), lastGrenade); // it went off
        }
        std::cout << "grenade " << (far ? "going off far" : "landing close") << ": heard " << s->heard - heard0 << " (landed "
                  << s->heardCue[int(BotCue::GrenadeLanded)] << ", went off " << s->heardCue[int(BotCue::Explosion)] << "), flees "
                  << s->grenadeFlees << ", distance to it on landing " << landDist << ", when it went off " << endDist << "\n";
        CHECK(!heardSameFrame);
        if (far) {
            CHECK(s->heardCue[int(BotCue::Explosion)] == 1);
            CHECK(s->grenadeFlees == 0);
        } else {
            CHECK(s->heardCue[int(BotCue::GrenadeLanded)] >= 1);
            CHECK(s->grenadeFlees == 1);
            CHECK(landDist > 0 && endDist > landDist + 150.0f); // it got away from the live grenade
        }
    }
    // e. The sounds of one fight: three enemies out of sight 900..1300 units away, close together, fire in turn every
    //    150 ms. The bot becomes aware (the shots of the others do not put off the cue it waits for), walks towards
    //    them, and keeps the way it planned, not planning anew for every sound.
    {
        Game g(vfs, "mp_toujane", "dm", 4, 1);
        run(g.w, 40);
        Entity* a = g.w.mpClientEntity(g.bots[0]);
        for (int k = 1; k < 4; ++k) g.w.mpBotSetCombat(g.bots[size_t(k)], false);
        auto& inv = g.w.inventory(*a); // no grenades: the walk is the thing measured
        for (const std::string& w : inv.owned)
            if (!field(g.w, w, "offhandclass").empty() && field(g.w, w, "offhandclass") != "none")
                inv.clip[field(g.w, w, "clipname")] = 0, inv.ammo[field(g.w, w, "ammoname")] = 0;
        V3 pa{}, pb{};
        CHECK(nodePair(g.w, 900, 1300, false, pa, pb));
        place(a, pa, pb);
        for (int k = 1; k < 4; ++k) { // the shooters stand close together at pb (a goal there keeps each in its place)
            const V3 at = iw2::game::navDropToFloor(g.w.collision(), {pb[0] + float(k - 2) * 40.0f, pb[1], pb[2] + 20});
            g.w.mpClientEntity(g.bots[size_t(k)])->obj->set("origin", Value(iw2::script::Vec3{at[0], at[1], at[2]}));
            g.w.mpBotSetGoal(g.bots[size_t(k)], at, 64.0f);
        }
        run(g.w, 2);
        iw2::game::MpBot* mb = g.w.mpBot(g.bots[0]);
        const float d0 = flat(a->obj->get("origin").asVector(), pb);
        int plans = 0, f = 0;
        int64_t lastPlan = mb->planMs;
        for (; f < 20 * 8; ++f) {
            for (int k = 1; k < 4; ++k) {
                Entity* s2 = g.w.mpClientEntity(g.bots[size_t(k)]);
                const V3 at = s2->obj->get("origin").asVector();
                if (f % 9 == (k - 1) * 3) g.w.debugFireBulletFrom(*s2, {at[0], at[1], at[2] + 60}, {at[0], at[1], at[2] + 400});
            }
            const V3 ao = a->obj->get("origin").asVector();
            mb->yaw = std::atan2(ao[1] - pb[1], ao[0] - pb[0]) * 57.29578f, mb->pitch = 0; // looking away: only the sounds lead it
            g.w.frame();
            if (mb->planMs != lastPlan) ++plans, lastPlan = mb->planMs;
        }
        const float d1 = flat(a->obj->get("origin").asVector(), pb);
        std::cout << "the sounds of one fight " << d0 << " units away: after 8 s " << d1 << " units away, heard " << g.w.mpBotStats(g.bots[0])->heard
                  << ", plans " << plans << "\n";
        CHECK(g.w.mpBotStats(g.bots[0])->heard >= 4);
        CHECK(d1 < d0 - 400.0f);
        CHECK(plans <= 2); // planning anew for every sound: one plan for each (5 here)
    }
}

// A client that is no bot and stands where the test puts it (the target of the behaviour scenarios); it cannot die.
struct Dummy {
    World& w;
    int client = -1;
    Entity* e = nullptr;
    explicit Dummy(World& world) : w(world) {
        client = w.mpConnect("dummy");
        w.mpBegin(client);
        w.frame();
        w.mpAutoJoin(client);
        e = w.mpClientEntity(client);
    }
    void hold(const V3& at, const V3& faceTo) {
        place(e, at, faceTo);
        e->obj->set("health", Value(100000));
    }
};

// 20. Giving way (docs/spec/mp-bots.md "Giving way"): on the rubble of mp_dawnville where the ctf carriers stood stuck
//     (the passage north of -336 -16784 is one body wide), a team mate parked on its objective in the way steps aside and
//     the bot walks through; an enemy that stands there (combat off, so nobody fights) is not asked. A player (no bot) of
//     the team whose command moves into the parked bot is given way too.
void giveWayTest(iw2::fs::Vfs& vfs) {
    const V3 from{-335, -16781, 55}, park{-336, -16730, 52}, to{-320, -16500, 40};
    for (const bool mates : {true, false}) {
        Game g(vfs, "mp_dawnville", "tdm", 3, 2);
        run(g.w, 60);
        // Two bots of one team (autoassign splits three as 2 / 1), or one of each.
        int a = -1, b = -1;
        for (int i : g.bots)
            for (int j : g.bots) {
                if (i == j || a >= 0) continue;
                const bool same = g.w.mpClientEntity(i)->obj->get("sessionteam").toString() == g.w.mpClientEntity(j)->obj->get("sessionteam").toString();
                if (same == mates) a = i, b = j;
            }
        CHECK(a >= 0 && b >= 0);
        if (a < 0 || b < 0) return;
        for (int c : g.bots) g.w.mpBotSetCombat(c, false);
        for (int c : g.bots)
            if (c != a && c != b) g.w.mpClientEntity(c)->obj->set("origin", Value(iw2::script::Vec3{1000, -15200, 0})), g.w.mpBotSetGoal(c, {1000, -15200, 0}, 400.0f);
        Entity* ea = g.w.mpClientEntity(a);
        Entity* eb = g.w.mpClientEntity(b);
        ea->obj->set("origin", Value(iw2::script::Vec3{from[0], from[1], from[2]}));
        eb->obj->set("origin", Value(iw2::script::Vec3{park[0], park[1], park[2]}));
        g.w.mpBotSetGoal(b, park, 300.0f); // reached: it stands
        g.w.mpBotSetGoal(a, to, 32.0f);
        int f = 0;
        for (; f < 20 * 15 && flat(ea->obj->get("origin").asVector(), to) > 48.0f; ++f) g.w.frame();
        const bool arrived = flat(ea->obj->get("origin").asVector(), to) <= 48.0f;
        std::cout << "give way (" << (mates ? "team mate" : "enemy") << " in the passage): " << (arrived ? "through after " : "not through in ") << f / 20.0f
                  << " s, steps aside " << g.w.mpBotStats(b)->giveWays << "\n";
        if (mates) {
            CHECK(arrived && f < 20 * 8);
            CHECK(g.w.mpBotStats(b)->giveWays >= 1);
        } else {
            CHECK(g.w.mpBotStats(b)->giveWays == 0);
        }
    }
    // A player (no bot) of the bot's team presses into it: its command moves forward towards the bot.
    {
        Game g(vfs, "mp_dawnville", "tdm", 3, 2);
        Dummy d(g.w);
        run(g.w, 60);
        int b = -1;
        for (int c : g.bots)
            if (g.w.mpClientEntity(c)->obj->get("sessionteam").toString() == d.e->obj->get("sessionteam").toString()) b = c;
        CHECK(b >= 0);
        if (b < 0) return;
        for (int c : g.bots) g.w.mpBotSetCombat(c, false);
        for (int c : g.bots)
            if (c != b) g.w.mpClientEntity(c)->obj->set("origin", Value(iw2::script::Vec3{1000, -15200, 0})), g.w.mpBotSetGoal(c, {1000, -15200, 0}, 400.0f);
        Entity* eb = g.w.mpClientEntity(b);
        eb->obj->set("origin", Value(iw2::script::Vec3{park[0], park[1], park[2]}));
        g.w.mpBotSetGoal(b, park, 300.0f);
        const V3 behind{park[0], park[1] - 32, park[2]};
        for (int f = 0; f < 20 * 2 && g.w.mpBotStats(b)->giveWays == 0; ++f) {
            d.hold(behind, park);
            if (iw2::game::MpPlayerInput* pi = g.w.mpPlayerInput(d.client)) pi->valid = true, pi->move.forwardMove = 1, pi->move.rightMove = 0;
            g.w.frame();
        }
        std::cout << "give way to a player pressing into the bot: steps aside " << g.w.mpBotStats(b)->giveWays << "\n";
        CHECK(g.w.mpBotStats(b)->giveWays >= 1);
    }
}

// 21a. A crevice of mp_matmata (docs/spec/mp-bots.md "Stuck watch", "Navigation graph"): a slot as wide as the box at about
//     4912 7696, its floor 8 units under the rim a node stood on. The graph has no node on the rim now; a bot that gets into
//     the slot anyway (it is put there: pmove holds it there without ground) is out of it within seconds, not after the 20 s
//     watch, and a bot placed on the slot's floor really is held there (the test is about this spot).
void creviceTest(iw2::fs::Vfs& vfs) {
    Game g(vfs, "mp_matmata", "sd", 1, 2);
    run(g.w, 60);
    const NavGraph& nav = g.w.mpNav();
    for (const V3& q : nav.nodes) CHECK(!(flat(q, V3{4912, 7696, 45}) < 12.0f && std::fabs(q[2] - 45.0f) < 16.0f));
    const int c = g.bots[0];
    g.w.mpBotSetCombat(c, false);
    Entity* e = g.w.mpClientEntity(c);
    const V3 slot{4913, 7695, 37};
    e->obj->set("origin", Value(iw2::script::Vec3{slot[0], slot[1], slot[2]}));
    const int far = nav.nearest(V3{4656, 7760, 3}, 64.0f, true);
    CHECK(far >= 0);
    if (far < 0) return;
    g.w.mpBotSetGoal(c, nav.nodes[size_t(far)], 32.0f);
    run(g.w, 3);
    const auto* b = g.w.mpBot(c);
    CHECK(b && flat(b->ps.origin, slot) < 4.0f && !b->ps.onGround); // held: a floor under it and no ground
    int f = 0;
    for (; f < 20 * 8 && flat(e->obj->get("origin").asVector(), slot) < 64.0f; ++f) g.w.frame();
    std::cout << "crevice: out of the slot after " << f / 20.0f << " s, lifts " << g.w.mpBotStats(c)->unwedges << "\n";
    CHECK(flat(e->obj->get("origin").asVector(), slot) >= 64.0f);
    CHECK(g.w.mpBotStats(c)->unwedges >= 1);
    CHECK(g.w.mpBotStats(c)->stuckEvents == 0);
}

// 21. A turret whose use place was off the graph (docs/spec/mp-bots.md "Turrets"): the machine gun in the loft of
//     mp_farmhouse, reached by a ladder only. A bot at the foot of the ladder that knows of an enemy out where the gun
//     overlooks (heard, not seen) climbs up and gets on the gun through the use search of its client.
void turretLoftTest(iw2::fs::Vfs& vfs) {
    Game g(vfs, "mp_farmhouse", "dm", 1, 2);
    Dummy d(g.w);
    d.e->obj->set("sessionstate", Value(std::string("spectator"))); // out of sight: only known
    run(g.w, 60);
    int gun = -1;
    for (size_t i = 0; i < g.w.mpBotTurrets().size(); ++i)
        if (flat(g.w.mpBotTurrets()[i].origin, V3{-2236, 1329, 293}) < 32) gun = int(i);
    CHECK(gun >= 0);
    if (gun < 0) return;
    const iw2::game::BotTurret& t = g.w.mpBotTurrets()[size_t(gun)];
    CHECK(t.node >= 0);
    // A place the gun overlooks: a main node 600..1500 units out inside its arcs with a clear line from the gun.
    const NavGraph& nav = g.w.mpNav();
    V3 out{};
    bool found = false;
    for (size_t i = 0; i < nav.mainCount && !found; i += 7) {
        const V3 q = nav.nodes[i];
        const float dd = flat(q, t.origin);
        if (dd < 600 || dd > 1500 || !iw2::game::botTurretCovers(t, q, -10.0f)) continue;
        found = g.w.collision().trace({t.origin[0], t.origin[1], t.origin[2] + 16}, {q[0], q[1], q[2] + 40}, {0, 0, 0}, {0, 0, 0},
                                      iw2::game::Contents::SightMaskNoCharacters, -1).fraction >= 0.999f;
        if (found) out = q;
    }
    CHECK(found);
    Entity* a = g.w.mpClientEntity(g.bots[0]);
    a->obj->set("origin", Value(iw2::script::Vec3{-2296, 1430, -1.875f}));
    run(g.w, 1);
    iw2::game::MpBot* mb = g.w.mpBot(g.bots[0]);
    mb->turretCooldownMs = 0; // the turret check rests 5 s after a spawn (or a move by script)
    int f = 0;
    for (; f < 20 * 40 && !g.w.mpClientOnTurret(g.bots[0]); ++f) {
        mb->enemy = d.e->number, mb->enemySeenMs = g.w.debugTimeMs(), mb->enemyLastPos = out, mb->enemyHeardOnly = true;
        g.w.frame();
    }
    const auto o = a->obj->get("origin").asVector();
    std::cout << "loft turret: " << (g.w.mpClientOnTurret(g.bots[0]) ? "on the gun after " : "not on the gun in ") << f / 20.0f << " s, at " << o[0] << " "
              << o[1] << " " << o[2] << ", mounts " << g.w.mpBotStats(g.bots[0])->turretMounts << "\n";
    CHECK(g.w.mpClientOnTurret(g.bots[0]));
    CHECK(g.w.mpBotStats(g.bots[0])->turretMounts == 1);
    CHECK(o[2] > 250); // up in the loft
}

// The humanlike behaviour scenarios (docs/spec/mp-bots.md): one veteran bot and a dummy on mp_toujane.
int behaviourTests(iw2::fs::Vfs& vfs) {
    struct Scene {
        Game g;
        Dummy d;
        Entity* a = nullptr;
        iw2::game::Inventory* inv = nullptr;
        std::string main, pistol;
        Scene(iw2::fs::Vfs& vfs, int skill = 3) : g(vfs, "mp_toujane", "dm", 1, skill), d(g.w) {
            run(g.w, 40);
            a = g.w.mpClientEntity(g.bots[0]);
            inv = &g.w.inventory(*a);
            const bool p1 = g.w.mpWeaponClass(inv->slot[1]) == "pistol";
            main = p1 ? inv->slot[2] : inv->slot[1], pistol = p1 ? inv->slot[1] : inv->slot[2];
        }
        int& clip(const std::string& w) { return inv->clip[field(g.w, w, "clipname")]; }
        int& reserve(const std::string& w) { return inv->ammo[field(g.w, w, "ammoname")]; }
        const MpBotStats& stats() { return *g.w.mpBotStats(g.bots[0]); }
        // Takes the bot's grenades away (a grenade thrown at where the dummy was seen last would kill it between two parts
        // of a scenario, when the test does not hold its health).
        void noGrenades() {
            for (const std::string& w : inv->owned)
                if (!field(g.w, w, "offhandclass").empty() && field(g.w, w, "offhandclass") != "none") clip(w) = 0, reserve(w) = 0;
        }
        // Puts the bot at `at` looking at `to` (its controller's view, which otherwise turns along its walk at once).
        // Held for 12 frames: the bots' list of the items on the ground is refreshed twice a second.
        void stand(const V3& at, const V3& to, int frames = 12) {
            place(a, at, to);
            for (int i = 0; i < frames && g.w.mpBot(g.bots[0])->detour.kind == iw2::game::BotDetour::None; ++i) {
                g.w.frame(); // the controller restarts at the new place
                iw2::game::MpBot* mb = g.w.mpBot(g.bots[0]);
                const V3 o = a->obj->get("origin").asVector();
                mb->yaw = std::atan2(to[1] - o[1], to[0] - o[0]) * 57.29578f;
                mb->pitch = 0;
                mb->detourCheckMs = 0;
            }
        }
        std::string held() { return a->obj->get("$weapon").toString(); }
    };

    // 1. An empty pistol in the hand, the main weapon with an empty clip and a reserve, an enemy in sight at 550-800
    //    units: the bot takes the main weapon, reloads it and fires; it is not left standing with nothing to fight with.
    {
        Scene sc(vfs);
        CHECK(sc.a->obj->get("sessionstate").toString() == "playing" && sc.g.w.mpWeaponClass(sc.pistol) == "pistol");
        V3 pa{}, pb{};
        CHECK(nodePair(sc.g.w, 550, 800, true, pa, pb));
        sc.d.hold(pb, pa);
        sc.stand(pa, pb, 1);
        sc.clip(sc.main) = 0, sc.reserve(sc.main) = 40, sc.clip(sc.pistol) = 0, sc.reserve(sc.pistol) = 0;
        sc.a->obj->set("$weapon", Value(sc.pistol));
        for (int f = 0; f < 20 * 8; ++f) {
            sc.d.hold(pb, pa);
            sc.g.w.frame();
        }
        std::cout << "empty pistol: held " << sc.held() << " shots " << sc.stats().shots << " reloads " << sc.stats().reloads << " defenceless frames "
                  << sc.stats().defencelessFrames << "\n";
        CHECK(sc.held() == sc.main);
        CHECK(sc.stats().reloads >= 1);
        CHECK(sc.stats().shots > 0);
        CHECK(sc.stats().defencelessFrames <= 4);
    }

    // 2. No round in either weapon and an enemy 120-220 units away: the bot charges and hits with the melee blow.
    {
        Scene sc(vfs);
        V3 pa{}, pb{};
        CHECK(nodePair(sc.g.w, 120, 220, true, pa, pb));
        sc.d.hold(pb, pa);
        sc.clip(sc.main) = sc.reserve(sc.main) = sc.clip(sc.pistol) = sc.reserve(sc.pistol) = 0;
        sc.stand(pa, pb, 10); // looking at the enemy for half a second: it is noticed
        for (int f = 0; f < 20 * 8; ++f) {
            sc.d.hold(pb, pa);
            sc.g.w.frame();
        }
        std::cout << "melee: swings " << sc.stats().melees << " hits " << sc.stats().meleeHits << "\n";
        CHECK(sc.stats().melees >= 1);
        CHECK(sc.stats().meleeHits >= 1);
        CHECK(sc.stats().shots == 0);
    }

    // 3. No round in either weapon and an enemy in sight 700-1000 units away: the bot does not stand there, it walks on.
    {
        Scene sc(vfs);
        V3 pa{}, pb{};
        CHECK(nodePair(sc.g.w, 700, 1000, true, pa, pb));
        sc.d.hold(pb, pa);
        sc.stand(pa, pb, 1);
        sc.clip(sc.main) = sc.reserve(sc.main) = sc.clip(sc.pistol) = sc.reserve(sc.pistol) = 0;
        float walked = 0;
        V3 prev = pa;
        for (int f = 0; f < 20 * 6; ++f) {
            sc.d.hold(pb, pa);
            sc.g.w.frame();
            const V3 o = sc.a->obj->get("origin").asVector();
            walked += flat(o, prev);
            prev = o;
        }
        std::cout << "unarmed: walked " << walked << " units in 6 s\n";
        CHECK(walked > 400);
    }

    // 4. Tactical reload: nobody around and the clip under 70%: the bot reloads to a full clip.
    {
        Scene sc(vfs);
        sc.d.e->obj->set("sessionstate", Value(std::string("spectator"))); // no enemy
        const int size = std::atoi(field(sc.g.w, sc.main, "clipsize").c_str());
        sc.a->obj->set("$weapon", Value(sc.main));
        run(sc.g.w, 30);
        sc.clip(sc.main) = std::max(1, size * 2 / 5);
        run(sc.g.w, 20 * 9);
        std::cout << "tactical reload: clip " << sc.clip(sc.main) << " / " << size << " reloads " << sc.stats().reloads << "\n";
        CHECK(sc.clip(sc.main) == size);
        CHECK(sc.stats().reloads >= 1);
    }

    // 5. A weapon on the ground: the bot's main weapon has no round left; 200-400 units ahead lies another weapon (the
    //    dummy's, dropped as the gametype drops a dead player's). The bot walks there, looks at it and takes it with the
    //    use button; its empty weapon lies there instead.
    {
        Scene sc(vfs);
        V3 pa{}, pb{};
        CHECK(nodePair(sc.g.w, 200, 400, true, pa, pb));
        sc.d.hold(pb, pa);
        auto& dinv = sc.g.w.inventory(*sc.d.e);
        std::string dropped;
        for (int sl = 1; sl <= 2; ++sl)
            if (!dinv.slot[size_t(sl)].empty() && sc.g.w.mpWeaponClass(dinv.slot[size_t(sl)]) != "pistol" && dinv.slot[size_t(sl)] != sc.main)
                dropped = dinv.slot[size_t(sl)];
        if (dropped.empty()) { // the same weapon as the bot's: give the dummy another one to drop
            dropped = sc.main == "thompson_mp" ? "m1garand_mp" : "thompson_mp";
            dinv.owned.insert(dropped);
            dinv.slot[1] = dropped;
            dinv.clip[field(sc.g.w, dropped, "clipname")] = 8;
            dinv.ammo[field(sc.g.w, dropped, "ammoname")] = 40;
        }
        Entity* item = sc.g.w.mpDropWeapon(*sc.d.e, dropped);
        CHECK(item != nullptr);
        sc.d.e->obj->set("sessionstate", Value(std::string("spectator"))); // nobody to fight
        sc.clip(sc.main) = 0, sc.reserve(sc.main) = 0;
        sc.stand(pa, pb);
        int f = 0, usePresses = 0;
        for (; f < 20 * 15 && !sc.inv->owned.count(dropped); ++f) {
            sc.g.w.frame();
            usePresses += (sc.g.w.mpState().clients[size_t(sc.g.bots[0])].buttons & 0x8) != 0;
        }
        run(sc.g.w, 20);
        std::cout << "pickup: " << dropped << " after " << f / 20.0f << " s, use presses " << usePresses << ", held " << sc.held() << ", pickups "
                  << sc.stats().weaponPickups << "\n";
        CHECK(sc.inv->owned.count(dropped) == 1);
        CHECK(sc.inv->owned.count(sc.main) == 0); // the empty weapon was the one given up
        CHECK(sc.inv->owned.count(sc.pistol) == 1);
        CHECK(usePresses == 1);                    // taken with the use button, pressed once
        CHECK(sc.held() == dropped);
        CHECK(sc.stats().weaponPickups == 1);
        CHECK(sc.clip(dropped) + sc.reserve(dropped) > 0);
        bool left = false; // the bot's old weapon lies where the item lay
        for (const auto& e : sc.g.w.entities())
            if (!e->obj->removed && e->classname == "weapon_" + sc.main) left = true;
        CHECK(left);
    }

    // 6. Ammunition: a weapon like the bot's own lies ahead and the bot's reserve is low: it walks over the item and
    //    has the rounds, without pressing use.
    {
        Scene sc(vfs);
        V3 pa{}, pb{};
        CHECK(nodePair(sc.g.w, 200, 400, true, pa, pb));
        sc.d.hold(pb, pa);
        auto& dinv = sc.g.w.inventory(*sc.d.e);
        dinv.owned.insert(sc.main);
        dinv.slot[1] = sc.main;
        dinv.clip[field(sc.g.w, sc.main, "clipname")] = 5;
        dinv.ammo[field(sc.g.w, sc.main, "ammoname")] = 30;
        CHECK(sc.g.w.mpDropWeapon(*sc.d.e, sc.main) != nullptr);
        sc.d.e->obj->set("sessionstate", Value(std::string("spectator")));
        sc.reserve(sc.main) = 0;
        sc.stand(pa, pb);
        int f = 0, usePresses = 0;
        for (; f < 20 * 15 && sc.reserve(sc.main) == 0; ++f) {
            sc.g.w.frame();
                        usePresses += (sc.g.w.mpState().clients[size_t(sc.g.bots[0])].buttons & 0x8) != 0;
        }
        std::cout << "ammunition: reserve " << sc.reserve(sc.main) << " after " << f / 20.0f << " s, pickups " << sc.stats().ammoPickups << "\n";
        CHECK(sc.reserve(sc.main) >= 30);
        CHECK(usePresses == 0);
        CHECK(sc.stats().ammoPickups >= 1);
    }

    // 7. A mounted machine gun: an enemy fires out of the bot's sight where a turret near the bot overlooks it. The bot
    //    walks behind the gun, presses use, turns the gun on the enemy and fires it. Hit from behind (outside the gun's
    //    arcs) it gets off; put back on, with nobody around any more it gets off after its hold time.
    {
        Scene sc(vfs);
        World& w = sc.g.w;
        const NavGraph& nav = w.mpNav();
        const auto& turrets = w.mpBotTurrets();
        std::cout << "turrets on mp_toujane: " << turrets.size();
        int usable = 0;
        for (const auto& t : turrets) usable += t.node >= 0 && t.coverage >= 12;
        std::cout << ", usable by bots " << usable << "\n";
        CHECK(usable > 0);
        // A turret, a place for the enemy the gun overlooks 500-1500 units away, and a start for the bot 100-300 units
        // from the place behind the gun.
        int ti = -1;
        V3 enemyAt{}, startAt{}, behindAt{};
        for (size_t i = 0; i < turrets.size() && ti < 0; ++i) {
            const auto& t = turrets[i];
            if (t.node < 0 || t.coverage < 12) continue;
            const V3 gun{t.origin[0], t.origin[1], t.origin[2] + 16};
            bool haveEnemy = false, haveStart = false, haveBehind = false;
            for (size_t n = 0; n < nav.mainCount && !haveEnemy; n += 7) {
                const V3 q = nav.nodes[n];
                const float d = flat(q, t.origin);
                if (d < 500 || d > 1500 || !iw2::game::botTurretCovers(t, q, -10)) continue;
                if (w.collision().trace(gun, {q[0], q[1], q[2] + 40}, {0, 0, 0}, {0, 0, 0}, iw2::game::Contents::SightMaskNoCharacters, t.entity).fraction < 0.999f) continue;
                if (w.collision().trace(gun, {q[0], q[1], q[2] + 60}, {0, 0, 0}, {0, 0, 0}, iw2::game::Contents::SightMaskNoCharacters, t.entity).fraction < 0.999f) continue;
                enemyAt = q, haveEnemy = true;
            }
            for (int n : nav.within(t.usePos, 300.0f)) {
                const V3 q = nav.nodes[size_t(n)];
                if (flat(q, t.usePos) < 100 || std::fabs(q[2] - t.usePos[2]) > 20 || w.paths().findPath(n, t.node).empty()) continue;
                // out of the enemy's sight, so that the bot hears it and does not fight it from where it stands
                if (w.collision().trace({q[0], q[1], q[2] + 60}, {enemyAt[0], enemyAt[1], enemyAt[2] + 60}, {0, 0, 0}, {0, 0, 0}, iw2::game::Contents::SightMaskNoCharacters, -1).fraction >= 0.999f) continue;
                startAt = q, haveStart = true;
                break;
            }
            // A place 150-500 units away outside the arcs from which the bot behind the gun is seen (the flank).
            for (size_t n = 0; n < nav.mainCount && !haveBehind; n += 3) {
                const V3 q = nav.nodes[n];
                const float d = flat(q, t.origin);
                if (d < 150 || d > 500 || iw2::game::botTurretCovers(t, q, 25)) continue;
                if (w.collision().trace({q[0], q[1], q[2] + 60}, {t.usePos[0], t.usePos[1], t.usePos[2] + 40}, {0, 0, 0}, {0, 0, 0}, iw2::game::Contents::SightMaskNoCharacters, -1).fraction < 0.999f) continue;
                behindAt = q, haveBehind = true;
            }
            if (haveEnemy && haveStart && haveBehind) ti = int(i);
        }
        CHECK(ti >= 0);
        if (ti >= 0) {
            const auto t = turrets[size_t(ti)];
            Entity* te = nullptr;
            for (const auto& e : w.entities())
                if (e->number == t.entity) te = e.get();
            place(sc.a, startAt, t.usePos);
            w.frame();
            w.mpBot(sc.g.bots[0])->turretCooldownMs = 0; // a bot leaves turrets alone for 5 s after its spawn
            int usePresses = 0, f = 0;
            auto step = [&](const V3& dummyAt) {
                sc.d.hold(dummyAt, t.origin);
                w.frame();
                usePresses += (w.mpState().clients[size_t(sc.g.bots[0])].buttons & 0x8) != 0;
            };
            for (; f < 20 * 25 && !w.mpClientOnTurret(sc.g.bots[0]); ++f) {
                if (f % 20 == 5) { // a shot into the air every second: heard, not seen
                    const V3 eye{enemyAt[0], enemyAt[1], enemyAt[2] + 60};
                    w.clientFire(*sc.d.e, eye, {0, 0, 1});
                }
                step(enemyAt);
            }
            std::cout << "turret: on the gun after " << f / 20.0f << " s, use presses " << usePresses << "\n";
            CHECK(w.mpClientOnTurret(sc.g.bots[0]));
            CHECK(usePresses == 1);
            CHECK(te && te->turret && te->turret->inUse);
            CHECK(sc.stats().turretMounts == 1);
            const int shotsBefore = sc.stats().shots;
            for (f = 0; f < 20 * 10 && sc.stats().turretShots < 20; ++f) step(enemyAt);
            std::cout << "turret: " << sc.stats().turretShots << " shots in " << f / 20.0f << " s, hits " << sc.stats().turretHits << "\n";
            CHECK(sc.stats().turretShots >= 20);
            CHECK(sc.stats().turretHits > 0);            // the gun was turned on the enemy
            CHECK(sc.stats().shots == shotsBefore);      // not a shot from the hand weapon meanwhile
            CHECK(w.mpClientOnTurret(sc.g.bots[0]));
            // Flanked: the enemy stands outside the arcs and hits the bot.
            int hitFrame = -1;
            for (f = 0; f < 20 * 4 && w.mpClientOnTurret(sc.g.bots[0]); ++f) {
                if (f >= 2 && hitFrame < 0) { // one bullet into the bot's back (the first that lands)
                    const V3 bo = sc.a->obj->get("origin").asVector();
                    sc.a->obj->set("health", Value(5000)); // the bullet must not kill
                    const float z = 30.0f + float((f - 2) % 3) * 10.0f;
                    w.debugFireBulletFrom(*sc.d.e, {behindAt[0], behindAt[1], behindAt[2] + 60}, {bo[0], bo[1], bo[2] + z});
                    if (sc.a->obj->get("health").asInt() < 5000) hitFrame = f;
                    sc.a->obj->set("health", Value(100));
                }
                step(behindAt);
            }
            std::cout << "turret: hit from behind in frame " << hitFrame << ", off the gun in frame " << f << ", use presses " << usePresses << "\n";
            CHECK(hitFrame >= 0 && f - hitFrame <= 3); // off at once, not after the second out of the arcs
            CHECK(!w.mpClientOnTurret(sc.g.bots[0]));
            CHECK(te && !te->turret->inUse);
            CHECK(usePresses == 2);
            const V3 off = sc.a->obj->get("origin").asVector();
            CHECK(flat(off, t.usePos) < 64); // back where it got on
        }
    }

    // 8. Quiet: a bot on a turret with nobody around gets off by itself after its hold time (12-25 s).
    {
        Scene sc(vfs);
        World& w = sc.g.w;
        const auto& turrets = w.mpBotTurrets();
        sc.d.e->obj->set("sessionstate", Value(std::string("spectator")));
        int ti = -1;
        for (size_t i = 0; i < turrets.size() && ti < 0; ++i)
            if (turrets[i].node >= 0 && turrets[i].coverage >= 25) ti = int(i);
        CHECK(ti >= 0);
        if (ti >= 0) {
            const auto t = turrets[size_t(ti)];
            // The bot is sent behind the gun; from there the turret is on its way (the idle rule: a gun that overlooks
            // much, now and then). The test asks again until it is on.
            int f = 0;
            place(sc.a, t.usePos, t.origin);
            for (; f < 20 * 60 && !w.mpClientOnTurret(sc.g.bots[0]); ++f) {
                if (f % 100 == 0) place(sc.a, t.usePos, t.origin);
                iw2::game::MpBot* mb = w.mpBot(sc.g.bots[0]);
                mb->turretCooldownMs = 0;
                for (int64_t& until : mb->turretIgnoreUntilMs) until = 0;
                w.frame();
            }
            std::cout << "turret (idle): on the gun after " << f / 20.0f << " s\n";
            CHECK(w.mpClientOnTurret(sc.g.bots[0]));
            for (f = 0; f < 20 * 40 && w.mpClientOnTurret(sc.g.bots[0]); ++f) w.frame();
            std::cout << "turret (idle): off after " << f / 20.0f << " s on it, shots " << sc.stats().turretShots << "\n";
            CHECK(!w.mpClientOnTurret(sc.g.bots[0]));
            CHECK(f >= 20 * 11 && f <= 20 * 27);
            CHECK(sc.stats().turretShots == 0); // nobody to fire at
        }
    }

    // 9. Perception follows the bot's view. The bot stands and looks along a line; an enemy stands still in its sight
    //    line of fire at some angle off that line, 400-700 units away.
    {
        Scene sc(vfs);
        World& w = sc.g.w;
        V3 pa{}, pb{};
        CHECK(nodePair(w, 400, 700, true, pa, pb));
        const float toEnemy = std::atan2(pb[1] - pa[1], pb[0] - pa[0]) * 57.29578f;
        iw2::game::MpBot* mb = w.mpBot(sc.g.bots[0]);
        sc.noGrenades();
        sc.d.hold(pb, pa);
        // Holds the bot at pa looking `off` degrees to the left of the enemy for `secs`; returns the time until it
        // noticed the enemy (-1: never). The API goal keeps it standing there.
        auto watch = [&](float off, float secs, bool scoped, bool enemyFires = false) {
            place(sc.a, pa, pb);
            w.mpBotSetGoal(sc.g.bots[0], pa, 64.0f);
            sc.d.e->obj->set("sessionstate", Value(std::string("spectator")));
            run(w, 30); // settles; forgets the enemy
            mb = w.mpBot(sc.g.bots[0]);
            mb->forceAds = scoped;
            mb->enemy = -1;
            for (float& nt : mb->notice) nt = 0;
            const int acquired = sc.stats().acquired, heard = sc.stats().heard;
            sc.d.e->obj->set("sessionstate", Value(std::string("playing")));
            for (int f = 0; f < int(secs * 20); ++f) {
                sc.d.hold(pb, pa);
                if (!mb->enemyVisible && sc.stats().heard == heard) mb->yaw = toEnemy + off, mb->pitch = 0; // until it reacts by itself
                // The scope is the test's doing (forceAds); a bot aims only at an enemy it sees, so once it has heard
                // something it looks for it without the scope.
                if (sc.stats().heard != heard) mb->forceAds = false;
                if (enemyFires && f == 20) w.clientFire(*sc.d.e, {pb[0], pb[1], pb[2] + 60}, {0, 0, 1});
                w.frame();
                if (sc.stats().acquired > acquired) return float(f + 1) / 20.0f;
            }
            return -1.0f;
        };
        // At the hip: in the middle of the view, at 30 degrees, at 2 o'clock (60), 3 o'clock (90) and behind.
        const float t0 = watch(0, 4, false), t30 = watch(30, 4, false), t60 = watch(60, 4, false), t90 = watch(90, 4, false), t180 = watch(180, 4, false);
        std::cout << "noticed at the hip: ahead " << t0 << " s, 30 deg " << t30 << " s, 60 deg " << t60 << " s, 90 deg " << t90 << " s, behind " << t180 << " s\n";
        CHECK(t0 > 0 && t0 <= 0.6f);
        CHECK(t30 > t0);            // slower away from the middle
        CHECK(t30 > 0 && t30 <= 1.5f);
        CHECK(t60 < 0 && t90 < 0 && t180 < 0); // outside the screen: not at all
        // Through a scope: give the bot a scoped rifle.
        const std::string scopeW = "springfield_mp";
        sc.inv->owned.insert(scopeW);
        sc.inv->slot[1] = scopeW, sc.inv->slot[2] = sc.pistol;
        sc.clip(scopeW) = 5, sc.reserve(scopeW) = 30;
        sc.a->obj->set("$weapon", Value(scopeW));
        run(w, 40);
        const float s0 = watch(0, 4, true), s5 = watch(15, 4, true), s30 = watch(30, 6, true), s60 = watch(60, 6, true);
        std::cout << "noticed through the scope: ahead " << s0 << " s, 15 deg " << s5 << " s, 30 deg " << s30 << " s, 60 deg " << s60 << " s (aim fraction "
                  << mb->weapon.adsFrac() << ")\n";
        CHECK(mb->weapon.adsFrac() == 1.0f);
        CHECK(s0 > 0 && s0 <= 0.6f);
        CHECK(s5 < 0 && s30 < 0 && s60 < 0); // what the hip sees at 30 degrees the scope does not: nothing outside its circle
        // The enemy at 2 o'clock fires: the scoped bot hears it, turns and only then sees it.
        const int heardBefore = sc.stats().heard;
        const float sf = watch(60, 6, true, true);
        std::cout << "scoped, the enemy at 60 deg fires after 1 s: seen after " << sf << " s, heard " << sc.stats().heard - heardBefore << "\n";
        CHECK(sc.stats().heard == heardBefore + 1);
        CHECK(sf > 1.3f && sf < 4.0f); // the shot at 1.0 s, then the delay, the turn and the noticing
        mb->forceAds = false;
        // Behind the bot (hip) and hitting it: the hit turns the bot around.
        w.mpBotSetGoal(sc.g.bots[0], pa, 64.0f);
        sc.d.e->obj->set("sessionstate", Value(std::string("spectator")));
        run(w, 30);
        mb->enemy = -1;
        for (float& nt : mb->notice) nt = 0;
        sc.d.e->obj->set("sessionstate", Value(std::string("playing")));
        const int acq = sc.stats().acquired;
        int seenFrame = -1, hitFrame = -1;
        for (int f = 0; f < 20 * 5 && seenFrame < 0; ++f) {
            sc.d.hold(pb, pa);
            if (hitFrame < 0) mb->yaw = toEnemy + 180, mb->pitch = 0;
            if (f >= 20 && hitFrame < 0) {
                sc.a->obj->set("health", Value(5000));
                const V3 ao = sc.a->obj->get("origin").asVector();
                w.debugFireBulletFrom(*sc.d.e, {pb[0], pb[1], pb[2] + 60}, {ao[0], ao[1], ao[2] + 30.0f + float(f % 3) * 10.0f});
                if (sc.a->obj->get("health").asInt() < 5000) hitFrame = f;
                sc.a->obj->set("health", Value(100));
            }
            w.frame();
            if (sc.stats().acquired > acq) seenFrame = f;
        }
        std::cout << "hit from behind in frame " << hitFrame << ", the attacker seen in frame " << seenFrame << "\n";
        CHECK(hitFrame >= 20 && seenFrame > hitFrame + 4 && seenFrame < hitFrame + 40); // not at once: a delay, then the turn
    }

    // 11. Aim and shooting: a bot fights an enemy that stands still 500-700 units away for 25 s. Its view moves smoothly
    //     (never faster than the skill's turn rate plus the weapon's kick), its shots leave along its view (some miss: the
    //     hand's error and the weapon's spread), the view is kicked by its shots, and a veteran hits more than a recruit.
    {
        float rate[2] = {0, 0};
        for (int k = 0; k < 2; ++k) {
            const int skill = k == 0 ? 0 : 3;
            Scene sc(vfs, skill);
            World& w = sc.g.w;
            V3 pa{}, pb{};
            CHECK(nodePair(w, 500, 700, true, pa, pb));
            sc.noGrenades();
            // Both get the same semi-automatic rifle with plenty of rounds (aimed down the sights at that distance, so
            // the weapon's own spread is small and the hand's error decides).
            const std::string wn = "m1garand_mp";
            sc.inv->owned.insert(wn);
            sc.inv->slot[1] = wn, sc.inv->slot[2].clear();
            sc.clip(wn) = 8, sc.reserve(wn) = 400;
            sc.a->obj->set("$weapon", Value(wn));
            sc.d.hold(pb, pa);
            w.mpBotSetGoal(sc.g.bots[0], pa, 64.0f); // stays about its place
            sc.stand(pa, pb, 1);
            const iw2::game::MpBot* mb = w.mpBot(sc.g.bots[0]);
            float maxStep = 0, maxKick = 0, prevYaw = mb->yaw;
            for (int f = 0; f < 20 * 25; ++f) {
                sc.d.hold(pb, pa);
                w.frame();
                float step = mb->yaw - prevYaw;
                while (step > 180) step -= 360;
                while (step < -180) step += 360;
                prevYaw = mb->yaw;
                maxStep = std::max(maxStep, std::fabs(step));
                maxKick = std::max(maxKick, std::fabs(mb->kick.viewKick()[0]));
            }
            const MpBotStats& st = sc.stats();
            rate[k] = float(st.hits) / float(std::max(1, st.shots));
            std::cout << "aim " << iw2::game::botSkill(skill).name << ": shots " << st.shots << " hits " << st.hits << " (" << 100.0f * rate[k]
                      << "%), fastest view turn " << maxStep * 20.0f << " deg/s, largest view kick " << maxKick << " deg\n";
            CHECK(st.shots >= 30);
            CHECK(st.hits >= 1 && st.hits < st.shots);                          // hits and misses
            CHECK(maxStep * 20.0f <= iw2::game::botSkill(skill).turnRate + 200.0f); // the spring's rate limit plus the kick's motion
            CHECK(maxKick > 0.2f);                                              // the shots kick the view up
        }
        CHECK(rate[1] > rate[0] * 1.3f);
    }

    // 12. The aimed body part follows what is in sight: the chest; the head when only the head is; the legs when only they are.
    {
        Scene sc(vfs);
        World& w = sc.g.w;
        V3 pa{}, pb{};
        CHECK(nodePair(w, 300, 500, true, pa, pb));
        sc.noGrenades();
        sc.d.hold(pb, pa);
        w.mpBotSetGoal(sc.g.bots[0], pa, 64.0f);
        sc.stand(pa, pb, 6);
        iw2::game::MpBot* mb = w.mpBot(sc.g.bots[0]);
        CHECK(mb->enemyVisible && mb->enemyParts == 7); // head, chest and legs in sight
        const V3 o = sc.d.e->obj->get("origin").asVector();
        mb->enemyParts = 7;
        const V3 chest = w.mpBotAimPoint(*mb, *sc.d.e);
        mb->enemyParts = 1;
        const V3 head = w.mpBotAimPoint(*mb, *sc.d.e);
        mb->enemyParts = 4;
        const V3 legs = w.mpBotAimPoint(*mb, *sc.d.e);
        std::cout << "aimed points above the feet: chest " << chest[2] - o[2] << ", head " << head[2] - o[2] << ", legs " << legs[2] - o[2] << "\n";
        CHECK(chest[2] - o[2] > 35 && chest[2] - o[2] < 56);
        CHECK(head[2] > chest[2] + 4 && head[2] - o[2] < 72);
        CHECK(legs[2] < chest[2] - 8 && legs[2] - o[2] > 5);
        CHECK(flat(chest, o) < 20 && flat(head, o) < 20 && flat(legs, o) < 20);
    }

    // 13. A grenade is thrown as a player throws it: the bot knows an enemy it does not see 500-900 units away, turns
    //     its view to the throw (up, toward the place) and the grenade leaves along that view and goes off near the place.
    {
        Scene sc(vfs);
        World& w = sc.g.w;
        V3 pa{}, pb{};
        // A clear line between the two places (the flat arc of a throw is free); the enemy is not in the game any more
        // (a spectator), so the bot does not see it there: it knows the place only.
        CHECK(nodePair(w, 500, 900, true, pa, pb));
        sc.d.hold(pb, pa);
        sc.d.e->obj->set("sessionstate", Value(std::string("spectator")));
        place(sc.a, pa, pb);
        w.mpBotSetGoal(sc.g.bots[0], pa, 64.0f);
        iw2::game::MpBot* mb = w.mpBot(sc.g.bots[0]);
        const int before = sc.stats().grenades;
        float throwYawErr = 999, throwPitch = 0;
        V3 last{};
        bool flying = false, exploded = false;
        for (int f = 0; f < 20 * 20 && !exploded; ++f) {
            // The bot knows the enemy there (as after seeing it go behind cover).
            mb->enemy = sc.d.e->number, mb->enemySeenMs = w.timeMs(), mb->enemyLastPos = {pb[0], pb[1], pb[2]};
            mb->nextGrenadeMs = 0, mb->grenadeCheckMs = 0;
            const int thrown = sc.stats().grenades;
            w.frame();
            if (sc.stats().grenades > thrown && throwYawErr > 900) {
                float dy = mb->yaw - std::atan2(pb[1] - pa[1], pb[0] - pa[0]) * 57.29578f;
                while (dy > 180) dy -= 360;
                while (dy < -180) dy += 360;
                throwYawErr = dy, throwPitch = mb->pitch;
            }
            bool any = false;
            for (const auto& e : w.entities())
                if (!e->obj->removed && e->grenade) any = true, last = e->obj->get("origin").asVector();
            if (any) flying = true;
            else if (flying) exploded = true;
        }
        std::cout << "grenade: thrown " << sc.stats().grenades - before << ", view at the throw " << throwYawErr << " deg off the line, pitch " << throwPitch
                  << "; it went off " << flat(last, pb) << " units from the place\n";
        CHECK(sc.stats().grenades > before);
        CHECK(std::fabs(throwYawErr) < 12.0f); // the view was turned to the throw
        CHECK(throwPitch < -3.0f);             // and up
        CHECK(exploded && flat(last, pb) < 350.0f);
    }

    // 14. Cover: a hardened bot fights an enemy 400-700 units away and is hurt down to 25 health. It breaks off, goes to a
    //     place the enemy does not see and crouches there; with its health back it comes out again.
    {
        Scene sc(vfs, 2);
        World& w = sc.g.w;
        const NavGraph& nav = w.mpNav();
        V3 pa{}, pb{};
        bool found = false;
        for (size_t i = 0; i < nav.mainCount && !found; i += 61)
            for (size_t j = 3; j < nav.mainCount && !found; j += 23) {
                const V3 p = nav.nodes[i], q = nav.nodes[j];
                const float d = flat(p, q);
                if (d < 400 || d > 700 || std::fabs(p[2] - q[2]) > 32) continue;
                if (w.collision().trace({p[0], p[1], p[2] + 60}, {q[0], q[1], q[2] + 60}, {0, 0, 0}, {0, 0, 0}, iw2::game::Contents::SightMaskNoCharacters, -1).fraction < 0.999f) continue;
                if (w.mpBotFindCover(p, {q[0], q[1], q[2] + 64}, sc.a->number) < 0) continue;
                pa = p, pb = q, found = true;
            }
        CHECK(found);
        sc.noGrenades();
        sc.d.hold(pb, pa);
        sc.stand(pa, pb, 8);
        iw2::game::MpBot* mb = w.mpBot(sc.g.bots[0]);
        CHECK(mb->enemyVisible);
        int coveredFrame = -1, crouchedInCover = 0;
        for (int f = 0; f < 20 * 8; ++f) {
            sc.d.hold(pb, pa);
            sc.a->obj->set("health", Value(25));
            if (f < 20) mb->hurtMs = w.timeMs(); // under fire for the first second
            w.frame();
            const V3 o = sc.a->obj->get("origin").asVector();
            const bool hidden = w.collision().trace({pb[0], pb[1], pb[2] + 60}, {o[0], o[1], o[2] + 44}, {0, 0, 0}, {0, 0, 0}, iw2::game::Contents::SightMaskNoCharacters, -1).fraction < 0.999f;
            if (hidden && mb->inCover) {
                if (coveredFrame < 0) coveredFrame = f;
                crouchedInCover += mb->ps.stance == Stance::Crouch;
            }
        }
        std::cout << "cover: retreats " << sc.stats().retreats << ", out of the enemy's sight after " << (coveredFrame + 1) / 20.0f << " s, crouched there for "
                  << crouchedInCover / 20.0f << " s\n";
        CHECK(sc.stats().retreats >= 1);
        CHECK(coveredFrame >= 0 && coveredFrame < 20 * 5);
        CHECK(crouchedInCover >= 10);
        // Health back: it leaves the cover within a few seconds and fights again.
        const int shots = sc.stats().shots;
        int outFrame = -1;
        for (int f = 0; f < 20 * 12 && outFrame < 0; ++f) {
            sc.d.hold(pb, pa);
            sc.a->obj->set("health", Value(100));
            w.frame();
            if (mb->detour.kind != iw2::game::BotDetour::Cover) outFrame = f;
        }
        for (int f = 0; f < 20 * 8; ++f) {
            sc.d.hold(pb, pa);
            sc.a->obj->set("health", Value(100));
            w.frame();
        }
        std::cout << "cover: left after " << (outFrame + 1) / 20.0f << " s with its health back, shots since " << sc.stats().shots - shots << "\n";
        CHECK(outFrame >= 0);
        CHECK(sc.stats().shots > shots);
    }

    // 15. Steps: an enemy 250-350 units behind the bot. Standing still it is not heard; running, the bot becomes aware of
    //     it after a moment, at a place it believes (up to 30 degrees off for a veteran), and turns to it; crouched and
    //     moving it is not heard.
    {
        Scene sc(vfs);
        World& w = sc.g.w;
        V3 pa{}, pb{};
        CHECK(nodePair(w, 250, 350, true, pa, pb));
        sc.noGrenades();
        const float away = std::atan2(pa[1] - pb[1], pa[0] - pb[0]) * 57.29578f; // the bot looks away from the enemy
        iw2::game::MpBot* mb = w.mpBot(sc.g.bots[0]);
        auto listen = [&](float speed, int viewHeight, float secs) {
            place(sc.a, pa, pb);
            w.mpBotSetGoal(sc.g.bots[0], pa, 64.0f);
            sc.d.e->obj->set("sessionstate", Value(std::string("spectator")));
            run(w, 100); // forgets
            mb = w.mpBot(sc.g.bots[0]);
            mb->enemy = -1;
            sc.d.e->obj->set("sessionstate", Value(std::string("playing")));
            const int heard = sc.stats().heard;
            for (int f = 0; f < int(secs * 20); ++f) {
                sc.d.hold(pb, pa);
                // The enemy's movement as its mover would report it (the test holds it in place).
                iw2::game::MpPlayerInput* pi = w.mpPlayerInput(sc.d.client);
                pi->valid = true;
                pi->move.xySpeed = speed;
                pi->move.viewHeightTarget = viewHeight;
                if (sc.stats().heard == heard) mb->yaw = away, mb->pitch = 0;
                w.frame();
                if (sc.stats().heard > heard) return float(f + 1) / 20.0f;
            }
            return -1.0f;
        };
        const float still = listen(0, 60, 3), crouched = listen(190, 40, 3), running = listen(190, 60, 3);
        const V3 ao = sc.a->obj->get("origin").asVector();
        float dyaw = std::atan2(mb->enemyLastPos[1] - ao[1], mb->enemyLastPos[0] - ao[0]) * 57.29578f - std::atan2(pb[1] - ao[1], pb[0] - ao[0]) * 57.29578f;
        while (dyaw > 180) dyaw -= 360;
        while (dyaw < -180) dyaw += 360;
        std::cout << "steps behind the bot: standing still " << still << " s, crouched " << crouched << " s, running heard after " << running
                  << " s, believed " << dyaw << " deg off\n";
        CHECK(still < 0 && crouched < 0);
        CHECK(running > 0.15f && running < 1.5f);
        CHECK(mb->enemy == sc.d.e->number && mb->enemyHeardOnly);
        CHECK(std::fabs(dyaw) <= 31.0f);
        // It turns to the steps and sees who makes them.
        int seen = -1;
        for (int f = 0; f < 20 * 4 && seen < 0; ++f) {
            sc.d.hold(pb, pa);
            w.frame();
            if (mb->enemyVisible) seen = f;
        }
        CHECK(seen >= 0);
    }

    // 16. Looking around: a bot standing on its goal with nobody known looks along several open directions in turn.
    {
        Scene sc(vfs);
        World& w = sc.g.w;
        const NavGraph& nav = w.mpNav();
        // An open place: at least six of twelve directions free for 180 units and more.
        V3 pa{}, pb{};
        bool found = false;
        for (size_t i = 0; i < nav.mainCount && !found; i += 37) {
            const V3 p = nav.nodes[i];
            const V3 eye{p[0], p[1], p[2] + 60};
            int open = 0;
            for (int k = 0; k < 12; ++k) {
                const float a = float(k) * 30.0f * 0.0174532925f;
                open += w.collision().trace(eye, {eye[0] + std::cos(a) * 600, eye[1] + std::sin(a) * 600, eye[2]}, {0, 0, 0}, {0, 0, 0},
                                            iw2::game::Contents::SightMaskNoCharacters, -1).fraction >= 0.3f;
            }
            if (open >= 6) pa = p, pb = {p[0] + 100, p[1], p[2]}, found = true;
        }
        CHECK(found);
        sc.d.e->obj->set("sessionstate", Value(std::string("spectator")));
        place(sc.a, pa, pb);
        w.mpBotSetGoal(sc.g.bots[0], pa, 64.0f);
        run(w, 40);
        const iw2::game::MpBot* mb = w.mpBot(sc.g.bots[0]);
        std::vector<float> looks;
        float last = 1e9f, maxStep = 0, prevYaw = mb->yaw;
        int closed = 0;
        for (int f = 0; f < 20 * 30; ++f) {
            w.frame();
            float step = mb->yaw - prevYaw;
            while (step > 180) step -= 360;
            while (step < -180) step += 360;
            maxStep = std::max(maxStep, std::fabs(step));
            prevYaw = mb->yaw;
            if (mb->scanYaw != last) {
                last = mb->scanYaw;
                looks.push_back(last);
                const V3 o = sc.a->obj->get("origin").asVector();
                const V3 eye{o[0], o[1], o[2] + 60};
                const float a = last * 0.0174532925f;
                if (w.collision().trace(eye, {eye[0] + std::cos(a) * 600, eye[1] + std::sin(a) * 600, eye[2]}, {0, 0, 0}, {0, 0, 0},
                                        iw2::game::Contents::SightMaskNoCharacters, sc.a->number).fraction < 0.29f)
                    ++closed;
            }
        }
        float spread = 0;
        for (float a : looks)
            for (float b2 : looks) {
                float d = a - b2;
                while (d > 180) d -= 360;
                while (d < -180) d += 360;
                spread = std::max(spread, std::fabs(d));
            }
        std::cout << "looking around: " << looks.size() << " directions in 30 s, up to " << spread << " deg apart, " << closed
                  << " of them closed, fastest view turn " << maxStep * 20.0f << " deg/s\n";
        CHECK(looks.size() >= 6 && looks.size() <= 22); // every 1.5-4 s
        CHECK(spread >= 80.0f);
        CHECK(closed <= 1); // open directions (the one it starts with is where it stood)
        CHECK(maxStep * 20.0f <= iw2::game::botSkill(3).turnRate * 0.6f + 1.0f); // calmly: 60% of the turn rate at most
    }

    // 17. An attacker from the side: the bot fights an enemy in front of it; another one hits it from where it does not
    //     look. On a hit (every second one on average) it turns to the attacker and fights that one.
    {
        Scene sc(vfs);
        World& w = sc.g.w;
        Dummy d2(w);
        const NavGraph& nav = w.mpNav();
        V3 pa{}, pb{}, pc{};
        bool found = false;
        for (size_t i = 0; i < nav.mainCount && !found; i += 53)
            for (size_t j = 1; j < nav.mainCount && !found; j += 19) {
                const V3 p = nav.nodes[i], q = nav.nodes[j];
                if (flat(p, q) < 400 || flat(p, q) > 700 || std::fabs(p[2] - q[2]) > 32) continue;
                if (w.collision().trace({p[0], p[1], p[2] + 60}, {q[0], q[1], q[2] + 60}, {0, 0, 0}, {0, 0, 0}, iw2::game::Contents::SightMaskNoCharacters, -1).fraction < 0.999f) continue;
                for (size_t k = 2; k < nav.mainCount && !found; k += 11) {
                    const V3 r = nav.nodes[k];
                    if (flat(p, r) < 300 || flat(p, r) > 700 || std::fabs(p[2] - r[2]) > 32) continue;
                    // behind the bot as it looks at q: more than 110 degrees off
                    const float dot = ((q[0] - p[0]) * (r[0] - p[0]) + (q[1] - p[1]) * (r[1] - p[1])) / (flat(p, q) * flat(p, r));
                    if (dot > -0.35f) continue;
                    if (w.collision().trace({p[0], p[1], p[2] + 60}, {r[0], r[1], r[2] + 60}, {0, 0, 0}, {0, 0, 0}, iw2::game::Contents::SightMaskNoCharacters, -1).fraction < 0.999f) continue;
                    pa = p, pb = q, pc = r, found = true;
                }
            }
        CHECK(found);
        sc.noGrenades();
        sc.d.hold(pb, pa);
        d2.hold(pc, pa);
        w.mpBotSetGoal(sc.g.bots[0], pa, 64.0f);
        sc.stand(pa, pb, 8);
        iw2::game::MpBot* mb = w.mpBot(sc.g.bots[0]);
        CHECK(mb->enemyVisible && mb->enemy == sc.d.e->number);
        int hits = 0, turnedFrame = -1, seenFrame = -1;
        for (int f = 0; f < 20 * 12 && seenFrame < 0; ++f) {
            sc.d.hold(pb, pa);
            d2.hold(pc, pa);
            if (f % 10 == 5 && turnedFrame < 0) { // a bullet into the bot every half second until it turns
                sc.a->obj->set("health", Value(5000));
                const V3 ao = sc.a->obj->get("origin").asVector();
                w.debugFireBulletFrom(*d2.e, {pc[0], pc[1], pc[2] + 60}, {ao[0], ao[1], ao[2] + 40});
                hits += sc.a->obj->get("health").asInt() < 5000;
            }
            sc.a->obj->set("health", Value(100));
            w.frame();
            if (turnedFrame < 0 && mb->enemy == d2.e->number) turnedFrame = f;
            if (mb->enemy == d2.e->number && mb->enemyVisible) seenFrame = f;
        }
        std::cout << "attacker from the side: " << hits << " hits, turned to it in frame " << turnedFrame << ", saw it in frame " << seenFrame << "\n";
        CHECK(hits >= 1 && hits <= 8);
        CHECK(turnedFrame >= 0 && seenFrame > turnedFrame);
    }

    // 18. The place of the last kill: a bot whose main weapon has no round, with no weapon in sight, walks to where its
    //     last kill fell (there the dead player's weapon lies).
    {
        Scene sc(vfs);
        World& w = sc.g.w;
        V3 pa{}, pb{};
        CHECK(nodePair(w, 500, 900, false, pa, pb)); // out of sight
        sc.d.e->obj->set("sessionstate", Value(std::string("spectator")));
        place(sc.a, pa, pb);
        w.frame();
        sc.clip(sc.main) = 0, sc.reserve(sc.main) = 0;
        iw2::game::MpBot* mb = w.mpBot(sc.g.bots[0]);
        mb->killPos = {pb[0], pb[1], pb[2]};
        mb->killMs = w.timeMs();
        mb->detourCheckMs = 0;
        bool detour = false;
        int f = 0;
        for (; f < 20 * 40; ++f) {
            w.frame();
            detour |= mb->detour.kind == iw2::game::BotDetour::KillSpot;
            if (flat(sc.a->obj->get("origin").asVector(), pb) < 80) break;
        }
        std::cout << "kill spot: there after " << f / 20.0f << " s\n";
        CHECK(detour);
        CHECK(f < 20 * 40);
    }

    // 19. A blocked traversal fails early: somebody stands between the bot and the ladder its way goes up, so the bot on
    //     the ladder's start node does not move. After 1.5 s without moving the traversal has failed; the second time the
    //     link is taken out of the graph and the bot's way no longer goes over it (the 8 s rule alone would need 16 s).
    {
        Scene sc(vfs);
        World& w = sc.g.w;
        const NavGraph& nav = w.mpNav();
        w.mpBotSetCombat(sc.g.bots[0], false); // it does not stop to fight who stands there
        namespace F = iw2::game::NavLinkFlag;
        int tried = 0, removedFrame = -1;
        for (size_t i = 0; i < nav.size() && tried < 6 && removedFrame < 0; ++i)
            for (const auto& l : nav.links[i]) {
                if ((l.flags & F::Special) != F::Ladder) continue;
                const V3 a = nav.nodes[i], b = nav.nodes[l.target];
                if (b[2] - a[2] < 60 || w.paths().findPath(int(i), int(l.target)).size() != 2) continue;
                ++tried;
                const int target = int(l.target);
                // 32 units from the start node along the link's view yaw (into the ladder's face).
                const float yaw = l.yaw * 0.0174532925f;
                const V3 block{a[0] + std::cos(yaw) * 32.0f, a[1] + std::sin(yaw) * 32.0f, a[2]};
                place(sc.a, a, b);
                w.mpBotSetGoal(sc.g.bots[0], {b[0], b[1], b[2]}, 8.0f);
                bool reached = false;
                for (int f = 0; f < 20 * 14 && removedFrame < 0 && !reached; ++f) {
                    sc.d.hold(block, a);
                    w.frame();
                    const V3 o = sc.a->obj->get("origin").asVector();
                    reached = flat(o, b) < 24 && std::fabs(o[2] - b[2]) < 24;
                    if (w.paths().findPath(int(i), target).size() != 2) removedFrame = f;
                }
                i += 40;
                break;
            }
        std::cout << "blocked ladder: " << tried << " tried, the link was given up after " << (removedFrame + 1) / 20.0f << " s\n";
        CHECK(removedFrame >= 0 && removedFrame < 20 * 12); // two failures, each well under the 8 s of a slow traversal
    }

    // 10. Smoke: a smoke grenade's cloud between the bot and an enemy in front of it hides the enemy while it stands.
    {
        Scene sc(vfs);
        World& w = sc.g.w;
        V3 pa{}, pb{};
        CHECK(nodePair(w, 500, 700, true, pa, pb));
        place(sc.a, pa, pb);
        w.mpBotSetGoal(sc.g.bots[0], pa, 64.0f);
        sc.d.e->obj->set("sessionstate", Value(std::string("spectator")));
        run(w, 30);
        w.mpBotSmoke({(pa[0] + pb[0]) * 0.5f, (pa[1] + pb[1]) * 0.5f, (pa[2] + pb[2]) * 0.5f});
        run(w, 20 * 2); // the cloud builds up for 1.5 s
        sc.d.e->obj->set("sessionstate", Value(std::string("playing")));
        iw2::game::MpBot* mb = w.mpBot(sc.g.bots[0]);
        const float yaw = std::atan2(pb[1] - pa[1], pb[0] - pa[0]) * 57.29578f;
        int seenFrame = -1;
        for (int f = 0; f < 20 * 30 && seenFrame < 0; ++f) {
            sc.d.hold(pb, pa);
            mb->yaw = yaw, mb->pitch = 0;
            w.frame();
            if (sc.stats().acquired > 0) seenFrame = f;
        }
        std::cout << "smoke: the enemy behind it seen " << (seenFrame + 40) / 20.0f << " s after the grenade went off\n";
        CHECK(seenFrame >= 20 * 17 && seenFrame <= 20 * 19); // the cloud hides it until 20 s after the grenade
    }
    // 22. The hit counter (docs/spec/mp-bots.md "Measurement"): a shot that hits counts once, whatever the pellets, and a
    //     killing hit once (its damage comes before the death); a team mate in the line of fire is no hit.
    {
        // a. A shotgun 150..250 units from the dummy: its pellets are one shot, at most one hit.
        {
            Scene sc(vfs);
            World& w = sc.g.w;
            V3 pa{}, pb{};
            CHECK(nodePair(w, 150, 250, true, pa, pb));
            sc.noGrenades();
            const std::string wn = "shotgun_mp";
            sc.inv->owned.insert(wn);
            sc.inv->slot[1] = wn, sc.inv->slot[2].clear();
            sc.clip(wn) = 6, sc.reserve(wn) = 60;
            sc.a->obj->set("$weapon", Value(wn));
            w.mpBotSetGoal(sc.g.bots[0], pa, 64.0f);
            sc.stand(pa, pb, 1);
            for (int f = 0; f < 20 * 8; ++f) {
                sc.d.hold(pb, pa);
                w.frame();
            }
            const MpBotStats& st = sc.stats();
            std::cout << "hit counter, shotgun: shots " << st.shots << " hits " << st.hits << " (shotgun shots " << st.classShots[5] << ")\n";
            CHECK(st.shots >= 3 && st.hits >= 1 && st.hits <= st.shots);
            CHECK(st.classShots[5] == st.shots);
        }
        // b. The dummy dies of the first hit: one hit, one kill.
        {
            Scene sc(vfs);
            World& w = sc.g.w;
            V3 pa{}, pb{};
            CHECK(nodePair(w, 300, 500, true, pa, pb));
            sc.noGrenades();
            w.mpBotSetGoal(sc.g.bots[0], pa, 64.0f);
            sc.stand(pa, pb, 1);
            place(sc.d.e, pb, pa);
            sc.d.e->obj->set("health", Value(1));
            for (int f = 0; f < 20 * 15 && sc.stats().kills == 0; ++f) w.frame();
            std::cout << "hit counter, a kill: shots " << sc.stats().shots << " hits " << sc.stats().hits << " kills " << sc.stats().kills << "\n";
            CHECK(sc.stats().kills == 1);
            CHECK(sc.stats().hits == 1);
        }
        // c. tdm: a team mate stands just in front of the enemy the bot fires at (no friendly fire: the bullets stop at
        //    the mate).
        {
            Game g(vfs, "mp_toujane", "tdm", 3, 3);
            Dummy d(g.w);
            run(g.w, 60);
            const std::string dt = d.e->obj->get("sessionteam").toString();
            int a = -1, b = -1;
            for (int c : g.bots) {
                if (g.w.mpClientEntity(c)->obj->get("sessionteam").toString() == dt) continue;
                (a < 0 ? a : b) = c;
            }
            CHECK(a >= 0 && b >= 0);
            if (a >= 0 && b >= 0) {
                for (int c : g.bots)
                    if (c != a) g.w.mpBotSetCombat(c, false);
                V3 pa{}, pb{};
                CHECK(nodePair(g.w, 400, 600, true, pa, pb));
                const float l = flat(pa, pb);
                const V3 mate{pb[0] + (pa[0] - pb[0]) / l * 40, pb[1] + (pa[1] - pb[1]) / l * 40, pb[2]};
                Entity* ea = g.w.mpClientEntity(a);
                Entity* eb = g.w.mpClientEntity(b);
                auto& inv = g.w.inventory(*ea);
                for (const std::string& w : inv.owned)
                    if (!field(g.w, w, "offhandclass").empty() && field(g.w, w, "offhandclass") != "none")
                        inv.clip[field(g.w, w, "clipname")] = 0, inv.ammo[field(g.w, w, "ammoname")] = 0;
                for (int c : g.bots)
                    if (c != a && c != b) g.w.mpBotSetGoal(c, g.w.mpClientEntity(c)->obj->get("origin").asVector(), 400.0f);
                g.w.mpBotSetGoal(a, pa, 64.0f);
                g.w.mpBotSetGoal(b, mate, 300.0f);
                place(ea, pa, pb), place(eb, mate, pa);
                for (int f = 0; f < 20 * 8; ++f) {
                    d.hold(pb, pa);
                    eb->obj->set("health", Value(100000));
                    g.w.frame();
                }
                const MpBotStats* st = g.w.mpBotStats(a);
                std::cout << "hit counter, a team mate in the line of fire: shots " << st->shots << " hits " << st->hits << "\n";
                CHECK(st->shots >= 5);
                CHECK(st->hits * 4 < st->shots); // the old counter took every bullet into the mate for a hit
            }
        }
    }
    giveWayTest(vfs);
    turretLoftTest(vfs);
    return 0;
}

int perfTest(iw2::fs::Vfs& vfs, const std::string& map) {
    Game g(vfs, map, "dm", 15, 1);
    (void)g.w.mpNav();
    run(g.w, 20 * 5); // join
    double sum = 0, worst = 0;
    const int frames = 20 * 60;
    for (int i = 0; i < frames; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        g.w.frame();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        sum += ms;
        worst = std::max(worst, ms);
    }
    const auto cost = g.w.mpBotCost();
    std::cout << map << " 15 bots: world frame mean " << sum / frames << " ms, max " << worst << " ms; bots mean " << cost.meanUs / 1000.0
              << " ms, max " << cost.maxUs / 1000.0 << " ms; most A* plans in a frame " << cost.maxPlans << "\n";
    print(map, g.sum());
    CHECK(cost.maxPlans <= 2);
    CHECK(sum / frames < 1000.0 / 60.0); // 60 frames of logic per second
    CHECK(g.w.vm().errors().empty());
    return 0;
}

int soakTest(iw2::fs::Vfs& vfs, const std::string& map) {
    const char* sv = std::getenv("IW2_BOT_SOAK_SECONDS");
    const int secs = sv ? std::max(1, std::atoi(sv)) : 60;
    Game g(vfs, map, "dm", 8, 1);
    run(g.w, 20 * secs);
    const MpBotStats s = g.sum();
    print(map + " " + std::to_string(secs) + " s", s);
    for (int c : g.bots)
        if (g.w.mpBotStats(c)->stuckEvents) std::cout << "  stuck: " << g.w.mpBotDescribe(c) << " at "
                                                      << g.w.mpClientEntity(c)->obj->get("origin").toString() << "\n";
    // No bot stays stuck: a 20 s stuck spell is left (a new goal) or, the second time at the same spot without a node in
    // reach, ended by the trapped bot's death; no bot stands within 64 units for a minute (fights and objectives aside).
    CHECK(s.longestStillMs < 60000);
    CHECK(s.trapped == 0); // no bot trapped where the graph has no way out
    CHECK(s.shots > 0);
    CHECK(g.w.vm().errors().empty());
    return 0;
}

// The measurement run (docs/spec/mp-bots.md "Measurement"): bots of one skill for `secs` game seconds, with `scripted` a
// scripted player among them; prints the kills (by bots, of and by the scripted player, by turret, by melee, own deaths),
// the accuracy, the time without anything to fight with, the turret use, the pickups and the stuck counts.
int statsRun(iw2::fs::Vfs& vfs, const std::string& map, const std::string& gt, int nbots, int skill, int secs, bool scripted) {
    Game g(vfs, map, gt, 0, skill);
    g.w.cvars["scr_" + gt + "_scorelimit"] = "0";
    g.w.cvars["scr_" + gt + "_timelimit"] = "0";
    std::unique_ptr<Scripted> sp;
    if (scripted) sp = std::make_unique<Scripted>(g.w);
    for (int i = 0; i < nbots; ++i) g.bots.push_back(g.w.mpAddBot());
    if (const char* cls = std::getenv("IW2_BOT_CLASS")) // every bot takes a weapon of this class (sniper, smg ...)
        for (int c : g.bots) g.w.mpBotSetWeaponClass(c, cls);
    double worst = 0, sum = 0;
    for (int f = 0; f < 20 * secs; ++f) {
        if (sp) sp->step(g.bots);
        const auto t0 = std::chrono::steady_clock::now();
        g.w.frame();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        sum += ms, worst = std::max(worst, ms);
    }
    const MpBotStats s = g.sum();
    int kills = 0, own = 0, turret = 0, melee = 0, grenade = 0, ofScripted = 0, byScripted = 0;
    const int spNum = sp && g.w.mpClientEntity(sp->client) ? g.w.mpClientEntity(sp->client)->number : -2;
    for (const auto& l : g.w.mpState().log) {
        if (l.rfind("K;", 0) != 0) continue;
        const auto f = logFields(l);
        if (f.size() < 12) continue;
        ++kills;
        if (f[2] == f[6] || f[6] == "-1") ++own;
        else if (f[2] == std::to_string(spNum)) ++ofScripted;
        else if (f[6] == std::to_string(spNum)) ++byScripted;
        if (f[9].find("bipod") != std::string::npos || f[9].find("30cal") != std::string::npos) ++turret;
        if (f[11] == "MOD_MELEE") ++melee;
        if (f[11].rfind("MOD_GRENADE", 0) == 0) ++grenade;
    }
    const double alive = std::max(1, s.aliveFrames) / 20.0;
    std::cout << "STATS " << map << " " << gt << " bots " << nbots << " skill " << iw2::game::botSkill(skill).name << " " << secs << " s"
              << (scripted ? " +scripted" : "") << "; rounds " << g.w.mpState().restarts + 1 << "\n"
              << "  kills " << kills << " (own deaths " << own << ", by turret " << turret << ", melee " << melee << ", grenade " << grenade << ")"
              << "; bots killed the scripted player " << ofScripted << " times, it killed bots " << byScripted << " times (its shots "
              << (sp ? sp->shots : 0) << ")\n"
              << "  bot shots " << s.shots << " hits " << s.hits << " accuracy " << (s.shots ? 100.0 * s.hits / s.shots : 0.0) << "%; reloads "
              << s.reloads << " weapon switches " << s.weaponSwitches << " grenades " << s.grenades << " (hits " << s.grenadeHits << ") melee swings "
              << s.melees << "\n"
              << "  accuracy by class:";
    static const char* kClass[7] = {"rifle", "bolt", "sniper", "smg", "mg", "shotgun", "pistol"};
    for (int k = 0; k < 7; ++k)
        if (s.classShots[k]) std::cout << " " << kClass[k] << " " << 100.0 * s.classHits[k] / s.classShots[k] << "% of " << s.classShots[k];
    std::cout << "; by distance:";
    static const char* kBand[3] = {"<500", "500-1000", ">1000"};
    for (int k = 0; k < 3; ++k)
        if (s.bandShots[k]) std::cout << " " << kBand[k] << " " << 100.0 * s.bandHits[k] / s.bandShots[k] << "% of " << s.bandShots[k];
    std::cout << "\n"
              << "  defenceless " << s.defencelessFrames / 20.0 << " s (" << 100.0 * (s.defencelessFrames / 20.0) / alive << "% of " << alive
              << " s alive); all weapons empty " << s.emptyFrames / 20.0 << " s\n"
              << "  pickups: weapons " << s.weaponPickups << " ammunition " << s.ammoPickups << "; turrets: mounts " << s.turretMounts << " time "
              << s.turretFrames / 20.0 << " s shots " << s.turretShots << " hits " << s.turretHits << " kills " << s.turretKills << "\n"
              << "  noticed by sight " << s.acquired << ", outside the view " << s.acquiredOutsideView << " ("
              << (s.acquired ? 100.0 * s.acquiredOutsideView / s.acquired : 0.0) << "%); heard " << s.heard << "; retreats " << s.retreats << "\n"
              << "  stuck events " << s.stuckEvents << " trapped " << s.trapped << " longest still " << s.longestStillMs / 1000.0
              << " s; frame mean " << sum / (20.0 * secs) << " ms max " << worst << " ms; bots mean " << g.w.mpBotCost().meanUs / 1000.0 << " ms max "
              << g.w.mpBotCost().maxUs / 1000.0 << " ms\n";
    CHECK(g.w.vm().errors().empty());
    for (const auto& e : g.w.vm().errors()) std::cerr << "script error: " << e << "\n";
    return 0;
}

// Stall report: spells of "wants to walk, stays within 48 units" by 128-unit hot spot, worst first.
int stallReport(iw2::fs::Vfs& vfs, const std::string& map, const std::string& gt, bool check) {
    const char* sv = std::getenv("IW2_BOT_SOAK_SECONDS");
    const int secs = sv ? std::max(1, std::atoi(sv)) : 60;
    Game g(vfs, map, gt, 8, 1);
    run(g.w, 20 * secs);
    const NavGraph& nav = g.w.mpNav();
    struct Spot { int n = 0, ms = 0, maxMs = 0, players = 0, noLink = 0; float travelled = 0; std::map<int, int> flags; std::map<std::string, int> what; V3 pos{}; };
    std::map<std::pair<int, int>, Spot> spots;
    int64_t total = 0;
    int over3 = 0, longest = 0;
    for (const auto& s : g.w.mpBotStalls()) {
        Spot& p = spots[{int(std::floor(s.pos[0] / 128)), int(std::floor(s.pos[1] / 128))}];
        ++p.n, p.ms += s.ms, p.maxMs = std::max(p.maxMs, s.ms), p.pos = s.pos, p.travelled += s.travelled;
        p.players += s.blockedByPlayer, p.noLink += s.from < 0, ++p.flags[s.flags], ++p.what[s.what.empty() ? "roam" : s.what];
        total += s.ms, over3 += s.ms > 3000, longest = std::max(longest, s.ms);
    }
    std::map<int, std::array<double, 3>> byFlags; // link flags -> spells, seconds, summed distance to the start node
    for (const auto& s : g.w.mpBotStalls()) {
        auto& f = byFlags[s.from < 0 ? -1 : s.flags];
        f[0] += 1, f[1] += s.ms / 1000.0, f[2] += s.fromDist;
    }
    for (const auto& [k, f] : byFlags) std::cout << "  link flags " << k << ": spells " << f[0] << " s " << f[1] << " mean dist to start " << f[2] / f[0] << "\n";
    std::vector<const Spot*> order;
    for (const auto& [k, p] : spots) order.push_back(&p);
    std::sort(order.begin(), order.end(), [](const Spot* a, const Spot* b) { return a->ms > b->ms; });
    std::cout << "stall " << map << " " << gt << " " << secs << " s nodes " << nav.size() << ": spells " << g.w.mpBotStalls().size() << " over3s " << over3
              << " longest " << longest / 1000.0 << " s, stalled " << total / 1000.0 << " s of " << g.w.mpBotWalkMs() / 1000.0 << " s walking ("
              << 100.0 * double(total) / double(std::max<int64_t>(1, g.w.mpBotWalkMs())) << "%)\n";
    // Limits with a margin over the fixed controller; before it mp_carentan stood at 16.8%, 16 spells over 3 s and 11 s
    // (docs/spec/mp-bots.md "Stuck watch"). The report mode itself prints only: the check is `stall-check`.
    if (check) {
        CHECK(100.0 * double(total) < 5.0 * double(g.w.mpBotWalkMs()));
        CHECK(over3 <= 5);
        CHECK(longest < 8000);
        CHECK(g.w.mpBotWalkMs() > 300000); // the bots did walk
        for (const auto& ls : nav.links)
            for (const auto& l : ls) CHECK(!((l.flags & iw2::game::NavLinkFlag::Crouch) && (l.flags & iw2::game::NavLinkFlag::Jump)));
    }
    for (size_t i = 0; i < order.size() && i < 12; ++i) {
        const Spot& p = *order[i];
        std::cout << "  spot " << p.pos[0] << " " << p.pos[1] << " " << p.pos[2] << ": n " << p.n << " sum " << p.ms / 1000.0 << " max " << p.maxMs / 1000.0
                  << " walked/s " << p.travelled / std::max(0.05f, float(p.ms) / 1000.0f) << " byPlayer " << p.players << " noLink " << p.noLink << " flags";
        for (const auto& [f, c] : p.flags) std::cout << " " << f << "x" << c;
        std::cout << " to";
        for (const auto& [f, c] : p.what) std::cout << " " << f << "x" << c;
        std::cout << "\n";
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    pureTests();
    purePlayerTests();
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return failures ? 1 : 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "probe" && argc > 8) { // probe <map> x y z x2 y2 z2: box traces at several heights (debugging)
        World w(vfs, argv[2], true);
        const V3 a{float(std::atof(argv[3])), float(std::atof(argv[4])), float(std::atof(argv[5]))};
        const V3 b{float(std::atof(argv[6])), float(std::atof(argv[7])), float(std::atof(argv[8]))};
        for (float z0 : {0.0f, 18.0f, 30.0f, 45.0f}) {
            const auto tr = w.collision().trace({a[0], a[1], a[2] + 1}, {b[0], b[1], a[2] + 1}, {-15, -15, z0}, {15, 15, z0 + 4},
                                                iw2::game::Contents::PlayerSolid, -1);
            std::cout << "z " << z0 << ": fraction " << tr.fraction << " at " << tr.endPos[0] << " " << tr.endPos[1] << " normal "
                      << tr.normal[0] << " " << tr.normal[1] << " " << tr.normal[2] << " startsolid " << tr.startSolid << "\n";
        }
        const auto up = w.collision().trace({a[0], a[1], a[2] + 1}, {a[0], a[1], a[2] + 200}, {-15, -15, 0}, {15, 15, 1},
                                            iw2::game::Contents::PlayerSolid, -1);
        std::cout << "ceiling " << up.endPos[2] << "\n";
        return 0;
    }
    if (mode == "perf" && argc > 2) perfTest(vfs, argv[2]);
    else if (mode == "soak" && argc > 2) soakTest(vfs, argv[2]);
    else if (mode == "stall" && argc > 2) stallReport(vfs, argv[2], argc > 3 ? argv[3] : "dm", false);
    else if (mode == "stall-check" && argc > 2) stallReport(vfs, argv[2], "dm", true);
    else if (mode == "stats" && argc > 6)
        statsRun(vfs, argv[2], argv[3], std::atoi(argv[4]), std::atoi(argv[5]), std::atoi(argv[6]), argc > 7 && std::string(argv[7]) == "scripted");
    else if (mode == "behaviour") behaviourTests(vfs);
    else if (mode == "giveway") giveWayTest(vfs);
    else if (mode == "crevice") creviceTest(vfs);
    else if (mode == "hearing") hearingTests(vfs);
    else if (mode == "turretloft") turretLoftTest(vfs);
    else if (mode == "weapons") weaponTests(vfs);
    else combatTests(vfs);
    return failures ? 1 : 0;
}
