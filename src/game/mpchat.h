// Multiplayer chat: name and text cleaning, the client chat buffer. Implemented from the spec in docs/spec/mp-chat.md.
// The server side (say / say_team, sayAll / sayTeam) is in World (src/game/mpchat.cpp).
#pragma once

#include <algorithm>
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

// A message broken into display lines the way the client chat buffer does (the same rules as MpChatBuffer::add, below).
std::vector<std::string> mpChatWrap(const std::string& msg);

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

// OpenIW2 addition, not in the original (docs/spec/mp-chat.md "Chat history"): the recent chat lines of the session, shown
// in a scrollable panel while the message field is open. Lines are kept wrapped like the chat lines; the oldest drop out
// beyond kCapacity. scroll() counts lines back from the newest (0 = the newest line is at the bottom).
class MpChatHistory {
public:
    static constexpr int kCapacity = 50; // lines kept
    static constexpr int kVisible = 8;   // lines shown at once (the most cg_chatHeight allows; keeps the window clear of the score HUD)
    void add(const std::string& msg);
    void clear() { lines_.clear(); scroll_ = 0; }
    int size() const { return int(lines_.size()); }
    int scroll() const { return scroll_; }
    // Moves the view by `delta` lines (positive = towards older lines), clamped to what exists.
    void scrollBy(int delta);
    void resetScroll() { scroll_ = 0; }
    int maxScroll() const { return std::max(0, size() - kVisible); }
    // The lines in view, oldest first (at most kVisible); the newest is last when scroll() is 0.
    std::vector<MpChatBuffer::Line> view() const;

private:
    std::vector<std::string> lines_;
    int scroll_ = 0;
};

// Whether mp_QuickMessage (0x4c1680) opens the menu: the local player state has pm_flags bit 0x800000 (docs/spec/mp-chat.md
// "Quick messages"). A network client reads the snapshot's / predicted player state. The server sets the bit for playing
// and dead clients that follow nobody; the port's server sends it while playing only, so a dead state (pm_type 6) that
// is not a follow view counts too (TODO(spec): the bit while dead on the wire).
constexpr bool mpQuickMessageAllowedPs(int pmFlags, int pmType) {
    return (pmFlags & 0x800000) != 0 || (pmType == 6 && (pmFlags & 0x400000) == 0);
}
// The same rule for a world that runs the gametype itself (listen server, local game): session state 0 playing / 1 dead.
constexpr bool mpQuickMessageAllowedSession(int sessionState) { return sessionState == 0 || sessionState == 1; }

// OpenIW2 addition: the history window's rectangle in virtual units, for the chat position (chatX, chatY) and the say
// position's baseline: it spans kVisible rows above chatY, a 14-unit header row, and the message field below.
struct MpChatPanelRect { float x0, y0, x1, y1; };
inline MpChatPanelRect mpChatPanelRect(float chatX, float chatY, float fieldBaseline) {
    return {chatX - 4.0f, mpChatLineTop(chatY, MpChatHistory::kVisible - 1) - 14.0f, chatX + 432.0f, fieldBaseline + 5.0f};
}

} // namespace iw2::game
