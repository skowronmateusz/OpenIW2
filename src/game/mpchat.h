// Multiplayer chat: name and text cleaning, the client chat buffer. Implemented from the spec in docs/spec/mp-chat.md.
// The server side (say / say_team, sayAll / sayTeam) is in World (src/game/mpchat.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace iw2::game {

// Text cleaning (0x44a8d0): "^<digit>" pairs and characters outside 0x20..0x7e are dropped.
std::string mpCleanString(const std::string& s);
// Player name cleaning (0x4fe010) into a 32-byte buffer; "UnnamedPlayer" when nothing visible is left.
std::string mpCleanName(const std::string& name);
// A received chat server command's text (0x4d1c9f / 0x4d1d0b) after localization: cut to 149 bytes, 0x19 bytes removed.
std::string mpChatReceived(const std::string& localized);

// Height in virtual units of text drawn at `textScale`: the font's pixel height times the font-normalized scale
// (textScale * 48 / pixel height), so 48 * textScale for every font.
inline float mpTextHeight(float textScale) { return 48.0f * textScale; }
// Baseline of the message field (caller 0x4caf70 passes cg_hudSayPosition.y + 24 rounded, 0x406d00 adds the text height at
// `textScale`, 0.2083). The history ends at cg_hudChatPosition.y, so with the defaults (126, 150) the field sits below it.
inline float mpChatFieldBaseline(float sayY, float textScale) { return float(int(sayY + 0.5f)) + 24.0f + mpTextHeight(textScale); }
// First row (top) of the history line `i` (newest 0): cg_hudChatPosition.y - 10 * (i + 1) (0x4c7760).
inline float mpChatLineTop(float chatY, int i) { return chatY - 10.0f * float(i + 1); }

// Client chat lines (add 0x4d0e10, draw 0x4c7760). Times are cg times in ms.
class MpChatBuffer {
public:
    static constexpr int kMaxHeight = 8;  // cg_chatHeight range 0..8
    static constexpr int kLineChars = 90; // visible characters per line
    // Adds a message (already localized). height = cg_chatHeight, chatTime = cg_chatTime.
    void add(const std::string& msg, int64_t now, int height, int chatTime);
    struct Line {
        std::string text;
        float alpha = 1.0f;
        int code = -1; // the digit of the line's leading "^N" (strip colour), -1 none
    };
    // One draw frame: drops the oldest line once it is older than chatTime (one per frame), then the lines to draw,
    // newest first (index 0 is drawn 10 units above the chat position, each older one 10 higher).
    std::vector<Line> frame(int64_t now, int height, int chatTime);
    void clear() { pos_ = last_ = 0; }

private:
    std::string text_[kMaxHeight];
    int64_t time_[kMaxHeight] = {};
    int pos_ = 0, last_ = 0;
};

} // namespace iw2::game
