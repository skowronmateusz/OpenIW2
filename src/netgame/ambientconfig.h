// The client side of the ambient configstring (docs/spec/sound.md "Ambient on a network client"), implemented from the
// spec: the value "n\<alias>\t\<end time>" plays a track, "t\<end time>" stops it, the fade is what is left to the time.
#pragma once

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>

namespace iw2::netgame {

struct AmbientChange {
    bool play = false;
    std::string alias;
    int fadeMs = 0;
};

// Parsed value. An empty value and one without a track are a stop. The fade is the end time minus the server time, at
// least 0: a client that joins after the fade (the value of the gamestate) starts at full volume.
inline AmbientChange parseAmbientConfig(const std::string& value, int serverTimeMs) {
    AmbientChange c;
    std::string alias, time;
    for (size_t i = 0; i < value.size();) {
        size_t k = value.find('\\', i);
        if (k == std::string::npos) break;
        size_t v = value.find('\\', k + 1);
        const std::string key = value.substr(i, k - i), val = value.substr(k + 1, v == std::string::npos ? v : v - k - 1);
        if (key == "n") alias = val;
        else if (key == "t") time = val;
        if (v == std::string::npos) break;
        i = v + 1;
    }
    c.play = !alias.empty();
    c.alias = alias;
    c.fadeMs = time.empty() ? 0 : std::max(0, std::atoi(time.c_str()) - serverTimeMs);
    return c;
}

// Reports a change once per new value of the configstring.
class AmbientConfigTracker {
public:
    std::optional<AmbientChange> update(const std::string& value, int serverTimeMs) {
        if (seen_ && value == last_) return std::nullopt;
        const bool first = !seen_;
        seen_ = true;
        last_ = value;
        AmbientChange c = parseAmbientConfig(value, serverTimeMs);
        if (first && !c.play) return std::nullopt; // no track on the map: nothing to stop
        return c;
    }
    void reset() { *this = AmbientConfigTracker{}; }

private:
    bool seen_ = false;
    std::string last_;
};

} // namespace iw2::netgame
