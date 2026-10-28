// AI sight builtins against the executable (needs IW2_MAIN): geteye (0x4eee60) is an actor's tag_eye (0x500910), and
// canshoot (0x53d800) traces from the muzzle (tag_flash, 0x500b40) plus the optional offset argument (0x53d891..0x53d8ca)
// to the target with 0x54cf40.

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

} // namespace

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "aisight: IW2_MAIN not set, skipped\n";
        return 0;
    }
    namespace fsys = std::filesystem;
    const fsys::path dir = fsys::temp_directory_path() / "iw2_test_aisight";
    fsys::remove_all(dir);
    fsys::create_directories(dir / "maps");
    std::ofstream(dir / "maps" / "iw2test_aisight.gsc") << R"(
check()
{
	level.t_eye = self geteye();
	level.t_tageye = self gettagorigin("tag_eye");
	flash = self gettagorigin("tag_flash");
	level.t_short = self canshoot(flash + (0, 0, 4));
	level.t_buried = self canshoot(flash + (0, 0, 4), (0, 0, -2000));
	level.t_down = self canshoot(flash + (0, 0, -2000));
}
)";
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir);
    iw2::game::World world(vfs, "toujane");
    world.start();
    for (int i = 0; i < 5; ++i) world.frame();
    iw2::game::Entity* actor = nullptr;
    for (const auto& e : world.entities())
        if (e->isActor && e->obj->get("health").isNumber() && e->obj->get("health").asInt() > 0) {
            actor = e.get();
            break;
        }
    CHECK(actor != nullptr);
    if (actor) {
        world.vm().start("maps/iw2test_aisight.gsc", "check", {}, iw2::script::Value(actor->obj));
        const auto level = world.vm().level();
        const auto eye = level->get("t_eye"), tagEye = level->get("t_tageye");
        CHECK(eye.isVector() && tagEye.isVector());
        if (eye.isVector() && tagEye.isVector()) {
            const auto a = eye.asVector(), b = tagEye.asVector();
            CHECK(std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]) < 1.0f); // geteye is tag_eye, not a pose height
        }
        CHECK(level->get("t_short").truthy());   // a 4-unit segment up from the muzzle is clear
        CHECK(!level->get("t_buried").truthy()); // the offset moves the start 2000 units down, through the ground
        CHECK(!level->get("t_down").truthy());   // straight down from the muzzle hits the ground
    }
    fsys::remove_all(dir);
    if (failures) return 1;
    std::cout << "aisight tests passed\n";
    return 0;
}
