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
    // Header fields (code-exact offsets 0x4c44a0).
    std::vector<uint8_t> g(kSaveHeaderSize, 0);
    auto put = [&](size_t off, int32_t v) { std::memcpy(g.data() + off, &v, 4); };
    put(0, 0x99);
    std::memcpy(g.data() + 0x20, "moscow", 7);
    put(0x4a0, 77), put(0x4a4, 2);
    put(0x4ac, 5), put(0x4b0, 7), put(0x4b4, 14), put(0x4b8, 3), put(0x4bc, 5), put(0x4c0, 106);
    SaveInfo si;
    check(saveHeaderInfo(g, si) && si.mapName == "moscow" && si.title == "moscow" && si.health == 77 && si.difficulty == 2, __LINE__);
    const SaveLocalize loc = [](const std::string& k) { return "<" + k + ">"; };
    check(saveDateText(si, loc) == "<EXE_MONTH_ABV_JUN> 3, 2006   14:07", __LINE__);
    check(saveDetailText(si, loc) == "moscow\n<GAME_LEVELTIME>: 14:07:05\n<GAME_DIFFICULTY_HARD>\n<GAME_HEALTH>: 77", __LINE__);
    std::memcpy(g.data() + 0x360, "Title", 6);
    put(0, 0x80);
    saveHeaderInfo(g, si);
    check(si.title == "Title" && saveDateText(si, loc) == "(old save ver: 128)", __LINE__);
    put(0x4a4, 9);
    saveHeaderInfo(g, si);
    check(saveDetailText(si, loc).find("<GAME_DIFFICULTY_UNKNOWN>") != std::string::npos, __LINE__);
    return failures ? 1 : 0;
}
