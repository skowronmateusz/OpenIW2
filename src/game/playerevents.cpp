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

const char* bulletTraceSurfaceType(float fraction, unsigned surfaceFlags) {
    if (fraction == 1.0f) return "none";
    const int surface = int((surfaceFlags >> 20) & 0x1f);
    return surface > 0 && surface < 23 ? kNames[surface] : "default";
}

bool playerStateEventSentToEntity(int ev) {
    return !(ev >= 0x8c && ev <= 0x8f) && ev != 0xb9 && ev != 0xba;
}

std::vector<std::string> playerEventSounds(int ev, bool footsteps, bool local) {
    std::vector<std::string> out;
    const std::string plr = local ? "_plr" : "";
    auto with = [&](const char* family, int base) { out.push_back(std::string(family) + plr + "_" + surfaceAliasName(ev - base)); };
    const std::string gearRun = "gear_rattle" + plr + "_run", gearWalk = "gear_rattle" + plr + "_walk";
    if (ev >= 1 && ev < 0x18) {
        if (footsteps) with("step_run", 1);
        out.push_back(gearRun);
    } else if (ev >= 0x18 && ev < 0x2f) {
        if (footsteps) with("step_walk", 0x18);
        out.push_back(gearWalk);
    } else if (ev >= 0x2f && ev < 0x46) {
        if (footsteps) with("step_prone", 0x2f);
        out.push_back(gearWalk);
    } else if (ev >= 0x46 && ev < 0x5d) {
        with("step_run", 0x46);
        out.push_back(gearRun);
    } else if (ev >= 0x5d && ev < 0x74) {
        with("land", 0x5d);
    } else if (ev >= 0x74 && ev < 0x8b) {
        with("land", 0x74);
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
    if (player_) clientFallEvent(*player_, ev, parm);
    // The listen host's own player: its entity's event ring carries the movement events too, so network clients hear
    // its steps (the player state to entity state copy, docs/spec/net.md section 13). A network client's push is a no-op.
    if (mp_ && playerStateEventSentToEntity(ev)) mpPushLocalClientEvent(ev, std::string());
}

// Another player's movement event (footsteps, jumps, landings): the non-"_plr" sounds at that player
// (playerEventSounds with local false).
void World::otherPlayerMoveEventSound(const Entity& client, int ev) {
    if (ev < 1 || ev > 0x8b) return;
    const Value o = client.obj->get("origin");
    if (!o.isVector()) return;
    const auto it = cvars.find("cg_footsteps");
    const bool footsteps = it == cvars.end() || it->second.empty() || std::atoi(it->second.c_str()) != 0;
    const Vec3 at = o.asVector();
    for (const std::string& alias : playerEventSounds(ev, footsteps, false)) startSound(alias, &at);
}

void World::clientFallEvent(Entity& client, int ev, int parm) {
    if (ev < 0x74 || ev >= 0x8b) return;
    const Value mh = client.obj->get("maxhealth");
    const int maxHealth = mh.isNumber() ? mh.asInt() : 100;
    // MP ClientEvents (0x80f5370..0x80f541c) stores the factor and the product as floats before truncating (50 % of 100 is 50);
    // the SP amount keeps the double product (fallDamageAmount).
    int dmg = fallDamageAmount(maxHealth, parm);
    if (mp_) {
        const float factor = parm > 99 ? 1.1f : float(parm) * 0.01f;
        dmg = factor == 0.0f ? 0 : int(float(maxHealth) * factor);
    }
    if (dmg > 0) damage(client, nullptr, dmg, {0, 0, 0}, {0, 0, 0}, "MOD_FALLING");
}

} // namespace iw2::game
