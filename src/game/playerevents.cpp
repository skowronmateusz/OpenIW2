#include "game/playerevents.h"

#include "game/world.h"

#include <cstdlib>

namespace iw2::game {

namespace {
const char* const kNames[23] = {"default", "bark", "brick", "carpet", "cloth", "concrete", "dirt", "flesh",
                                "foliage", "glass", "grass", "gravel", "ice", "metal", "mud", "paper",
                                "plaster", "rock", "sand", "snow", "water", "wood", "asphalt"};
}

const char* surfaceAliasName(int surface) { return surface >= 0 && surface < 23 ? kNames[surface] : "default"; }

std::vector<std::string> playerEventSounds(int ev, bool footsteps) {
    std::vector<std::string> out;
    auto with = [&](const char* family, int base) { out.push_back(std::string(family) + "_" + surfaceAliasName(ev - base)); };
    if (ev >= 1 && ev < 0x18) {
        if (footsteps) with("step_run_plr", 1);
        out.push_back("gear_rattle_plr_run");
    } else if (ev >= 0x18 && ev < 0x2f) {
        if (footsteps) with("step_walk_plr", 0x18);
        out.push_back("gear_rattle_plr_walk");
    } else if (ev >= 0x2f && ev < 0x46) {
        if (footsteps) with("step_prone_plr", 0x2f);
        out.push_back("gear_rattle_plr_walk");
    } else if (ev >= 0x46 && ev < 0x5d) {
        with("step_run_plr", 0x46);
        out.push_back("gear_rattle_plr_run");
    } else if (ev >= 0x5d && ev < 0x74) {
        with("land_plr", 0x5d);
    } else if (ev >= 0x74 && ev < 0x8b) {
        with("land_plr", 0x74);
        out.push_back("land_damage");
    } else if (ev == 0x8b) {
        // 0x4bac0b (jump table 0x4bb74c entry 0): only the alias loaded at 0x4a29f0 ("movement_foliage"), no footsteps gate.
        // The generators: PM_FoliageSounds 0x50ac30 (pmove.cpp foliageSounds) for the player and 0x548650
        // (World::actorFoliageSound) for actors; dvars bg_foliagesnd_* (0x1c7efb4 .. 0x1c7f03c, see docs/spec/ai.md).
        out.push_back("movement_foliage");
    }
    return out;
}

int fallDamageAmount(int maxHealth, int percent) {
    const double factor = percent >= 100 ? double(1.1f) : double(percent) * double(0.01f);
    if (factor == 0.0) return 0;
    return int(double(maxHealth) * factor);
}

// The own player's state event: the cgame sounds (CG_EntityEvent 0x4ba8d0, at the player so heard locally) and the
// game's fall damage (ClientEvents 0x4e75f0 -> G_Damage 0x528050 with no inflictor, attacker, direction or point,
// means of death MOD_FALLING).
void World::applyPlayerEvent(int ev, int parm) {
    const auto it = cvars.find("cg_footsteps");
    const bool footsteps = it == cvars.end() || it->second.empty() || std::atoi(it->second.c_str()) != 0;
    for (const std::string& alias : playerEventSounds(ev, footsteps)) startSound(alias, nullptr);
    if (ev >= 0x74 && ev < 0x8b && player_) {
        const Value mh = player_->obj->get("maxhealth");
        const int dmg = fallDamageAmount(mh.isNumber() ? mh.asInt() : 100, parm);
        if (dmg > 0) damage(*player_, nullptr, dmg, {0, 0, 0}, {0, 0, 0}, "MOD_FALLING");
    }
}

} // namespace iw2::game
