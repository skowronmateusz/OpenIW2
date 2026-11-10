// Multiplayer movement and bullet differences (docs/spec/mp-pmove.md): the MP dvar defaults (fall damage heights, jump
// slowdown, inertia), the landing / friction / re-jump slowdown, and with IW2_MAIN the rifle bullet pass-through and its
// damage flags in an offline DM game.

#include "assets/d3dbsp.h"
#include "fs/vfs.h"
#include "game/bulletdamage.h"
#include "game/pmove.h"
#include "game/viewweapon.h"
#include "game/world.h"

#include <cmath>
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

bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

using namespace iw2::game;

// A flat box floor whose top is z = 0 around the origin, far outside the map's brushes (entity clips need a loaded map).
constexpr float kFloorX = 60000.0f;
void addFloor(CollisionWorld& cw) {
    EntityClip floor;
    floor.id = 1000;
    floor.origin = {kFloorX, kFloorX, 0};
    floor.mins = {-4000, -4000, -64};
    floor.maxs = {4000, 4000, 0};
    floor.contents = Contents::Solid;
    cw.setEntityClips({floor});
}

// A player standing on the floor.
PlayerState standing() {
    PlayerState ps;
    ps.origin = {kFloorX, kFloorX, 0};
    ps.onGround = true;
    ps.timeMs = 100000;
    return ps;
}

// Drops a player from `height` above the floor at rest and returns the landing's fall damage (0..100).
int fallDamage(const CollisionWorld& cw, float height, const MoveParams& mp) {
    PlayerState ps;
    ps.origin = {kFloorX, kFloorX, height};
    ps.timeMs = 100000;
    PlayerInput in;
    for (int i = 0; i < 200 && ps.landSeq == 0; ++i) playerMove(cw, ps, in, 0.01f, mp);
    CHECK(ps.landSeq == 1);
    return ps.landDamage;
}

void printErrors(World& w) {
    for (const auto& e : w.vm().errors()) std::cerr << "  script error: " << e << "\n";
}

int health(Entity* e) {
    const auto v = e->obj->get("health");
    return v.isNumber() ? v.asInt() : -1;
}

} // namespace

int main() {
    // The MP dvar defaults; everything else stays the SP value.
    {
        const MoveParams sp, mp = multiplayerMoveParams();
        CHECK(!sp.jumpSlowdown && mp.jumpSlowdown);
        CHECK(sp.fallDamageMinHeight == 200.0f && sp.fallDamageMaxHeight == 350.0f);
        CHECK(mp.fallDamageMinHeight == 256.0f && mp.fallDamageMaxHeight == 480.0f);
        CHECK(sp.inertiaAngle == -1000.0f && mp.inertiaAngle == 0.0f && mp.inertiaMax == 50.0f);
        CHECK(mp.speed == 190 && mp.gravity == 800.0f && mp.jumpHeight == 39.0f && mp.friction == 5.5f && mp.stopSpeed == 100.0f);
        CHECK(mp.backSpeedScale == sp.backSpeedScale && mp.strafeSpeedScale == sp.strafeSpeedScale);
    }
    // The slowdown factor: 1 without jump_slowdownEnable, 2.5 above 1699 ms, 1 + t * 1.5 / 1700 below.
    {
        CHECK(jumpSlowdownScale(1800, false) == 1.0f && jumpSlowdownScale(0, false) == 1.0f);
        CHECK(jumpSlowdownScale(1800, true) == 2.5f && jumpSlowdownScale(1700, true) == 2.5f);
        CHECK(near(jumpSlowdownScale(1699, true), 1.0f + 1699.0f * 1.5f / 1700.0f, 1e-5f));
        CHECK(near(jumpSlowdownScale(850, true), 1.75f, 1e-5f));
        CHECK(jumpSlowdownScale(0, true) == 1.0f);
    }
    // Bullet damage flags: rifleBullet 0x40 in SP, 0x20 in MP; armorPiercing 2.
    {
        CHECK(bulletDamageFlags(true, false, false) == 0x40 && bulletDamageFlags(true, false, true) == 0x20);
        CHECK(bulletDamageFlags(false, true, true) == 2 && bulletDamageFlags(true, true, true) == 0x22);
        CHECK(bulletDamageFlags(false, false, false) == 0);
    }
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, movement and world parts skipped\n";
        return failures ? 1 : 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    const auto bytes = vfs.read("maps/mp/mp_toujane.d3dbsp");
    CHECK(bytes.has_value());
    if (!bytes) return 1;
    const iw2::assets::D3dBsp bsp(*bytes);
    CollisionWorld floorCw(bsp);
    addFloor(floorCw);
    auto floorWorld = [&]() -> const CollisionWorld& { return floorCw; };

    // Fall damage between the MP heights 256 and 480 (the SP ones are 200 and 350).
    {
        CHECK(landingOutcome(250.0f, 0, false).damage == 33);
        CHECK(landingOutcome(250.0f, 0, false, 256.0f, 480.0f).damage == 0);
        CHECK(landingOutcome(368.0f, 0, false, 256.0f, 480.0f).damage == 50);
        CHECK(landingOutcome(480.0f, 0, false, 256.0f, 480.0f).damage == 100);
        CHECK(landingOutcome(400.0f, 0, false).damage == 100);
        // A drop of about 300 units: SP (300 - 200) / 150, MP (300 - 256) / 224.
        const CollisionWorld& cw = floorWorld();
        const int spDmg = fallDamage(cw, 300.0f, MoveParams{});
        const int mpDmg = fallDamage(cw, 300.0f, multiplayerMoveParams());
        std::cout << "300-unit drop: SP damage " << spDmg << ", MP damage " << mpDmg << "\n";
        CHECK(spDmg >= 62 && spDmg <= 70);
        CHECK(mpDmg >= 16 && mpDmg <= 23);
        // The view dip rebuilds the height from the damage with the same dvars: 50 % is 275 (SP) or 368 (MP), both capped at 24.
        CHECK(ViewImpulses::dipFromDamage(50) == 24 && ViewImpulses::dipFromDamage(0, 256.0f, 480.0f) == 24);
    }
    // The landing after a jump: the timer starts at 1800 for a landing below the jump's origin + 18; with the MP slowdown the
    // velocity is scaled by 0.65 and the friction control by 2.5. SP: friction only.
    {
        const CollisionWorld& cw = floorWorld();
        auto land = [&](const MoveParams& mp) {
            PlayerState ps = standing();
            ps.jumping = true;
            ps.jumpOriginZ = 0;
            ps.velocity = {200, 0, 0};
            playerMove(cw, ps, PlayerInput{}, 0.01f, mp);
            CHECK(ps.pmTimeMs == 1800);
            return ps.velocity[0];
        };
        const float sp = land(MoveParams{}), mp = land(multiplayerMoveParams());
        std::cout << "landing speed: SP " << sp << ", MP " << mp << "\n";
        CHECK(near(sp, 200.0f - 200.0f * 5.5f * 0.01f, 0.05f));               // 189
        CHECK(near(mp, 130.0f - 130.0f * 2.5f * 5.5f * 0.01f, 0.05f));        // 112.125
        // A landing at least 18 above the jump's origin: timer 1200 and x0.5.
        PlayerState hi = standing();
        hi.jumping = true;
        hi.jumpOriginZ = -20;
        hi.velocity = {200, 0, 0};
        playerMove(cw, hi, PlayerInput{}, 0.01f, multiplayerMoveParams());
        CHECK(hi.pmTimeMs == 1200);
        CHECK(near(hi.velocity[0], 100.0f - 100.0f * (1.0f + 1200.0f * 1.5f / 1700.0f) * 5.5f * 0.01f, 0.05f));
    }
    // A jump while the landing slowdown runs: 2 g h is divided by the factor of the remaining pm_time (1790 ms -> 2.5).
    {
        const CollisionWorld& cw = floorWorld();
        auto jump = [&](const MoveParams& mp) {
            PlayerState ps = standing();
            ps.jumping = true;
            ps.pmTimeMs = 1800;
            PlayerInput in;
            in.jump = true;
            playerMove(cw, ps, in, 0.01f, mp);
            CHECK(!ps.onGround);
            return ps.velocity[2];
        };
        const float sp = jump(MoveParams{}), mp = jump(multiplayerMoveParams());
        std::cout << "re-jump speed: SP " << sp << ", MP " << mp << "\n";
        // The air move's gravity (800 * 0.01) comes off both.
        CHECK(near(sp, std::sqrt(2.0f * 39.0f * 800.0f) - 8.0f, 0.05f));
        CHECK(near(mp, std::sqrt(2.0f * 39.0f * 800.0f / 2.5f) - 8.0f, 0.05f));
        // A first jump (no flag) is the full jump in MP too.
        PlayerState first = standing();
        PlayerInput in;
        in.jump = true;
        playerMove(cw, first, in, 0.01f, multiplayerMoveParams());
        CHECK(near(first.velocity[2], sp, 1e-3f));
    }
    // Inertia: reversing against the smoothed velocity caps the acceleration at inertiaMax 50 in MP (SP never caps).
    {
        const CollisionWorld& cw = floorWorld();
        auto reverse = [&](const MoveParams& mp) {
            PlayerState ps = standing();
            ps.velocity = {30, 0, 0};
            ps.oldVelocity = {200, 0};
            PlayerInput in;
            in.forward = -1.0f;
            playerMove(cw, ps, in, 0.05f, mp);
            return ps.velocity[0];
        };
        const float sp = reverse(MoveParams{}), mp = reverse(multiplayerMoveParams());
        std::cout << "reversal: SP " << sp << ", MP " << mp << "\n";
        // Friction leaves 2.5; wishspeed 190 * 0.7 = 133, accelspeed min(135.5, 133 * 0.05 * 9 = 59.85) turns the velocity against
        // the smoothed one (dot < 0), so MP caps it at 50.
        CHECK(near(mp - sp, 59.85f - 50.0f, 0.05f));
        // Moving on in the same direction is never capped.
        PlayerState ps = standing();
        ps.oldVelocity = {100, 0};
        ps.velocity = {100, 0, 0};
        PlayerInput fwd;
        fwd.forward = 1.0f;
        PlayerState a = ps, b = ps;
        playerMove(cw, a, fwd, 0.05f, MoveParams{});
        playerMove(cw, b, fwd, 0.05f, multiplayerMoveParams());
        CHECK(near(a.velocity[0], b.velocity[0], 1e-4f));
        // The smoothed velocity moves towards the velocity by the frame time.
        PlayerState s = standing();
        s.velocity = {100, 0, 0};
        playerMove(cw, s, PlayerInput{}, 0.05f, MoveParams{});
        CHECK(near(s.oldVelocity[0], s.velocity[0] * 0.05f, 1e-3f));
    }

    // Rifle bullets in an offline DM game: three clients on a line; a rifle bullet goes through the first and hurts the second
    // too, a pistol bullet stops in the first.
    auto shot = [&](const std::string& weapon, int& h1, int& h2, const char* friendlyFire = nullptr) {
        World w(vfs, "mp_toujane", true);
        w.mpStart("dm");
        Entity* p[3]{};
        for (int c = 0; c < 3; ++c) {
            const int slot = w.mpConnect("c" + std::to_string(c));
            p[c] = w.mpClientEntity(slot);
            w.mpBegin(slot);
            w.frame();
            w.mpMenuResponse(slot, "team_britishgerman", "autoassign");
            w.frame();
            const std::string wmenu = w.mpState().clients[size_t(slot)].openMenu;
            w.mpMenuResponse(slot, wmenu, wmenu == "weapon_british" ? "thompson_mp" : "mp40_mp");
            w.frame();
            w.frame();
        }
        printErrors(w);
        // In a line along x, away from walls of the spawn: the shooter's own spawn point, the others 100 and 200 units further.
        const auto o = p[0]->obj->get("origin").asVector();
        p[1]->obj->set("origin", iw2::script::Value(iw2::script::Vec3{o[0] + 100, o[1], o[2]}));
        p[2]->obj->set("origin", iw2::script::Value(iw2::script::Vec3{o[0] + 200, o[1], o[2]}));
        p[0]->obj->set("$weapon", iw2::script::Value(weapon));
        if (friendlyFire) { // the shooter and the first victim on one team
            w.cvars["scr_friendlyfire"] = friendlyFire;
            p[0]->obj->set("sessionteam", iw2::script::Value(std::string("axis")));
            p[1]->obj->set("sessionteam", iw2::script::Value(std::string("axis")));
            p[2]->obj->set("sessionteam", iw2::script::Value(std::string("allies")));
        }
        w.frame();
        const int before1 = health(p[1]), before2 = health(p[2]);
        const iw2::script::Vec3 start{o[0] + 20, o[1], o[2] + 40}, end{o[0] + 1000, o[1], o[2] + 40};
        w.debugFireBulletFrom(*p[0], start, end);
        w.frame();
        h1 = before1 - health(p[1]);
        h2 = before2 - health(p[2]);
    };
    int r1 = 0, r2 = 0, q1 = 0, q2 = 0;
    shot("kar98k_mp", r1, r2);
    std::cout << "rifle: first " << r1 << ", second " << r2 << "\n";
    CHECK(r1 > 0 && r2 > 0 && r2 < r1);
    shot("colt_mp", q1, q2);
    std::cout << "pistol: first " << q1 << ", second " << q2 << "\n";
    CHECK(q1 > 0 && q2 == 0);
    // scr_friendlyfire 0 stops a rifle bullet in a team mate; any other value lets it through.
    int t1 = 0, t2 = 0, u1 = 0, u2 = 0;
    shot("kar98k_mp", t1, t2, "0");
    shot("kar98k_mp", u1, u2, "1");
    std::cout << "team mate in front, ff 0: second " << t2 << "; ff 1: second " << u2 << "\n";
    CHECK(t2 == 0 && u2 > 0);
    // The Trench Gun (weaponClass "spread", docs/spec/mp-pmove.md "Spread weapons"): one shot fires shotCount (8) pellets, each
    // reaching at most minDamageRange (800); a single fire sound. A plain rifle fires one bullet.
    {
        World w(vfs, "mp_toujane", true);
        w.mpStart("dm");
        Entity* p[2]{};
        for (int c = 0; c < 2; ++c) {
            const int slot = w.mpConnect("s" + std::to_string(c));
            p[c] = w.mpClientEntity(slot);
            w.mpBegin(slot);
            w.frame();
            w.mpMenuResponse(slot, "team_britishgerman", "autoassign");
            w.frame();
            const std::string wmenu = w.mpState().clients[size_t(slot)].openMenu;
            w.mpMenuResponse(slot, wmenu, wmenu == "weapon_british" ? "thompson_mp" : "mp40_mp");
            w.frame();
            w.frame();
        }
        CHECK(w.player() == p[0]);
        if (w.player() == p[0]) {
            const auto o = p[0]->obj->get("origin").asVector();
            const iw2::script::Vec3 eye{o[0], o[1], o[2] + 60};
            p[0]->obj->set("$weapon", iw2::script::Value(std::string("shotgun_mp")));
            // Pellet traces: a shot straight up into the sky counts 8 bullet traces, a rifle shot one.
            auto fire = [&](const std::string& weapon, float spread) {
                p[0]->obj->set("$weapon", iw2::script::Value(weapon));
                const int before = w.stats.shots;
                w.playerFire(eye, {0, 0, 1}, false, spread);
                return w.stats.shots - before;
            };
            const int pellets = fire("shotgun_mp", 6.0f);
            std::cout << "shotgun pellets per shot: " << pellets << "\n";
            CHECK(pellets == 8);
            CHECK(fire("kar98k_mp", 6.0f) == 1);
            // Damage: a client 200 units in front takes several pellets of 50 (under maxDamageRange 384); one 1000 units away
            // (beyond minDamageRange 800) takes nothing.
            auto victim = [&](float dist) {
                p[1]->obj->set("origin", iw2::script::Value(iw2::script::Vec3{o[0] + dist, o[1], o[2]}));
                p[1]->obj->set("health", iw2::script::Value(int32_t(100000)));
                w.frame();
                p[1]->obj->set("origin", iw2::script::Value(iw2::script::Vec3{o[0] + dist, o[1], o[2]}));
                p[0]->obj->set("origin", iw2::script::Value(o));
                p[0]->obj->set("$weapon", iw2::script::Value(std::string("shotgun_mp")));
                const int before = health(p[1]);
                w.playerFire({o[0] + 20, o[1], o[2] + 40}, {1, 0, 0}, false, 2.0f);
                w.frame();
                return before - health(p[1]);
            };
            const int near200 = victim(200), far1000 = victim(1000);
            std::cout << "shotgun damage at 200: " << near200 << ", at 1000: " << far1000 << "\n";
            CHECK(near200 >= 100 && far1000 == 0);
        }
    }
    // A landing event with 50 % fall damage on a client (a bot's or a network client's): MOD_FALLING through the gametype's
    // damage callback takes maxhealth * 0.5.
    {
        World w(vfs, "mp_toujane", true);
        w.mpStart("dm");
        const int slot = w.mpConnect("faller");
        Entity* p = w.mpClientEntity(slot);
        w.mpBegin(slot);
        w.frame();
        w.mpMenuResponse(slot, "team_britishgerman", "autoassign");
        w.frame();
        const std::string wmenu = w.mpState().clients[size_t(slot)].openMenu;
        w.mpMenuResponse(slot, wmenu, wmenu == "weapon_british" ? "thompson_mp" : "mp40_mp");
        w.frame();
        w.frame();
        const int before = health(p);
        w.clientFallEvent(*p, 0x74, 50);
        w.frame();
        std::cout << "fall: " << before << " -> " << health(p) << " maxhealth " << p->obj->get("maxhealth").toString() << "\n";
        CHECK(before == 100 && health(p) == 50);
        w.clientFallEvent(*p, 0x5d, 8); // a landing without damage
        w.frame();
        CHECK(health(p) == 50);
    }
    return failures ? 1 : 0;
}
