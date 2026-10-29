#include "game/clientinput.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace iw2::game {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Splits a cfg line into tokens; a quoted string is one token, // starts a comment.
std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= line.size()) break;
        if (line.compare(i, 2, "//") == 0) break;
        if (line[i] == '"') {
            const size_t e = line.find('"', i + 1);
            out.push_back(line.substr(i + 1, e == std::string::npos ? std::string::npos : e - i - 1));
            i = e == std::string::npos ? line.size() : e + 1;
        } else {
            size_t e = i;
            while (e < line.size() && !std::isspace(static_cast<unsigned char>(line[e]))) ++e;
            out.push_back(line.substr(i, e - i));
            i = e;
        }
    }
    return out;
}

struct KeyName {
    const char* name;
    int num;
};
// The key-name table at 0x59a0e0 ({name, key number} pairs up to the null entry); the 128..147 entries are named by the
// decimal code of the extended character.
const KeyName kKeyNames[] = {{"TAB", 9}, {"ENTER", 13}, {"ESCAPE", 27}, {"SPACE", 32}, {"BACKSPACE", 127}, {"UPARROW", 154}, {"DOWNARROW", 155}, {"LEFTARROW", 156}, {"RIGHTARROW", 157}, {"ALT", 158}, {"CTRL", 159}, {"SHIFT", 160}, {"CAPSLOCK", 151}, {"F1", 167}, {"F2", 168}, {"F3", 169}, {"F4", 170}, {"F5", 171}, {"F6", 172}, {"F7", 173}, {"F8", 174}, {"F9", 175}, {"F10", 176}, {"F11", 177}, {"F12", 178}, {"INS", 161}, {"DEL", 162}, {"PGDN", 163}, {"PGUP", 164}, {"HOME", 165}, {"END", 166}, {"MOUSE1", 200}, {"MOUSE2", 201}, {"MOUSE3", 202}, {"MOUSE4", 203}, {"MOUSE5", 204}, {"MWHEELUP", 206}, {"MWHEELDOWN", 205}, {"AUX1", 207}, {"AUX2", 208}, {"AUX3", 209}, {"AUX4", 210}, {"AUX5", 211}, {"AUX6", 212}, {"AUX7", 213}, {"AUX8", 214}, {"AUX9", 215}, {"AUX10", 216}, {"AUX11", 217}, {"AUX12", 218}, {"AUX13", 219}, {"AUX14", 220}, {"AUX15", 221}, {"AUX16", 222}, {"KP_HOME", 182}, {"KP_UPARROW", 183}, {"KP_PGUP", 184}, {"KP_LEFTARROW", 185}, {"KP_5", 186}, {"KP_RIGHTARROW", 187}, {"KP_END", 188}, {"KP_DOWNARROW", 189}, {"KP_PGDN", 190}, {"KP_ENTER", 191}, {"KP_INS", 192}, {"KP_DEL", 193}, {"KP_SLASH", 194}, {"KP_MINUS", 195}, {"KP_PLUS", 196}, {"KP_NUMLOCK", 197}, {"KP_STAR", 198}, {"KP_EQUALS", 199}, {"PAUSE", 153}, {"SEMICOLON", 59}, {"COMMAND", 150}, {"181", 128}, {"191", 129}, {"223", 130}, {"224", 131}, {"225", 132}, {"228", 133}, {"229", 134}, {"230", 135}, {"231", 136}, {"232", 137}, {"233", 138}, {"236", 139}, {"241", 140}, {"242", 141}, {"243", 142}, {"246", 143}, {"248", 144}, {"249", 145}, {"250", 146}, {"252", 147}};

// 0x43f6d0: a case-insensitive comparison (ASCII letters only) that stops at the end of both strings.
bool nameEquals(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) return false;
    return i == a.size() && !b[i];
}

int hexDigit(char c) { // 0x40b8c2..0x40b911: a decimal digit or a-f (lowercase); anything else counts as 0
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

// The name the port stores a key under for a key number.
std::string portKeyName(int num) {
    for (const KeyName& k : kKeyNames)
        if (k.num == num) return upper(k.name);
    if (num > 0 && num < 128) return upper(std::string(1, char(num)));
    return std::string();
}

} // namespace

int ClientInput::keyNumber(const std::string& name) {
    if (name.empty()) return -1;
    if (name.size() == 1) return int(static_cast<signed char>(name[0]));
    if (name.size() == 4 && name[0] == '0' && name[1] == 'x') return hexDigit(name[2]) * 16 + hexDigit(name[3]);
    for (const KeyName& k : kKeyNames)
        if (nameEquals(name, k.name)) return k.num;
    return -1;
}

bool ClientInput::keyDown(const std::string& name) const {
    const int n = keyNumber(name);
    return n >= 0 && keysDown_.count(portKeyName(n)) != 0;
}

std::string ClientInput::commandFromKey(const std::string& name) const {
    const int n = keyNumber(name);
    if (n < 0) return std::string();
    const auto it = binds_.find(portKeyName(n));
    return it == binds_.end() ? std::string() : it->second;
}

void ClientInput::loadConfig(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const auto t = tokens(line);
        if (t.empty()) continue;
        const std::string cmd = lower(t[0]);
        if (cmd == "unbindall") binds_.clear(), seq_.clear();
        else if (cmd == "bind" && t.size() >= 3) binds_[upper(t[1])] = t[2], seq_[upper(t[1])] = nextSeq_++;
        else if (cmd == "unbind" && t.size() >= 2) binds_.erase(upper(t[1])), seq_.erase(upper(t[1]));
    }
}

std::vector<std::string> ClientInput::keysFor(const std::string& command) const {
    std::vector<std::string> keys;
    for (const auto& [k, c] : binds_)
        if (lower(c) == lower(command)) keys.push_back(k);
    std::sort(keys.begin(), keys.end(), [&](const std::string& a, const std::string& b) { return seq_.at(a) < seq_.at(b); });
    return keys;
}

void ClientInput::rebind(const std::string& command, const std::string& rawKey) {
    const std::string key = upper(rawKey);
    if (key.empty()) { // exe 0x4ce1ea: clear both slots
        for (const auto& k : keysFor(command)) binds_.erase(k), seq_.erase(k);
        return;
    }
    binds_.erase(key), seq_.erase(key); // 0x4ce1a0: the key leaves any other binding (the other slot moves up)
    const auto cur = keysFor(command);
    if (cur.size() >= 2) // both slots taken: replace by this key alone (0x4ce237)
        for (const auto& k : cur) binds_.erase(k), seq_.erase(k);
    binds_[key] = command, seq_[key] = nextSeq_++; // slot 0 or slot 1 free (0x4ce21a / 0x4ce22f)
}

std::string ClientInput::formatBindings() const {
    std::vector<std::pair<int, std::string>> order;
    for (const auto& [k, c] : binds_) order.emplace_back(seq_.at(k), k);
    std::sort(order.begin(), order.end());
    std::string out = "unbindall\n";
    for (const auto& [n, k] : order) out += "bind " + k + " \"" + binds_.at(k) + "\"\n";
    return out;
}

void ClientInput::key(const std::string& keyName, bool down) {
    if (down) keysDown_.insert(upper(keyName)); // the key table's down flag exists for unbound keys too (0x40c0fd)
    else keysDown_.erase(upper(keyName));
    auto it = binds_.find(upper(keyName));
    if (it == binds_.end()) return;
    const std::string& c = it->second;
    if (!c.empty() && c[0] == '+') command(c, down, upper(keyName));
    else if (down) command(c, true, upper(keyName));
}

// IN_KeyDown / IN_KeyUp (0x408b50 / 0x408bd0): a button stays down while any of its two keys holds it; the first
// press sets active and wasPressed.
void ClientInput::press(KButton& b, const std::string& key) {
    if (b.down[0] == key || b.down[1] == key) return;
    if (b.down[0].empty()) b.down[0] = key;
    else if (b.down[1].empty()) b.down[1] = key;
    else return; // "Three keys down for a button!"
    if (!b.active) b.active = b.wasPressed = true;
}

void ClientInput::release(KButton& b, const std::string& key) {
    if (key.empty()) b.down[0].clear(), b.down[1].clear(); // typed in the console: releases all
    else if (b.down[0] == key) b.down[0].clear();
    else if (b.down[1] == key) b.down[1].clear();
    else return;
    if (!b.down[0].empty() || !b.down[1].empty()) return;
    b.active = false;
}

void ClientInput::command(const std::string& cmdIn, bool down, const std::string& keyName) {
    std::string name = lower(cmdIn);
    if (!name.empty() && (name[0] == '+' || name[0] == '-')) {
        if (name[0] == '-') down = false;
        name = name.substr(1);
        const std::map<std::string, KButton*> buttons{
            {"attack", &attack_},       {"melee", &melee_},         {"holdbreath", &holdBreath_},
            {"frag", &frag_},           {"smoke", &smoke_},         {"binoculars", &binoculars_},
            {"activate", &activate_},   {"reload", &reload_},       {"usereload", &useReload_},
            {"leanleft", &leanLeft_},   {"leanright", &leanRight_}, {"speed", &speed_},
            {"forward", &forward_},     {"back", &back_},           {"moveleft", &moveLeft_},
            {"moveright", &moveRight_}};
        auto act = [&](KButton& b) { down ? press(b, keyName) : release(b, keyName); };
        if (name == "scores") { objectivesHeld_ = down; return; } // cgame 0x4a4d40 (+scores) / 0x4a4d20 (-scores): the objectives screen
        if (auto it = buttons.find(name); it != buttons.end()) return act(*it->second);
        if (name == "melee_breath") { act(melee_); act(holdBreath_); return; }          // 0x408f70
        if (name == "breath_binoculars") { act(holdBreath_); act(binoculars_); return; } // 0x409010
        if (name == "prone") { // 0x409110: hold to lie down
            act(prone_);
            return;
        }
        if (name == "gostand") { // 0x4093a0: stands up; when already standing it jumps
            if (!down) { release(goStand_, keyName); release(jump_, keyName); return; }
            press(goStand_, keyName);
            if (stance_ == 0) press(jump_, keyName);
            else if (!stanceLocked()) stance_ = 0;
            return;
        }
        if (name == "moveup") { // 0x408d00: one stance up, jump when standing
            if (!down) { release(goStand_, keyName); release(jump_, keyName); return; }
            press(goStand_, keyName);
            if (stanceLocked()) return;
            if (stance_ > 1) stance_ = 1;
            else if (stance_ > 0) stance_ = 0;
            else press(jump_, keyName);
            return;
        }
        if (name == "movedown") { act(moveDown_); return; } // 0x408d70: hold to crouch
        return;
    }
    if (!down) return;
    if (name == "toggleads") adsToggle_ = !adsToggle_; // 0x409260
    else if (name == "leaveads") adsToggle_ = false; // 0x409270
    else if (stanceLocked()) return;                 // the stance commands below do nothing while +prone / +movedown
    else if (name == "gocrouch") stance_ = 1;        // 0x409380
    else if (name == "goprone") stance_ = 2;         // 0x409360
    else if (name == "togglecrouch") stance_ = stance_ != 1 ? 1 : 0; // 0x409300
    else if (name == "toggleprone") stance_ = stance_ == 2 ? 0 : 2;  // 0x409330
    else if (name == "lowerstance") stance_ = std::min(stance_ + 1, 2); // 0x409280
    else if (name == "raisestance") stance_ = std::max(stance_ - 1, 0); // 0x4092c0
    if (name == "weapnext") cycle_ = 1;  // cgame 0x4b6fd0
    if (name == "weapprev") cycle_ = -1; // cgame 0x4b7030
}

uint32_t ClientInput::buttons(bool inVehicle, bool forcedAds) {
    uint32_t b = 0;
    auto take = [&](KButton& k, uint32_t bit) {
        if (held(k)) b |= bit;
        k.wasPressed = false;
    };
    take(attack_, Button::Attack);
    take(holdBreath_, Button::HoldBreath);
    take(frag_, Button::Frag);
    take(smoke_, Button::Smoke);
    take(binoculars_, Button::Binoculars);
    take(melee_, Button::Melee);
    take(activate_, Button::Activate);
    take(reload_, Button::Reload);
    take(useReload_, Button::UseReload);
    take(leanLeft_, Button::LeanLeft);
    take(leanRight_, Button::LeanRight);
    take(jump_, Button::Jump);
    if (inVehicle) take(goStand_, Button::Jump);
    else goStand_.wasPressed = false;
    // Stance (0x409500): +prone / +movedown held win over the stance setting and add 0x2000.
    b &= ~(Button::Prone | Button::Crouch | Button::StanceHeld);
    if (prone_.active) b |= Button::Prone | Button::StanceHeld;
    else if (moveDown_.active) b |= Button::Crouch | Button::StanceHeld;
    else if (stance_ == 1) b |= Button::Crouch;
    else if (stance_ == 2) b |= Button::Prone;
    prone_.wasPressed = moveDown_.wasPressed = false;
    // Aim (0x40959d): +speed held XOR the toggleads state.
    if (speed_.active != adsToggle_ || forcedAds) b |= Button::Ads;
    return b;
}

} // namespace iw2::game
