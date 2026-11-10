// Localization of a server text message (cgame 0x496e80, used by the gm / gmb client commands 0x4a6bff / 0x4a6c3f): the
// message is a sequence of segments separated by the marker bytes 0x14, 0x15 and 0x16. A segment is localized (its text is a
// string reference) while the "localize" state is set; the state starts set, 0x14 sets it for the following segment and 0x15
// clears it (a literal). The first segment starts the result; every later one replaces the placeholder "&&N" of the result
// (N counting from 1, 0x4970bf) or, when the result has no such placeholder, is appended. Example: the item pickup sends
// `GAME_PICKUP_AMMO\x14<weapon displayName>` (0x58e080), giving the localized "Picked up &&1 ammo" with the localized weapon name.
// Hint strings and iprintln use the same reader. The port's objective text separates its arguments with 0x1f; it is treated like 0x15.
// 0x497021 / 0x4971b4: while a marker flag is clear the first "&&<digit>" of a piece has its '&' parked as 0x16, shown as '%' at the end. Only
// a string holding such a placeholder where no argument follows reaches this; the 0x16 byte itself is never built by the engine (buildHintString rejects it).
// TODO(spec): the 1024-byte overflow error (0x496f82) and the "could not translate" error (0x4971d0) keep the raw reference instead.
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

// A hud element's label joined with its value (number, timer or text): the label's text up to "&&1", the value, then the
// rest of the label after "&&1"; a label without "&&1" is followed directly by the value, with no separator added
// (VAs in the patch 1.3 MP executable 0x4dd330, called from 0x4dd430 when both strings are non-empty; at most 255
// characters). A space between them comes only from the localized label itself: the English MP_TIME_TILL_SPAWN ends
// with one, the Polish text does not, so the original also shows the Polish respawn timer without a gap.
inline std::string hudElemText(const std::string& label, const std::string& value) {
    if (label.empty()) return value;
    if (value.empty()) return label;
    const size_t at = label.find("&&1");
    std::string out = at == std::string::npos ? label + value : label.substr(0, at) + value + label.substr(at + 3);
    if (out.size() > 255) out.resize(255);
    return out;
}

} // namespace iw2::game
