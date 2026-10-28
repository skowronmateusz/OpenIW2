// Objectives and level changes (docs/spec/campaign.md).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace iw2::game {

struct Objective {
    int state = 0;                         // 0 empty, 1 active, 2 invisible, 3 done, 4 current, 5 failed
    std::string text;                      // localized reference or plain text
    std::array<std::array<float, 3>, 8> positions{};
    std::string icon;
    bool ring = false;
    int64_t ringStartMs = -1;              // pointer +0x464: pulse start (cg time), -1 none
};

struct LevelChange {
    bool pending = false;
    bool failed = false;                   // missionfailed(): restart the current level
    std::string map;
    bool persistent = false;
    int64_t atMs = 0;                      // world time when the change happens (exitTime)
    bool loadContinue = false;             // loadgame_continue (0x4d9f90 -> 0x447190): load sv_lastSaveGame, restart if there is none
    // Set by World::takeDueLevelChange when the movie-pending flag (0x1c60fe4, set by missionsuccess and cinematic) was set and no
    // cinematic name is stored: the server sends `openvictoryscreen` instead of loading (0x4d9ffc); the map stays stored for `nextlevel`.
    bool victoryScreen = false;
};

// Client commands the front end shows (obj_update "<text>", obj_complete "<text>", menu_show_notify "2").
struct ClientCommand {
    std::string name;
    std::string arg;
};

} // namespace iw2::game
