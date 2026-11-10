// Menu execKey (docs/spec/mp-chat.md "Quick messages"; menu parse 0x544150, key handler 0x53e900): the top menu's
// script for a character key runs; a non-fullscreen menu does not take the other keys (0x5345b0).

#include "ui/frontend.h"

#include <iostream>
#include <string>

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
    using namespace iw2;
    fs::Vfs vfs;
    ui::MenuLibrary lib(vfs);
    lib.parseSource("{\n"
                    "menuDef {\n  name \"pick\"\n  fullscreen 0\n  rect 0 0 640 480\n"
                    "  execKey \"1\" { close pick; open sub }\n"
                    "  execKey \"2\" { scriptMenuResponse \"2\"; close pick }\n"
                    "}\n"
                    "menuDef {\n  name \"sub\"\n  fullscreen 1\n  rect 0 0 640 480\n}\n"
                    "}\n",
                    "test.menu");
    std::string response;
    ui::MenuEnv env;
    env.getCvar = [](const std::string&) { return std::string(); };
    env.setCvar = [](const std::string&, const std::string&) {};
    env.scriptMenuResponse = [&](const std::string& menu, const std::string& r) { response = menu + ":" + r; };
    ui::MenuStack stack(lib, env);
    CHECK(stack.open("pick"));
    CHECK(!stack.takesAllKeys());
    CHECK(!stack.execKey('x'));
    CHECK(stack.execKey('1'));
    CHECK(stack.top() && stack.top()->name == "sub");
    CHECK(stack.takesAllKeys());
    stack.closeAll();
    CHECK(stack.open("pick"));
    CHECK(stack.execKey('2'));
    CHECK(response == "pick:2");
    CHECK(stack.empty());
    if (failures) std::cerr << failures << " failures\n";
    return failures ? 1 : 0;
}
