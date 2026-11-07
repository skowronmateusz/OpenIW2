// The view turn of a player link, shown between server frames (docs/spec/pmove.md "Linked player").
//
// A script link (playerlinktodelta / playerlinktoabsolute) changes the view angles once per 20 Hz server frame (ClientEndFrame 0x4e7f90 ->
// 0x4d6ec0). The client interpolates the player state between the last two snapshots by the frame fraction (0x4ab740 sets it, CG_InterpolatePlayerState
// 0x4b9940 applies it to origin and view angles), so the turn of a vehicle shows as a smooth rotation, not as 20 Hz steps. This holds the part of
// the turn that is still to be shown and releases it as the fraction advances. TODO(spec): the original interpolates the whole angle, the mouse
// look included (one server frame late); only the link's turn is spread here, the mouse look stays immediate.
#pragma once

#include <algorithm>
#include <cmath>

namespace iw2::game {

struct LinkViewSmoother {
    float pend[2] = {0, 0}; // pitch, yaw still owed
    float done = 1.0f;      // the part of the current server interval already shown

    // A server frame produced `target` (pitch, yaw) while the view shown was (pitch, yaw). A delta link owes the turn on top of what was
    // not shown yet; an absolute link owes the way to the tag's angles.
    void push(float targetPitch, float targetYaw, float pitch, float yaw, bool absolute) {
        const float dp = std::remainder(targetPitch - pitch, 360.0f), dy = std::remainder(targetYaw - yaw, 360.0f);
        if (absolute) {
            pend[0] = dp, pend[1] = dy;
        } else {
            pend[0] += dp, pend[1] += dy;
        }
        done = 0.0f;
    }

    void clear() { pend[0] = pend[1] = 0.0f; }

    // The frame fraction is now `frac` (0..1 inside the server interval): shows the share of the owed turn that this advance releases.
    void step(float frac, float& pitch, float& yaw) {
        if (pend[0] == 0.0f && pend[1] == 0.0f) return;
        const float s = done < 1.0f ? std::clamp((frac - done) / (1.0f - done), 0.0f, 1.0f) : 1.0f;
        pitch = std::clamp(pitch + pend[0] * s, -89.0f, 89.0f);
        yaw += pend[1] * s;
        pend[0] -= pend[0] * s;
        pend[1] -= pend[1] * s;
        done = std::max(done, frac);
    }
};

} // namespace iw2::game
