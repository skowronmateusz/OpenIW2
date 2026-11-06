// The ground drop of animscripted's deathplant mode (start 0x53b190; docs/spec/animscripted.md "Start").
#pragma once

#include "game/collision.h"

namespace iw2::game {

// A trace of the box (-15,-15,0)..(15,15,72) (0x56f704 / 0x56f710) from 36 above the origin to 18 below it (0x591f5c /
// 0x591f44), clip mask 0x820011, the entity itself passed (the trace takes the entity number, 0x44bf00). Without a
// start-solid and with a hit, the origin takes the height where the box stopped (fraction 1 or a start-solid keeps it).
inline std::array<float, 3> deathPlantOrigin(const CollisionWorld& cw, const std::array<float, 3>& origin, int passEntity) {
    const std::array<float, 3> start{origin[0], origin[1], origin[2] + 36.0f}, end{origin[0], origin[1], origin[2] - 18.0f};
    const std::array<float, 3> mins{-15.0f, -15.0f, 0.0f}, maxs{15.0f, 15.0f, 72.0f};
    const TraceResult tr = cw.trace(start, end, mins, maxs, 0x820011u, passEntity);
    std::array<float, 3> out = origin;
    if (!tr.startSolid && tr.fraction < 1.0f) out[2] = (end[2] - start[2]) * tr.fraction + start[2];
    return out;
}

// The per-frame plant (0x53bb84..0x53bc53): the same box from `lift` + 18 above the animated position to `lift` - 18. A hit moves the
// position to the hit height and keeps the difference as the new lift (+0x50); a miss or a start-solid leaves both as they are.
inline void deathPlantFrame(const CollisionWorld& cw, std::array<float, 3>& pos, float& lift, int passEntity) {
    const std::array<float, 3> start{pos[0], pos[1], pos[2] + lift + 18.0f}, end{pos[0], pos[1], pos[2] + lift - 18.0f};
    const std::array<float, 3> mins{-15.0f, -15.0f, 0.0f}, maxs{15.0f, 15.0f, 72.0f};
    const TraceResult tr = cw.trace(start, end, mins, maxs, 0x820011u, passEntity);
    if (tr.startSolid || !(tr.fraction < 1.0f)) return;
    const float z = (end[2] - start[2]) * tr.fraction + start[2];
    lift = z - pos[2];
    pos[2] = z;
}

} // namespace iw2::game
