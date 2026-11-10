// Multiplayer HUD feedback: obituary lines, head icons, crosshair names (docs/spec/mp-hud.md). Implemented from the spec.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace iw2::game {

// Client team numbers (client info +0x2c): 0 none (DM), 1 axis, 2 allies, 3 spectator.
inline int mpTeamNumber(const std::string& team) {
    if (team == "axis") return 1;
    if (team == "allies") return 2;
    if (team == "spectator") return 3;
    return 0;
}

// Means-of-death names by index (lnxded table 0x8187c40; docs/spec/mp.md).
int mpModIndex(const std::string& mod);

// One obituary event as the server sends it (0x8113128): victim and attacker client numbers (attacker -1 for none) and
// the event byte: bit 0x80 with the means-of-death index for melee, head shot, crush, falling and suicide, else the
// weapon name (the weapon index on the wire).
struct MpObituary {
    int victim = -1, attacker = -1;
    bool modByte = false;
    int mod = 0;
    std::string weapon;
    // A network client: the names and teams of the client states (client info) at the event; the puppets may already
    // be gone from the snapshot (a dead victim).
    bool hasClientInfo = false;
    std::string victimName, attackerName;
    int victimTeam = 0, attackerTeam = 0;
};

// The server's choice of the event byte (0x81131eb..0x8113224).
MpObituary mpMakeObituary(int victim, int attacker, const std::string& weapon, const std::string& mod);

// The weapon fields the client reads for the icon (weapon def +0x34c / +0x350 / +0x354).
struct KillIconInfo {
    std::string killIcon;
    bool wide = false, flip = false;
};

// The client's line (0x4e03f0): the attacker text (empty when there is none or it is the victim), the victim text, each
// with a team number for its colour, the icon material with its width / height in line heights, the flip, and the
// centre print for the local client ("" when none) with its argument.
struct ObituaryLine {
    std::string attacker, victim;
    int attackerTeam = 0, victimTeam = 0;
    std::string icon;
    float iconW = 1.4f, iconH = 1.4f;
    bool flip = false;
    std::string centerPrint; // localization key: CGAME_YOUKILLED / CGAME_YOUWEREKILLED
    std::string centerName;  // the other player's name
    bool teammate = false;   // the CGAME_TEAMMATE form
    // The centre print as a text message for the localizer (0x14 = localized segment, 0x15 = literal): the key, then the
    // name; the teammate form "^1&&2^7 <name>" with CGAME_TEAMMATE as the second argument (formats 0x5b6c58..0x5b6ca8).
    std::string centerText;
};

ObituaryLine obituaryLine(const MpObituary& ev, const KillIconInfo& weapon, const std::string& victimName, int victimTeam,
                          const std::string& attackerName, int attackerTeam, int localClient);

// Team colour of a name (0x4d9e50): axis g_TeamColor_Axis, allies g_TeamColor_Allies, else white. The arguments are the
// two dvar values ("r g b a"; empty uses the defaults registered by the server, 0x8108acb..0x8108b3b).
std::array<float, 4> mpTeamColor(int team, const std::string& axisDvar = {}, const std::string& alliesDvar = {});

// Whether a script head icon shows for the local client (0x4dbcc0): headiconteam none, the local client a spectator,
// or the same team.
inline bool mpHeadIconVisible(int iconTeam, int localTeam) { return iconTeam == 0 || localTeam == 3 || iconTeam == localTeam; }

// Whether the crosshair name of a client shows (0x4c97c0): the local team is not 0 and the target is a teammate or the
// local client is a spectator.
inline bool mpCrosshairNameVisible(int localTeam, int targetTeam) {
    return localTeam != 0 && (localTeam == 3 || targetTeam == localTeam);
}

// Crosshair name alpha (0x4c2860 with 150 / 100 ms): ms since the last hit of the trace; < 0 when not drawn.
inline float mpCrosshairNameAlpha(int64_t sinceMs) {
    if (sinceMs < 0 || sinceMs >= 150) return -1.0f;
    const int64_t left = 150 - sinceMs;
    return left < 100 ? float(left) / 100.0f : 1.0f;
}

} // namespace iw2::game
