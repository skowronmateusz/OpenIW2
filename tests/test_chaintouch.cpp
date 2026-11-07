// trigger_friendlychain touch (0x529140 -> 0x518bf0, needs IW2_MAIN): the trigger's target node becomes the toucher's chain node,
// unless the dvar g_spawnai (default 1) is 0.
#include "fs/vfs.h"
#include "game/world.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace iw2::game;

static int failures = 0;
#define CHECK(c)                                                                        \
    do {                                                                                \
        if (!(c)) { std::cout << "FAIL line " << __LINE__ << ": " #c "\n"; ++failures; } \
    } while (0)

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "chaintouch test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    const auto dir = std::filesystem::temp_directory_path() / ("iw2_test_chain_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "maps");
    std::ofstream(dir / "maps/chainpaths.gsc") << "main()\n{\n\tself connectpaths();\n}\nmain2()\n{\n\tself disconnectpaths();\n}\n";
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir.string());
    World w(vfs, "trainyard");
    w.start();
    w.spawnAllActors();
    for (int i = 0; i < 20; ++i) w.frame();
    Entity* trigger = nullptr;
    Entity* actor = nullptr;
    for (const auto& e : w.entities()) {
        if (!trigger && e->classname == "trigger_friendlychain" && !e->obj->removed) trigger = e.get();
        if (!actor && e->isActor && !e->obj->removed && w.debugIsSentient(*e)) actor = e.get();
    }
    CHECK(trigger && actor);
    if (!trigger || !actor) return 1;
    const int before = w.debugChainNode(*actor);
    w.cvars["g_spawnai"] = "0";
    w.debugTouchFriendlyChain(*trigger, *actor);
    CHECK(w.debugChainNode(*actor) == before); // the dvar gates the whole routine
    w.cvars.erase("g_spawnai");
    w.debugTouchFriendlyChain(*trigger, *actor);
    CHECK(w.debugChainNode(*actor) >= 0); // default 1: the node named by the trigger's target
    // connectpaths / disconnectpaths (0x4eff40 / 0x4efe80) need the DYNAMICPATH flag, which an actor does not have.
    const size_t errs = w.vm().errors().size();
    w.vm().start("maps/chainpaths", "main", {}, iw2::script::Value(actor->obj));
    w.vm().start("maps/chainpaths", "main2", {}, iw2::script::Value(actor->obj));
    CHECK(w.vm().errors().size() == errs + 2);
    if (w.vm().errors().size() >= errs + 2) {
        CHECK(w.vm().errors()[errs].find("entity of type '") != std::string::npos && w.vm().errors()[errs].find("cannot connect paths") != std::string::npos);
        CHECK(w.vm().errors()[errs + 1].find("cannot disconnect paths") != std::string::npos);
    }
    std::filesystem::remove_all(dir);
    if (failures) return 1;
    std::cout << "chaintouch test passed\n";
    return 0;
}
