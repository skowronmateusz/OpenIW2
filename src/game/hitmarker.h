// Hit marker (openiw2 option iw2_hitMarker, default off; a deliberate addition, not original single-player behaviour).
// The multiplayer damage feedback shows the shader "damage_feedback" as a script HUD element: horizontally and vertically centred
// with an offset of (-12, -12), 24 x 24 virtual units, alpha 1 fading linearly to 0 over one second, restarted by every hit
// (maps/mp/gametypes/_damagefeedback.gsc:12-33). The multiplayer sound is not played here. The gametype damage callbacks call it
// for every damage a player deals to another player (dm.gsc:274, tdm.gsc:344), whatever the means of death.
#pragma once

#include <cstdint>
#include <string>

namespace iw2::game {

struct HitMarkerLook {
    float size = 24.0f;      // virtual units (640 x 480 screen), square
    float offset = -12.0f;   // x and y from the screen centre
    int fadeMs = 1000;
};

// Two teams are enemies when they are the two opposing sides; "neutral" and unset teams are nobody's enemy.
inline bool hitMarkerEnemies(const std::string& a, const std::string& b) {
    return (a == "axis" && b == "allies") || (a == "allies" && b == "axis");
}

// Whether a damage the engine applied counts as a hit on an enemy: the attacker is the player, the victim is an actor that was alive
// when the damage arrived (corpses, the player, script models, vehicles and the world do not count) and belongs to the player's
// enemy team (friendlies and neutrals do not). Grenade and rocket splash, bullets, turret fire and melee all arrive here the same way
// (the shooter is the player), as in multiplayer where the means of death is not looked at.
inline bool hitMarkerTrigger(bool attackerIsPlayer, bool targetIsActor, bool targetAlive, const std::string& attackerTeam,
                             const std::string& targetTeam, int amount) {
    return attackerIsPlayer && targetIsActor && targetAlive && amount > 0 && hitMarkerEnemies(attackerTeam, targetTeam);
}

// Alpha of the marker `nowMs - hitMs` after the last hit (hitMs < 0: never hit).
inline float hitMarkerAlpha(int64_t nowMs, int64_t hitMs, const HitMarkerLook& look = {}) {
    if (hitMs < 0 || nowMs < hitMs || look.fadeMs <= 0) return 0.0f;
    const float t = float(nowMs - hitMs) / float(look.fadeMs);
    return t >= 1.0f ? 0.0f : 1.0f - t;
}

} // namespace iw2::game
