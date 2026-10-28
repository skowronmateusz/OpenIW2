// sethintstring buffer builder and the 32-slot table (0x4eb9b0, 0x4f0da0).
#include "game/hintstring.h"

#include <cstdio>

using namespace iw2::game;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static bool throws(const std::vector<HintArg>& a, const std::string& msg, bool warnErr = false) {
    try { buildHintString(a, "Hint String", false, warnErr); } catch (const std::exception& e) { return std::string(e.what()).find(msg) != std::string::npos; }
    return false;
}

int main() {
    CHECK(buildHintString({{true, "PLATFORM_HOLD_TO_USE"}}, "Hint String", false, false) == "PLATFORM_HOLD_TO_USE");
    CHECK(buildHintString({{true, "A"}, {true, "B"}, {false, "12"}, {false, "34"}}, "Hint String", false, false) == std::string("A\x14" "B\x15" "1234"));
    CHECK(buildHintString({{false, "5"}}, "Hint String", false, false) == std::string("\x15" "5"));
    CHECK(buildHintString({{false, "1\x01\xff"}}, "Hint String", false, false) == std::string("\x15" "1.."));
    std::string warn;
    CHECK(buildHintString({{false, "ab"}}, "Hint String", false, false, &warn) == std::string("\x15" "ab") && !warn.empty());
    CHECK(buildHintString({{false, "ab"}}, "Hint String", true, false) == std::string("\x15" "UNLOCALIZED: ab"));
    CHECK(throws({{false, "ab"}}, "are not allowed to have letters in them", true));
    CHECK(throws({{false, "1\x15"}}, "bad escape character (21)"));
    CHECK(throws({{true, "a b"}}, "Illegal localized string reference"));
    CHECK(buildHintString({{true, "a"}}, "Hint String", false, false) == "a");
    CHECK(throws({{false, std::string(1100, '1')}}, "Hint String is too long. Max length is 1024"));

    HintStringTable t;
    CHECK(t.intern("A") == 0 && t.intern("B") == 1 && t.intern("A") == 0 && t.at(1) == "B");
    for (int i = 2; i < kHintStringSlots; ++i) CHECK(t.intern("S" + std::to_string(i)) == i);
    CHECK(t.intern("new") == -1 && t.intern("S5") == 5);

    auto lk = [](const std::string& k) -> const std::string* { static const std::string v = "Press"; return k == "KEY" ? &v : nullptr; };
    CHECK(hintStringText(std::string("KEY\x14X\x15" "12"), lk) == "PressX12");

    if (failures == 0) std::printf("hintstring OK\n");
    return failures ? 1 : 0;
}
