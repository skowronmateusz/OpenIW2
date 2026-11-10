// Multiplayer chat, implemented from the spec in docs/spec/mp-chat.md: the say function of the game module (say /
// say_team client commands, sayAll / sayTeam methods), name and text cleaning, the client chat buffer.
#include "game/mpchat.h"

#include "game/mpgame.h"
#include "game/world.h"

#include <stdexcept>

namespace iw2::game {

using script::CallContext;
using script::Value;

std::string mpCleanString(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '^' && i + 1 < s.size() && s[i + 1] != '^' && s[i + 1] >= '0' && s[i + 1] <= '9') {
            ++i;
            continue;
        }
        if (c >= 0x20 && c <= 0x7e) out += c;
    }
    return out;
}

std::string mpCleanName(const std::string& in) {
    constexpr int kSize = 32;
    std::string out;
    int len = 0, spaces = 0;
    bool visible = false;
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (out.empty() && c == ' ') continue; // leading spaces
        if (c == '^') {
            if (i + 1 >= in.size()) break; // a trailing '^' ends the name
            const char n = in[++i];
            if (n == '0') continue;          // black is not allowed
            if (len > kSize - 1 - 2) break;  // the pair needs two bytes
            out += '^';
            out += n;
            len += 2;
            continue;
        }
        if (c == ' ') {
            if (++spaces > 3) continue; // more than three spaces in a row
        } else {
            spaces = 0;
        }
        if (len > kSize - 1 - 1) break;
        out += c;
        visible = true;
        ++len;
    }
    if (out.empty() || !visible) return "UnnamedPlayer";
    return out;
}

std::string mpChatReceived(const std::string& localized) {
    std::string out;
    for (size_t i = 0; i < localized.size() && i < 149; ++i)
        if (localized[i] != '\x19') out += localized[i];
    return out;
}

void MpChatBuffer::add(const std::string& msg, int64_t now, int height, int chatTime) {
    height = std::min(height, kMaxHeight);
    if (height <= 0 || chatTime <= 0) {
        pos_ = last_ = 0;
        return;
    }
    std::string* cur = &text_[pos_ % height];
    cur->clear();
    int count = 0;
    char colour = '7';
    size_t spaceOut = std::string::npos, spaceIn = 0;
    size_t i = 0;
    while (i < msg.size()) {
        if (count > kLineChars - 1) {
            if (spaceOut != std::string::npos) { // break at the last space, which is dropped
                cur->resize(spaceOut);
                i = spaceIn + 1;
            }
            time_[pos_ % height] = now;
            ++pos_;
            cur = &text_[pos_ % height];
            *cur = std::string("^") + colour;
            count = 0;
            spaceOut = std::string::npos;
            if (i >= msg.size()) break;
        }
        const char c = msg[i];
        if (c == '^' && i + 1 < msg.size() && msg[i + 1] != '^' && msg[i + 1] >= '0' && msg[i + 1] <= '9') {
            *cur += c;
            *cur += msg[i + 1];
            colour = msg[i + 1];
            i += 2;
            continue;
        }
        if (c == ' ') spaceOut = cur->size(), spaceIn = i;
        *cur += c;
        ++i;
        ++count;
    }
    time_[pos_ % height] = now;
    ++pos_;
    if (pos_ - last_ > height) last_ = pos_ - height;
}

std::vector<MpChatBuffer::Line> MpChatBuffer::frame(int64_t now, int height, int chatTime) {
    std::vector<Line> out;
    height = std::min(height, kMaxHeight);
    if (height <= 0 || last_ == pos_) return out;
    if (now - time_[last_ % height] > chatTime) ++last_;
    for (int i = pos_ - 1; i >= last_; --i) {
        const float remaining = float(chatTime) - float(now - time_[i % height]);
        const float alpha = remaining > 200.0f ? 1.0f : remaining * 0.005f;
        if (!(alpha > 0.0f)) continue;
        Line l;
        l.text = text_[i % height];
        l.alpha = alpha;
        if (l.text.size() >= 2 && l.text[0] == '^' && l.text[1] != '^' && l.text[1] >= '0' && l.text[1] <= '9') l.code = l.text[1] - '0';
        out.push_back(std::move(l));
    }
    return out;
}

namespace {

// Session team and state numbers of the client fields (team 1 axis, 2 allies, 3 spectator; state 0 playing).
int teamNumber(const std::string& t) { return t == "axis" ? 1 : t == "allies" ? 2 : t == "spectator" ? 3 : 0; }

std::string textOf(const Value& v) {
    if (std::holds_alternative<script::LocalizedString>(v.v)) return std::get<script::LocalizedString>(v.v).key;
    return v.toString();
}

} // namespace

// Say (0x502820) with no target; mode 0 all, 1 team (2, the private form, has no caller here).
void World::mpSay(int client, int mode, const std::string& text) {
    Entity* speaker = mpClientEntity(client);
    if (!speaker || mp_->clients[size_t(client)].state == MpClientState::Free) return;
    const int team = teamNumber(speaker->obj->get("sessionteam").toString());
    if (mode == 1 && team != 1 && team != 2) mode = 0;
    const std::string name = mpCleanString(mp_->clients[size_t(client)].name.substr(0, 63));
    const bool playing = speaker->obj->get("sessionstate").toString() == "playing";
    const std::string colour = team == 1 ? "^9" : team == 2 ? "^8" : "";
    std::string marker;
    if (team == 3) marker = "\x15(\x14GAME_SPECTATOR\x15)";
    else if (!playing) marker = "\x15" + colour + "(\x14GAME_DEAD\x15)";
    else marker = "\x15" + colour;
    marker = marker.substr(0, 63);
    std::string namePart;
    char textColour = '7';
    if (mode == 1) {
        mp_->log.push_back("sayteam;0;" + std::to_string(client) + ";" + name + ";" + text + "\n");
        namePart = marker + "(\x14" + (team == 1 ? "GAME_AXIS" : "GAME_ALLIES") + "\x15)" + name + "^7: ";
        textColour = '5';
    } else {
        mp_->log.push_back("say;0;" + std::to_string(client) + ";" + name + ";" + text + "\n");
        namePart = marker + name + "^7: ";
    }
    namePart = namePart.substr(0, 127);
    const std::string body = text.substr(0, 149);
    const std::string command = mode == 1 ? "i" : "h";
    const std::string message = "\x15" + namePart + "^" + textColour + body;
    const bool deadChat = [&] {
        const auto it = cvars.find("g_deadChat");
        return it != cvars.end() && std::atoi(it->second.c_str()) != 0;
    }();
    // Delivery (0x502750) to every client slot in order.
    for (size_t r = 0; r < mp_->clients.size(); ++r) {
        const MpClient& cl = mp_->clients[r];
        if (cl.state != MpClientState::Active || !cl.ent) continue;
        if (mode == 1) {
            const int rt = teamNumber(cl.ent->obj->get("sessionteam").toString());
            if (team == 0 || rt != team) continue;
        }
        if (!deadChat && !playing && cl.ent->obj->get("sessionstate").toString() == "playing") continue;
        mp_->chat.push_back({int(r), command + " " + message});
        if (int(r) == mp_->localClient) clientCommands_.push_back({command, message});
    }
}

void World::registerChatBuiltins() {
    auto speaker = [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        const int cl = e ? mpClientOf(*e) : -1;
        if (cl < 0) throw std::runtime_error("entity " + std::to_string(e ? e->number : -1) + " is not a player");
        return cl;
    };
    // sayAll / sayTeam (0x52dcc0 / 0x52dd90): the message (formatted like iprintln's) gets the 0x14 marker, so the
    // receiving client localizes it.
    auto say = [this, speaker](int mode) {
        return [this, speaker, mode](CallContext& c) {
            const int cl = speaker(c);
            std::string text = c.args.empty() ? std::string() : textOf(c.arg(0));
            for (size_t i = 1; i < c.args.size(); ++i) text += "\x1f" + c.arg(i).toString();
            mpSay(cl, mode, "\x14" + text);
            return Value();
        };
    };
    vm_.registerBuiltin("sayall", say(0));
    vm_.registerBuiltin("sayteam", say(1));
    // pingPlayer (0x52add0): TODO(spec) the compass ping of the player for its team; nothing here.
    vm_.registerBuiltin("pingplayer", [speaker](CallContext& c) {
        speaker(c);
        return Value();
    });
}

} // namespace iw2::game
