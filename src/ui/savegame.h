// Savegame file naming and header rules proven from the exe (docs/spec/savegame.md section 2). The original body is a zlib stream of raw
// structure dumps (section 2.3) that openiw2 cannot reproduce; openiw2 writes its own body (src/game/savestate.h) behind this header.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace iw2::ui {

constexpr size_t kSaveHeaderSize = 0x4d8;     // bytes read from the start of a .svg file by the loader check (0x4e06e0) and the list builder (0x4c44a0)
constexpr int32_t kSaveVersion = 0x99;        // the only version the loader check accepts (0x4e0761)
constexpr int32_t kSaveMinListVersion = 0x75; // older headers down to this version still appear in the list, marked "(old save ver: %d)" (0x4c4551)
constexpr size_t kSaveListMax = 0x100;        // list capacity (0x4c45e2)

// The file a save name maps to: "save/<name>.svg" (exe format 0x5812ec; the writer uses "save\<name>.svg", 0x5837c8).
// Returns an empty string when the name is invalid: it must be shorter than 64 characters and consist of letters, digits, '_' and '-'
// (0x4e02e0..0x4e0318); '/' and '\' stay '/', except with `autosaveDir` set a slash that is not right after the 8-character prefix
// "autosave" becomes '-' (0x4e031e..0x4e0345); any other character is an error ("invalid character", 0x5837d8).
std::string saveFileName(const std::string& name, bool autosaveDir = false);

// First dword of the header = the version. Loader check (0x4e06e0): the header must be read whole (0x4d8 bytes) and the version == 0x99.
bool saveHeaderLoadable(const std::vector<uint8_t>& bytes);
// The list builder (0x4c44a0) shows a header whose version is 0x75..0x99.
bool saveHeaderListable(const std::vector<uint8_t>& bytes);

// The 9 header dwords at +0x4ac..+0x4cc that the list builder copies into the list entry (entry +0x1c) and sorts on.
using SaveSortKey = std::array<int32_t, 9>;
bool saveSortKey(const std::vector<uint8_t>& bytes, SaveSortKey& out);
// List order (binary insertion 0x4c4580..0x4c45d4): by the dwords at +0x4c0, +0x4c8, +0x4b4, +0x4b0, +0x4ac, larger first
// (a new entry that compares lower goes after the existing one).
bool saveListBefore(const SaveSortKey& a, const SaveSortKey& b);

// Header fields the list builder (0x4c44a0) shows (offsets from the file start). The 9 dwords at +0x4ac are a C `struct tm` of the save time
// (sec, min, hour, mday, mon, year-1900, wday, yday, isdst): the date column uses mday +0x4b8, mon +0x4bc (index of EXE_MONTH_ABV_*), year +0x4c0,
// hour +0x4b4, min +0x4b0; the "level time" line of the detail text prints hour:min:sec of the same dwords (code-exact, 0x4c47a8..0x4c47cb).
struct SaveInfo {
    int32_t version = 0;    // +0x00 (the date column shows "(old save ver: %d)" unless it is 0x99)
    std::string mapName;    // +0x20, string (entry +0xc)
    std::string image;      // +0x320, 0x40: the savegame image name (script savegame 3rd argument, default "$default"; entry +0x8, unused by menus)
    std::string title;      // +0x360, 0x100: the save description (script savegame 2nd argument); empty -> the map name (code-exact fallback)
    int32_t health = 0;     // +0x4a0
    int32_t difficulty = 0; // +0x4a4: 0 EASY, 1 MEDIUM, 2 HARD, 3 FU, other UNKNOWN (jump table 0x4c48e0)
    SaveSortKey tm{};       // +0x4ac..+0x4cc
};
bool saveHeaderInfo(const std::vector<uint8_t>& bytes, SaveInfo& out);
using SaveLocalize = std::function<std::string(const std::string& key)>; // localized string of a key (unknown key = the key)
// List column 1 (entry +0x14): "<EXE_MONTH_ABV_x> <mday>, <year>   HH:MM"; old headers: "(old save ver: N)".
std::string saveDateText(const SaveInfo& info, const SaveLocalize& loc);
// Detail text (entry +0x10, selected into the UI state 0x59b724+0x41e): "<title>\n<GAME_LEVELTIME>: H:MM:SS\n<difficulty>\n<GAME_HEALTH>: n".
// The exe format 0x5811e4 also carries colour/escape bytes 0x14 / 0x15 around the localized pieces, which are left out here.
std::string saveDetailText(const SaveInfo& info, const SaveLocalize& loc);

// Header writer (layout of 0x4d4f70, docs/spec/savegame.md 2.2). Fields the original fills from engine state that openiw2 does not
// have (script fingerprints +0x14..+0x1c, changelist +0x1a0) stay zero; build string +0x120 is "openiw2".
struct SaveHeaderFields {
    uint32_t bspChecksum = 0;   // +0x004
    uint32_t bodyChecksum = 0;  // +0x008 (openiw2: CRC-32 of the uncompressed body; the exe uses 0x43bc30)
    int32_t saveId = 0;         // +0x00c
    std::string mapName;        // +0x020
    std::string campaign;       // +0x220 ui_campaign
    std::string image;          // +0x320
    std::string description;    // +0x360
    std::string name;           // +0x460
    int32_t healthPercent = 1;  // +0x4a0 (saveHealthPercent)
    int32_t difficulty = 0;     // +0x4a4 g_gameskill
    bool manual = false;        // +0x4a8: save type 0 (not an autosave)
    bool noRecord = false;      // +0x4a9: do not become sv_lastSaveGame
    SaveSortKey tm{};           // +0x4ac struct tm
    uint32_t bodySize = 0;      // +0x4d0 uncompressed
    uint32_t compressedSize = 0; // +0x4d4
};
std::vector<uint8_t> buildSaveHeader(const SaveHeaderFields& f);
// +0x4a0 (0x4d5126..0x4d5173): 1 when health or max is 0, else int(100 * health / max) clamped to 1..100.
int32_t saveHealthPercent(int health, int maxHealth);
// The struct tm dwords of a time_t (local time), as 0x427cd0 stores them.
SaveSortKey saveTimeNow();
// 0x44a2f0: true when a path segment of the name starts with "internal/" (case-insensitive); such saves never become sv_lastSaveGame.
bool saveNameInternal(const std::string& name);

} // namespace iw2::ui
