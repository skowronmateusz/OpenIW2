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
    Harness() {
        vm.print = [this](const std::string& s) { out.push_back(s); };
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
        CHECK(h.joined() == "main|got 7x|after notify|");
        h.vm.runFrame(200);
        CHECK(h.joined() == "main|got 7x|after notify|"); // ender was killed by endon before "tick"
        CHECK(h.vm.liveThreadCount() == 0);
        if (h.joined() != "main|got 7x|after notify|") std::cerr << h.joined() << "\n";
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
        CHECK(h.joined() == "back 0|");
        h.vm.notify(h.vm.level()->get("ent").asObject(), "stop");
        CHECK(h.joined() == "back 0|back 1|");
        h.vm.notify(h.vm.level()->get("ent").asObject(), "stop"); // endon of the returned call is gone
        h.vm.notify(h.vm.level(), "later");
        CHECK(h.joined() == "back 0|back 1|later|");
        if (h.joined() != "back 0|back 1|later|") std::cerr << h.joined() << "\n";
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

    if (failures) { std::cerr << failures << " check(s) failed\n"; return 1; }
    std::cout << "test_vm: all checks passed\n";
    return 0;
}
