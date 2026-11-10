// Multiplayer map rotation: the map_rotate console command that exitLevel queues (docs/spec/mp.md, "End of the map").
// Implemented from the spec; no World dependency, the cvars come through two callbacks.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace iw2::game {

struct MapRotateResult {
    enum class Kind {
        Map,     // "map <map>" runs: a new map with the (possibly changed) g_gametype
        Restart, // no map in sv_mapRotation: fast_restart of the current map
    };
    Kind kind = Kind::Restart;
    std::string map;                  // Kind::Map: the bare map name
    bool gametypeChanged = false;     // a "gametype" keyword changed g_gametype while the server ran (persist off)
    std::vector<std::string> console; // the lines the command prints
};

using RotationGet = std::function<std::string(const std::string&)>;
using RotationSet = std::function<void(const std::string&, const std::string&)>;

// map_rotate: reads sv_mapRotation / sv_mapRotationCurrent / g_gametype, consumes the next entries of
// sv_mapRotationCurrent and sets g_gametype for each "gametype" keyword.
MapRotateResult mapRotate(const RotationGet& get, const RotationSet& set);

// The token reader of the rotation string: whitespace-separated words or quoted strings; returns false at the end.
// `rest` becomes the text after the token.
bool rotationToken(const std::string& text, std::string& token, std::string& rest);

} // namespace iw2::game
