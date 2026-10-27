// Player-state events of the local player (footsteps, jumps, landings): the sounds the cgame starts for them
// (CG_EntityEvent 0x4ba8d0) and the fall damage the game applies (ClientEvents 0x4e75f0).
#pragma once

#include <string>
#include <vector>

namespace iw2::game {

// Surface type names in the order of the engine's table (0x5a0358; index 0 "default"), used as sound alias suffixes.
const char* surfaceAliasName(int surface);

// The sound aliases the cgame starts, in order, for the own player's event `ev` (the "_plr" families of the
// registration at 0x4a27b0, "%s_%s" with the surface name):
//   1..0x17 step_run_plr_<surface>, 0x18..0x2e step_walk_plr_<surface>, 0x2f..0x45 step_prone_plr_<surface>
//   (all three only with cg_footsteps), each followed by gear_rattle_plr_run / _walk / _walk;
//   0x46..0x5c jumps: step_run_plr_<surface> and gear_rattle_plr_run;
//   0x5d..0x73 landings: land_plr_<surface>;
//   0x74..0x8a hard landings: land_plr_<surface> and land_damage.
// Other events give no sound. `footsteps` is the cg_footsteps dvar (default 1).
std::vector<std::string> playerEventSounds(int ev, bool footsteps);

// ClientEvents (0x4e75f0), events 0x74..0x8a with the damage percent as parameter: the damage is
// trunc(maxHealth * factor), factor 1.1 from 100 percent up (0x56fe28), else percent * 0.01 (0x591ea0); 0 when the
// factor is 0.
int fallDamageAmount(int maxHealth, int percent);

} // namespace iw2::game
