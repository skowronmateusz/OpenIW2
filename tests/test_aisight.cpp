// AI sight builtins against the executable (needs IW2_MAIN): geteye (0x4eee60) is an actor's tag_eye (0x500910), and
// canshoot (0x53d800) traces from the muzzle (tag_flash, 0x500b40) plus the optional offset argument (0x53d891..0x53d8ca)
// to the target with 0x54cf40. The player's eye (0x4e8b70) is its origin raised by the pmove view height, and an
// actor's `lastenemysightpos` (+0x203c) follows 0x4fe830 / 0x515150.

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

playereye()
{
	level.t_peye = level.player geteye();
	level.t_porg = level.player.origin;
}

sightpos()
{
	level.t_lesp = self.lastEnemySightPos;
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
    // 0x4e8b70: the player's eye height is the view height of the player state (prone 11 here), not a standing 60.
    {
        iw2::game::World::PlayerCommand cmd;
        cmd.viewHeight = 11.0f;
        world.setPlayerCommand(cmd);
        world.vm().start("maps/iw2test_aisight.gsc", "playereye", {}, iw2::script::Value(world.vm().level()));
        const auto level = world.vm().level();
        const auto eye = level->get("t_peye"), org = level->get("t_porg");
        CHECK(eye.isVector() && org.isVector());
        if (eye.isVector() && org.isVector()) {
            CHECK(std::fabs(eye.asVector()[2] - org.asVector()[2] - 11.0f) < 0.01f);
            CHECK(std::fabs(eye.asVector()[0] - org.asVector()[0]) < 0.01f);
        }
        cmd.viewHeight = 60.0f;
        world.setPlayerCommand(cmd);
    }
    // 0x515150 / 0x4fe830: an enemy change clears lastenemysightpos and sets it to the new enemy's eye only when the
    // enemy was seen this frame; it survives losing sight (no clear in 0x4fe830) and is undefined without an enemy.
    if (actor && world.player()) {
        auto sightPos = [&] {
            world.vm().start("maps/iw2test_aisight.gsc", "sightpos", {}, iw2::script::Value(actor->obj));
            return world.vm().level()->get("t_lesp");
        };
        iw2::game::Entity* player = world.player();
        auto& info = actor->sentientInfo[player->obj.get()];
        world.debugSetEnemy(*actor, nullptr);
        info.visible = false;
        info.lastSeenMs = world.timeMs() - 50;
        world.debugSetEnemy(*actor, player);
        CHECK(!sightPos().isVector()); // not seen this frame: undefined
        world.debugSetEnemy(*actor, nullptr);
        info.visible = true;
        info.lastSeenMs = world.timeMs();
        world.debugSetEnemy(*actor, player);
        const auto p = sightPos();
        CHECK(p.isVector());
        if (p.isVector()) {
            const auto o = player->obj->get("origin").asVector();
            CHECK(std::fabs(p.asVector()[2] - o[2] - 60.0f) < 0.01f); // the player's eye (view height 60)
        }
        world.debugSetEnemy(*actor, nullptr);
        CHECK(!sightPos().isVector()); // cleared with the enemy
    }
    fsys::remove_all(dir);
    if (failures) return 1;
    std::cout << "aisight tests passed\n";
    return 0;
}
