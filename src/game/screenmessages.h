// On-screen game messages (the game-message window, docs/spec/ui.md): a queue of rows with a start and an end time. Rows are
// added by client commands (gm / gmb / obj_*; the print messages of the cheat commands, openiw2) and drop out when their end time
// has passed. Times are in nanoseconds of the caller's clock.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace iw2::game {

struct ScreenMessage {
    std::string text;
    uint64_t untilNs = 0;
    bool bold = false;
    uint64_t startNs = 0, endNs = 0;
};

constexpr uint64_t kGameMessageNs = 5000000000ull;     // con_gamemessagetime 5 s (0x571118)
constexpr uint64_t kBoldGameMessageNs = 8000000000ull; // con_boldgamemessagetime 8 s (0x571100)

// `text` may carry the 0x14 localisation marker (a string reference), as the client commands do.
inline void addScreenMessage(std::vector<ScreenMessage>& q, const std::string& text, bool bold, uint64_t nowNs) {
    const uint64_t d = bold ? kBoldGameMessageNs : kGameMessageNs;
    q.push_back({text, nowNs + d, bold, nowNs, nowNs + d});
}

// Rows whose end time lies in the past are removed (the draw code does this every frame).
inline void expireScreenMessages(std::vector<ScreenMessage>& q, uint64_t nowNs) {
    std::erase_if(q, [&](const ScreenMessage& m) { return m.untilNs < nowNs; });
}

// What the openiw2 HUD shows for a client `print` message: a localisation reference to the string (the cheat commands send
// the GAME_* keys). Plain text (no key shape) is shown as it is. TODO(spec): the original sends `print` to the console; only
// with con_minicon (0xf56a04, default 0) does 0x4bfa70 draw a mini console (window 2); its position and fade are not proven.
inline std::string printMessageText(const std::string& arg) {
    const bool key = !arg.empty() && arg.find(' ') == std::string::npos && arg.rfind("GAME_", 0) == 0;
    return key ? "\x14" + arg : arg;
}

} // namespace iw2::game
