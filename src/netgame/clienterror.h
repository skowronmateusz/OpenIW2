// The error dialog after a network disconnect (docs/spec/net.md section 14), implemented from the spec.
#pragma once

#include <string>
#include <strings.h>

#include "game/screenmessages.h"

namespace iw2::netgame {

struct ErrorDialog {
    std::string titleKey; // MENU_NOTICE or MENU_ERROR (localized into com_errorTitle)
    std::string message;  // com_errorMessage before localization: 0x14 marks a localization key (SEH text message)
};

// serverDisconnect: the server sent `w`; otherwise the connection was refused or lost.
inline ErrorDialog errorDialog(bool serverDisconnect, const std::string& reason) {
    static const char* const kNotice[] = {"EXE_SERVER_DISCONNECTED", "EXE_DISCONNECTED", "EXE_SERVERISFULL"}; // 0x5cef94
    bool notice = serverDisconnect;
    for (const char* k : kNotice) notice = notice || strcasecmp(k, reason.c_str()) == 0;
    return {notice ? "MENU_NOTICE" : "MENU_ERROR", game::printMessageText(reason)};
}

// CL_Disconnect's main menu is the multiplayer one when the game left was a multiplayer game. A `--connect` start has
// the single-player menu set loaded until then (its "Resume game" belongs to single player).
inline bool mpMenuAfterDisconnect(bool mpFrontEndLoaded, bool leftMultiplayer) { return leftMultiplayer && !mpFrontEndLoaded; }

} // namespace iw2::netgame
