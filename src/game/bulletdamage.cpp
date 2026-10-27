#include "game/bulletdamage.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

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

bool parseAccuracyGraph(const std::string& text, AccuracyGraph& out) {
    std::istringstream in(text);
    std::string magic;
    int n = 0;
    if (!(in >> magic >> n) || magic != "WEAPONACCUFILE" || n < 0) return false;
    out.points.clear();
    for (int i = 0; i < n; ++i) {
        float x, y;
        if (!(in >> x >> y)) return false;
        out.points.emplace_back(x, y);
    }
    return true;
}

float evalAccuracyGraph(const AccuracyGraph& g, float distance) {
    float x = distance * 0.00025f;
    if (x > 1.0f) x = 1.0f;
    const auto& p = g.points;
    for (size_t i = 1; i < p.size(); ++i) {
        if (x <= p[i].first) {
            const float t = (x - p[i - 1].first) / (p[i].first - p[i - 1].first);
            return (p[i].second - p[i - 1].second) * t + p[i - 1].second;
        }
    }
    return -1.0f;
}

float aiHitChance(const AccuracyGraph& g, float distance, float actorAccuracy, float shootAccuracyArg) {
    const float acc = evalAccuracyGraph(g, distance) * actorAccuracy * shootAccuracyArg;
    return std::clamp(acc, 0.0f, 1.0f);
}

} // namespace iw2::game
