#include "game/bulletdamage.h"

#include <algorithm>
#include <cmath>
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

namespace {
Vec3f madd(Vec3f p, Vec3f d, float s) { return {p.x + d.x * s, p.y + d.y * s, p.z + d.z * s}; }
} // namespace

Vec3f aiSideAxis(Vec3f v, Vec3f up) {
    Vec3f c{v.y * up.z - v.z * up.y, v.z * up.x - v.x * up.z, v.x * up.y - v.y * up.x};
    const float l = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
    if (l < 1e-6f) return {0, 0, 0};
    return {c.x / l, c.y / l, c.z / l};
}

Vec3f aiHitPoint(Vec3f T, Vec3f c, Vec3f up, bool targetIsClient, float accuracy, float u1, float u2) {
    float zBase, W, H;
    if (targetIsClient) {
        zBase = 10.0f; // 0x5a5bf0 − 60
        W = 8.0f;
        H = -44.0f;
    } else {
        zBase = 6.0f;
        const float t = 1.0f - accuracy;
        W = (t * 0.9f + 0.1f) * 15.0f;
        H = (t + 1.0f) * 0.5f * -44.0f;
    }
    T = madd(T, c, (2.0f * u1 - 1.0f) * W);
    return madd(T, up, u2 * H + zBase);
}

Vec3f aiMissPoint(Vec3f T, Vec3f v, Vec3f c, Vec3f up, bool targetIsClient, float u1, float u2, float u3) {
    const float sign = u1 > 0.5f ? 1.0f : -1.0f;
    T = madd(T, v, -16.0f);
    if (targetIsClient) {
        T = madd(T, c, sign * 8.0f * (1.0f + u2));
        return madd(T, up, 10.0f - 44.0f * u3);
    }
    T = madd(T, c, sign * std::sqrt(1350.0f));
    return madd(T, up, (2.0f * u2 - 1.0f) * 12.0f);
}

} // namespace iw2::game
