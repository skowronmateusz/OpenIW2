// Player-state events of the local player (footsteps, jumps, landings): the sounds the cgame starts for them
// (CG_EntityEvent 0x4ba8d0) and the fall damage the game applies (ClientEvents 0x4e75f0).
#pragma once

#include <string>
#include <vector>

namespace iw2::game {

// Surface type names in the order of the engine's table (0x5a0358; index 0 "default"), used as sound alias suffixes.
const char* surfaceAliasName(int surface);

// The "surfacetype" string of a bullettrace result (0x4f2230): "none" when nothing was hit (fraction 1), else the
// surface type of the hit material, (surfaceFlags >> 20) & 0x1f, named from the engine's table (0x5a0344) for
// types 1..22 and "default" for 0 and anything above.
const char* bulletTraceSurfaceType(float fraction, unsigned surfaceFlags);

// The sound aliases the cgame starts, in order, for the own player's event `ev` (the "_plr" families of the
// registration at 0x4a27b0, "%s_%s" with the surface name):
//   1..0x17 step_run_plr_<surface>, 0x18..0x2e step_walk_plr_<surface>, 0x2f..0x45 step_prone_plr_<surface>
//   (all three only with cg_footsteps), each followed by gear_rattle_plr_run / _walk / _walk;
//   0x46..0x5c jumps: step_run_plr_<surface> and gear_rattle_plr_run;
//   0x5d..0x73 landings: land_plr_<surface>;
//   0x74..0x8a hard landings: land_plr_<surface> and land_damage.
// Other events give no sound. `footsteps` is the cg_footsteps dvar (default 1).
// `local` is the cgame's own-player test of the event handler (MP 0x4e09c9: a first-person snapshot, flags 0xc00000,
// and the event's entity is the player state's clientNum); any other player's event uses the same families without
// "_plr" (step_run_<surface>, step_walk_, step_prone_, land_<surface>, gear_rattle_run / _walk, the land_damage of a hard
// landing; tables registered at MP 0x4bf9f0..0x4bfaf6, chosen at MP 0x4e0a64..0x4e0ca2), started at that entity.
std::vector<std::string> playerEventSounds(int ev, bool footsteps, bool local = true);

// The player-state events the server copies into the player's entity event ring (MP server 0x80e00d2..0x80e01b7 in
// the 1.3 Linux dedicated server: every new event of the player state's ring, with its parameter, except those of the
// list at 0x815486c: 0x8c..0x8f and 0xb9 / 0xba).
bool playerStateEventSentToEntity(int ev);

// ClientEvents (0x4e75f0), events 0x74..0x8a with the damage percent as parameter: the damage is
// trunc(maxHealth * factor), factor 1.1 from 100 percent up (0x56fe28), else percent * 0.01 (0x591ea0); 0 when the
// factor is 0.
int fallDamageAmount(int maxHealth, int percent);

} // namespace iw2::game
