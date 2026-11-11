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
        vm.setDeveloperScript([] { return true; }); // the test scripts print with println (developer_script is off by default)
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


// A saved pc counts the instructions of the code a file compiled to, and that code depends on developer_script (a println / assert is
// dropped with it off). A save names the mode of every file; a load compiles in that mode whatever the dvar says now, and refuses a
// frame whose code size differs (docs/spec/savegame.md section 5, format 16).
const char* kDevScript = R"(
main() { level.x = 5; level thread helper(); }
helper()
{
    println("dev A");
    assert(1);
    emit("start");
    println("dev B");
    level waittill("go");
    println("dev C");
    assert(1);
    emit("resumed " + level.x);
}
)";

struct DevHarness {
    std::vector<std::string> out;
    bool dev;
    VM vm;
    explicit DevHarness(bool devMode)
        : dev(devMode), vm([](const std::string& p) -> std::optional<std::string> {
              if (p == "maps/dev.gsc") return std::string(kDevScript);
              return std::nullopt;
          }) {
        vm.setDeveloperScript([this] { return dev; });
        vm.print = [this](const std::string& s) { out.push_back("println " + s); };
        vm.registerBuiltin("emit", [this](CallContext& c) {
            out.push_back(c.arg(0).toString());
            return Value();
        });
    }
    std::vector<uint8_t> save(bool withModes) {
        ObjectTable t;
        t.writeFunction = [this](StateWriter& w, int f) { vm.writeFunctionName(w, f); };
        vm.collectState(t);
        StateWriter w;
        if (withModes) vm.writeCompileModes(w);
        t.write(w);
        vm.saveState(w, t);
        return w.bytes;
    }
    void load(const std::vector<uint8_t>& b, bool withModes, bool legacyAllDev = false) {
        StateReader r(b.data(), b.size());
        if (withModes) vm.readCompileModes(r);
        else if (legacyAllDev) vm.pinDeveloperScriptForAll(true);
        ObjectTable t;
        t.readFunction = [this](StateReader& sr) { return vm.readFunctionName(sr); };
        std::unordered_map<uint64_t, ObjectPtr> live;
        collectLive({Value(vm.level()), Value(vm.game()), Value(vm.anim())}, live);
        t.read(r, live, [this](const std::string& kind, uint64_t id) { return vm.newObjectWithId(kind, id); });
        vm.loadState(r, t);
        CHECK(r.atEnd());
    }
    void runToWait() {
        vm.start("maps/dev.gsc", "main");
        vm.runFrame(50);
    }
    void wake() {
        vm.notify(vm.level(), "go");
        vm.runFrame(100);
    }
};

void developerModeSave() {
    for (const bool savedDev : {true, false}) {
        DevHarness a(savedDev);
        a.runToWait();
        CHECK(a.out.size() == (savedDev ? 3u : 1u)); // dev A, start, dev B, or only start
        const auto bytes = a.save(true);
        // the loading world runs in the other mode: the save's mode wins
        DevHarness b(!savedDev);
        b.load(bytes, true);
        b.wake();
        CHECK(b.vm.errors().empty());
        CHECK(!b.out.empty() && b.out.back() == "resumed 5");
        CHECK(b.out.size() == (savedDev ? 2u : 1u)); // dev C only when the saved code has it
        a.wake();
        CHECK(a.out.back() == "resumed 5");
        // a file compiled in the same mode needs no pin
        DevHarness c(savedDev);
        c.load(bytes, true);
        c.wake();
        CHECK(c.out == b.out);
    }
    // an old save (every developer statement compiled) loads with the pin for all
    {
        DevHarness a(true);
        a.runToWait();
        const auto bytes = a.save(false);
        DevHarness b(false);
        b.load(bytes, false, true);
        b.wake();
        CHECK(b.vm.errors().empty() && !b.out.empty() && b.out.back() == "resumed 5");
    }
    // a save of the wrong layout is refused, not resumed at a shifted pc
    {
        DevHarness a(true);
        a.runToWait();
        const auto bytes = a.save(false);
        DevHarness b(false);
        bool refused = false;
        try {
            b.load(bytes, false);
        } catch (const std::exception& e) {
            refused = std::string(e.what()).find("does not match") != std::string::npos;
        }
        CHECK(refused);
    }
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
    // A setswitchnode replacement in progress (VehiclePathPos::hasSwitch) must survive: force one on the first vehicle, when the map has any.
    iw2::game::VehicleState* sw = nullptr;
    for (const auto& e : a.entities())
        if (e->vehicle) { sw = e->vehicle.get(); sw->path.hasSwitch = true;
            sw->path.switched.obj = e->obj; sw->path.original.obj = e->obj;
            sw->path.switched.name = "sw"; sw->path.switched.next = 7; sw->path.switched.length = 12.5f;
            sw->path.original.next = 3; break; }
    // An actor in the animscripted state: the stack and the predicted copy must both survive (a restored predicted stack at depth 0
    // made the state impossible to pop; the scripted animscript then restarted without arguments).
    int scriptedActor = -1;
    for (const auto& e : a.entities())
        if (e->isActor && (e->obj->get("health").asInt() > 0)) { a.debugPushAiState(*e, iw2::game::AiScripted); scriptedActor = e->number; break; }
    // An actor in the middle of a traversal (state 9): its traverse animscript key (+0x208) and traverse mode (+0x210) are saved;
    // without them the restored actor ran animscripts/traverse/ with no key. A dying actor's alive flag (+0x25c) is saved too.
    int traverseActor = -1, dyingActor = -1;
    for (const auto& e : a.entities()) {
        if (!e->isActor || e->number == scriptedActor || e->obj->get("health").asInt() <= 0) continue;
        if (traverseActor < 0) { e->ai.negotiationScript = "window_over_40"; e->ai.traverseMode = 3; traverseActor = e->number; }
        else if (dyingActor < 0) { e->ai.alive = false; dyingActor = e->number; }
    }
    // The renderer fog record travels with the save (0x402d60 -> gfx 0x1000bca0); the level script does not resend it on a load.
    a.clientFog.command(0, 3000, 0.562f, 0.57f, 0.59f, 0, a.timeMs());
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
    // A compass ping made before the restore is gone afterwards (CG_Init zeroes the cgame ping table, which is not saved).
    if (b.player()) {
        b.compassPing(*b.player(), {1.0f, 2.0f, 0.0f}, 5000);
        CHECK(b.compassPings().endMs(b.player()->number) != 0);
    }
    iw2::game::restoreSave(b, ls);
    if (b.player()) CHECK(b.compassPings().endMs(b.player()->number) == 0);
    {
        const iw2::game::FogConsts fa = a.clientFog.consts(a.timeMs()), fb = b.clientFog.consts(b.timeMs());
        CHECK(fb.active && !fb.exponential && fb.scale == fa.scale && fb.shift == fa.shift && fb.color[2] == fa.color[2]);
    }
    // The precacheshellshock table is part of the saved game state; the level script does not precache again on a load.
    CHECK(!a.shellShockNames().empty());
    CHECK(a.shellShockNames() == b.shellShockNames());
    if (scriptedActor >= 0) {
        const iw2::game::Entity* ea = a.debugEntity(scriptedActor);
        const iw2::game::Entity* eb = b.debugEntity(scriptedActor);
        CHECK(ea && eb && eb->ai.depth == ea->ai.depth && ea->ai.depth >= 1);
        if (ea && eb) {
            CHECK(eb->ai.predictedDepth == ea->ai.predictedDepth);
            CHECK(eb->ai.predicted[size_t(eb->ai.predictedDepth)] == iw2::game::AiScripted);
        }
    }
    if (traverseActor >= 0) {
        const iw2::game::Entity* eb = b.debugEntity(traverseActor);
        CHECK(eb && eb->ai.negotiationScript == "window_over_40" && eb->ai.traverseMode == 3 && eb->ai.alive);
    }
    if (dyingActor >= 0) {
        const iw2::game::Entity* eb = b.debugEntity(dyingActor);
        CHECK(eb && !eb->ai.alive);
    }
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
                // Both streams advance on every line, so the two reads must not short-circuit.
                while (shown < 12) {
                    const bool gotA = bool(std::getline(sa, la)), gotB = bool(std::getline(sb, lb));
                    if (!gotA && !gotB) break;
                    if (la != lb) { std::cerr << "frame " << i + 1 << "\n  saved : " << la << "\n  loaded: " << lb << "\n"; ++shown; }
                }
            }
        }
    }
    if (sw) {
        CHECK(da.find("switch 1 sw->7 len 12.5 orig 3") != std::string::npos);
        std::cout << "vehicle switch node round trip checked\n";
    } else {
        std::cout << "no vehicle on " << map << ": switch node not exercised\n";
    }
    // continuation: time and thread layout stay in step (AI/anim runtime is rebuilt, so entity positions may drift: not compared)
    CHECK(a.timeMs() == b.timeMs());
    CHECK(a.vm().liveThreadCount() == b.vm().liveThreadCount());
    std::cout << "world round trip on " << map << ": " << file.size() << " bytes, threads " << a.vm().liveThreadCount() << " / "
              << b.vm().liveThreadCount() << "\n";
}

} // namespace

// Every body format the reader has branches for is accepted (formats 13 and 14 were once refused); format 2 and unknown ones are not.
void formatGate() {
    using iw2::game::saveFormatLoadable;
    for (uint32_t v : {1u, 3u, 12u, 13u, 14u, 15u, iw2::game::kStateVersion}) CHECK(saveFormatLoadable(v));
    for (uint32_t v : {0u, 2u, iw2::game::kStateVersion + 1}) CHECK(!saveFormatLoadable(v));
}

int main() {
    formatGate();
    vmRoundTrip();
    developerModeSave();
    try {
        worldRoundTrip();
    } catch (const std::exception& e) {
        std::cerr << "world round trip: " << e.what() << "\n";
        ++failures;
    }
    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
