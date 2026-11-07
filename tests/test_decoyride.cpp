// The opening ride of decoytown (needs IW2_MAIN=<game main dir>). The player is linked to the carrier by playerlinktodelta; when the
// carrier has stopped the map script drops that link, links the player with a plain linkto to a script_origin and moves that along
// two points, then unlinks (maps/decoytown.gsc, the player-ride thread). ClientEndFrame (0x4e7fd6) runs the link placement (0x4d6ec0)
// for any link on a client, so the plain link carries the player out of the carrier's hull. Without it the player stayed at the seat,
// inside the carrier's collision, and every move started solid.
#include "fs/vfs.h"
#include "game/linkview.h"
#include "game/pmove.h"
#include "game/world.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace iw2::game;

static int failures = 0;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) { std::cout << "FAIL line " << __LINE__ << ": " #c "\n"; ++failures; } \
    } while (0)

int main() {
    {
        // The turn a link made in a server frame is shown over the following interval, not at once (CG_InterpolatePlayerState 0x4b9940).
        LinkViewSmoother sv;
        float pitch = 0, yaw = 350;
        sv.push(0, 10, pitch, yaw, false); // +20 degrees across the wrap
        sv.step(0.25f, pitch, yaw);
        CHECK(std::fabs(std::remainder(yaw - 350, 360.0f) - 5.0f) < 1e-3f);
        sv.step(0.5f, pitch, yaw);
        CHECK(std::fabs(std::remainder(yaw - 350, 360.0f) - 10.0f) < 1e-3f);
        sv.step(0.99f, pitch, yaw);
        sv.step(0.99f, pitch, yaw);
        CHECK(std::fabs(std::remainder(yaw - 350, 360.0f) - 19.8f) < 0.05f);
        // the next frame owes the rest on top of its own turn
        sv.push(0, yaw + 20, pitch, yaw, false);
        CHECK(sv.pend[1] > 20.0f && sv.pend[1] < 20.3f);
        // an absolute link owes the way to the tag's angles, replacing what was owed
        sv.push(30, 100, 0, 90, true);
        CHECK(std::fabs(sv.pend[0] - 30) < 1e-4f && std::fabs(sv.pend[1] - 10) < 1e-4f);
        sv.clear();
        float p2 = 5, y2 = 5;
        sv.step(1.0f, p2, y2);
        CHECK(p2 == 5 && y2 == 5);
    }
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << (failures ? "FAILED\n" : "decoytown ride test: view smoothing OK, level part skipped (IW2_MAIN not set)\n");
        return failures ? 1 : 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "decoytown");
    w.start();
    int linkViews = 0, linkedFrames = 0, noInterp = 0;
    bool wasLinked = false, released = false;
    Vec3 seat{}, out{};
    for (int i = 0; i < 20 * 40; ++i) {
        w.frame();
        w.player()->obj->set("$godmode", iw2::script::Value(1));
        const auto o = w.player()->obj->get("origin").asVector();
        if (w.takePlayerNoInterp()) ++noInterp;
        if (const auto lv = w.takePlayerLinkView()) {
            CHECK(!lv->absolute); // the ride uses the delta mode
            ++linkViews;
        }
        if (w.playerHasLinkParent()) {
            wasLinked = true;
            ++linkedFrames;
            if (w.playerLinked()) seat = {o[0], o[1], o[2]}; // the playerlinktodelta seat; later the plain link
        } else if (wasLinked && !released) {
            released = true;
            out = {o[0], o[1], o[2]};
        }
    }
    std::cout << "linked frames " << linkedFrames << ", link views " << linkViews << ", released " << released << "\n";
    CHECK(wasLinked && released);
    CHECK(noInterp == 1); // the script calls dontinterpolate on the player once, before the link
    CHECK(linkViews > 100); // the vehicle turns while the view follows it
    // The plain link on the script_origin moved the player away from the seat.
    const float moved = std::hypot(out[0] - seat[0], out[1] - seat[1]);
    std::cout << "seat -> release: " << moved << " units\n";
    CHECK(moved > 30.0f);
    // At the release the player is out of every solid.
    const auto o = w.player()->obj->get("origin").asVector();
    MoveParams mp;
    mp.passEntity = w.player()->number;
    const TraceResult t = w.collision().trace({o[0], o[1], o[2]}, {o[0], o[1], o[2]}, {-15, -15, 0}, {15, 15, 70}, mp.traceMask, mp.passEntity);
    CHECK(!t.startSolid);
    std::cout << (failures ? "FAILED\n" : "decoytown ride test OK\n");
    return failures ? 1 : 0;
}
