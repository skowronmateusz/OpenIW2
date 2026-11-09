// The gun pose a shot uses: view angles at hip, the gun's own angles and offset while aiming, the command staleness
// (docs/spec/weapons.md, "Shot direction and muzzle"), the spread disc axes.

#include "game/axis.h"
#include "game/viewweapon.h"

#include <cmath>
#include <iostream>
#include <map>
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
bool near(float a, float b, float e = 1e-3f) { return std::fabs(a - b) < e; }
using namespace iw2::game;
} // namespace

int main() {
    { // Hip: the final view angles (with the view kick the caller put in) and no offset.
        GunAimPose p;
        p.adsWeapon = true;
        p.viewAngles = {-3.0f, 30.0f, 0};
        p.moveBeforeLag = {5, 5, 5};
        const GunAim a = computeGunAim(p);
        CHECK(a.valid && a.pitch == -3.0f && a.yaw == 30.0f);
        CHECK(a.offset[0] == 0 && a.offset[1] == 0 && a.offset[2] == 0);
    }
    { // No aimDownSight: the view angles and no offset at any aim fraction.
        GunAimPose p;
        p.adsFrac = 0.5f;
        p.viewAngles = {1, 2, 0};
        p.gunAngles = {5, 5, 0};
        p.moveBeforeLag = {2, 0, 0};
        const GunAim a = computeGunAim(p);
        CHECK(a.pitch == 1.0f && a.yaw == 2.0f && a.offset[0] == 0);
    }
    { // The scope overlay: the view angles, but the offset stays.
        GunAimPose p;
        p.adsWeapon = true;
        p.adsFrac = 0.5f;
        p.overlay = true;
        p.viewAngles = {0, 0, 0};
        p.gunAngles = {5, 5, 0};
        p.moveBeforeLag = {2, 0, 0};
        const GunAim a = computeGunAim(p);
        CHECK(a.pitch == 0 && a.yaw == 0);
        CHECK(near(a.offset[0], 1.0f) && near(a.offset[1], 0) && near(a.offset[2], 0));
    }
    { // Aiming: the gun's rotation in the frame of the view, each angle cut to 16 bits (truncated).
        GunAimPose p;
        p.adsWeapon = true;
        p.adsFrac = 0.5f;
        p.viewAngles = {0, 30.0f, 0};
        p.gunAngles = {0, 10.0f, 0};
        const GunAim a = computeGunAim(p);
        CHECK(near(a.yaw, float(int(40.0f * 182.04444885f)) * 0.0054931640625f));
        CHECK(near(a.pitch, 0.0f));
        // A pitch relative to a pitched and turned view: compare with the frames composed by hand.
        p.viewAngles = {20.0f, 45.0f, 0};
        p.gunAngles = {3.0f, 4.0f, 1.0f};
        const GunAim b = computeGunAim(p);
        const auto exact = axisAngles(axisMul(anglesAxis({3.0f, 4.0f, 1.0f}), anglesAxis({20.0f, 45.0f, 0})));
        CHECK(near(b.pitch, exact[0], 0.01f) && near(b.yaw, exact[1], 0.01f));
        CHECK(!near(b.yaw, 49.0f, 0.05f)); // not the plain sum of the yaws at this pitch
        // A negative result wraps into 0..360.
        p.viewAngles = {-10.0f, 0, 0};
        p.gunAngles = {0, 0, 0};
        const GunAim c = computeGunAim(p);
        CHECK(near(c.pitch, 350.0f, 0.01f));
    }
    { // Offset: move rotated by the raw bob, then the lag on the left and up parts, in view axes, the landing dip on z, times the aim.
        GunAimPose p;
        p.adsWeapon = true;
        p.adsFrac = 1.0f;
        p.moveBeforeLag = {1, 0, 0};
        p.rawBob = {0, 90.0f, 0}; // turns forward to left (+y)
        p.lagSide = 0.5f;
        p.lagUp = 0.25f;
        p.landZ = -2.0f;
        p.viewAngles = {0, 90.0f, 0}; // view forward +y, left -x
        const GunAim a = computeGunAim(p);
        CHECK(near(a.offset[0], -0.5f)); // local left 1 - 0.5 along the view's left (-x)
        CHECK(near(a.offset[1], 0.0f));
        CHECK(near(a.offset[2], 0.25f - 2.0f));
        p.adsFrac = 0.5f;
        CHECK(near(computeGunAim(p).offset[2], (0.25f - 2.0f) * 0.5f));
    }
    { // The delay: a shot sees the pose handed in on the step before.
        GunAimDelay d;
        GunAim a, b;
        a.valid = b.valid = true;
        a.pitch = 1;
        b.pitch = 2;
        CHECK(!d.step(a).valid);
        CHECK(d.step(b).pitch == 1);
        CHECK(d.step(b).pitch == 2);
        d.reset();
        CHECK(!d.step(a).valid);
    }
    { // ViewWeaponMotion splits the lag out of move().
        std::map<std::string, float> w = {{"swayMaxAngle", 30}, {"swayLerpSpeed", 6}, {"swayHorizScale", 1}, {"swayVertScale", 1}, {"swayPitchScale", 0.1f}, {"swayYawScale", 0.1f}};
        const ViewWeaponMotion::Field f = [&](const char* k) { const auto it = w.find(k); return it == w.end() ? 0.0f : it->second; };
        ViewWeaponMotion m;
        m.update(f, 16, 0, 190, 60, 0);
        m.sway(f, 16, 0, 0, 0, 0, false);
        m.update(f, 16, 0, 190, 60, 0);
        m.sway(f, 16, 5, 5, 0, 0, false);
        CHECK(m.lagSide() != 0 && m.lagUp() != 0);
        CHECK(near(m.move()[1], m.moveBeforeLag()[1] - m.lagSide()));
        CHECK(near(m.move()[2], m.moveBeforeLag()[2] + m.lagUp()));
    }
    { // The spread disc: x along the right vector, y along the up vector (0x531210).
        const Axis ax = anglesAxis({0, 0, 0});
        const auto x = spreadDiscOffset(ax, 1, 0), y = spreadDiscOffset(ax, 0, 1);
        CHECK(near(x[0], 0) && near(x[1], -1.0f) && near(x[2], 0));
        CHECK(near(y[0], 0) && near(y[1], 0) && near(y[2], 1.0f));
    }
    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "gunaim ok\n";
    return failures ? 1 : 0;
}
