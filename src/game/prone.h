// Prone fit test (exe 0x4fb530) and its helpers; docs/spec/ai.md "Prone".
#pragma once

#include "game/collision.h"

#include <cstdint>
#include <functional>

namespace iw2::game {

// One box sweep from start to end (mins/maxs relative to the swept point). The caller binds the mask and the pass
// entity; the exe selects the sweep through the table 0x5a9190 (entry 1 = 0x4db7f0 -> G_Trace 0x44bf00).
using ProneTraceFn = std::function<TraceResult(const Vec& start, const Vec& end, const Vec& mins, const Vec& maxs)>;

struct ProneFitArgs {
    Vec origin{};               // a2
    float radius = 15.0f;       // a3: half width of the standing box, also used in two distance terms
    float height = 48.0f;       // a4: standing box height; the probe height is height - 6
    float yaw = 0.0f;           // a5: facing; the body is laid out towards yaw - 180
    bool alreadyProne = false;  // a9: skips the standing-box test
    bool strict = true;         // a10: a blocked probe or a failed final check returns false instead of "fits, no tilt"
    const Vec* groundNormal = nullptr; // a11: with strict, a normal whose z is below 0.7 fails at once
    float length = 66.0f;       // a14: body length along the floor
};

struct ProneFitResult {
    bool fits = false;
    // a7 / a8: pitch (normalised to -180..180) of origin - hip point and of hip point - foot point; 0 when the
    // non-strict fallback answers.
    float pitchBody = 0.0f;
    float pitchLegs = 0.0f;
};

// Body pitch of a direction in degrees, 0..360 (0x421990): -atan2(z, |xy|) in degrees, plus 360 when negative;
// a vertical vector gives 270 pointing up and 90 pointing down.
float pronePitchOf(const Vec& v);
// a - b wrapped into -180..180 by whole turns (0x423f50).
float proneAngleDelta(float a, float b);
// Angle snapped to the 16-bit angle grid and returned in -180..180 (0x424040).
float proneAngleNormalize180(float a);

// The fit test. Every sweep goes through `trace`; the exe's checkprone passes mask 0x820011 and the actor's entity.
ProneFitResult proneFit(const ProneFitArgs& args, const ProneTraceFn& trace);

} // namespace iw2::game
