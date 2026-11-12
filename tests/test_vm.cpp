// Tests for the GSC virtual machine using small scripts.

#include "script/vm.h"

#include <iostream>
#include <map>
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

struct Harness {
    std::map<std::string, std::string> files;
    std::vector<std::string> out;
    iw2::script::VM vm{[this](const std::string& p) -> std::optional<std::string> {
        auto it = files.find(p);
        if (it == files.end()) return std::nullopt;
        return it->second;
    }};
    bool developerScript = true; // the other cases print with println, so they compile with developer_script on
    Harness() {
        vm.print = [this](const std::string& s) { out.push_back(s); };
        vm.setDeveloperScript([this] { return developerScript; });
    }
    std::string joined() const {
        std::string s;
        for (const auto& o : out) s += o + "|";
        return s;
    }
};

} // namespace

int main() {
    // Arithmetic, strings, arrays, structs, loops, switch, function pointers.
    {
        Harness h;
        h.files["test/basic.gsc"] = R"(
main()
{
    a = 5;
    b = a * 2 + 1;
    println("b=" + b);
    println(7 / 2);
    println(7 % 3);
    arr = [];
    for (i = 0; i < 4; i++)
        arr[i] = i * i;
    println(arr.size + " " + arr[3]);
    copy = arr;
    copy[0] = 99;
    println(arr[0] + " " + copy[0]);
    s = spawnstruct();
    s.name = "x";
    s.list["k"] = 3;
    println(s.name + s.list["k"]);
    level.counter = 0;
    while (level.counter < 3)
        level.counter++;
    println(level.counter);
    switch (b)
    {
        case 10: println("ten"); break;
        case 11: println("eleven");
        case 12: println("fall"); break;
        default: println("other");
    }
    f = ::helper;
    println([[f]](2));
    v = (1, 2, 3) + (1, 1, 1);
    println(v[2]);
    if (!isdefined(nothing) && isdefined(a))
        println("defs ok");
}
helper(x)
{
    return x + 40;
}
)";
        h.vm.start("test/basic.gsc", "main");
        CHECK(h.joined() == "b=11|3.5|1|4 9|0 99|x3|3|eleven|fall|42|4|defs ok|");
        if (h.joined() != "b=11|3.5|1|4 9|0 99|x3|3|eleven|fall|42|4|defs ok|") std::cerr << h.joined() << "\n";
        CHECK(h.vm.errors().empty());
    }

    // Frame list order (docs/spec/gsc-vm.md): one stack by filing, frame-end threads at the tail, oldest first.
    {
        CHECK(iw2::script::VM::waitFrames(0.12f) == 2);
        CHECK(iw2::script::VM::waitFrames(0.13f) == 3);
        CHECK(iw2::script::VM::waitFrames(0.001f) == 1);
        Harness h;
        h.files["test/order.gsc"] = R"(
main()
{
    level thread timed("t1");
    level thread timed("t2");
    level thread listener("go", "w1");
    level thread listener("go", "w2");
    level thread listener("xf2", "woke");
    level thread frameend("f1");
    level thread frameend("f2");
    wait 0.05;
    level notify("go");
}
timed(n)
{
    wait 0.05;
    println(n);
}
listener(e, n)
{
    level waittill(e);
    println(n);
}
frameend(n)
{
    wait 0.05;
    waittillframeend;
    println(n);
    level notify("x" + n);
}
)";
        h.vm.start("test/order.gsc", "main");
        h.vm.runFrame(50);
        // main is the newest filing, so what it wakes runs before the older timed threads; f2 reached the tail before f1, and
        // the thread f2 wakes runs before f1.
        CHECK(h.joined() == "w2|w1|t2|t1|f2|woke|f1|");
        if (h.joined() != "w2|w1|t2|t1|f2|woke|f1|") std::cerr << h.joined() << "\n";
    }

    // Threads, wait, notify/waittill with arguments, endon.
    {
        Harness h;
        h.files["test/threads.gsc"] = R"(
main()
{
    level thread waiter();
    level thread ender();
    println("main");
    wait 0.1;
    level notify("go", 7, "x");
    println("after notify");
    wait 0.1;
    level notify("stop");
    level notify("tick");
}
waiter()
{
    level waittill("go", n, s);
    println("got " + n + s);
}
ender()
{
    level endon("stop");
    for (;;)
    {
        level waittill("tick");
        println("tick");
    }
}
)";
        h.vm.start("test/threads.gsc", "main");
        CHECK(h.joined() == "main|");
        h.vm.runFrame(50);
        CHECK(h.joined() == "main|");
        h.vm.runFrame(100);
        CHECK(h.joined() == "main|after notify|got 7x|");
        h.vm.runFrame(200);
        CHECK(h.joined() == "main|after notify|got 7x|"); // ender was killed by endon before "tick"
        CHECK(h.vm.liveThreadCount() == 0);
        if (h.joined() != "main|after notify|got 7x|") std::cerr << h.joined() << "\n";
    }

    // An endon registered by a called function ends that call only; the caller continues (the shipped
    // animscripts/shared.gsc DoNoteTracksForTime relies on it), and a returned call's endons no longer apply.
    {
        Harness h;
        h.files["test/endon.gsc"] = R"(
main()
{
    ent = spawnstruct();
    level.ent = ent;
    for (i = 0; i < 2; i++)
    {
        timed(ent);
        println("back " + i);
    }
    level waittill("later");
    println("later");
}
timed(ent)
{
    ent endon("stop");
    level waittill("never");
}
)";
        h.vm.start("test/endon.gsc", "main");
        CHECK(h.joined() == "");
        h.vm.notify(h.vm.level()->get("ent").asObject(), "stop");
        h.vm.drainReady();
        CHECK(h.joined() == "back 0|");
        h.vm.notify(h.vm.level()->get("ent").asObject(), "stop");
        h.vm.drainReady();
        CHECK(h.joined() == "back 0|back 1|");
        h.vm.notify(h.vm.level()->get("ent").asObject(), "stop"); // endon of the returned call is gone
        h.vm.notify(h.vm.level(), "later");
        CHECK(h.joined() == "back 0|back 1|"); // a notify only queues the woken thread (exe 0x46ca60)
        h.vm.drainReady();
        CHECK(h.joined() == "back 0|back 1|later|");
        if (h.joined() != "back 0|back 1|later|") std::cerr << h.joined() << "\n";
    }

    // notifyRun: the woken waiter runs inside the call, ahead of an older queued thread, and the queue order is kept.
    {
        Harness h;
        h.files["test/notifyrun.gsc"] = R"(
main()
{
    level.ent = spawnstruct();
    thread early();
    thread late();
}
early()
{
    level waittill("first");
    println("early");
}
late()
{
    level.ent waittill("kill");
    println("late");
}
)";
        h.vm.start("test/notifyrun.gsc", "main");
        h.vm.notify(h.vm.level(), "first");
        CHECK(h.joined() == "");
        h.vm.notifyRun(h.vm.level()->get("ent").asObject(), "kill");
        CHECK(h.joined() == "late|"); // ran at once, before the thread an earlier notify queued
        h.vm.drainReady();
        CHECK(h.joined() == "late|early|");
        if (h.joined() != "late|early|") std::cerr << h.joined() << "\n";
    }

    // A caller whose called function was ended by endon goes on from the frame list, not inside the notify (0x46c130).
    {
        Harness h;
        h.files["test/endonfile.gsc"] = R"(
main()
{
    level.ent = spawnstruct();
    inner(level.ent);
    println("caller");
}
inner(ent)
{
    ent endon("stop");
    ent waittill("never");
}
)";
        h.vm.start("test/endonfile.gsc", "main");
        h.vm.notify(h.vm.level()->get("ent").asObject(), "stop");
        CHECK(h.joined() == "");
        h.vm.drainReady();
        CHECK(h.joined() == "caller|");
    }

    // A call ended by endon in the middle of an assignment (`w = inner();`) leaves no reference behind: the
    // caller's own pending assignment still targets its variable (crossroads --spawn-all wrote past the locals).
    {
        Harness h;
        h.files["test/endonref.gsc"] = R"(
main()
{
    ent = spawnstruct();
    level.ent = ent;
    a = 1;
    b = 2;
    c = 3;
    x = timed(ent);
    println(a + " " + b + " " + c + " " + isdefined(x));
}
timed(ent)
{
    ent endon("stop");
    p = 0;
    q = 0;
    w = inner();
    return 5;
}
inner()
{
    level waittill("never");
    return 1;
}
)";
        h.vm.start("test/endonref.gsc", "main");
        h.vm.notify(h.vm.level()->get("ent").asObject(), "stop");
        h.vm.drainReady();
        CHECK(h.joined() == "1 2 3 0|");
        if (h.joined() != "1 2 3 0|") std::cerr << h.joined() << "\n";
    }

    // Includes and qualified calls, runtime errors kill only the failing thread.
    {
        Harness h;
        h.files["maps/_util.gsc"] = "twice(x) { return x * 2; }\n";
        h.files["maps/level.gsc"] = "#include maps\\_util;\nmain() { println(twice(4)); println(maps\\_util::twice(5)); thread bad(); println(\"still\"); }\nbad() { x = undefined; y = x.field; }\n";
        h.vm.start("maps/level.gsc", "main");
        CHECK(h.out.size() >= 3 && h.out[0] == "8" && h.out[1] == "10");
        CHECK(h.vm.errors().size() == 1);
        CHECK(!h.out.empty() && h.out.back() == "still");
    }

    // Plain calls inherit self.
    {
        Harness h;
        h.files["test/self.gsc"] = "main() { s = spawnstruct(); s.name = \"guy\"; s thread outer(); }\nouter() { inner(); }\ninner() { println(self.name); }\n";
        h.vm.start("test/self.gsc", "main");
        CHECK(h.joined() == "guy|");
    }

    // A thread call yields the return value of a thread that ends before suspending, undefined otherwise.
    {
        Harness h;
        h.files["test/threadret.gsc"] = "main() { a = thread now(); b = thread later(); println(a); println(isdefined(b)); }\n"
                                        "now() { return 7; }\nlater() { wait 1; return 8; }\n";
        h.vm.start("test/threadret.gsc", "main");
        CHECK(h.joined() == "7|0|");
    }

    // Several waiters of one notify run newest-registered first (beltot: the generic vehicle-node trigger thread, created
    // at load, sends `pause` without arguments; the truck_pause_node thread registered later sends it with script_wait.
    // The truck's waittill takes the first notify, so the later waiter must run first).
    {
        Harness h;
        h.files["test/waiterorder.gsc"] = R"(
main()
{
    node = spawnstruct();
    truck = spawnstruct();
    node thread early(truck);
    wait 0.05;
    truck thread pauser();
    wait 0.05;
    node thread late(truck);
    wait 0.05;
    node notify("trigger");
    wait 0.05;
}
early(truck)
{
    self waittill("trigger");
    truck notify("pause");
}
late(truck)
{
    self waittill("trigger");
    truck notify("pause", 1);
}
pauser()
{
    self waittill("pause", t);
    println(isdefined(t));
}
)";
        h.vm.start("test/waiterorder.gsc", "main");
        for (int t = 50; t <= 500; t += 50) h.vm.runFrame(t);
        CHECK(h.vm.errors().empty());
        CHECK(h.joined() == "1|");
    }

    // Registration order, not thread creation order: a thread created first that registers later is the newer waiter.
    {
        Harness h;
        h.files["test/waiterorder2.gsc"] = R"(
main()
{
    level thread slow("slow");
    level thread fast("fast");
    wait 0.1;
    level notify("go");
    wait 0.05;
}
slow(name)
{
    wait 0.05;
    level waittill("go");
    println(name);
}
fast(name)
{
    level waittill("go");
    println(name);
}
)";
        h.vm.start("test/waiterorder2.gsc", "main");
        for (int t = 50; t <= 500; t += 50) h.vm.runFrame(t);
        CHECK(h.joined() == "slow|fast|");
    }

    // Freeing an object (entity delete): a thread in a waittill on it ends; a thread with an endon on it goes on when it is in a
    // timed wait (flood spawner after its trigger is deleted) and is ended when it is parked in a waittill (docs/spec/gsc-vm.md).
    {
        Harness h;
        h.files["test/free.gsc"] = R"(
main()
{
    level.trig = spawnstruct();
    thread sleeper(level.trig);
    thread parked(level.trig);
    thread waiter(level.trig);
}
sleeper(trig)
{
    trig endon("antispawn");
    for (;;)
    {
        wait 0.5;
        println("sleeper");
    }
}
parked(trig)
{
    trig endon("antispawn");
    level waittill("never");
    println("parked woke");
}
waiter(trig)
{
    trig waittill("something");
    println("waiter woke");
}
)";
        h.vm.start("test/free.gsc", "main");
        h.vm.freeObject(h.vm.level()->get("trig").asObject());
        for (int t = 50; t <= 1500; t += 50) h.vm.runFrame(t);
        CHECK(h.joined() == "sleeper|sleeper|sleeper|");
        h.vm.notify(h.vm.level(), "never");
        h.vm.drainReady();
        // A waittill on another object keeps waiting (0x46d1f0 only drops the endon); the waiter on the freed object ended.
        CHECK(h.joined() == "sleeper|sleeper|sleeper|parked woke|");
        if (h.joined() != "sleeper|sleeper|sleeper|parked woke|") std::cerr << h.joined() << "\n";
    }

    { // waittill on the freed object (0x46c2e0 with 0): calls without an endon end; one still holding an endon stays parked
        Harness h;
        h.files["test/free2.gsc"] = R"(
main()
{
    level.trig = spawnstruct();
    level.other = spawnstruct();
    thread outer();
    thread plain();
}
outer()
{
    guarded();
    println("outer back");
}
guarded()
{
    level.other endon("stop");
    inner();
    println("guarded back");
}
inner()
{
    level.trig waittill("never");
    println("inner woke");
}
plain()
{
    level.trig endon("antispawn");
    level.trig waittill("never");
    println("plain woke");
}
)";
        h.vm.start("test/free2.gsc", "main");
        h.vm.freeObject(h.vm.level()->get("trig").asObject());
        for (int t = 50; t <= 500; t += 50) h.vm.runFrame(t);
        CHECK(h.joined() == ""); // guarded() stays parked, plain() ended (its only endon was on the freed object)
        h.vm.notify(h.vm.level()->get("other").asObject(), "stop");
        h.vm.drainReady();
        CHECK(h.joined() == "outer back|"); // the endon ends guarded(), outer() goes on (0x46c6f0 -> 0x46c130)
        if (h.joined() != "outer back|") std::cerr << h.joined() << "\n";
    }

    { // the first call of a thread is never asked (the saved stack holds no code position below it, 0x46be00 / 0x46a75c):
      // an endon held only there, even on an object that is not freed, does not keep the thread parked
        Harness h;
        h.files["test/free3.gsc"] = R"(
main()
{
    level.trig = spawnstruct();
    level.other = spawnstruct();
    thread firstonly();
}
firstonly()
{
    level.other endon("stop");
    level.trig waittill("never");
    println("firstonly woke");
}
)";
        h.vm.start("test/free3.gsc", "main");
        CHECK(h.vm.describeThreads().size() == 1);
        h.vm.freeObject(h.vm.level()->get("trig").asObject());
        h.vm.runFrame(50);
        CHECK(h.vm.describeThreads().empty()); // the thread ended instead of staying parked on level.other's endon
        h.vm.notify(h.vm.level()->get("other").asObject(), "stop");
        h.vm.drainReady();
        CHECK(h.joined() == "");
    }

    { // the frame-end free pass clears the freed object's fields (0x467150 -> 0x464660): a stale reference keeps an empty object, so
      // what the fields held is released (a struct cycle through the object no longer survives it)
        Harness h;
        h.files["test/free4.gsc"] = R"(
main()
{
    ent = spawnstruct();
    ent.chatter = spawnstruct();
    ent.chatter.owner = ent;
    ent.chatter.priority = 1;
    level.stale = ent;
    thread reader(ent);
}
reader(ent)
{
    ent waittill("never");
}
)";
        h.vm.start("test/free4.gsc", "main");
        const auto ent = h.vm.level()->get("stale").asObject();
        std::weak_ptr<iw2::script::Object> chatter = ent->get("chatter").asObject();
        h.vm.freeObject(ent);
        CHECK(!ent->fields.empty()); // until the frame end the threads of the frame still see the fields
        h.vm.runFrame(50);
        CHECK(ent->removed);
        CHECK(ent->fields.empty());
        CHECK(chatter.expired()); // the cycle ent -> chatter -> ent is broken
        CHECK(h.vm.describeThreads().empty());
    }

    {
      // OriginWatch (IW2_NET_TRACE): an origin write outside the movement's own one is recorded for the watched object only
        using iw2::script::OriginWatch;
        iw2::script::Object a, b;
        OriginWatch::enabled = true;
        OriginWatch::object[3] = &a;
        OriginWatch::depth[3] = 0;
        OriginWatch::allowed = true;
        a.set("origin", iw2::script::Value(1.0f));
        CHECK(OriginWatch::depth[3] == 0); // the movement's write
        OriginWatch::allowed = false;
        b.set("origin", iw2::script::Value(1.0f));
        a.set("angles", iw2::script::Value(1.0f));
        CHECK(OriginWatch::depth[3] == 0); // another object, another field
        a.set("origin", iw2::script::Value(2.0f));
        CHECK(OriginWatch::depth[3] > 0);
        OriginWatch::depth[3] = 0;
        OriginWatch::object[3] = nullptr;
        OriginWatch::enabled = false;
    }

    {
        // developer_script 0 (the retail default): developer-only built-ins outside a /# #/ block, and the block itself, compile to
        // nothing. println prints nothing, a failing assert / assertex / assertmsg does not end the thread, arguments are not run.
        Harness h;
        h.developerScript = false;
        h.files["test/dev.gsc"] = R"(
main()
{
    level.hits = 0;
    println("hidden");
    assert(false);
    assertex(false, "msg");
    assertmsg("never");
    println(count());
    /# println("in block"); assert(false); #/
    if (level.hits == 0)
        assert(false);
    iprintln("shown");
    level.reached = 1;
}
count()
{
    level.hits++;
    return 1;
}
)";
        h.vm.start("test/dev", "main");
        h.vm.runFrame(0);
        CHECK(h.joined() == "shown|");
        CHECK(h.vm.level()->get("reached").asInt() == 1);
        CHECK(h.vm.level()->get("hits").asInt() == 0); // the argument of the skipped println never ran
    }
    {
        // developer_script 1: everything runs; a failed assert ends the thread with an error.
        Harness h;
        h.files["test/dev.gsc"] = R"(
main()
{
    println("printed");
    /# println("in block"); #/
    level.before = 1;
    assert(false);
    level.after = 1;
}
)";
        h.vm.start("test/dev", "main");
        h.vm.runFrame(0);
        CHECK(h.joined().find("printed|in block|") == 0);
        CHECK(h.vm.level()->get("before").asInt() == 1);
        CHECK(!h.vm.level()->get("after").isDefined());
        CHECK(h.vm.errors().size() == 1);
    }
    for (const bool dev : {false, true}) {
        // A function between a file-scope /# and #/ exists only with developer_script (its body may use developer-only values).
        Harness h;
        h.developerScript = dev;
        h.vm.registerBuiltin("getdebugcvarint", [](iw2::script::CallContext&) { return iw2::script::Value(int32_t(3)); });
        h.files["test/devfn.gsc"] = R"(
main()
{
    /# level.value = dbg(); #/
    level.done = 1;
}
/#
dbg()
{
    return getdebugcvarint("x") + 1;
}
#/
)";
        h.vm.start("test/devfn", "main");
        h.vm.runFrame(0);
        CHECK(h.vm.level()->get("done").asInt() == 1);
        CHECK(h.vm.findFunction("test/devfn", "dbg").has_value() == dev);
        CHECK(h.vm.level()->get("value").isDefined() == dev);
        if (dev) CHECK(h.vm.level()->get("value").asInt() == 4);
    }
    {
        // A developer-only call used for its value outside a block (the original rejects the file; openiw2 gives undefined, see vm.cpp).
        Harness h;
        h.developerScript = false;
        h.vm.registerBuiltin("getdebugcvar", [](iw2::script::CallContext&) { return iw2::script::Value(std::string("x")); });
        h.files["test/devval.gsc"] = R"(
main()
{
    level.x = getdebugcvar("a");
    level.done = 1;
}
)";
        h.vm.start("test/devval", "main");
        h.vm.runFrame(0);
        CHECK(h.vm.level()->get("done").asInt() == 1);
        CHECK(!h.vm.level()->get("x").isDefined());
    }

    // .size (docs/spec/gsc-vm.md): an object that is not an array has size 1 (a loop over one entity runs once), a string its
    // length; other types raise "size cannot be applied to <type>".
    {
        Harness h;
        h.files["test/size.gsc"] = R"(
main()
{
    s = spawnstruct();
    n = 0;
    for (i = 0; i < s.size; i++)
        n++;
    println(n + " " + level.size + " " + "abcd".size);
    thread vecsize();
    thread undefsize();
    println("after");
}
vecsize() { v = (1, 2, 3); x = v.size; }
undefsize() { u = undefined; x = u.size; }
)";
        h.vm.start("test/size.gsc", "main");
        CHECK(!h.out.empty() && h.out.front() == "1 1 4");
        CHECK(!h.out.empty() && h.out.back() == "after");
        const auto& errs = h.vm.errors();
        CHECK(errs.size() == 2);
        CHECK(errs.size() == 2 && errs[0].find("size cannot be applied to vector") != std::string::npos);
        CHECK(errs.size() == 2 && errs[1].find("size cannot be applied to undefined") != std::string::npos);
        if (errs.size() != 2) for (const auto& e : errs) std::cerr << e << "\n";
    }

    if (failures) { std::cerr << failures << " check(s) failed\n"; return 1; }
    std::cout << "test_vm: all checks passed\n";
    return 0;
}
