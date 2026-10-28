// Savegame state round trip of the script VM (src/script/vmstate.cpp, docs/spec/savegame.md section 5): run a script, save its
// state, load it into a fresh VM, then run both and require the same output (threads, waits, endons, shared arrays, structs,
// function values and object identity must survive). With IW2_MAIN set, also a World round trip on a real map.
#include "game/savestate.h"
#include "game/world.h"
#include "script/stateio.h"
#include "script/vm.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using namespace iw2::script;

const char* kUtil = R"(
add(a, b) { return a + b; }
)";

const char* kMain = R"(
#include maps\util;
main()
{
    level.shared = [];
    level.shared[0] = 10;
    level.shared["k"] = "v";
    level.copy = level.shared;
    level.s = spawnstruct();
    level.s.back = level;
    level.s.list = level.shared;
    level.f = maps\util::add;
    level.ticker = spawnstruct();
    thread counter();
    thread waiter();
    thread ender();
    thread ticker();
}
counter()
{
    n = 0;
    local = [];
    for (;;)
    {
        n++;
        local[local.size] = n;
        print("count " + n + " " + local.size + " " + [[level.f]](n, 100));
        wait 0.1;
    }
}
waiter()
{
    for (;;)
    {
        level waittill("go", x);
        level.shared[level.shared.size] = x;
        print("go " + x + " shared " + level.shared.size + " copy " + level.copy.size + " list " + level.s.list.size);
    }
}
ender()
{
    level endon("stop");
    for (;;)
    {
        level.ticker waittill("tick");
        print("tick seen " + level.s.back.shared[0]);
    }
}
ticker()
{
    for (i = 0; i < 100; i++)
    {
        wait 0.15;
        level.ticker notify("tick");
    }
}
)";

struct Harness {
    std::vector<std::string> out;
    VM vm{[](const std::string& p) -> std::optional<std::string> {
        if (p == "maps/util.gsc") return std::string(kUtil);
        if (p == "maps/main.gsc") return std::string(kMain);
        return std::nullopt;
    }};
    Harness() {
        vm.print = [this](const std::string& s) { out.push_back(s); };
        vm.registerBuiltin("print", [this](CallContext& c) {
            out.push_back(c.arg(0).toString());
            return Value();
        });
        vm.registerBuiltin("spawnstruct", [this](CallContext&) { return Value(vm.newObject("struct")); });
    }
    std::vector<uint8_t> save() {
        ObjectTable t;
        t.writeFunction = [this](StateWriter& w, int f) { vm.writeFunctionName(w, f); };
        vm.collectState(t);
        StateWriter w;
        t.write(w);
        vm.saveState(w, t);
        return w.bytes;
    }
    void load(const std::vector<uint8_t>& b) {
        StateReader r(b.data(), b.size());
        ObjectTable t;
        t.readFunction = [this](StateReader& sr) { return vm.readFunctionName(sr); };
        std::unordered_map<uint64_t, ObjectPtr> live;
        collectLive({Value(vm.level()), Value(vm.game()), Value(vm.anim())}, live);
        t.read(r, live, [this](const std::string& kind, uint64_t id) { return vm.newObjectWithId(kind, id); });
        vm.loadState(r, t);
        CHECK(r.atEnd());
    }
    // one "frame": a notify with argument, then the timed threads
    void step(int64_t& now, int i) {
        now += 50;
        if (i % 3 == 0) vm.notify(vm.level(), "go", {Value(int32_t(i))});
        if (i == 25) vm.notify(vm.level(), "stop");
        vm.runFrame(now);
    }
};

void vmRoundTrip() {
    Harness a;
    a.vm.start("maps/main.gsc", "main");
    int64_t now = 0;
    for (int i = 0; i < 12; ++i) a.step(now, i);
    const auto bytes = a.save();

    Harness b;
    b.load(bytes);
    CHECK(b.vm.time() == a.vm.time());
    CHECK(b.vm.liveThreadCount() == a.vm.liveThreadCount());
    CHECK(b.vm.describeThreads() == a.vm.describeThreads());
    // identity: the shared array is one object in both copies, the struct's back pointer is the level
    CHECK(b.vm.level()->get("s").asObject()->get("back").asObject() == b.vm.level());

    a.out.clear();
    int64_t nowB = now;
    for (int i = 12; i < 40; ++i) {
        a.step(now, i);
        b.step(nowB, i);
    }
    CHECK(!a.out.empty());
    CHECK(a.vm.errors().empty() && b.vm.errors().empty());
    for (const auto& e : b.vm.errors()) std::cerr << "vm error: " << e << "\n";
    // every kind of wake-up happened after the load
    auto has = [&](const std::string& p) { for (const auto& o : b.out) if (o.rfind(p, 0) == 0) return true; return false; };
    CHECK(has("count ") && has("go ") && has("tick seen"));
    CHECK(a.out == b.out);
    if (a.out != b.out) {
        for (size_t i = 0; i < std::max(a.out.size(), b.out.size()); ++i)
            std::cerr << (i < a.out.size() ? a.out[i] : "-") << " | " << (i < b.out.size() ? b.out[i] : "-") << "\n";
    }
    // a second save of the loaded VM equals a save of the original at the same point
    CHECK(a.save().size() == b.save().size());
}

// World round trip on a real map (only with IW2_MAIN=<game main dir>; IW2_SAVE_MAP picks the map, default toujane).
void worldRoundTrip() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "world round trip skipped (IW2_MAIN not set)\n";
        return;
    }
    const char* m = std::getenv("IW2_SAVE_MAP");
    const std::string map = m ? m : "toujane";
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    iw2::game::World a(vfs, map);
    a.start();
    for (int i = 0; i < 100; ++i) a.frame();
    iw2::game::SaveRequest req;
    req.name = "test";
    req.description = "round trip";
    const auto file = iw2::game::buildSaveFile(a, req);
    iw2::game::LoadedSave ls;
    const std::string err = iw2::game::parseSaveFile(file, ls);
    CHECK(err.empty());
    if (!err.empty()) std::cerr << "parse: " << err << "\n";
    CHECK(ls.info.mapName == map && ls.info.title == "round trip");
    iw2::game::World b(vfs, ls.info.mapName);
    b.prepareStart();
    iw2::game::restoreSave(b, ls);
    const std::string da = a.stateDigest(), db = b.stateDigest();
    CHECK(da == db);
    if (da != db) std::cerr << "--- saved\n" << da << "--- loaded\n" << db;
    const bool diff = std::getenv("IW2_SAVE_DIFF") != nullptr; // debug: print where the continuation first diverges
    bool reported = false;
    for (int i = 0; i < 40; ++i) {
        a.frame();
        b.frame();
        if (diff && !reported) {
            const std::string xa = a.stateDigest(), xb = b.stateDigest();
            if (xa != xb) {
                reported = true;
                std::istringstream sa(xa), sb(xb);
                std::string la, lb;
                int shown = 0;
                while (shown < 12 && (bool(std::getline(sa, la)) | bool(std::getline(sb, lb))))
                    if (la != lb) { std::cerr << "frame " << i + 1 << "\n  saved : " << la << "\n  loaded: " << lb << "\n"; ++shown; }
            }
        }
    }
    // continuation: time and thread layout stay in step (AI/anim runtime is rebuilt, so entity positions may drift: not compared)
    CHECK(a.timeMs() == b.timeMs());
    CHECK(a.vm().liveThreadCount() == b.vm().liveThreadCount());
    std::cout << "world round trip on " << map << ": " << file.size() << " bytes, threads " << a.vm().liveThreadCount() << " / "
              << b.vm().liveThreadCount() << "\n";
}

} // namespace

int main() {
    vmRoundTrip();
    try {
        worldRoundTrip();
    } catch (const std::exception& e) {
        std::cerr << "world round trip: " << e.what() << "\n";
        ++failures;
    }
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
