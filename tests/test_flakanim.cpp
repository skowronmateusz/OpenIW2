// The flak88 firing animation is drawn at the client-frame anim time: the state before the last server frame advanced by the
// interpolation fraction (cgame 0x4afb70 advances DObj anim time per client frame), so the time grows smoothly between two
// 50 ms server frames instead of stepping. Needs IW2_MAIN (the flak88 animtree and its fire animation).
#include "fs/vfs.h"
#include "game/animtree.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace iw2::game;

static int failures = 0;
#define CHECK(c)                                                                        \
    do {                                                                                \
        if (!(c)) { std::cout << "FAIL line " << __LINE__ << ": " #c "\n"; ++failures; } \
    } while (0)

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "flak anim test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    AnimLibrary lib(vfs);
    const AnimTree* tree = lib.tree("flak88");
    CHECK(tree != nullptr);
    if (!tree) return 1;
    const int fire = tree->find("flak88_gun_fire_antitank");
    CHECK(fire >= 0);
    if (fire < 0) return 1;
    AnimState st(*tree, lib);
    AnimState::SetArgs a;
    a.blendTime = 0;
    st.set(fire, a);
    auto noNote = [](const std::string&, const std::string&) {};
    int midChecked = 0;
    for (int frame = 0; frame < 6; ++frame) {
        const AnimState before = st; // the state before this server frame
        st.updateWeights(0.05f);
        st.updateTruncated(0.05f, true, noNote);
        const float t0 = before.time(fire), t1 = st.time(fire);
        float last = t0;
        for (int k = 1; k <= 5; ++k) { // five rendered frames between the two server frames
            const float frac = float(k) / 5.0f;
            const float tc = clientAnimState(before, frac).time(fire);
            CHECK(tc > last - 1e-6f); // never goes back
            if (frac < 1.0f && t1 > t0 + 1e-4f && t1 < 1.0f) CHECK(tc < t1 - 1e-6f); // strictly between the server states
            last = tc;
        }
        // Not a 20 Hz step: the mid-frame time differs from both server-frame times while the anim plays.
        if (t1 > t0 + 1e-4f && t1 < 1.0f) {
            const float mid = clientAnimState(before, 0.5f).time(fire);
            CHECK(mid > t0 + 1e-5f && mid < t1 - 1e-5f);
            ++midChecked;
        }
        // At fraction 1 the client state equals the server state.
        CHECK(std::fabs(clientAnimState(before, 1.0f).time(fire) - t1) < 1e-4f);
    }
    CHECK(midChecked >= 3);
    return failures ? 1 : 0;
}
