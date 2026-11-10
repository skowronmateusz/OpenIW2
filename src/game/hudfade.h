// HUD fade helpers (cgame).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace iw2::game {

// CG_FadeColor-style helper (exe 0x4b0900): start 0 or elapsed >= duration -> inactive (returns -1); otherwise 1 while the remaining
// time is >= span, else remaining / span.
inline float hudFadeAlpha(int64_t startMs, int64_t nowMs, int64_t durationMs, int64_t spanMs) {
    if (startMs == 0) return -1.0f;
    const int64_t el = nowMs - startMs;
    if (el >= durationMs) return -1.0f;
    const int64_t rem = durationMs - el;
    return rem < spanMs ? float(rem) / float(spanMs) : 1.0f;
}

// Default seconds of the hud_fade_* dvars. The SP values are the ones the SP HUD already used (docs/spec/ui.md); the MP client
// registers ammodisplay, compass and offhand with 0 (never fade: the fade helper returns 1 for 0 s), healthbar 2 and
// stance 1.7 (registration at 0x4c3430 in the patch 1.3 MP executable, docs/spec/mp-hud.md).
enum class HudFadeDvar { AmmoDisplay, HealthBar, Compass, Stance, Offhand };
inline float hudFadeDefaultSeconds(HudFadeDvar d, bool multiplayer) {
    switch (d) {
    case HudFadeDvar::AmmoDisplay: return multiplayer ? 0.0f : 3.0f;
    case HudFadeDvar::HealthBar: return 2.0f;
    case HudFadeDvar::Compass: return multiplayer ? 0.0f : 8.0f;
    case HudFadeDvar::Stance: return 1.7f;
    case HudFadeDvar::Offhand: return multiplayer ? 0.0f : 3.0f;
    }
    return 0.0f;
}

// Health fraction (0x4a8790): 0 when health or maxhealth is 0 (or pm_type 4), else health / maxhealth clamped to 1.
inline float hudHealthFraction(int health, int maxHealth) {
    if (health <= 0 || maxHealth <= 0) return 0.0f;
    const float f = float(health) / float(maxHealth);
    return f > 1.0f ? 1.0f : f;
}

// Which weapon the weapon-info ownerdraws (ammo, name, firemode icon: 0x4a79b0, 0x4a7da0, 0x4a8040) describe: the selected weapon
// (cg.weaponSelect, 0xf796e0) when it names a weapon the player owns (index in range and its bit set in 0xf70e00), else the player
// state's current weapon (0xf70998). Names are compared lower-case by the caller.
inline const std::string& hudShownWeapon(const std::string& selected, const std::string& current, const std::function<bool(const std::string&)>& owned) {
    return !selected.empty() && owned(selected) ? selected : current;
}

} // namespace iw2::game
