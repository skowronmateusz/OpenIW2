// AI sight builtins against the executable (needs IW2_MAIN): geteye (0x4eee60) is an actor's tag_eye (0x500910), and
// canshoot (0x53d800) traces from the muzzle (tag_flash, 0x500b40) plus the optional offset argument (0x53d891..0x53d8ca)
// to the target with 0x54cf40. The player's eye (0x4e8b70) is its origin raised by the pmove view height, and an
// actor's `lastenemysightpos` (+0x203c) follows 0x4fe830 / 0x515150.

#include <unistd.h>
#include <string>
#include "game/world.h"

#include <algorithm>
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
    const fsys::path dir = fsys::temp_directory_path() / ("iw2_test_aisight_" + std::to_string(::getpid()));
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

blendok()
{
	b = spawn("sound_blend", (0, 0, 0));
	b setSoundBlend("a_null", "a", 0.5);
	level.t_blend = 1;
}

blendplay()
{
	b = spawn("sound_blend", (10, 20, 30));
	level.t_bent = b;
	b setSoundBlend("a_null", "a", 0.25);
}

blendstep()
{
	level.t_bent setSoundBlend("a_null", "a", 0.75);
}

blendkill()
{
	level.t_bent delete();
}

blendtype()
{
	o = spawn("script_origin", (0, 0, 0));
	o setSoundBlend("a_null", "a", 0.5);
}

blendrange()
{
	b = spawn("sound_blend", (0, 0, 0));
	b setSoundBlend("a_null", "a", 1.5);
}

spread()
{
	level.t_s0 = bulletspread((0, 0, 0), (1, 0, 0), 0);
	level.t_s1 = bulletspread((0, 0, 0), (1, 0, 0), 10);
}

kick()
{
	kicked = level.player; kicked viewkick(127, kicked.origin + (0, 100, 0));
}

noeye()
{
	o = spawn("script_origin", (0, 0, 0));
	o geteye();
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
    // 0x4eee60: geteye on an entity without a sentient is a script error naming its classname (string 0x5871b0).
    {
        const size_t before = world.vm().errors().size();
        world.vm().start("maps/iw2test_aisight.gsc", "noeye", {}, iw2::script::Value(world.vm().level()));
        CHECK(world.vm().errors().size() == before + 1);
        if (world.vm().errors().size() > before)
            CHECK(world.vm().errors().back().find("getEye must be called on an AI or player, not on a 'script_origin'") != std::string::npos);
    }
    // bulletspread (0x4ee760): no spread is the point 8192 units ahead, a spread stays inside the disc of radius tan(deg) * 8192.
    {
        world.vm().start("maps/iw2test_aisight.gsc", "spread", {}, iw2::script::Value(world.vm().level()));
        const auto s0 = world.vm().level()->get("t_s0"), s1 = world.vm().level()->get("t_s1");
        CHECK(s0.isVector() && s1.isVector());
        if (s0.isVector() && s1.isVector()) {
            CHECK(s0.asVector()[0] == 8192.0f && s0.asVector()[1] == 0.0f && s0.asVector()[2] == 0.0f);
            const auto v = s1.asVector();
            CHECK(v[0] == 8192.0f && std::hypot(v[1], v[2]) <= std::tan(10.0f * 0.0174532925f) * 8192.0f + 0.01f);
            CHECK(v[1] != 0.0f || v[2] != 0.0f);
        }
    }
    // viewkick (0x4f7a40) fills the damage fields G_Damage fills; the next damage feedback (0x4e7040) reports the capped count and the direction
    // from the player to the origin's opposite side (player origin - origin = (0, -100, 0): yaw 270, pitch 0).
    if (world.player()) {
        world.vm().start("maps/iw2test_aisight.gsc", "kick", {}, iw2::script::Value(world.vm().level()));
        world.frame();
        const auto& fb = world.playerDamageFeedback();
        const int mh = world.player()->obj->get("maxhealth").asInt();
        CHECK(fb.count == std::min(127, ((127 * mh + 50) / 100) * 100 / mh)); // damage 127 percent of the max health, capped
        CHECK(fb.yaw == int(270.0f * 0.7111111283302307f) && fb.pitch == 0);
    }
    // setsoundblend (0x4f7d10): the type check precedes the lerp range check.
    {
        const size_t before = world.vm().errors().size();
        world.vm().start("maps/iw2test_aisight.gsc", "blendok", {}, iw2::script::Value(world.vm().level()));
        CHECK(world.vm().errors().size() == before && world.vm().level()->get("t_blend").truthy());
        world.vm().start("maps/iw2test_aisight.gsc", "blendtype", {}, iw2::script::Value(world.vm().level()));
        world.vm().start("maps/iw2test_aisight.gsc", "blendrange", {}, iw2::script::Value(world.vm().level()));
        CHECK(world.vm().errors().size() == before + 2);
        if (world.vm().errors().size() == before + 2) {
            CHECK(world.vm().errors()[before].find("Entity is not a sound_blend") != std::string::npos);
            CHECK(world.vm().errors()[before + 1].find("Lerp must be between") != std::string::npos);
        }
    }
    // ET_SOUND_BLEND client side (0x4ac8a0): nothing plays before both aliases are set; then the pair, fraction and origin go to the generic play once
    // and later frames update the same voice; a voice the mixer dropped (culled) is started again; deleting the entity stops the loop.
    {
        struct Rec { int plays = 0, sets = 0, stops = 0; float f = -1; std::string lo, hi; iw2::game::Vec3 at{}; int owner = -2; bool alive = true; } rec;
        world.playBlendSound = [&](const std::string& lo, const std::string& hi, float f, const iw2::game::Vec3& o, int owner) {
            if (o[0] != 10) return 0; // the blend entities of the earlier checks (origin 0) are not of interest
            ++rec.plays; rec.lo = lo; rec.hi = hi; rec.f = f; rec.at = o; rec.owner = owner; rec.alive = true;
            return 77;
        };
        world.setBlendSound = [&](int h, float f, const iw2::game::Vec3& o) { if (h != 77) return false; ++rec.sets; rec.f = f; rec.at = o; return h == 77 && rec.alive; };
        world.stopSound = [&](int h) { if (h == 77) ++rec.stops; };
        world.vm().start("maps/iw2test_aisight.gsc", "blendplay", {}, iw2::script::Value(world.vm().level()));
        world.frame();
        CHECK(rec.plays == 1 && rec.lo == "a_null" && rec.hi == "a" && rec.f == 0.25f && rec.at[0] == 10 && rec.at[2] == 30 && rec.owner >= 0);
        world.vm().start("maps/iw2test_aisight.gsc", "blendstep", {}, iw2::script::Value(world.vm().level()));
        world.frame();
        CHECK(rec.plays == 1 && rec.sets >= 1 && rec.f == 0.75f);
        rec.alive = false; // the mixer culled the voice
        world.frame();
        CHECK(rec.plays == 2);
        world.vm().start("maps/iw2test_aisight.gsc", "blendkill", {}, iw2::script::Value(world.vm().level()));
        CHECK(rec.stops == 1);
        const int plays = rec.plays;
        world.frame();
        CHECK(rec.plays == plays);
        world.playBlendSound = [](const std::string&, const std::string&, float, const iw2::game::Vec3&, int) { return 0; };
        world.setBlendSound = [](int, float, const iw2::game::Vec3&) { return false; };
        world.stopSound = [](int) {};
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
    // 0x52624c..0x526272: a rocket impact tells the enemy actors around the parent's line to the impact an AI event of type 0x11
    // (not forced: the parent's team must have enemies), which marks the parent as their attacker.
    if (actor && world.player()) {
        iw2::game::Entity* player = world.player();
        actor->obj->set("team", iw2::script::Value(std::string("axis")));
        const auto po = player->obj->get("origin").asVector();
        const auto ao = actor->obj->get("origin").asVector();
        actor->sentientInfo.erase(player->obj.get());
        player->obj->set("$weapon", iw2::script::Value(std::string("panzerschreck")));
        // Aimed just under the actor's feet, so the rocket hits the ground at the actor and the segment to the impact passes it.
        iw2::game::Vec3 dir{ao[0] - po[0], ao[1] - po[1], ao[2] - 30.0f - (po[2] + 60.0f)};
        const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        for (float& c : dir) c /= dl;
        world.playerFire({po[0], po[1], po[2] + 60}, dir);
        CHECK(world.stats.rockets >= 1);
        for (int i = 0; i < 40; ++i) world.frame();
        const auto it = actor->sentientInfo.find(player->obj.get());
        CHECK(it != actor->sentientInfo.end() && it->second.lastAttackedMs > 0);
    }
    fsys::remove_all(dir);
    if (failures) return 1;
    std::cout << "aisight tests passed\n";
    return 0;
}
