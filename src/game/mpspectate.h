// Multiplayer spectating (docs/spec/mp-spectate.md). Implemented from the spec.
#pragma once

#include <string>

namespace iw2::game {

// Session state numbers of the client (setter 0x80f7efc): 0 playing, 1 dead, 2 spectator, 3 intermission.
int mpSessionStateNumber(const std::string& sessionstate);

// The MP movement type of a client by session state (ClientThink / ClientEndFrame of the MP game): 0 playing,
// 4 spectator, 5 intermission, 6 dead. These are MP numbers; the SP port's pm_type 4 / 5 are its dead types.
int mpClientPmType(int sessionState);

// Speed of the free spectator camera: ps speed 400 with free look allowed, else 0 (0x80f4eb2), times
// player_spectateSpeedScale (default 2, 0..20) in the command scale for pm_type 4 (0x515810, 0x5158e0).
inline float mpSpectatorSpeed(bool freelook, float spectateSpeedScale = 2.0f) { return freelook ? 400.0f * spectateSpeedScale : 0.0f; }

} // namespace iw2::game
