// Prone fit test (exe 0x4fb530); stage walk-through in docs/spec/ai.md "Prone".
#include "game/prone.h"

#include <cmath>

namespace iw2::game {

namespace {

Vec add(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec sub(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec scale(const Vec& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
Vec lerp(const Vec& a, const Vec& b, float t) { return add(a, scale(sub(b, a), t)); } // 0x47f390

// 0x421270 / 0x4210d0: unit vector (zero when the length is 0) and the length.
float normalize(Vec& v) {
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len != 0.0f) v = scale(v, 1.0f / len);
    else v = {0.0f, 0.0f, 0.0f};
    return len;
}

// Constants of 0x4fb530, read from .rdata.
constexpr float kProbe = 6.0f;          // 0x56fcc8: probe half size, height offset, ground offset
constexpr float kStandRise = 10.0f;     // 0x591e8c
constexpr float kMinNormalZ = 0.7f;     // 0x591f30
constexpr float kRadiusMargin = 2.0f;   // 0x591ed0
constexpr float kReach = 48.0f;         // 0x591f40
constexpr float kRaise = 22.0f;         // 0x5921a0
constexpr float kDropScale = 2.5f;      // 0x591e44
constexpr float kSlopeRatio = -0.75f;   // 0x59219c
constexpr float kStepUp = 18.0f;        // 0x591f44
constexpr float kPitchMin = -50.0f;     // 0x592030
constexpr float kPitchMax = 70.0f;      // 0x592198
constexpr float kLineLift = 5.0f;       // 0x591ef0

} // namespace

float pronePitchOf(const Vec& v) {
    if (v[1] == 0.0f && v[0] == 0.0f) return v[2] > 0.0f ? 270.0f : 90.0f;
    const float horiz = std::sqrt(v[0] * v[0] + v[1] * v[1]);
    float p = std::atan2(v[2], horiz) * -180.0f / 3.14159274f;
    if (p < 0.0f) p += 360.0f;
    return p;
}

float proneAngleDelta(float a, float b) {
    float d = a - b;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

float proneAngleNormalize180(float a) {
    // 0x565994 (_ftol2) truncates towards zero; the 16-bit wrap keeps the low bits of the integer.
    const int units = int(a * 182.044449f) & 0xffff;
    float r = float(units) * (360.0f / 65536.0f);
    if (r > 180.0f) r -= 360.0f;
    return r;
}

ProneFitResult proneFit(const ProneFitArgs& a, const ProneTraceFn& trace) {
    ProneFitResult out;
    // A blocked probe or a failed last check: strict callers get "no", the others "fits, flat".
    auto fallback = [&]() {
        out = {};
        out.fits = !a.strict;
        return out;
    };
    const Vec& o = a.origin;

    // Standing box: radius x radius x height lifted 10 units must not start all-solid.
    if (!a.alreadyProne) {
        const TraceResult t = trace(o, {o[0], o[1], o[2] + kStandRise}, {-a.radius, -a.radius, 0.0f},
                                    {a.radius, a.radius, a.height});
        if (t.allSolid) return out;
    }
    if (a.strict && a.groundNormal && (*a.groundNormal)[2] < kMinNormalZ) return out;

    const Vec pmins{-kProbe, -kProbe, -kProbe}, pmaxs{kProbe, kProbe, kProbe};
    // Direction the body lies in: behind the facing.
    const float yawRad = (a.yaw - 180.0f) * 0.0174532924f;
    Vec dir{std::cos(yawRad), std::sin(yawRad), 0.0f};
    const float probeZ = a.height - kProbe;
    const float reach = a.length - kProbe;

    // Probe 1: horizontal sweep at probe height from the origin over the body length.
    Vec start{o[0], o[1], o[2] + probeZ};
    Vec end = add(start, scale(dir, reach));
    TraceResult t = trace(start, end, pmins, pmaxs);
    bool shortened = false; // the body does not fit at full length
    float bodyLen = a.length;
    if (t.fraction < 1.0f) {
        if (!a.strict) return out;
        shortened = true;
        bodyLen = reach * t.fraction + kProbe;
        if (bodyLen < a.radius + kRadiusMargin) return out;
        const float limit = probeZ * kMinNormalZ + kReach;
        if (bodyLen < limit) {
            // Retry aimed 22 units higher; the body direction follows the raised line.
            end[2] += kRaise;
            shortened = false;
            dir = sub(end, start);
            const float len = normalize(dir);
            t = trace(start, end, pmins, pmaxs);
            if (t.fraction >= 1.0f) {
                bodyLen = a.length;
            } else {
                shortened = true;
                bodyLen = len * t.fraction + kProbe;
                if (bodyLen < limit) return out;
            }
        }
    }
    Vec foot = lerp(start, end, t.fraction); // end of the free body line

    // Probe 2: drop at 48 along the body to find the hip ground point.
    start = add(add(o, scale(dir, kReach)), {0.0f, 0.0f, probeZ});
    const float drop = a.radius * kDropScale + probeZ - kProbe;
    end = {start[0], start[1], start[2] - drop};
    t = trace(start, end, pmins, pmaxs);
    if (t.fraction == 1.0f) return fallback();
    if (t.normal[2] < kMinNormalZ) return out;
    const float hipDrop = drop * t.fraction + kProbe;
    Vec hip = lerp(start, end, t.fraction);
    hip[2] -= kProbe;

    if (shortened) {
        if (bodyLen - hipDrop < hipDrop * kSlopeRatio) return fallback();
        // Probe 3: from the hip probe start towards the midpoint of the full-length body end and the hip line
        // continued along the hip-to-foot direction.
        Vec slope = add(scale(dir, kProbe), sub(foot, hip));
        slope[2] += kProbe;
        normalize(slope);
        const float rest = reach - kReach;
        const Vec fullEnd = add(o, scale(dir, reach));
        end = {(fullEnd[0] + start[0] + slope[0] * rest) * 0.5f, (fullEnd[1] + start[1] + slope[1] * rest) * 0.5f,
               start[2] + slope[2] * rest};
        t = trace(start, end, pmins, pmaxs);
        if (t.fraction < 1.0f) {
            // Step up 18 from where it stopped and try once more.
            start = lerp(start, end, t.fraction);
            start[2] += kStepUp;
            end[2] += kStepUp;
            t = trace(start, end, pmins, pmaxs);
            if (t.fraction < 1.0f) return fallback();
        }
        foot = lerp(start, end, t.fraction);
    }

    // Probe 4: drop from the foot line to the ground, twice its height above the hip plus the radius.
    start = foot;
    end = {foot[0], foot[1], foot[2] - ((foot[2] - hip[2]) * 2.0f + a.radius)};
    t = trace(start, end, pmins, pmaxs);
    if (t.fraction == 1.0f) return fallback();
    if (t.normal[2] < kMinNormalZ) return out;
    foot = lerp(start, end, t.fraction);
    foot[2] -= kProbe;

    // Bend between the torso line (origin -> hip) and the leg line (hip -> foot).
    bool ok = true;
    const float bend = proneAngleDelta(pronePitchOf(sub(foot, hip)), pronePitchOf(sub(hip, o)));
    if (bend < kPitchMin || kPitchMax < bend) ok = false;
    // Lines 5 units above the ground: origin -> hip -> foot must be clear (point sweeps).
    const Vec zero{0.0f, 0.0f, 0.0f};
    const Vec lift{0.0f, 0.0f, kLineLift};
    if (trace(add(o, lift), add(hip, lift), zero, zero).fraction < 1.0f) ok = false;
    if (trace(add(hip, lift), add(foot, lift), zero, zero).fraction < 1.0f) ok = false;
    out.pitchBody = proneAngleNormalize180(pronePitchOf(sub(o, hip)));
    out.pitchLegs = proneAngleNormalize180(pronePitchOf(sub(hip, foot)));
    if (ok) {
        out.fits = true;
        return out;
    }
    return fallback();
}

} // namespace iw2::game
