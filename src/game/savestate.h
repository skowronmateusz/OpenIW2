// openiw2 savegame files (docs/spec/savegame.md sections 2 and 5): the original 0x4d8-byte header (so the save list shows our saves)
// followed by a zlib-deflated openiw2 body (magic "OIW2SAVE"), which holds the VM and World state (World::saveState).
#pragma once

#include "ui/savegame.h"

#include <cstdint>
#include <string>
#include <vector>

namespace iw2::game {

class World;

constexpr char kStateMagic[8] = {'O', 'I', 'W', '2', 'S', 'A', 'V', 'E'};
constexpr uint32_t kStateVersion = 3; // 2: movie-pending flag (0x1c60fe4) after the level change; 3: vehicle setswitchnode replacement

struct SaveRequest {
    std::string name;                // save name without "save/" and ".svg" (header +0x460)
    std::string description;         // header +0x360 (the list's title)
    std::string image = "$default";  // header +0x320
    int32_t saveId = 0;              // header +0x00c
    bool manual = false;             // header +0x4a8: save type 0
};

// Builds a whole .svg file of the world's current state (between frames only).
std::vector<uint8_t> buildSaveFile(World& world, const SaveRequest& req);

struct LoadedSave {
    ui::SaveInfo info;
    std::string name;                // header +0x460
    std::vector<uint8_t> body;       // inflated openiw2 stream
};
// Checks the header like G_LoadGame (0x4d5270: size, version 0x99, size limits) and inflates the body. Returns an error text
// ("" = ok); a file whose body is not an openiw2 stream (an original CoD2 save) gives "not an openiw2 save".
std::string parseSaveFile(const std::vector<uint8_t>& file, LoadedSave& out);
// Restores the state into a world freshly constructed for out.info.mapName with prepareStart() already run.
void restoreSave(World& world, const LoadedSave& save);

} // namespace iw2::game
