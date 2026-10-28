#include "ui/savegame.h"

#include <algorithm>
#include <cstdio>
#include <strings.h>
#include <cstring>
#include <ctime>

namespace iw2::ui {

std::string saveFileName(const std::string& name, bool autosaveDir) {
    if (name.size() >= 0x40) return {};
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '/' || c == '\\') {
            out += (autosaveDir && !(i == 8 && name.compare(0, 8, "autosave") == 0)) ? '-' : '/';
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            out += c;
        } else {
            return {};
        }
    }
    return "save/" + out + ".svg";
}

static int32_t dword(const std::vector<uint8_t>& b, size_t off) {
    int32_t v;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

bool saveHeaderLoadable(const std::vector<uint8_t>& b) { return b.size() >= kSaveHeaderSize && dword(b, 0) == kSaveVersion; }

bool saveHeaderListable(const std::vector<uint8_t>& b) {
    if (b.size() < kSaveHeaderSize) return false;
    const int32_t v = dword(b, 0);
    return v >= kSaveMinListVersion && v <= kSaveVersion;
}

bool saveSortKey(const std::vector<uint8_t>& b, SaveSortKey& out) {
    if (b.size() < kSaveHeaderSize) return false;
    for (size_t i = 0; i < 9; ++i) out[i] = dword(b, 0x4ac + 4 * i);
    return true;
}

bool saveListBefore(const SaveSortKey& a, const SaveSortKey& b) {
    // dword indices relative to +0x4ac: +0x4c0 = 5, +0x4c8 = 7, +0x4b4 = 2, +0x4b0 = 1, +0x4ac = 0
    for (const size_t i : {size_t(5), size_t(7), size_t(2), size_t(1), size_t(0)})
        if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

static std::string cstr(const std::vector<uint8_t>& b, size_t off, size_t max) {
    std::string r;
    for (size_t i = off; i < off + max && i < b.size() && b[i]; ++i) r += char(b[i]);
    return r;
}

bool saveHeaderInfo(const std::vector<uint8_t>& b, SaveInfo& out) {
    if (b.size() < kSaveHeaderSize || !saveSortKey(b, out.tm)) return false;
    out.version = dword(b, 0);
    out.mapName = cstr(b, 0x20, 0x100);
    out.image = cstr(b, 0x320, 0x40);
    out.title = cstr(b, 0x360, 0x100);
    if (out.title.empty()) out.title = out.mapName; // 0x4c4642..0x4c466b: "\x15%s" of +0x20 into the title buffer
    out.health = dword(b, 0x4a0);
    out.difficulty = dword(b, 0x4a4);
    return true;
}

std::string saveDateText(const SaveInfo& i, const SaveLocalize& loc) {
    char buf[96];
    if (i.version != kSaveVersion) {
        std::snprintf(buf, sizeof buf, "(old save ver: %d)", i.version);
        return buf;
    }
    static const char* const months[12] = {"JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUN", "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"};
    const int mon = i.tm[4]; // TODO(spec): the exe indexes the table 0x5a4d64 without a range check
    const std::string m = mon >= 0 && mon < 12 ? loc(std::string("EXE_MONTH_ABV_") + months[mon]) : std::string();
    std::snprintf(buf, sizeof buf, "%s %i, %i   %02i:%02i", m.c_str(), i.tm[3], i.tm[5] + 1900, i.tm[2], i.tm[1]);
    return buf;
}

std::string saveDetailText(const SaveInfo& i, const SaveLocalize& loc) {
    char t[48];
    // "%2i:%s%i:%s%i": hour, "0" when min <= 9, min, "0" when sec <= 9, sec (strings 0x5766b8 "0" / 0x56cd07 "")
    std::snprintf(t, sizeof t, "%2i:%s%i:%s%i", i.tm[2], i.tm[1] > 9 ? "" : "0", i.tm[1], i.tm[0] > 9 ? "" : "0", i.tm[0]);
    static const char* const diff[4] = {"EASY", "MEDIUM", "HARD", "FU"};
    const std::string d = loc(std::string("GAME_DIFFICULTY_") + (i.difficulty >= 0 && i.difficulty < 4 ? diff[i.difficulty] : "UNKNOWN"));
    return i.title + "\n" + loc("GAME_LEVELTIME") + ": " + t + "\n" + d + "\n" + loc("GAME_HEALTH") + ": " + std::to_string(i.health);
}

static void putDword(std::vector<uint8_t>& b, size_t off, uint32_t v) { std::memcpy(b.data() + off, &v, 4); }
static void putStr(std::vector<uint8_t>& b, size_t off, size_t max, const std::string& s) {
    std::memcpy(b.data() + off, s.data(), std::min(s.size(), max - 1)); // always NUL-terminated (the exe copies with a size limit)
}

std::vector<uint8_t> buildSaveHeader(const SaveHeaderFields& f) {
    std::vector<uint8_t> b(kSaveHeaderSize, 0);
    putDword(b, 0x000, uint32_t(kSaveVersion));
    putDword(b, 0x004, f.bspChecksum);
    putDword(b, 0x008, f.bodyChecksum);
    putDword(b, 0x00c, uint32_t(f.saveId));
    putStr(b, 0x020, 0x100, f.mapName);
    putStr(b, 0x120, 0x80, "openiw2");
    putStr(b, 0x220, 0x100, f.campaign);
    putStr(b, 0x320, 0x40, f.image);
    putStr(b, 0x360, 0x100, f.description);
    putStr(b, 0x460, 0x40, f.name);
    putDword(b, 0x4a0, uint32_t(f.healthPercent));
    putDword(b, 0x4a4, uint32_t(f.difficulty));
    b[0x4a8] = f.manual ? 1 : 0;
    b[0x4a9] = f.noRecord ? 1 : 0;
    for (size_t i = 0; i < 9; ++i) putDword(b, 0x4ac + 4 * i, uint32_t(f.tm[i]));
    putDword(b, 0x4d0, f.bodySize);
    putDword(b, 0x4d4, f.compressedSize);
    return b;
}

int32_t saveHealthPercent(int health, int maxHealth) {
    if (health == 0 || maxHealth == 0) return 1;
    const int v = int(float(health * 100) / float(maxHealth));
    return v < 1 ? 1 : v > 100 ? 100 : v;
}

SaveSortKey saveTimeNow() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    return {tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon, tm.tm_year, tm.tm_wday, tm.tm_yday, tm.tm_isdst};
}

bool saveNameInternal(const std::string& name) {
    for (size_t i = 0;; ++i) {
        if (name.size() - i >= 9 && strncasecmp(name.c_str() + i, "internal", 8) == 0 && name[i + 8] == '/') return true;
        // advance to the character after the next separator
        while (i < name.size() && name[i] != '/' && name[i] != '\\') ++i;
        if (i >= name.size()) return false;
    }
}

} // namespace iw2::ui
