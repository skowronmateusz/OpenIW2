// Savegame file naming and header rules proven from the exe (docs/spec/savegame.md). The save body (a compressed memory snapshot of the
// game state, G_LoadGame / SaveMemory_CreateCompressedSaveGame) is NOT decoded; only what the menus and the loader check first is here.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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

} // namespace iw2::ui
