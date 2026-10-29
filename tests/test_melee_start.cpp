// Melee view animations: the first frame of a melee animation is compared with the weapon's idle pose (needs IW2_MAIN).
// The SVT-40 melee animation is authored starting at a pose far from the idle pose (the gun is turned by about 168
// degrees at tag_weapon and the final frame is the idle pose), while the other rifles start near their idle pose.
// The view model plays an action animation at weight 1 with no blend-in (docs/spec/weapons.md), so the visible jump
// at the start of the SVT-40 melee is in the shipped data and not an engine fault.
#include "assets/xanim.h"
#include "fs/vfs.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace iw2;

static double angleDeg(const assets::Quat& a, const assets::Quat& b) {
    const double dot = std::fabs(double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z + double(a.w) * b.w);
    const double n = std::sqrt((double(a.x) * a.x + double(a.y) * a.y + double(a.z) * a.z + double(a.w) * a.w) *
                               (double(b.x) * b.x + double(b.y) * b.y + double(b.z) * b.z + double(b.w) * b.w));
    return 2 * std::acos(std::min(1.0, dot / n)) * 180 / M_PI;
}

// Mean rotation difference over the bones both animations animate: melee frame `meleeFrame` against idle frame 0.
static double meanDiff(fs::Vfs& vfs, const std::string& weapon, float meleeFrame) {
    auto dm = vfs.read("xanim/viewmodel_" + weapon + "_melee");
    auto di = vfs.read("xanim/viewmodel_" + weapon + "_idle");
    if (!dm || !di) return -1;
    const auto m = assets::parseXAnim(*dm), i = assets::parseXAnim(*di);
    double sum = 0;
    int n = 0;
    for (auto& bm : m.bones)
        for (auto& bi : i.bones)
            if (bm.name == bi.name && !bm.rot.keys.empty() && !bi.rot.keys.empty()) {
                sum += angleDeg(assets::sampleRot(bm.rot, meleeFrame, m.keyCount()), assets::sampleRot(bi.rot, 0, i.keyCount()));
                ++n;
            }
    return n ? sum / n : -1;
}

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "melee_start test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    fs::Vfs vfs;
    vfs.mountDirectory(main);
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++failures; }
    };
    const double svt = meanDiff(vfs, "svt40", 0);
    check(svt > 20, "SVT-40 melee starts far from its idle pose");
    for (const char* w : {"kar98", "mosinnagant", "m1garand"}) {
        const double d = meanDiff(vfs, w, 0);
        check(d >= 0 && d < 3, "other rifles' melee starts near the idle pose");
    }
    check(meanDiff(vfs, "svt40", 16) < svt / 2, "SVT-40 melee ends nearer to idle than it starts");
    std::cout << (failures ? "melee_start FAILED\n" : "melee_start ok\n");
    return failures ? 1 : 0;
}
