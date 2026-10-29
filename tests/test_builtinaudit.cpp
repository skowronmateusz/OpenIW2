// Script built-ins that were no-op stubs: attackbuttonpressed / usebuttonpressed / getplayerangles /
// getnormalizedmovement (player input queries, exe 0x538600..0x538820), getammocount (0x4f4540), setspawnerteam
// (0x4f8750) and playlooprumble (0x4f8c00). Needs the game files (IW2_MAIN).

#include <unistd.h>
#include <string>
#include "fs/vfs.h"
#include "game/clientinput.h"
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
        std::cout << "IW2_MAIN not set, skipped\n";
        return 0;
    }
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("iw2_builtinaudit_" + std::to_string(::getpid()));
    fs::create_directories(dir / "maps");
    {
        std::ofstream f(dir / "maps" / "zz_builtinaudit.gsc");
        f << "run()\n{\n"
             "\tlevel.attack = self attackbuttonpressed();\n"
             "\tlevel.use = self usebuttonpressed();\n"
             "\tlevel.angles = self getplayerangles();\n"
             "\tlevel.move = self getnormalizedmovement();\n"
             "\tlevel.ammonone = self getammocount(\"none\");\n"
             "\tself playlooprumble(\"x\");\n"
             "\tlevel.bp_w = self buttonpressed(\"w\");\n"
             "\tlevel.bp_f = self buttonpressed(\"F\");\n"
             "\tlevel.bp_pad = self buttonpressed(\"BUTTON_A\");\n"
             "\tlevel.cmd_w = getcommandfromkey(\"W\");\n"
             "\tlevel.cmd_mouse = getcommandfromkey(\"mouse1\");\n"
             "\tlevel.cmd_pad = getcommandfromkey(\"BUTTON_RSTICK\");\n"
             "\tlevel.done = 1;\n}\n"
             "save()\n{\n\tsavegame(\"zz\", \"d\", \"$default\");\n}\n"
             "nocommit()\n{\n\tlevel.sid = savegamenocommit(\"zy\", \"d\", \"$default\");\n}\n"
             "hit()\n{\n\tself dodamage(500, (0, 0, 0));\n\tlevel.hit = 1;\n}\n"
             "commit()\n{\n\tcommitsave(0);\n\tlevel.committed = 1;\n}\n";
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir.string());
    iw2::game::World w(vfs, "moscow");
    for (const char* n : {"attackbuttonpressed", "usebuttonpressed", "getplayerangles", "getnormalizedmovement", "getammocount",
                          "setspawnerteam", "playlooprumble", "buttonpressed", "getcommandfromkey"})
        CHECK(!w.stubbedBuiltins().count(n));
    iw2::game::ClientInput keys; // buttonpressed / getcommandfromkey read the client key table
    keys.loadConfig("unbindall\nbind w \"+forward\"\nbind MOUSE1 \"+attack\"\n");
    keys.key("w", true);
    keys.key("f", true); // an unbound key still has a down flag
    w.clientInput = &keys;
    w.prepareStart();
    CHECK(w.player() != nullptr);
    if (!w.player()) return 1;
    iw2::game::World::PlayerCommand cmd;
    cmd.forward = 127;
    cmd.right = -127;
    cmd.buttons = 0x20; // +usereload
    cmd.viewAngles = {10, 90, 0};
    w.setPlayerCommand(cmd);
    w.vm().start("maps/zz_builtinaudit.gsc", "run", {}, iw2::script::Value(w.player()->obj));
    w.vm().drainReady();
    for (const auto& e : w.vm().errors()) std::cerr << "script error: " << e << "\n";
    auto* lv = w.vm().level().get();
    CHECK(lv->get("done").isInt());
    CHECK(lv->get("attack").asInt() == 0);
    CHECK(lv->get("use").asInt() == 1);
    const auto a = lv->get("angles").asVector();
    CHECK(a[0] == 10 && a[1] == 90 && a[2] == 0);
    const auto m = lv->get("move").asVector();
    CHECK(std::fabs(m[0] - 1.0f) < 1e-5f && std::fabs(m[1] + 1.0f) < 1e-5f && m[2] == 0);
    CHECK(lv->get("ammonone").asInt() == 0);
    // buttonpressed (0x538980): the key's down flag; a name that is no key (console BUTTON_*) is 0. getcommandfromkey (0x4ece00).
    CHECK(lv->get("bp_w").asInt() == 1 && lv->get("bp_f").asInt() == 1 && lv->get("bp_pad").asInt() == 0);
    CHECK(lv->get("cmd_w").toString() == "+forward" && lv->get("cmd_mouse").toString() == "+attack");
    CHECK(lv->get("cmd_pad").toString().empty());
    // The previous command's buttons count as well (the exe ORs client+0x8640 with +0x8634).
    cmd.buttons = 1;
    w.setPlayerCommand(cmd);
    w.vm().start("maps/zz_builtinaudit.gsc", "run", {}, iw2::script::Value(w.player()->obj));
    w.vm().drainReady();
    CHECK(lv->get("attack").asInt() == 1);
    CHECK(lv->get("use").asInt() == 1);
    // tankhunt flags its grenade trigger with enablegrenadetouchdamage; the class check must accept it (no script error).
    {
        iw2::game::World t(vfs, "tankhunt");
        t.start();
        for (int i = 0; i < 40; ++i) t.frame();
        for (const auto& e : t.vm().errors()) CHECK(e.find("supported on damage") == std::string::npos);
        CHECK(!t.stubbedBuiltins().count("enablegrenadetouchdamage") && !t.stubbedBuiltins().count("getgroundenttype"));
    }
    // commitsave (0x4f3db0): after a savegame() write (byte 0x1b15ac1 set by the engine's own commit) the first commitsave returns
    // at once; the byte is consumed, so the next one reaches the checks and fails for lack of a pending save.
    {
        iw2::game::World s(vfs, "moscow");
        s.prepareStart();
        s.vm().start("maps/zz_builtinaudit.gsc", "save", {}, iw2::script::Value(s.player()->obj));
        s.vm().drainReady();
        s.frame();
        CHECK(s.takeSaveFiles().size() == 1);
        s.vm().start("maps/zz_builtinaudit.gsc", "commit", {}, iw2::script::Value(s.player()->obj));
        s.vm().drainReady();
        CHECK(s.vm().errors().empty() && s.vm().level()->get("committed").isInt());
        s.vm().start("maps/zz_builtinaudit.gsc", "commit", {}, iw2::script::Value(s.player()->obj));
        s.vm().drainReady();
        CHECK(!s.vm().errors().empty());
    }
    // A savegame() write while a savegamenocommit save is still uncommitted and the player is alive commits that save first
    // (0x4e3fec..0x4e4003), with the byte set, so a commitsave afterwards returns silently.
    {
        iw2::game::World s(vfs, "moscow");
        s.prepareStart();
        s.vm().start("maps/zz_builtinaudit.gsc", "nocommit", {}, iw2::script::Value(s.player()->obj));
        s.vm().drainReady();
        s.frame();
        CHECK(s.takeSaveFiles().empty());
        s.vm().start("maps/zz_builtinaudit.gsc", "save", {}, iw2::script::Value(s.player()->obj));
        s.vm().drainReady();
        s.frame();
        CHECK(s.takeSaveFiles().size() == 2);
        s.vm().start("maps/zz_builtinaudit.gsc", "commit", {}, iw2::script::Value(s.player()->obj));
        s.vm().drainReady();
        CHECK(s.vm().errors().empty());
    }
    // The cheat commands god / demigod / notarget (0x4e9f90 / 0x4ea030 / 0x4ea0d0): same gate, flags +0x128 & 1 / 2 / 4.
    {
        iw2::game::World c(vfs, "moscow");
        c.prepareStart();
        CHECK(!c.cheatGod() && !c.playerGod());
        auto cmds = c.takeClientCommands();
        CHECK(cmds.size() == 1 && cmds[0].arg == "GAME_CHEATSNOTENABLED");
        c.cvars["sv_cheats"] = "1";
        CHECK(c.cheatGod() && c.playerGod() && c.player()->obj->get("$godmode").truthy());
        CHECK(c.cheatDemigod() && c.playerDemigod() && c.cheatNotarget() && c.playerNotarget());
        cmds = c.takeClientCommands();
        CHECK(cmds.size() == 3 && cmds[0].arg == "GAME_GODMODE_ON" && cmds[1].arg == "GAME_DEMI_GODMODE_ON" && cmds[2].arg == "GAME_NOTARGETON");
        CHECK(c.cheatGod() && !c.playerGod() && !c.player()->obj->get("$godmode").truthy() && c.cheatNotarget() && !c.playerNotarget());
        cmds = c.takeClientCommands();
        CHECK(cmds[0].arg == "GAME_GODMODE_OFF" && cmds[1].arg == "GAME_NOTARGETOFF");
        c.player()->obj->set("health", iw2::script::Value(int32_t(0)));
        CHECK(!c.cheatGod());
    }
    // demigod (flags & 2, 0x5283d2): a lethal hit is cut to health - 1; god (flags & 1, 0x5282b3) takes no damage; notarget is the
    // `$notarget` mark next to ignoreme that the enemy search (0x54e326) and getclosestenemysqdist (0x53c37b) skip.
    {
        iw2::game::World c(vfs, "moscow");
        c.prepareStart();
        c.cvars["sv_cheats"] = "1";
        c.player()->obj->set("health", iw2::script::Value(int32_t(100)));
        CHECK(c.cheatDemigod());
        c.vm().start("maps/zz_builtinaudit.gsc", "hit", {}, iw2::script::Value(c.player()->obj));
        c.vm().drainReady();
        CHECK(c.player()->obj->get("health").asInt() == 1);
        CHECK(c.cheatDemigod() && c.cheatGod());
        c.vm().start("maps/zz_builtinaudit.gsc", "hit", {}, iw2::script::Value(c.player()->obj));
        c.vm().drainReady();
        CHECK(c.player()->obj->get("health").asInt() == 1);
        CHECK(c.cheatNotarget() && c.player()->obj->get("$notarget").truthy());
        CHECK(c.cheatNotarget() && !c.player()->obj->get("$notarget").truthy());
    }
    // The player's node claim (0x4e7910): standing on a free node claims it, leaving releases it; a dontinterpolate on an entity
    // without a client toggles the demigod bit (0x4edfe1); the cooked grenade event is skipped by god / demigod (0x4e776b).
    {
        iw2::game::World c(vfs, "moscow");
        c.prepareStart();
        if (!c.nodes().empty()) {
            int idx = -1;
            for (size_t i = 0; i < c.nodes().size() && idx < 0; ++i)
                if (!c.nodes()[i].owner && c.nodes()[i].ownerUntilMs == 0 && (c.nodes()[i].spawnflags & 1) == 0) idx = int(i);
            CHECK(idx >= 0);
            if (idx >= 0) {
                c.player()->obj->set("origin", c.nodes()[size_t(idx)].obj->get("origin"));
                c.frame();
                CHECK(c.player()->claimedNode >= 0);
                c.player()->obj->set("origin", iw2::script::Value(iw2::game::Vec3{1.0e5f, 1.0e5f, 1.0e5f}));
                c.frame();
                CHECK(c.player()->claimedNode < 0);
            }
        }
        c.cvars["sv_cheats"] = "1";
        c.cheatGod();
        c.playerGrenadeInHand({0, 0, 0});
        CHECK(c.player()->obj->get("health").asInt() > 0);
    }
    // The cheat commands noclip / ufo (0x4ea170 / 0x4ea220 behind the gate 0x4e9500): sv_cheats and health decide.
    {
        iw2::game::World c(vfs, "moscow");
        c.prepareStart();
        CHECK(!c.cheatNoclip() && !c.playerNoclip());
        auto cmds = c.takeClientCommands();
        CHECK(cmds.size() == 1 && cmds[0].name == "print" && cmds[0].arg == "GAME_CHEATSNOTENABLED");
        c.cvars["sv_cheats"] = "1";
        CHECK(c.cheatNoclip() && c.playerNoclip() && c.playerPmType() == 2);
        cmds = c.takeClientCommands();
        CHECK(cmds.size() == 1 && cmds[0].arg == "GAME_NOCLIPON");
        CHECK(c.cheatUfo() && c.playerUfo() && c.playerPmType() == 2); // noclip wins over ufo
        CHECK(c.cheatNoclip() && !c.playerNoclip() && c.playerPmType() == 3);
        CHECK(c.cheatUfo() && c.playerPmType() == 0);
        cmds = c.takeClientCommands();
        CHECK(cmds.back().arg == "GAME_UFOOFF");
        c.player()->obj->set("health", iw2::script::Value(int32_t(0)));
        CHECK(!c.cheatNoclip() && c.playerPmType() == 4);
        cmds = c.takeClientCommands();
        CHECK(cmds.size() == 1 && cmds[0].arg == "GAME_MUSTBEALIVECOMMAND");
    }
    // pm_type 1 / 5 for every link parent (ent+0x1c8), not only playerlinkto: `linkto` and the vehicle seat set it too.
    {
        iw2::game::World c(vfs, "moscow");
        c.prepareStart();
        CHECK(c.playerPmType() == 0 && !c.playerHasLinkParent());
        c.debugLinkPlayer(c.player()->number == 0 ? 1 : 0);
        CHECK(c.playerHasLinkParent() && !c.playerLinked() && c.playerPmType() == 1);
        c.player()->obj->set("health", iw2::script::Value(int32_t(0)));
        CHECK(c.playerPmType() == 5);
        c.debugUnlinkPlayer();
        CHECK(c.playerPmType() == 4);
    }
    fs::remove_all(dir);
    if (failures) std::cerr << failures << " failure(s)\n";
    else std::cout << "builtinaudit ok\n";
    return failures ? 1 : 0;
}
