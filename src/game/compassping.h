// Compass sound pings (cgame): the per-actor table at 0xf79a88 (32 slots, stride 24: +0 last-seen ms, +4 x, +8 y, +12 yaw,
// +16 ping end ms, +20 enemy byte) written by 0x4c0e20 and drawn by ownerdraw 87 (0x4c0f20).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace iw2::game {

// Ping alpha (0x4c12aa..0x4c1315, again 0x4c149b..0x4c167d): fade = cg_hudCompassSoundPingFadeTime (0xf48ce4, default 2 s,
// range 0..10) * 1000 ms. Inactive (returns < 0) when endMs + fade < now; otherwise 1 while endMs >= now, then 1 - (now - endMs)
// / fade. The caller clamps the result to the compass fade alpha (0x4c1341 / 0x4c168a: min).
inline float compassPingAlpha(int64_t endMs, int64_t nowMs, float fadeSeconds) {
    const float fade = fadeSeconds * 1000.0f;
    if (float(endMs) + fade < float(nowMs)) return -1.0f;
    if (endMs >= nowMs) return 1.0f;
    return 1.0f - float(nowMs - endMs) / fade;
}

class CompassPings {
public:
    struct Ping { int64_t endMs = 0; bool enemy = false; std::array<float, 2> pos{}; };
    // 0x4c0e20 (centity, durationMs, position): the caller has already excluded entity type 0xb. team is the actor's sentient team
    // (0x1c6421c[slot], 1 axis, 2 allies, 3 neutral; 4 TODO(spec)); 3 and 4 leave the entry alone. Otherwise end = now + duration and
    // enemy = (team == 1); only an enemy ping stores the position (x, y), so an allied entry keeps its friendly position.
    // The exe keys the entry by the actor slot (0x1c630bc[entnum]); here by entity number. TODO(spec): slot reuse by a new actor.
    void ping(int entnum, int team, float x, float y, int64_t nowMs, int durationMs) {
        if (team == 3 || team == 4) return;
        Ping& p = pings_[entnum];
        p.endMs = nowMs + durationMs;
        p.enemy = team == 1;
        if (p.enemy) p.pos = {x, y};
    }
    // Ping end of an entity (0 when it never pinged): the friendly pass of 0x4c0f20 reads +16 for each drawn friendly.
    int64_t endMs(int entnum) const {
        const auto it = pings_.find(entnum);
        return it == pings_.end() ? 0 : it->second.endMs;
    }
    // Second pass of 0x4c0f20 (0x4c1480..0x4c16e6, all slots): enemy byte set, end != 0 and end + fade >= now. The position is the
    // one stored at the shot; no age, alive or drawoncompass test.
    std::vector<Ping> activeEnemyPings(int64_t nowMs, float fadeSeconds) const {
        std::vector<Ping> out;
        for (const auto& [n, p] : pings_)
            if (p.enemy && p.endMs != 0 && compassPingAlpha(p.endMs, nowMs, fadeSeconds) >= 0.0f) out.push_back(p);
        return out;
    }
    // CG_Init (0x4a5c10) zeroes 0x8b93 dwords from 0xf5bd00, which covers this table (0xf79a88, 32 x 24 bytes); the table is not part
    // of the savegame, so World::loadState clears it.
    void clear() { pings_.clear(); }

private:
    std::map<int, Ping> pings_;
};

} // namespace iw2::game
