// Localization of a server text message (cgame 0x496e80, used by the gm / gmb client commands 0x4a6bff / 0x4a6c3f): the
// message is a sequence of segments separated by the marker bytes 0x14, 0x15 and 0x16. A segment is localized (its text is a
// string reference) while the "localize" state is set; the state starts set, 0x14 sets it for the following segment and 0x15
// clears it (a literal). The first segment starts the result; every later one replaces the placeholder "&&N" of the result
// (N counting from 1, 0x4970bf) or, when the result has no such placeholder, is appended. Example: the item pickup sends
// `GAME_PICKUP_AMMO\x14<weapon displayName>` (0x58e080), giving the localized "Picked up &&1 ammo" with the localized weapon name.
// The port's own iprintln encoding (campaign.cpp) separates literal arguments with 0x1f; it is treated like 0x15.
// TODO(spec): the 0x16 marker's '%' handling (0x497021 / 0x4971b4) is only treated as a separator; the 1024-byte overflow
// error (0x496f82) and the "could not translate" error (0x4971d0) keep the raw reference instead.
#pragma once

#include <functional>
#include <string>

namespace iw2::game {

inline std::string localizeTextMessage(const std::string& msg, const std::function<const std::string*(const std::string&)>& find) {
    std::string out;
    bool localize = true, first = true;
    int argN = 1;
    size_t i = 0;
    while (i <= msg.size()) {
        size_t j = i;
        while (j < msg.size() && msg[j] != '\x14' && msg[j] != '\x15' && msg[j] != '\x16' && msg[j] != '\x1f') ++j;
        if (j > i) {
            const std::string seg = msg.substr(i, j - i);
            const std::string* t = localize && find ? find(seg) : nullptr;
            const std::string s = t ? *t : seg;
            if (first) {
                out = s;
                first = false;
            } else {
                const std::string mark = "&&" + std::to_string(argN);
                if (const size_t k = out.find(mark); k != std::string::npos) {
                    out.replace(k, mark.size(), s);
                    ++argN;
                } else {
                    out += s;
                }
            }
        }
        if (j >= msg.size()) break;
        if (msg[j] == '\x14') localize = true;
        else if (msg[j] == '\x15' || msg[j] == '\x1f') localize = false;
        i = j + 1;
    }
    return out;
}

} // namespace iw2::game
