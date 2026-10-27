// Script menus: precachemenu / openmenu / openmenunomouse / closemenu / freezecontrols and the "menuresponse" notify
// (docs/spec/ui.md "Script menus"). The menu itself lives in the front end (ui/scriptmenus/<name>.menu).
#include "game/world.h"

#include <cctype>
#include <cstdlib>

namespace iw2::game {

using namespace script;

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

void World::registerMenuBuiltins() {
    // precachemenu(name) (0x4f8860): the name goes into the first free of 32 configstring slots 0x4ad..0x4cc after a
    // case-insensitive search for it; a menu that is already there only prints a message (0x585264), the index stays.
    // TODO(spec): the original errors outside level load (0x5852a0); that check is not reproduced.
    vm_.registerBuiltin("precachemenu", [this](CallContext& c) {
        const std::string name = c.arg(0).toString();
        for (const auto& m : menus_)
            if (lower(m) == lower(name)) return Value();
        if (menus_.size() >= 32) throw std::runtime_error("precachemenu: no free menu slot for " + name);
        menus_.push_back(name);
        return Value();
    });
    // Client-side "popupopen <index>" / "popupopen <index> 1" (openmenunomouse) and "popupclose" (0x4a7094 dispatches
    // them on the client; the menu file is loaded from "ui/scriptmenus/" + name, 0x4c3ad0). The entity must be the
    // player (client present, else "entity %i is not a player", 0x590ab8); a client not in state 2 sends nothing.
    auto playerOf = [this](CallContext& c) -> Entity& {
        Entity* e = entityOf(c.self);
        if (!e || !e->isPlayer) throw std::runtime_error("entity " + std::to_string(e ? e->number : -1) + " is not a player");
        return *e;
    };
    auto openMenu = [this, playerOf](CallContext& c, bool mouse) {
        playerOf(c);
        const std::string name = c.arg(0).toString();
        int index = -1;
        for (size_t i = 0; i < menus_.size(); ++i)
            if (lower(menus_[i]) == lower(name)) index = int(i);
        // TODO(spec): the error text of the name lookup (0x4f89f0) for a menu that was not precached.
        if (index < 0) throw std::runtime_error("openmenu: menu " + name + " was not precached");
        clientCommands_.push_back({"popupopen", std::to_string(index) + (mouse ? "" : " 1")});
        return Value();
    };
    vm_.registerBuiltin("openmenu", [openMenu](CallContext& c) { return openMenu(c, true); });
    vm_.registerBuiltin("openmenunomouse", [openMenu](CallContext& c) { return openMenu(c, false); });
    vm_.registerBuiltin("closemenu", [this, playerOf](CallContext& c) {
        playerOf(c);
        clientCommands_.push_back({"popupclose", ""});
        return Value();
    });
    // freezecontrols(on) (0x5392e0) stores the argument in client +0x8630; ClientThink (0x4e7bc0) then sets ps pm_flags
    // 0x8000.
    // TODO(spec): the exact pmove effect of pm_flags 0x8000; the front end ignores movement and buttons meanwhile.
    vm_.registerBuiltin("freezecontrols", [this, playerOf](CallContext& c) {
        playerOf(c);
        controlsFrozen_ = c.arg(0).truthy();
        return Value();
    });
}

// Client command "mr" (0x4ea700): the menu index selects the precached name (indices outside 0..31 leave the name empty),
// the response text follows; both go to the script as the arguments of the player's "menuresponse" notify.
void World::menuResponse(int menuIndex, const std::string& response) {
    if (!player_) return;
    const std::string name = menuIndex >= 0 && size_t(menuIndex) < menus_.size() ? menus_[size_t(menuIndex)] : std::string();
    vm_.notify(player_->obj, "menuresponse", {Value(name), Value(response)});
}

} // namespace iw2::game
