// Frame-time scale of the death slow motion (client time_slow, VAs in the patch 1.3 SP executable: 0x4bec20 start,
// 0x4bee00 per-frame update, 0x42eb70 use).
#pragma once

#include <cstdint>

namespace iw2::game {

// 0x4bee00: with a slow-down running, start < now: linear from -> to over [start, end) of real time, then `to` stays.
// Before start (or with none running) the scale is unchanged (`current`).
inline float frameScaleAt(int64_t startMs, int64_t endMs, int64_t nowMs, float from, float to, float current) {
    if (startMs == 0 || nowMs <= startMs) return current;
    if (nowMs >= endMs) return to;
    const float t = float(nowMs - startMs) / float(endMs - startMs);
    return (1.0f - t) * from + to * t;
}

// 0x42eb70 (called with every Com_Frame msec, the result drives the server frame and the client): the frame time is
// multiplied by the scale; so the simulation (player movement, actors, effects) all run at the scaled speed.
inline float scaledFrameTime(float dt, float scale) { return dt * scale; }

} // namespace iw2::game
