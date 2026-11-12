// The network client's weapon selection (docs/spec/net.md section 10 "Weapon selection"): the weapon its commands ask
// for. It changes by the player's input, by the server command `a <index>`, and from the player state's weapon only at
// a spawn, a new gamestate, with no selection yet, or when the selected weapon is no longer owned. A snapshot that
// still shows an older weapon never changes it.
#pragma once

#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "netgame/worldgame.h"

namespace iw2::netgame {

class NetWeaponSelection {
public:
    // The server command `a <index>` (index in the weapon list configstring). Returns true when it named a weapon and
    // `selection` was set to it.
    static bool serverCommand(const std::vector<std::string>& tokens, const std::map<int, std::string>& configstrings,
                              std::string& selection) {
        if (tokens.size() < 2 || tokens[0] != "a") return false;
        const int index = std::atoi(tokens[1].c_str());
        if (index <= 0 || index > kWeaponConfigstringCount) return false;
        const std::string name = weaponNameFromList(configstrings, index);
        if (name.empty()) return false;
        selection = name;
        return true;
    }

    // Once per frame with the newest snapshot: its pm_type, the gamestate count, the player state's weapon (empty: none)
    // and the owned weapons. Sets `selection` to the player state's weapon at a spawn or a new gamestate (once that
    // weapon is known), when there is no selection, or when the selection is no longer owned. Returns true when it
    // changed the selection.
    bool snapshot(int pmType, int gamestate, const std::string& psWeapon, const std::set<std::string>& owned,
                  std::string& selection) {
        if (gamestate != gamestate_ || (pmType == 0 && pmType_ != 0)) pending_ = true;
        gamestate_ = gamestate;
        pmType_ = pmType;
        if (pmType != 0 || psWeapon.empty()) return false;
        const bool lost = !selection.empty() && !owned.empty() && !owned.count(selection);
        if (!pending_ && !selection.empty() && !lost) return false;
        pending_ = false;
        if (selection == psWeapon) return false;
        selection = psWeapon;
        return true;
    }

    void clear() { *this = NetWeaponSelection{}; }

private:
    int gamestate_ = -1;
    int pmType_ = -1;
    bool pending_ = true;
};

} // namespace iw2::netgame
