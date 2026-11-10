// The client's per-frame clock (cg.time 0xf708dc; docs/spec/net.md section 10 "Frame clock") and the length of a server frame on
// the render side. Simulation code keeps its own frame constants.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace iw2::game {

constexpr int kServerFrameMs = 50;           // sv_fps 20
constexpr float kServerFrameSeconds = 0.05f;

// The part of the current server frame already played: gameClock is the time accumulated since the last tick, in seconds.
inline float serverFrameLerp(float gameClock) { return std::clamp(gameClock / kServerFrameSeconds, 0.0f, 1.0f); }

// The client time of a rendered frame (read per drawn frame by the shake 0x4bf540 / 0x4bf700, the effect system 0x486b30): the
// display time lags the newest server frame by one step and advances by the interpolation fraction, so it changes every rendered
// frame, not once per server frame. Never below 0: a level starts at time 0 and 0 means "no stamp" to many of the readers.
inline int64_t clientRenderTimeMs(int64_t serverMs, float lerpFrac) {
    return std::max<int64_t>(0, serverMs - kServerFrameMs + int64_t(lerpFrac * float(kServerFrameMs)));
}

// Time since a stamp of the game (level time, up to one server frame ahead of the client clock), read on the client clock: an
// effect started by the newest server frame stands at its start until the clock reaches that frame.
inline int elapsedMs(int64_t clientMs, int64_t stampMs) { return int(std::max<int64_t>(0, clientMs - stampMs)); }

// A network client's offset from server time to the client clock (the snapshot times in the HUD elements and the shell shock). The
// two clocks run at the same rate but tick differently, so the offset is held while the measured one stays within kHoldMs of it: a
// stamp shifted by it must not move from frame to frame.
inline int64_t heldClockOffset(std::optional<int64_t>& held, int64_t measured) {
    constexpr int64_t kHoldMs = 16;
    if (!held || std::max(*held, measured) - std::min(*held, measured) > kHoldMs) held = measured;
    return *held;
}

} // namespace iw2::game
