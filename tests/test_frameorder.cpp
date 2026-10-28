// Server frame order, docs/spec/ai.md "Server frame order": the first frame runs at level time 50 (frame start 0x44a6a0
// before 0x4da470), and movers run in G_RunFrame's entity pass (stage 8) after the script frame (stage 5), so a script
// sees a mover's position from the previous frame's pass; a linked entity runs its link parent first (+0x1c8), so a
// child spawned before its parent still follows the parent's position of the same frame.
// Needs the game files (IW2_MAIN); uses cityhall.

#include "fs/vfs.h"
#include "game/world.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

int failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

float num(const iw2::script::Value& v) { return v.isNumber() ? v.asFloat() : -9999.0f; }

} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    namespace fsys = std::filesystem;
    const fsys::path dir = fsys::temp_directory_path() / "iw2_test_frameorder";
    fsys::remove_all(dir);
    fsys::create_directories(dir / "maps");
    std::ofstream(dir / "maps" / "iw2test_frameorder.gsc") << R"(
movers()
{
	c = spawn("script_origin", (0, 0, 10));
	e = spawn("script_origin", (0, 0, 0));
	e moveto((100, 0, 0), 1);
	c linkto(e);
	level.t_e = e;
	level.t_c = c;
	for (i = 0; i < 3; i++)
	{
		wait 0.05;
		if (i == 0) { level.t_p0 = e.origin[0]; level.t_c0 = c.origin[0]; }
		if (i == 1) { level.t_p1 = e.origin[0]; level.t_c1 = c.origin[0]; }
		if (i == 2) { level.t_p2 = e.origin[0]; level.t_c2 = c.origin[0]; }
	}
}
)";
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir);
    {
        iw2::game::World w(vfs, "cityhall");
        w.start();
        CHECK(w.timeMs() == 50); // the first frame and the level main run at level time 50
        w.frame();
        CHECK(w.timeMs() == 100);
    }
    {
        iw2::game::World w(vfs, "cityhall");
        w.prepareStart();
        const auto level = w.vm().level();
        w.vm().start("maps/iw2test_frameorder.gsc", "movers", {}, iw2::script::Value(level));
        for (int i = 0; i < 4; ++i) w.frame();
        // moveto at time 0; the frame at 50 resumes the script (stage 5) before the pass moves the mover (stage 8).
        std::cout << "mover seen " << num(level->get("t_p0")) << " " << num(level->get("t_p1")) << " " << num(level->get("t_p2"))
                  << ", child " << num(level->get("t_c0")) << " " << num(level->get("t_c1")) << " " << num(level->get("t_c2")) << "\n";
        CHECK(std::fabs(num(level->get("t_p0")) - 0.0f) < 0.01f);
        CHECK(std::fabs(num(level->get("t_p1")) - 5.0f) < 0.01f);
        CHECK(std::fabs(num(level->get("t_p2")) - 10.0f) < 0.01f);
        // the child (lower entity number) follows the parent of the same pass
        CHECK(std::fabs(num(level->get("t_c1")) - num(level->get("t_p1"))) < 0.01f);
        CHECK(std::fabs(num(level->get("t_c2")) - num(level->get("t_p2"))) < 0.01f);
    }
    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "frameorder ok\n";
    return failures ? 1 : 0;
}
