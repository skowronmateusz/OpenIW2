// weapnext / weapprev (cgame 0x4b6fd0 / 0x4b7030, docs/spec/controls.md).
#pragma once
#include <cstdint>

namespace iw2::game {

// Whether the command acts: no cycle while paused, with frozen controls (pm_flags 0x8000) or in a vehicle (0x20000), nor sooner
// than cg_weaponCycleDelay ms after the last accepted cycle. The caller stamps lastCycleMs when this returns true.
inline bool weaponCycleAllowed(bool paused, uint32_t pmFlags, uint32_t stateFlags, int64_t nowMs, int64_t lastCycleMs, int delayMs) {
    if (paused || (pmFlags & 0x8000u) || (stateFlags & 0x20000u)) return false;
    return nowMs - lastCycleMs >= delayMs;
}

} // namespace iw2::game
