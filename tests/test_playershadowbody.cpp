// The player's shadow-only body pose (src/game/playershadowbody.*, docs/render-enhanced.md "Player shadow"): the multiplayer anim
// tree and mp/playeranim.script are loaded from the game files (IW2_MAIN), and the pose is checked for each state of the
// player: stance, movement, strafing, aim, weapon switch, reload, shot, and the cases with no body.
#include "game/playershadowbody.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace iw2;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::cout << "FAIL: " << what << "\n";
    }
}

struct Fixture {
    fs::Vfs vfs;
    std::unique_ptr<game::AnimLibrary> lib;
    game::PlayerAnimScript script;
    assets::XModelParts parts;
    bool ok = false;

    explicit Fixture(const char* main) {
        vfs.mountDirectory(main);
        lib = std::make_unique<game::AnimLibrary>(vfs);
        const auto bytes = vfs.read("mp/playeranim.script");
        if (!bytes || !script.parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()))) return;
        const auto md = vfs.read("xmodel/playerbody_american_normandy01");
        if (!md) return;
        const auto xm = assets::parseXModel(*md);
        const auto pd = vfs.read("xmodelparts/" + xm.lods.at(0).name);
        if (!pd) return;
        parts = assets::parseXModelParts(*pd);
        ok = lib->tree("multiplayer") != nullptr;
    }
    int bone(const char* name) const {
        for (size_t i = 0; i < parts.bones.size(); ++i)
            if (parts.bones[i].name == name) return int(i);
        return -1;
    }
};

using Vec = std::array<float, 3>;
float dist(const Vec& a, const Vec& b) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }

// A body and its clock: step() advances the render clock by `ms` in frames of `frameMs`.
struct Rig {
    Fixture& f;
    game::PlayerShadowBody body;
    game::PlayerShadowInput in;
    explicit Rig(Fixture& fx) : f(fx), body(*fx.lib, fx.script) {
        in.renderMs = 5000;
        in.weapon = "thompson", in.weaponClass = "smg", in.mpAnimType = "thompson";
        in.clip = 20;
    }
    void step(int ms, int frameMs = 16) {
        for (int t = 0; t < ms; t += frameMs) {
            in.renderMs += std::min(frameMs, ms - t);
            body.update(in);
        }
    }
    Vec at(const char* name) const { return body.pose(f.parts).position[size_t(f.bone(name))]; }
    std::string legs() const { return body.anim().tree().nodes()[size_t(body.state().legsNode)].name; }
    std::string torso() const { return body.state().torsoActive ? body.anim().tree().nodes()[size_t(body.state().torsoNode)].name : std::string(); }
};

void stanceTests(Fixture& f) {
    Vec head[3], root[3];
    const int heights[3] = {60, 40, 11};
    for (int i = 0; i < 3; ++i) {
        Rig r(f);
        r.in.viewHeightTarget = heights[i];
        check(r.body.update(r.in), "a body in every stance");
        check(r.body.on(), "on() after an active update");
        r.step(500);
        head[i] = r.at("j_head"), root[i] = r.at("j_mainroot");
        if (i == 0) check(r.body.moveType() == "idle", "stand: idle");
        if (i == 1) check(r.body.moveType() == "idlecr", "crouch: idlecr");
        if (i == 2) check(r.body.moveType() == "idleprone", "prone: idleprone");
    }
    check(head[0][2] > head[1][2] + 10.0f, "the standing head is above the crouched head");
    check(head[1][2] > head[2][2] + 10.0f, "the crouched head is above the prone head");
    check(root[0][2] > root[1][2] + 5.0f && root[1][2] > root[2][2] + 5.0f, "the pelvis lowers with the stance");
    check(dist(head[0], head[1]) > 10.0f && dist(head[1], head[2]) > 10.0f && dist(head[0], head[2]) > 10.0f, "three different poses");
    // Prone lies along the view yaw: the feet are well behind the head (not a standing pose lowered).
    Rig p(f);
    p.in.viewHeightTarget = 11;
    p.step(500);
    check(p.at("j_ankle_le")[0] < p.at("j_head")[0] - 20.0f, "prone: the feet lie behind the head");
}

void moveTests(Fixture& f) {
    Rig idle(f), run(f);
    run.in.vx = 190.0f;
    idle.step(300), run.step(300);
    check(run.body.moveType() == "run" && idle.body.moveType() == "idle", "move types: run and idle");
    check(run.legs() != idle.legs(), "running plays another legs animation than standing");
    // The standing body is at rest: the same pose a quarter of a second later; the running legs move.
    const Vec i0 = idle.at("j_ankle_le"), r0 = run.at("j_ankle_le");
    idle.step(250), run.step(250);
    check(dist(i0, idle.at("j_ankle_le")) < 0.01f, "standing: the legs stay still over time");
    check(dist(r0, run.at("j_ankle_le")) > 1.0f, "running: the legs move over time");
    // The animation advances with the render clock: the same clock, the same pose; a clock that stands does not step it.
    const float t0 = run.body.anim().nodeStates()[size_t(run.body.state().legsNode)].time;
    run.body.update(run.in);
    check(run.body.anim().nodeStates()[size_t(run.body.state().legsNode)].time == t0, "a frame with the same render time does not step");
    // Frame-rate independence: one second at 60 and at 144 frames per second reach the same animation time.
    Rig a(f), b(f);
    a.in.vx = b.in.vx = 190.0f;
    a.step(400), b.step(400);
    a.step(1000, 16), b.step(1000, 7);
    const int na = a.body.state().legsNode, nb = b.body.state().legsNode;
    check(na == nb && na > 0, "the same legs node at any frame rate");
    const float ta = a.body.anim().nodeStates()[size_t(na)].time, tb = b.body.anim().nodeStates()[size_t(nb)].time;
    const float d = std::fabs(ta - tb);
    check(std::min(d, 1.0f - d) < 0.05f, "the same animation time after the same render time at 60 and 144 fps");
    // The step is capped: a stall of ten seconds moves the animation like a tenth.
    Rig g(f), h(f);
    g.in.vx = h.in.vx = 190.0f;
    g.step(400), h.step(400);
    g.in.renderMs += 10000, g.body.update(g.in);
    h.in.renderMs += 100, h.body.update(h.in);
    check(dist(g.at("j_ankle_le"), h.at("j_ankle_le")) < 0.5f, "a long gap steps at most 100 ms");
    // A clock that goes back (a level change) starts afresh: still a body, and it moves on from the new clock.
    run.in.renderMs -= 5000;
    check(run.body.update(run.in) && run.body.on() && run.body.moveType() == "run", "a clock that goes back starts afresh");
    const Vec back0 = run.at("j_ankle_le");
    run.step(100);
    check(dist(back0, run.at("j_ankle_le")) > 0.5f, "the animation goes on from the new clock");
    // Walking back, crouch running and prone crawling choose their own move types.
    Rig bk(f), cr(f), cw(f);
    bk.in.vx = -190.0f, cr.in.vx = 100.0f, cr.in.viewHeightTarget = 40, cw.in.vx = 30.0f, cw.in.viewHeightTarget = 11;
    bk.step(300), cr.step(300), cw.step(300);
    check(bk.body.moveType() == "runbk", "backwards: runbk");
    check(cr.body.moveType() == "runcr" || cr.body.moveType() == "walkcr", "crouched moving: runcr / walkcr");
    check(cw.body.moveType() == "walkprone", "prone moving: walkprone");
    check(dist(cw.at("j_ankle_le"), Rig(f).at("j_ankle_le")) > 1.0f, "prone crawling differs from standing");
}

void strafeTests(Fixture& f) {
    Rig fwd(f), right(f), left(f);
    fwd.in.vx = 190.0f;
    right.in.vy = -190.0f; // yaw 0: the view's right is -y
    left.in.vy = 190.0f;
    fwd.step(300), right.step(300), left.step(300);
    check(right.body.strafing() == "right" && left.body.strafing() == "left" && fwd.body.strafing() == "not", "the strafing value");
    check(right.legs() != fwd.legs() && left.legs() != fwd.legs() && left.legs() != right.legs(), "strafing selects its own animations");
    // The move direction follows the view yaw: the same velocity at yaw 90 is a forward run.
    Rig turned(f);
    turned.in.yaw = 90.0f, turned.in.vy = 190.0f;
    turned.step(300);
    check(turned.legs() == fwd.legs(), "the same movement relative to the view is the same animation");
}

void aimTests(Fixture& f) {
    Rig hip(f), ads(f);
    ads.in.aiming = true, ads.in.adsFrac = 1.0f;
    hip.step(500), ads.step(500);
    auto aimName = [](const Rig& r) { return r.body.aimNode() > 0 ? r.body.anim().tree().nodes()[size_t(r.body.aimNode())].name : std::string(); };
    check(!aimName(ads).empty() && aimName(ads) != ads.legs(), "the aim idle plays beside the hip idle");
    check(ads.body.aimWeight() == 1.0f && hip.body.aimWeight() == 0.0f, "aimed: all the weight on the aim idle; at the hip none");
    check(dist(hip.at("tag_weapon_right"), ads.at("tag_weapon_right")) > 1.0f, "the weapon hand moves with the aim");
    // The aim pose's weight is the view model's aim fraction, frame by frame, as it rises (the weapon's ADS transition in) and falls
    // (out): no threshold, no blend of its own. The fraction here rises linearly over 300 ms and falls over 200 ms, as a view model's.
    Rig change(f);
    change.step(500);
    const float full = dist(ads.at("tag_weapon_right"), hip.at("tag_weapon_right"));
    bool equal = true, monotonic = true;
    float last = 0.0f;
    change.in.aiming = true;
    for (int t = 16; t <= 320; t += 16) {
        change.in.adsFrac = std::min(1.0f, float(t) / 300.0f);
        change.step(16);
        equal &= std::fabs(change.body.aimWeight() - change.in.adsFrac) < 1e-5f;
        const float d = dist(change.at("tag_weapon_right"), hip.at("tag_weapon_right"));
        monotonic &= d >= last - 0.05f;
        last = d;
        if (t == 48) check(d > 0.01f && d < 0.5f * full, "a sixth of the way into the raise, the pose is a little way to the aim");
    }
    check(equal, "raising: the aim weight equals the view model's aim fraction every frame");
    check(monotonic, "raising: the weapon hand moves steadily to the aim pose");
    check(dist(change.at("tag_weapon_right"), ads.at("tag_weapon_right")) < 0.05f * full, "fully raised: the aim pose");
    change.in.aiming = false;
    for (int t = 16; t <= 208; t += 16) {
        change.in.adsFrac = std::max(0.0f, 1.0f - float(t) / 200.0f);
        change.step(16);
        equal &= std::fabs(change.body.aimWeight() - change.in.adsFrac) < 1e-5f;
        if (t == 48) check(change.body.aimWeight() > 0.7f, "letting go: the aim pose stays while the view model's aim is still high");
    }
    check(equal, "lowering: the aim weight equals the view model's aim fraction every frame");
    check(dist(change.at("tag_weapon_right"), hip.at("tag_weapon_right")) < 0.05f * full, "fully lowered: the hip pose");
    // Half raised, the weapon hand is about half way (the two animations are siblings in the tree, so the pose blends by the share).
    Rig half(f);
    half.in.aiming = true, half.in.adsFrac = 0.5f;
    half.step(500);
    const Vec mid = half.at("tag_weapon_right"), h0 = hip.at("tag_weapon_right"), a1 = ads.at("tag_weapon_right");
    const Vec lerp{0.5f * (h0[0] + a1[0]), 0.5f * (h0[1] + a1[1]), 0.5f * (h0[2] + a1[2])};
    check(dist(mid, lerp) < 0.25f * full, "half raised: the weapon hand about half way");
    // The hip and aim animations share a parent in the tree, in every stance and move type and for another weapon type.
    auto siblings = [&](int height, float vx, const char* weapon, const char* cls) {
        Rig r(f);
        r.in.viewHeightTarget = height, r.in.vx = vx, r.in.weapon = weapon, r.in.mpAnimType = weapon, r.in.weaponClass = cls;
        r.in.aiming = true, r.in.adsFrac = 0.5f;
        r.step(500);
        const auto& nodes = r.body.anim().tree().nodes();
        const int a = r.body.aimNode(), l = r.body.state().legsNode;
        return a == 0 || (l > 0 && nodes[size_t(a)].parent == nodes[size_t(l)].parent);
    };
    bool sib = true;
    for (const int h : {60, 40, 11})
        for (const float v : {0.0f, 190.0f}) sib &= siblings(h, v, "thompson", "smg") && siblings(h, v, "kar98k", "rifle");
    check(sib, "the hip and aim legs animations are siblings in every stance, at rest and moving, for two weapon types");
    // Walking while aiming: the aim walk weighs in with the fraction beside the run, and the run comes back as it falls.
    Rig run(f), aimRun(f);
    run.in.vx = aimRun.in.vx = 190.0f;
    run.step(500), aimRun.step(500);
    aimRun.in.aiming = true, aimRun.in.adsFrac = 0.5f;
    aimRun.step(16);
    check(aimRun.legs() == run.legs() && !aimName(aimRun).empty() && aimName(aimRun) != run.legs(), "aiming while running: the aim walk beside the run");
    check(std::fabs(aimRun.body.aimWeight() - 0.5f) < 1e-5f, "half raised: half the weight on the aim walk");
    aimRun.in.aiming = false, aimRun.in.adsFrac = 0.0f;
    aimRun.step(16);
    check(aimRun.body.aimWeight() == 0.0f && aimRun.legs() == run.legs(), "lowered: the run alone");
    // The torso follows the view pitch (our own rule): looking down bends it forward and down, not sideways (yaw 0: forward is +x).
    Rig level(f), down(f), up(f);
    down.in.pitch = 50.0f, up.in.pitch = -50.0f;
    level.step(500), down.step(500), up.step(500);
    const Vec l = level.at("j_head"), dn = down.at("j_head"), upv = up.at("j_head");
    check(dn[0] - l[0] > 3.0f && dn[2] - l[2] < -1.0f, "looking down: the head goes forward and down");
    check(std::fabs(dn[1] - l[1]) < 0.25f * (dn[0] - l[0]), "looking down: no sideways bend");
    check(upv[0] - l[0] < -3.0f && std::fabs(upv[1] - l[1]) < 0.25f * (l[0] - upv[0]), "looking up leans the torso back");
    check(dist(level.at("j_ankle_le"), down.at("j_ankle_le")) < 0.01f, "the legs do not follow the view pitch");
    // Prone: the body lies along the ground; the pitch keeps it near the ground and bends no more than standing does.
    Rig p0(f), p60(f), pm60(f);
    p0.in.viewHeightTarget = p60.in.viewHeightTarget = 11;
    p60.in.pitch = 60.0f, pm60.in.pitch = -60.0f;
    pm60.in.viewHeightTarget = 11;
    p0.step(500), p60.step(500), pm60.step(500);
    check(dist(p0.at("j_head"), p60.at("j_head")) < 12.0f, "prone: the pitch bends the torso only a little");
    check(p60.at("j_head")[2] > p0.at("j_head")[2] - 2.0f && pm60.at("j_head")[2] > p0.at("j_head")[2] - 2.0f, "prone: the head does not go through the ground");
    check(dist(p0.at("j_ankle_le"), p60.at("j_ankle_le")) < 0.01f, "prone: the legs do not follow the view pitch");
}

void weaponTests(Fixture& f) {
    // A weapon switch: the drop plays the torso pose of putting the weapon away, then the torso falls back.
    Rig sw(f), rest(f);
    sw.step(500), rest.step(500);
    check(sw.torso().empty(), "no torso animation at rest");
    const Vec before = sw.at("tag_weapon_right");
    sw.in.weaponState = 2;
    sw.step(100);
    check(!sw.torso().empty(), "the weapon drop plays a torso animation");
    sw.step(100);
    check(dist(before, sw.at("tag_weapon_right")) > 1.0f, "the weapon switch moves the torso");
    sw.in.weaponState = 1, sw.in.weapon = "mp40", sw.in.weaponClass = "smg", sw.in.mpAnimType = "mp40";
    sw.step(100);
    sw.in.weaponState = 0;
    sw.step(2500);
    check(sw.torso().empty(), "the torso falls back to the legs animation when the hold ends");
    check(dist(rest.at("j_spine4"), sw.at("j_spine4")) < 3.0f, "back at the idle pose after the switch");
    // A reload plays its torso animation and holds it.
    Rig rl(f);
    rl.step(500);
    rl.in.weaponState = 5;
    rl.step(100);
    check(!rl.torso().empty() && rl.torso() != sw.torso(), "a reload plays a torso animation");
    const std::string reloadAnim = rl.torso();
    // A shot (the clip falls) plays the firing torso animation when standing still, none while moving.
    Rig shot(f), runShot(f);
    shot.step(500), runShot.step(500);
    runShot.in.vx = 190.0f;
    runShot.step(300);
    shot.in.clip = 19, runShot.in.clip = 19;
    shot.step(50), runShot.step(50);
    check(!shot.torso().empty() && shot.torso() != reloadAnim, "a shot standing plays the firing animation");
    check(runShot.torso().empty(), "a shot while moving plays no extra animation");
    // A melee swing.
    Rig me(f);
    me.step(300);
    me.in.weaponState = 10;
    me.step(50);
    check(!me.torso().empty(), "a melee swing plays a torso animation");
    // Another weapon class is another idle: a pistol holds differently from a rifle.
    Rig pistol(f), rifle(f);
    pistol.in.weaponClass = "pistol", pistol.in.mpAnimType = "colt", rifle.in.weaponClass = "rifle", rifle.in.mpAnimType = "kar98k";
    pistol.step(400), rifle.step(400);
    check(pistol.legs() != rifle.legs(), "the weapon's class changes the pose");
}

void stanceChangeTests(Fixture& f) {
    // Crouch to prone plays the transition on the legs, then the prone idle.
    Rig r(f);
    r.in.viewHeightTarget = 40;
    r.step(400);
    r.in.viewHeightTarget = 11;
    r.step(100);
    const std::string transition = r.legs();
    check(transition != "" && r.body.moveType() == "idleprone", "going prone is the prone idle move type");
    r.step(3000);
    check(r.legs() != transition, "after the transition the prone idle plays");
    // Standing up from crouch changes the move type and the legs.
    Rig s(f);
    s.in.viewHeightTarget = 40;
    s.step(400);
    const std::string crouched = s.legs();
    s.in.viewHeightTarget = 60;
    s.step(400);
    check(s.legs() != crouched && s.body.moveType() == "idle", "standing up plays the standing idle");
}

void offTests(Fixture& f) {
    // Each excluded case gives no body, and drops the state.
    for (int which = 0; which < 5; ++which) {
        Rig r(f);
        r.step(300);
        check(r.body.on(), "on while nothing excludes it");
        game::PlayerShadowInput in = r.in;
        (which == 0 ? in.onTurret : which == 1 ? in.inVehicle : which == 2 ? in.linked : which == 3 ? in.dead : in.spectating) = true;
        check(in.excluded(), "the flag excludes");
        in.renderMs += 16;
        check(!r.body.update(in), "no body on a turret, in a vehicle, linked, dead or spectating");
        check(!r.body.on(), "an excluded frame drops the state");
    }
    // The first pose after the gap is taken at once: the same pose as a fresh body, not a blend from the old one.
    Rig r(f);
    r.step(300);
    r.in.dead = true;
    r.body.update(r.in);
    r.in.dead = false;
    r.in.viewHeightTarget = 11;
    r.in.renderMs += 1000;
    check(r.body.update(r.in), "the body returns");
    Rig fresh(f);
    fresh.in.viewHeightTarget = 11;
    fresh.in.renderMs = r.in.renderMs;
    fresh.body.update(fresh.in);
    check(dist(r.at("j_head"), fresh.at("j_head")) < 0.01f, "the first pose is taken at once");
    // No weapon: the unarmed animations, still a body.
    Rig un(f);
    un.in.weapon.clear();
    check(un.body.update(un.in), "a body without a weapon");
}

} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    Fixture f(main);
    if (!f.ok) {
        std::cout << "FAIL: the animation tree, the script or the body model did not load\n";
        return 1;
    }
    stanceTests(f);
    moveTests(f);
    strafeTests(f);
    aimTests(f);
    weaponTests(f);
    stanceChangeTests(f);
    offTests(f);
    std::cout << (failures ? "FAILED" : "ok") << "\n";
    return failures ? 1 : 0;
}
