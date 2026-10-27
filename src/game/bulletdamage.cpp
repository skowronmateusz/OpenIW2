#include "game/bulletdamage.h"

#include <cstdlib>

namespace iw2::game {

int bulletDamageAtDistance(const std::map<std::string, std::string>& def, bool targetIsClient, float distance) {
    auto num = [&](const char* key, int fallback) {
        auto it = def.find(key);
        return it == def.end() ? fallback : std::atoi(it->second.c_str());
    };
    auto wc = def.find("weaponclass");
    const bool turret = wc != def.end() && wc->second == "turret";
    int damage = num("damage", 0), minDamage = num("mindamage", 0);
    if (turret && targetIsClient) {
        damage = num("playerdamage", 0);
        minDamage = num("minplayerdamage", 0);
    }
    const float maxRange = float(num("maxdamagerange", 0)), minRange = float(num("mindamagerange", 0));
    if (distance < maxRange) return damage;
    if (distance >= minRange) return minDamage;
    if (minRange - maxRange == 0.0f) return damage;
    const float t = (distance - maxRange) / (minRange - maxRange);
    return int((1.0f - t) * float(damage) + float(minDamage) * t);
}

} // namespace iw2::game
