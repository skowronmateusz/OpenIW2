// Savegame naming and header rules (src/ui/savegame.h).
#include "ui/savegame.h"

#include <cstring>
#include <iostream>

int main() {
    using namespace iw2::ui;
    int failures = 0;
    auto check = [&](bool c, int line) { if (!c) { std::cerr << "line " << line << " failed\n"; ++failures; } };
    check(saveFileName("mysave_1") == "save/mysave_1.svg", __LINE__);
    check(saveFileName("a-b") == "save/a-b.svg", __LINE__);
    check(saveFileName("bad name").empty() && saveFileName("x.y").empty(), __LINE__);
    check(saveFileName(std::string(63, 'a')) == "save/" + std::string(63, 'a') + ".svg" && saveFileName(std::string(64, 'a')).empty(), __LINE__);
    check(saveFileName("autosave/x", true) == "save/autosave/x.svg" && saveFileName("a/b", true) == "save/a-b.svg" && saveFileName("a/b") == "save/a/b.svg", __LINE__);
    std::vector<uint8_t> h(kSaveHeaderSize, 0);
    auto setv = [&](int32_t v) { std::memcpy(h.data(), &v, 4); };
    setv(0x99);
    check(saveHeaderLoadable(h) && saveHeaderListable(h), __LINE__);
    setv(0x98);
    check(!saveHeaderLoadable(h) && saveHeaderListable(h), __LINE__);
    setv(0x74);
    check(!saveHeaderListable(h), __LINE__);
    setv(0x99);
    h.resize(kSaveHeaderSize - 1);
    check(!saveHeaderLoadable(h) && !saveHeaderListable(h), __LINE__);
    SaveSortKey a{}, b{};
    a[5] = 2006, b[5] = 2005; // year
    check(saveListBefore(a, b) && !saveListBefore(b, a) && !saveListBefore(a, a), __LINE__);
    a[5] = b[5] = 2006;
    a[7] = 10, b[7] = 12; // day of year
    check(saveListBefore(b, a), __LINE__);
    return failures ? 1 : 0;
}
