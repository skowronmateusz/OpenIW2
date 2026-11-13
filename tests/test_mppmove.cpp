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
#include <cstdio>
#include <vector>
#include <cstdlib>
#include <iostream>
#include <memory>
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

    // The prone body (docs/spec/mp-pmove.md "Prone body") against the mp_toujane walls: from each mp_dm_spawn on flat
    // ground, the nearest wall (a ±6 box at waist height) between 120 and 400 units away, with flat floor up to it.
    struct WallSpot { Vec origin; float yaw; float dist; }; // yaw: towards the wall
    std::vector<WallSpot> spots;
    {
        CollisionWorld& cw = floorCw;
        for (const auto& ent : bsp.entities()) {
            const std::string* cls = ent.get("classname");
            const std::string* org = ent.get("origin");
            if (!cls || !org || *cls != "mp_dm_spawn") continue;
            Vec o{};
            if (std::sscanf(org->c_str(), "%f %f %f", &o[0], &o[1], &o[2]) != 3) continue;
            const TraceResult down = cw.trace(o, {o[0], o[1], o[2] - 200}, {-15, -15, 0}, {15, 15, 30}, Contents::PlayerClipMask);
            if (down.startSolid || down.fraction == 1.0f || down.normal[2] < 0.99f) continue;
            o = down.endPos;
            for (int k = 0; k < 36; ++k) {
                const float yaw = float(k * 10), r = yaw * float(M_PI) / 180.0f;
                const Vec dir{std::cos(r), std::sin(r), 0};
                const Vec s{o[0], o[1], o[2] + 24}, e{s[0] + dir[0] * 400, s[1] + dir[1] * 400, s[2]};
                const TraceResult t = cw.trace(s, e, {-6, -6, -6}, {6, 6, 6}, 0x810011);
                if (t.startSolid || t.fraction == 1.0f || std::fabs(t.normal[2]) > 0.1f) continue;
                const float d = t.fraction * 400 + 6;
                if (d < 120) continue;
                bool flat = true; // the floor up to the wall stays at the spawn's height
                for (float x = 0; x + 20 < d && flat; x += 8) {
                    const Vec p{o[0] + dir[0] * x, o[1] + dir[1] * x, o[2] + 20};
                    const TraceResult g = cw.trace(p, {p[0], p[1], p[2] - 40}, {0, 0, 0}, {0, 0, 0}, 0x810011);
                    flat = g.fraction < 1.0f && std::fabs(g.endPos[2] - o[2]) < 0.5f;
                }
                if (flat) spots.push_back({o, yaw, d});
            }
        }
        std::cout << "prone wall spots on mp_toujane: " << spots.size() << "\n";
        CHECK(!spots.empty());
    }
    auto at = [](const WallSpot& w, float fromWall) {
        const float r = w.yaw * float(M_PI) / 180.0f;
        PlayerState ps;
        const float k = w.dist - fromWall;
        ps.origin = {w.origin[0] + std::cos(r) * k, w.origin[1] + std::sin(r) * k, w.origin[2]};
        ps.onGround = true;
        ps.timeMs = 100000;
        return ps;
    };
    auto goProne = [&](PlayerState& ps, const MoveParams& mp, float yaw) {
        PlayerInput in;
        in.yaw = yaw;
        in.stance = Stance::Crouch;
        playerMove(floorCw, ps, in, 0.05f, mp);
        in.stance = Stance::Prone;
        const int seq = ps.eventSeq;
        playerMove(floorCw, ps, in, 0.05f, mp);
        return seq;
    };
    // Footsteps (docs/spec/mp-pmove.md "Footsteps"): the MP gate (0x5184e0) gives no step event while crouched or prone,
    // SP (0x50a830) gives them in every stance. The player walks back and forth on the spawn's flat ground towards the wall.
    if (!spots.empty()) {
        const WallSpot& w = spots.front();
        auto steps = [&](const MoveParams& mp, Stance stance, bool leanRight) {
            PlayerState ps = at(w, w.dist);
            PlayerInput in;
            in.stance = stance;
            for (int i = 0; i < 30; ++i) playerMove(floorCw, ps, in, 0.02f, mp); // settle into the stance
            int n = 0, seen = ps.eventSeq;
            in.forward = 1;
            in.leanRight = leanRight;
            for (int leg = 0; leg < 8; ++leg) {
                in.yaw = leg % 2 ? w.yaw + 180.0f : w.yaw;
                for (int i = 0; i < 25; ++i) {
                    playerMove(floorCw, ps, in, 0.02f, mp);
                    for (; seen != ps.eventSeq; ++seen)
                        if (const int ev = ps.events[seen & 3]; ev >= 1 && ev < 0x46) ++n; // run, walk, prone steps
                }
            }
            return n;
        };
        const MoveParams sp, mp = multiplayerMoveParams();
        CHECK(mp.footstepGate && !sp.footstepGate && mp.footstepsThreshold == 0.0f);
        const int mpStand = steps(mp, Stance::Stand, false), mpCrouch = steps(mp, Stance::Crouch, false);
        const int mpProne = steps(mp, Stance::Prone, false), spCrouch = steps(sp, Stance::Crouch, false);
        std::cout << "footsteps in 4 s: MP stand " << mpStand << ", MP crouch " << mpCrouch << ", MP prone " << mpProne
                  << ", SP crouch " << spCrouch << "\n";
        CHECK(mpStand >= 4 && mpCrouch == 0 && mpProne == 0 && spCrouch >= 2);
        // The gate itself: the stance by its view height target, the slow aim walk (pm_flags 0x100), the speed threshold.
        PlayerState g;
        g.viewHeightTarget = 60;
        CHECK(footstepAllowed(g, 100.0f, mp));
        g.adsSlow = true;
        CHECK(!footstepAllowed(g, 100.0f, mp) && footstepAllowed(g, 100.0f, sp));
        g.adsSlow = false;
        g.viewHeightTarget = 40;
        CHECK(!footstepAllowed(g, 100.0f, mp) && footstepAllowed(g, 100.0f, sp));
        g.viewHeightTarget = 11;
        CHECK(!footstepAllowed(g, 100.0f, mp));
        g.viewHeightTarget = 60;
        MoveParams slow = mp;
        slow.footstepsThreshold = 120.0f;
        CHECK(!footstepAllowed(g, 100.0f, slow) && footstepAllowed(g, 120.0f, slow));
    }
    if (!spots.empty()) {
        const WallSpot& w = spots.front();
        const MoveParams mp = multiplayerMoveParams();
        CHECK(mp.proneBodyCheck && !MoveParams{}.proneBodyCheck);
        // Lying down with the legs into the wall 30 units behind is refused; the client is asked to stay crouched.
        PlayerState ps = at(w, 30);
        const int seq = goProne(ps, mp, w.yaw + 180.0f); // facing away from the wall: the legs point at it
        CHECK(ps.stance == Stance::Crouch);
        bool ev = false;
        for (int i = seq; i < ps.eventSeq; ++i) ev |= ps.events[i & 3] == 0x8d;
        CHECK(ev);
        // Facing the wall (legs away from it) fits.
        PlayerState ps2 = at(w, 30);
        goProne(ps2, mp, w.yaw);
        CHECK(ps2.stance == Stance::Prone);
        CHECK(near(std::remainder(ps2.proneYaw - w.yaw, 360.0f), 0.0f, 0.01f));
        // SP accepts the body in the wall (0x4fc000 always passes).
        PlayerState ps3 = at(w, 30);
        goProne(ps3, MoveParams{}, w.yaw + 180.0f);
        CHECK(ps3.stance == Stance::Prone);

        // Crawling backwards towards the wall stops when the legs reach it, not when the box does.
        PlayerState pc = at(w, w.dist - 10);
        goProne(pc, mp, w.yaw + 180.0f);
        CHECK(pc.stance == Stance::Prone);
        PlayerInput in;
        in.stance = Stance::Prone;
        in.yaw = w.yaw + 180.0f;
        in.forward = -1.0f;
        for (int i = 0; i < 600; ++i) playerMove(floorCw, pc, in, 0.05f, mp);
        const float r = w.yaw * float(M_PI) / 180.0f;
        auto gap = [&](const PlayerState& p) {
            return w.dist - ((p.origin[0] - w.origin[0]) * std::cos(r) + (p.origin[1] - w.origin[1]) * std::sin(r));
        };
        std::cout << "prone crawl: MP gap to the wall " << gap(pc) << "\n";
        CHECK(gap(pc) > 50.0f && gap(pc) < 80.0f);
        PlayerState sp = at(w, w.dist - 10);
        goProne(sp, MoveParams{}, w.yaw + 180.0f);
        for (int i = 0; i < 600; ++i) playerMove(floorCw, sp, in, 0.05f, MoveParams{});
        std::cout << "prone crawl: SP gap to the wall " << gap(sp) << "\n";
        CHECK(gap(sp) < 20.0f);

        // Turning: lying with the legs away from the wall 30 units off, the mouse turns the view half round (5 degrees
        // a step; the view correction is taken into the view as the network server and the front end do). In MP the
        // body stops before its legs reach the wall and the view stays within the body yaw ± 85; in SP the body follows.
        auto turn = [&](const MoveParams& m) {
            PlayerState p = at(w, 30);
            goProne(p, m, w.yaw);
            CHECK(p.stance == Stance::Prone);
            PlayerInput ti;
            ti.stance = Stance::Prone;
            float view = w.yaw;
            for (int i = 0; i < 120; ++i) {
                if (i < 36) view += 5.0f;
                ti.yaw = view;
                playerMove(floorCw, p, ti, 0.05f, m);
                view += p.viewYawCorrection;
                p.viewYawCorrection = 0;
                CHECK(std::fabs(std::remainder(p.proneYaw - view, 360.0f)) <= 85.01f);
            }
            std::cout << "prone turn: body " << p.proneYaw << " view " << view << "\n";
            return std::fabs(std::remainder(p.proneYaw - w.yaw, 360.0f)); // how far the body turned
        };
        const float mpTurn = turn(mp), spTurn = turn(MoveParams{});
        CHECK(mpTurn < spTurn - 30.0f);
        CHECK(spTurn > 50.0f); // the SP body follows a view pushed past bg_prone_yawcap - 5
    }

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
