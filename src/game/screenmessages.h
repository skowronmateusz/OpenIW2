// On-screen game messages (the game-message window, docs/spec/ui.md): a queue of rows with a start and an end time. Rows are
// added by client commands (gm / gmb / obj_*; the print messages of the cheat commands, openiw2) and drop out when their end time
// has passed. Times are in nanoseconds of the caller's clock.
#pragma once

#include <cmath>

#include <memory>
#include <cstdint>
#include <string>
#include <vector>

namespace iw2::game {

struct ScreenMessage {
    std::string text;
    uint64_t untilNs = 0;
    bool bold = false;
    uint64_t startNs = 0, endNs = 0;
    // MP obituary line (docs/spec/mp-hud.md): attacker text, icon, victim text, each name in its team colour; null for text rows.
    struct Obit {
        std::string attacker, victim, icon;
        float attackerRgb[3] = {1, 1, 1}, victimRgb[3] = {1, 1, 1};
        float iconW = 1.4f, iconH = 1.4f; // in line heights
        bool flip = false;
    };
    std::shared_ptr<const Obit> obit = nullptr;
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
// A server print of the 1.3 server is a localisation key with its parameters behind 0x15 (docs/spec/net.md section 10).
inline std::string printMessageText(const std::string& arg) {
    const std::string head = arg.substr(0, arg.find('\x15'));
    bool key = !head.empty() && head.find('_') != std::string::npos;
    for (char ch : head) key = key && ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_');
    return key ? "\x14" + arg : arg;
}

// Multiplayer game-message window (iprintln, obituaries; VAs in the patch 1.3 MP executable 0x4cad60): its anchor follows
// the HUD menu "Compass" (looked up in the MP HUD menus by 0x4c0ab0, which keeps the rect's y and height):
// y = round(compass y - (cg_hudCompassSize - 1) * compass height + 12), relative to the bottom of the 640x480 screen as
// the compass rect is (VERTICAL_ALIGN_BOTTOM). x 6, character height 12 as in SP. With the stock rect (-140, height 160)
// and size 1 this is -128: the rows stack upwards from just above the compass instead of from y 100 at the top, where
// they ran into the MP clock and team scores. TODO(spec): the alignment codes passed to the window draw (0x406a20).
inline float mpGameMessageWindowY(float compassY, float compassH, float compassSize) {
    return std::floor(compassY - (compassSize - 1.0f) * compassH + 12.0f + 0.5f);
}

} // namespace iw2::game
