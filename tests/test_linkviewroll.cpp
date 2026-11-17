// The player view while an absolute link holds the player (docs/spec/animscripted.md "Player links"): the client copies all three link angles,
// roll included (0x4af097), and the view's field of view starts from the cg_fov dvar (CG_CalcFov 0x4ae7b0), which duhoc_assault narrows to 55
// for the shell-shocked moments on the beach (duhoc_assault.gsc:1160-1170). The level part needs the game files (IW2_MAIN).
#include "fs/vfs.h"
#include "game/linkview.h"
#include "game/world.h"

#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                                \
        }                                                                              \
    } while (0)
bool near(float a, float b, float eps = 0.05f) { return std::fabs(std::remainder(a - b, 360.0f)) < eps; }
} // namespace

int main() {
    using iw2::game::LinkViewSmoother;
    {
        // An absolute link owes the tag's roll as well and shows it over the server interval.
        LinkViewSmoother sv;
        float pitch = 0, yaw = 0;
        sv.push(-10, 178, pitch, yaw, true, 14);
        sv.step(0.5f, pitch, yaw);
        CHECK(near(sv.roll, 7) && near(yaw, 89) && near(pitch, -5));
        sv.step(1.0f, pitch, yaw);
        CHECK(near(sv.roll, 14) && near(yaw, 178) && near(pitch, -10));
        // dontinterpolate drops what is owed but keeps the roll shown
        sv.push(-10, 178, pitch, yaw, true, 40);
        sv.clear();
        sv.step(1.0f, pitch, yaw);
        CHECK(near(sv.roll, 14));
        // a delta link brings no roll: the shown roll goes back to 0
        sv.push(pitch, yaw + 5, pitch, yaw, false);
        sv.step(1.0f, pitch, yaw);
        CHECK(near(sv.roll, 0) && near(yaw, 183));
        // without a link nothing is owed and the roll is gone
        sv.push(0, 0, pitch, yaw, true, 25);
        sv.reset();
        CHECK(sv.roll == 0.0f && sv.pend[0] == 0.0f && sv.pend[1] == 0.0f && sv.pend[2] == 0.0f);
    }
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << (failures ? "FAILED\n" : "link view roll OK, level part skipped (IW2_MAIN not set)\n");
        return failures ? 1 : 0;
    }
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("iw2_linkviewroll_" + std::to_string(::getpid()));
    fs::create_directories(dir / "maps");
    {
        std::ofstream f(dir / "maps" / "zz_linkviewroll.gsc");
        f << "link()\n{\n\tm = spawn(\"script_model\", level.player.origin);\n\tm.angles = (10, 90, 30);\n"
             "\tlevel.player playerlinktoabsolute(m);\n}\n"
             "unlinkplayer()\n{\n\tlevel.player unlink();\n}\n"
             "big()\n{\n\tsetsavedcvar(\"cg_fov\", \"500\");\n}\n"
             "small()\n{\n\tsetsavedcvar(\"cg_fov\", \"0\");\n}\n";
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir.string());
    iw2::game::World w(vfs, "duhoc_assault");
    w.start();
    if (!w.player()) return 1;
    auto run = [&](const char* file, const char* fn) {
        w.vm().start(file, fn, {}, iw2::script::Value(w.vm().level()));
        w.vm().drainReady();
    };
    // cg_fov: 80 until a script changes it; getcvar answers the dvar's value
    CHECK(w.cgFov() == 80.0f);
    CHECK(w.cvars["cg_fov"] == "80");
    // the shipped shell-shock helper narrows the view to 55 and puts the saved value back after 20.4 s
    run("maps/duhoc_assault.gsc", "player_higgins_explosion_cameraAnimate_fovChange");
    CHECK(w.cgFov() == 55.0f);
    for (int i = 0; i < 20 * 20; ++i) w.frame();
    CHECK(w.cgFov() == 55.0f);
    for (int i = 0; i < 20; ++i) w.frame();
    CHECK(w.cgFov() == 80.0f);
    // the dvar range 1..160
    run("maps/zz_linkviewroll.gsc", "big");
    CHECK(w.cgFov() == 160.0f);
    run("maps/zz_linkviewroll.gsc", "small");
    CHECK(w.cgFov() == 1.0f);
    // an absolute link reports the parent's whole frame as the view angles, roll included
    run("maps/zz_linkviewroll.gsc", "link");
    w.takePlayerLinkView();
    w.frame();
    const auto lv = w.takePlayerLinkView();
    CHECK(lv && lv->absolute);
    if (lv) CHECK(near(lv->angles[0], 10) && near(lv->angles[1], 90) && near(lv->angles[2], 30));
    run("maps/zz_linkviewroll.gsc", "unlinkplayer");
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (failures) return 1;
    std::cout << "ok\n";
    return 0;
}
