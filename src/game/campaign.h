// Objectives and level changes (docs/spec/campaign.md).
#pragma once

#include <array>
#include <string>
#include <vector>

namespace iw2::game {

struct Objective {
    int state = 0;                         // 0 empty, 1 active, 2 invisible, 3 done, 4 current, 5 failed
    std::string text;                      // localized reference or plain text
    std::array<std::array<float, 3>, 8> positions{};
    std::string icon;
    bool ring = false;
};

struct LevelChange {
    bool pending = false;
    bool failed = false;                   // missionfailed(): restart the current level
    std::string map;
    bool persistent = false;
    int64_t atMs = 0;                      // world time when the change happens (exitTime)
};

// Client commands the front end shows (obj_update "<text>", obj_complete "<text>", menu_show_notify "2").
struct ClientCommand {
    std::string name;
    std::string arg;
};

} // namespace iw2::game
