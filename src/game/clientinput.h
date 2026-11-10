// Client input: key bindings from the game's default.cfg and the client commands they run (+attack, toggleads,
// gocrouch, +gostand, weapnext, …), turned into the usercmd button bits and stance (docs/spec/controls.md).
#pragma once

#include <cstdint>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace iw2::game {

// usercmd button bits (CL_CmdButtons 0x409af0, stance 0x409500).
namespace Button {
constexpr uint32_t Attack = 0x1, Melee = 0x4, Activate = 0x8, Reload = 0x10, UseReload = 0x20, LeanLeft = 0x40,
                   LeanRight = 0x80, Prone = 0x100, Crouch = 0x200, Jump = 0x400, Ads = 0x1000, StanceHeld = 0x2000,
                   Binoculars = 0x4000, HoldBreath = 0x8000, Frag = 0x10000, Smoke = 0x20000;
}

class ClientInput {
public:
    // Parses `bind <key> "<command>"` lines (unbindall clears). Key names are upper-cased (W, SHIFT, MOUSE1, …).
    void loadConfig(const std::string& text);
    const std::map<std::string, std::string>& bindings() const { return binds_; }
    // Keys bound to a command, for getkeybinding.
    std::vector<std::string> keysFor(const std::string& command) const; // oldest binding first (slot 0, slot 1)

    // Menu BIND item capture (exe 0x4ce100): `key` empty = clear the command's slots (Del). Otherwise the key is
    // first taken from any command that holds it; then it fills slot 0, else slot 1 (if slot 0 is another key), else
    // both slots are replaced by this key alone.
    void rebind(const std::string& command, const std::string& key);
    // `unbindall` + `bind KEY "cmd"` lines for config.cfg, in slot order.
    std::string formatBindings() const;

    // A key went down or up: runs its bound command ("+x" on down, "-x" on up, others on down only).
    void key(const std::string& keyName, bool down);
    // Key name -> key number (0x40b880): a one-character name is the character itself, "0x??" (4 characters, lowercase hex
    // digits) the byte, anything else a case-insensitive lookup in the key-name table (0x59a0e0); -1 when unknown.
    static int keyNumber(const std::string& name);
    // The key table of the exe (12-byte entries at 0x5d2728: +0 down, +4 repeats, +8 binding), looked up by key name.
    // buttonpressed (0x538980) reads the down flag, getcommandfromkey (0x4ece00) the binding text; a name that is not a key
    // gives false / "". The port stores keys under upper-cased names (TODO(spec): the exe keeps one-character names as typed).
    bool keyDown(const std::string& name) const;
    std::string commandFromKey(const std::string& name) const;
    // Runs one command as the console would.
    void command(const std::string& cmd, bool down = true, const std::string& keyName = {});

    // The button bits of this frame's command (CL_CmdButtons + stance). `inVehicle`: pm_flags 0x20000 without 0x80000
    // (or pm_type 2/3): the +gostand key counts as jump (0x409c89). `forcedAds`: the player state's weapon state
    // is 0x13 or 0x14 (0x4095b6). Clears the "was pressed" latches.
    uint32_t buttons(bool inVehicle, bool forcedAds = false);

    int stance() const { return stance_; } // 0 stand, 1 crouch, 2 prone (client stance, 0x59b724 +8)
    void setStance(int s) { stance_ = s; }
    // Player-state events 0x8c / 0x8d / 0x8e (cgame 0x4bac21..0x4bac60): the client stance becomes stand / crouch / prone, unless
    // +prone or +movedown is held (client +0xc, kept by 0x408cd0). This is what ends a blocked rise after one event.
    void stanceEvent(int ev) { if (ev >= 0x8c && ev <= 0x8e && !stanceLocked()) stance_ = ev - 0x8c; }
    bool adsToggled() const { return adsToggle_; }
    void clearAds() { adsToggle_ = false; } // map load (0x40e821)
    // weapnext / weapprev requests since the last call: +1 next, −1 previous, 0 none.
    int takeWeaponCycle() { int c = cycle_; cycle_ = 0; return c; }

private:
    struct KButton {
        std::string down[2]; // keys holding it
        bool active = false, wasPressed = false;
    };
    void press(KButton& b, const std::string& key);
    void release(KButton& b, const std::string& key);
    bool held(const KButton& b) const { return b.active || b.wasPressed; }
    bool stanceLocked() const { return prone_.active || moveDown_.active; }

    std::map<std::string, std::string> binds_;
    std::set<std::string> keysDown_; // key-table down flags (0x5d2728, written by CL_KeyEvent 0x40c0fd), upper-cased names
    std::map<std::string, int> seq_; // binding order per key (slot order of a command)
    int nextSeq_ = 0;
    KButton attack_, melee_, holdBreath_, frag_, smoke_, binoculars_, activate_, reload_, useReload_, leanLeft_,
        leanRight_, prone_, stanceKey_, jump_, goStand_, moveDown_, speed_, forward_, back_, moveLeft_, moveRight_,
        left_, right_, lookUp_, lookDown_, strafe_; // +left / +right (0x5d1fa8 / 0x5d1fbc), +lookup / +lookdown, +strafe (0x5d2058)
    bool mlook_ = false;       // +mlook (0x408b10 sets the byte 0x5d20bc, -mlook 0x408b20 clears it)
    bool centerView_ = false;  // centerview (0x409240) or -mlook without cl_freelook, taken by the caller
    bool mlookReleased_ = false;
    int mouseSide_ = 0, mouseForward_ = 0; // CL_MouseMove contributions of this frame (m_side / m_forward)
    int stance_ = 0;
    bool adsToggle_ = false;
    bool objectivesHeld_ = false; // +scores held (cgame 0xf791fc): shows the objectives screen
    int cycle_ = 0;
    std::vector<std::string> console_;

public:
    bool objectivesHeld() const { return objectivesHeld_; }
    // Bound commands that are not input commands (togglemenu, openscriptmenu <menu> <response>, ...), in key order, for
    // the front end to run; the call empties the list.
    std::vector<std::string> takeConsoleCommands() { return std::exchange(console_, {}); }
    // Movement keys held (W/S/A/D): forward and right command values.
    // CL_KeyMove 0x409500: with +strafe held, +right / +left add +-127 to the side move (0x4095da); CL_MouseMove adds the mouse's
    // side / forward part; the command bytes are clamped to -128..127.
    int forwardMove() const { return std::clamp((forward_.active ? 127 : 0) - (back_.active ? 127 : 0) + mouseForward_, -128, 127); }
    int rightMove() const {
        const int turn = strafe_.active ? (right_.active ? 127 : 0) - (left_.active ? 127 : 0) : 0;
        return std::clamp((moveRight_.active ? 127 : 0) - (moveLeft_.active ? 127 : 0) + turn + mouseSide_, -128, 127);
    }
    // Key state of the turn and look buttons (CL_KeyState 0x408c60), the strafe / mlook / speed modifiers, for CL_AdjustAngles.
    // TODO(spec): [stays: sub-frame fraction] 0x408c60 returns the fraction of the frame a key was down; held keys give 1 here,
    // as for the movement keys above.
    float turnLeft() const { return left_.active ? 1.0f : 0.0f; }
    float turnRight() const { return right_.active ? 1.0f : 0.0f; }
    float lookUp() const { return lookUp_.active ? 1.0f : 0.0f; }
    float lookDown() const { return lookDown_.active ? 1.0f : 0.0f; }
    bool strafeHeld() const { return strafe_.active; }
    bool speedHeld() const { return speed_.active; }
    bool mlookHeld() const { return mlook_; }
    void setMouseMove(int side, int forward) { mouseSide_ = side, mouseForward_ = forward; }
    // centerview requested since the last call (the command, or -mlook; the caller drops the -mlook one when cl_freelook is set).
    bool takeCenterView(bool freelook) {
        const bool r = centerView_ || (mlookReleased_ && !freelook);
        centerView_ = mlookReleased_ = false;
        return r;
    }
};

} // namespace iw2::game
