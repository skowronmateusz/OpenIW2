// Player movement. Rules marked with an address follow CoD2SP_s.exe 1.3 (docs/spec/pmove.md).
#include "game/pmove.h"

#include "assets/xanim.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace iw2::game {
namespace {

float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec add(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec scale(const Vec& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }

Vec cross(const Vec& a, const Vec& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float normalize(Vec& v) {
    const float l = std::sqrt(dot(v, v));
    if (l != 0) v = scale(v, 1.0f / l);
    return l;
}

constexpr float kMinWalkNormal = 0.7f;     // DAT_00591f30
constexpr float kGroundTraceOffset = 0.25f; // DAT_00591fe0
constexpr float kStepSize = 18.0f, kProneStepSize = 10.0f; // 0x54f2e0
constexpr float kStickToGround = 9.0f;      // DAT_00591e88
constexpr float kMinStepNormal = 0.3f;      // DAT_00592018
constexpr uint32_t kBodyContents = 0x200c000; // 0x507b00: bodies a trace may start in
constexpr int kMaxClipPlanes = 8;           // 0x54eac0

// Per-move locals (the original's pml): frame time, ground state and the trace mask that 0x507b00 may narrow.
struct Pml {
    float frametime = 0;
    bool walking = false, groundPlane = false;
    TraceResult groundTrace;
    uint32_t mask = 0;
    int passEntity = -1;
    Vec mins{}, maxs{};
    float prevOriginZ = 0, prevVelZ = 0; // pml+0x64 / +0x70: origin z and vertical speed at the start of the step
    float gravity = 800.0f;
    bool dead = false; // pm_type >= 4
};

} // namespace

// Surface type of a trace's flags (0x508f80 and friends): 0 for a surface flagged 0x2000, else flags >> 20 & 0x1f.
static int surfaceTypeOf(uint32_t surfaceFlags) { return (surfaceFlags & 0x2000) ? 0 : int((surfaceFlags >> 20) & 0x1f); }

// PM_CrashLand (0x508fe0): the fall height h = vz²/(2g) + (previous z − z) from the speed at the start of the step.
// Damage 0..100 between bg_fallDamageMinHeight (200) and bg_fallDamageMaxHeight (350) (none on a no-damage surface,
// flag 1, or when dead). A damaging landing below 100 (and not on slick ground, flag 2) sets pm_time to 500 + 35 ·
// damage ms (pm_flags 0x200) and scales the velocity by 0.5 falling to 0.2 over 500..1500 ms (0x5091a7-0x5091eb); a
// lethal or slick one scales it by 0.67; the event is 0x74 + surface with the damage as parameter. Without damage:
// h <= 4 nothing; h < 8 the walk footstep (0x18 + surface); h < 12 the run footstep (1 + surface); otherwise the
// velocity is scaled by 0.67 and the event is the landing 0x5d + surface with the view dip as parameter.
LandingOutcome landingOutcome(float h, uint32_t surfaceFlags, bool dead) {
    LandingOutcome o;
    constexpr float kMinH = 200.0f, kMaxH = 350.0f; // bg_fallDamageMinHeight / MaxHeight defaults (0x4fa818, 0x4fa83b)
    if (h > kMinH && !(surfaceFlags & 1) && !dead)
        o.damage = h >= kMaxH ? 100 : std::clamp(int((h - kMinH) / (kMaxH - kMinH) * 100.0f), 0, 100);
    if (h > 12.0f) o.dip = std::min(24, int(((h - 12.0f) * 0.03846153989434242f + 1.0f) * 4.0f));
    const int surf = surfaceTypeOf(surfaceFlags);
    if (o.damage != 0) {
        if (o.damage < 100 && !(surfaceFlags & 2)) {
            const int t = o.damage * 35 + 500;
            o.pmTimeMs = t;
            o.landTimer = true;
            if (t > 2000) o.velScale = 0.2f, o.pmTimeMs = 2000;
            else if (t <= 500) o.velScale = 0.5f;
            else if (t >= 1500) o.velScale = 0.2f;
            else o.velScale = 0.5f - (float(t) - 500.0f) * 0.001f * 0.3f;
        } else {
            o.velScale = 0.67f;
        }
        o.event = 0x74 + surf;
        o.eventParm = o.damage;
        return o;
    }
    if (h <= 4.0f) return o;
    if (h < 8.0f) {
        o.event = surf ? surf + 0x18 : 0;
    } else if (h < 12.0f) {
        o.event = surf ? surf + 1 : 0;
    } else {
        o.velScale = 0.67f;
        o.event = 0x5d + surf;
        o.eventParm = o.dip;
    }
    return o;
}

void addPlayerEvent(PlayerState& ps, int event, int parm) {
    if (event == 0) return;
    ps.events[ps.eventSeq & 3] = event & 0xff;
    ps.eventParms[ps.eventSeq & 3] = parm & 0xffff;
    ++ps.eventSeq;
}

namespace {

// Applies PM_CrashLand: the landing report the cgame turns into the view dip, the speed loss and the event.
// TODO(spec): the exact velocity at the start of the step is the pml copy; the pm_time of a damaging landing only
// matters through the walk accel (x0.25) and friction (x0.3) of pm_flags 0x200.
void crashLand(PlayerState& ps, const Pml& pml) {
    const float g = pml.gravity;
    const float d = pml.prevVelZ * pml.prevVelZ + 2.0f * g * (pml.prevOriginZ - ps.origin[2]);
    if (d < 0) return;
    const float s = std::sqrt(d);
    const float h = s * s / (2.0f * g);
    const LandingOutcome o = landingOutcome(h, pml.groundTrace.surfaceFlags, pml.dead);
    ps.landSeq++;
    ps.landHeight = h;
    ps.landDamage = o.damage;
    if (o.pmTimeMs) {
        ps.pmTimeMs = o.pmTimeMs;
        ps.landTimer = o.landTimer;
    }
    if (o.velScale != 1.0f) ps.velocity = scale(ps.velocity, o.velScale);
    addPlayerEvent(ps, o.event, o.eventParm);
}

// 0x507c20 and the inline copies: v − n·(d − |d|·0.001) with d = v·n.
Vec clipVelocity(const Vec& v, const Vec& n) {
    float d = dot(v, n);
    d = -(d - std::fabs(d) * 0.001f);
    return {d * n[0] + v[0], d * n[1] + v[1], d * n[2] + v[2]};
}

// 0x507b00: the player trace; a trace that starts inside a body (contents 0x200c000) drops those contents from the
// mask for the rest of the move and is repeated.
TraceResult pmTrace(const CollisionWorld& w, Pml& pml, const Vec& start, const Vec& end) {
    TraceResult tr = w.trace(start, end, pml.mins, pml.maxs, pml.mask, pml.passEntity);
    if (tr.startSolid && (tr.contents & kBodyContents) != 0) {
        pml.mask &= ~kBodyContents;
        tr = w.trace(start, end, pml.mins, pml.maxs, pml.mask, pml.passEntity);
    }
    return tr;
}

Vec lerpTo(const Vec& a, const Vec& b, float f) { return {(b[0] - a[0]) * f + a[0], (b[1] - a[1]) * f + a[1], (b[2] - a[2]) * f + a[2]}; }

void noGround(PlayerState& ps, Pml& pml) {
    ps.onGround = false;
    pml.groundPlane = pml.walking = false;
}

// 0x5092f0: tries the 26 unit offsets of the table at 0x56f920; the first free position is taken and dropped by up
// to one unit, and that trace becomes the ground trace.
bool correctAllSolid(const CollisionWorld& w, PlayerState& ps, Pml& pml, TraceResult& tr) {
    static constexpr float kJitter[26][3] = {
        {0, 0, 1}, {-1, 0, 1}, {0, -1, 1}, {1, 0, 1}, {0, 1, 1}, {-1, 0, 0}, {0, -1, 0}, {1, 0, 0}, {0, 1, 0},
        {0, 0, -1}, {-1, 0, -1}, {0, -1, -1}, {1, 0, -1}, {0, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1},
        {-1, 1, 1}, {-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}, {-1, -1, -1}, {1, -1, -1}, {1, 1, -1},
        {-1, 1, -1}};
    for (const auto& j : kJitter) {
        const Vec p{ps.origin[0] + j[0], ps.origin[1] + j[1], ps.origin[2] + j[2]};
        if (pmTrace(w, pml, p, p).startSolid) continue;
        ps.origin = p;
        const Vec down{p[0], p[1], p[2] - 1.0f};
        tr = pmTrace(w, pml, ps.origin, down);
        pml.groundTrace = tr;
        ps.origin = lerpTo(ps.origin, down, tr.fraction);
        return true;
    }
    noGround(ps, pml);
    ps.jumping = false;
    ps.jumpOriginZ = 0;
    return false;
}

// Ground trace (0x509480): a 0.25-unit trace above to below the origin. TODO(spec): the landing (fall damage and
// slowdown, 0x508fe0) when the player lands, ladders (pm_flags 0x20).
// Ladder state (decoded from the exe, not yet ported): PM_CheckLadderMove is 0x50bd90. pm_flags 0x20 is set by 0x50c04e / cleared
// by 0x50bd70 (which turns a cleared 0x20 into 0x40000, ladder-just-left). It is skipped for pm type >= 4, while 0x40000 is set,
// or for 300 ms after the last jump (ps+0x70) unless the move state is 0x28 / 0xb. It traces from the origin along the horizontal
// facing (ps+0x64 ladder normal when already on it, else a 0x4210d0 yaw vector) over the player box with the pm trace mask
// (pml+0xa8) and needs trace fraction < 1 and surface flag 0x8 (ladder) on the hit; a new contact stores the hit normal in ps+0x64..0x6c.
// TODO(spec): the ladder velocity/move itself (not located yet) and the distances (consts 0x41000000 / 0x41f00000 by the aim state).

void groundTrace(const CollisionWorld& w, PlayerState& ps, Pml& pml) {
    Vec start{ps.origin[0], ps.origin[1], ps.origin[2] + kGroundTraceOffset};
    const Vec end{ps.origin[0], ps.origin[1], ps.origin[2] - kGroundTraceOffset};
    TraceResult tr = pmTrace(w, pml, start, end);
    pml.groundTrace = tr;
    if (tr.allSolid && !correctAllSolid(w, ps, pml, tr)) return;
    if (tr.startSolid) {
        // Retried from just below the (possibly corrected) origin, at the first trace's x and y.
        start[2] = ps.origin[2] - 0.001f;
        tr = pmTrace(w, pml, start, end);
        if (tr.startSolid) {
            noGround(ps, pml);
            return;
        }
        pml.groundTrace = tr;
    }
    if (tr.fraction == 1.0f) {
        noGround(ps, pml);
        return;
    }
    // Moving up and away from the plane: a jump, not ground.
    if (ps.velocity[2] > 0 && dot(ps.velocity, tr.normal) > 10.0f) {
        noGround(ps, pml);
        return;
    }
    if (tr.normal[2] < kMinWalkNormal) {
        ps.onGround = false;
        pml.groundPlane = true;
        pml.walking = false;
        ps.jumping = false;
        ps.jumpOriginZ = 0;
        return;
    }
    pml.groundPlane = pml.walking = true;
    if (!ps.onGround) crashLand(ps, pml); // 0x509678: the ground entity was none
    ps.onGround = true;
}

// 0x54ea10: plane indices ordered by velocity·normal, ascending; returns the smallest.
float sortPlanes(const Vec& v, const Vec* planes, int count, int* order) {
    float d[kMaxClipPlanes + 1];
    for (int i = 0; i < count; ++i) {
        d[i] = dot(planes[i], v);
        int k = i;
        for (; k > 0 && d[i] <= d[order[k - 1]]; --k) order[k] = order[k - 1];
        order[k] = i;
    }
    return d[order[0]];
}

// Slide move (0x54eac0): up to 4 traces, clipping the velocity against up to 8 planes (the ground plane and the
// velocity direction first). With gravity the move uses the average of the start and end vertical speed.
// Returns true when something was hit.
bool slideMove(const CollisionWorld& w, PlayerState& ps, Pml& pml, bool gravity, const MoveParams& mp) {
    Vec endVel = ps.velocity;
    if (gravity) {
        endVel[2] = ps.velocity[2] - mp.gravity * pml.frametime;
        ps.velocity[2] = (endVel[2] + ps.velocity[2]) * 0.5f;
        if (pml.groundPlane) ps.velocity = clipVelocity(ps.velocity, pml.groundTrace.normal);
    }
    const Vec primal{endVel[0], endVel[1], endVel[2]};
    Vec planes[kMaxClipPlanes + 1];
    int numPlanes = 0;
    if (pml.groundPlane) planes[numPlanes++] = pml.groundTrace.normal;
    planes[numPlanes] = ps.velocity;
    normalize(planes[numPlanes++]);

    float timeLeft = pml.frametime;
    int bump = 0;
    for (; bump < 4; ++bump) {
        const Vec end = add(ps.origin, scale(ps.velocity, timeLeft));
        const TraceResult tr = pmTrace(w, pml, ps.origin, end);
        if (tr.allSolid) {
            ps.velocity[2] = 0;
            return true;
        }
        if (tr.fraction > 0) ps.origin = lerpTo(ps.origin, end, tr.fraction);
        if (tr.fraction == 1.0f) break;
        timeLeft -= tr.fraction * timeLeft;
        if (numPlanes >= kMaxClipPlanes) {
            ps.velocity = {0, 0, 0};
            return true;
        }
        // The same plane again: nudge the velocity off it.
        bool same = false;
        for (int i = 0; i < numPlanes && !same; ++i)
            if (dot(tr.normal, planes[i]) > 0.999f) {
                ps.velocity = add(clipVelocity(ps.velocity, tr.normal), tr.normal);
                same = true;
            }
        if (same) continue;
        planes[numPlanes++] = tr.normal;
        int order[kMaxClipPlanes + 1];
        if (sortPlanes(ps.velocity, planes, numPlanes, order) >= 0.1f) continue;
        const Vec& pi = planes[order[0]];
        Vec clip = clipVelocity(ps.velocity, pi), endClip = clipVelocity(endVel, pi);
        for (int k = 1; k < numPlanes; ++k) {
            const Vec& pj = planes[order[k]];
            if (dot(clip, pj) >= 0.1f) continue;
            clip = clipVelocity(clip, pj);
            endClip = clipVelocity(endClip, pj);
            if (dot(clip, pi) >= 0) continue;
            // Along the crease of both planes; stop when a third plane blocks that too.
            Vec dir = cross(pi, pj);
            normalize(dir);
            clip = scale(dir, dot(dir, ps.velocity));
            endClip = scale(dir, dot(dir, endVel));
            for (int m = 1; m < numPlanes; ++m)
                if (m != k && dot(clip, planes[order[m]]) < 0.1f) {
                    ps.velocity = {0, 0, 0};
                    return true;
                }
        }
        ps.velocity = clip;
        endVel = endClip;
    }
    if (gravity) ps.velocity = endVel;
    if (ps.pmTimeMs != 0) ps.velocity = primal;
    return bump != 0;
}

// 0x541270: a jump may step up to jump_stepSize, but not above the jump's apex (origin z + jump_height).
bool jumpStep(const PlayerState& ps, float startZ, float& stepSize, const MoveParams& mp) {
    const float apex = ps.jumpOriginZ + mp.jumpHeight;
    if (!(apex > startZ)) return false;
    stepSize = mp.jumpStepSize;
    if (mp.jumpStepSize + startZ > apex) stepSize = apex - startZ;
    return true;
}

// 0x541430: after a jump step the vertical speed may not carry the player above the apex.
void capJumpStep(PlayerState& ps, float startZ, const MoveParams& mp) {
    if (!(ps.origin[2] - startZ > 0)) return;
    const float left = mp.jumpHeight + ps.jumpOriginZ - ps.origin[2];
    if (left >= 0.1f) {
        const float v = std::sqrt((left + left) * mp.gravity);
        if (v < ps.velocity[2]) ps.velocity[2] = v;
    } else {
        ps.velocity[2] = 0;
    }
}

// Step slide move (0x54f2e0): the slide, then on a hit the same move from up to 18 units higher (10 prone), dropped
// back down; on the ground the result sticks to ground up to 9 units below. The step is kept when it got further
// along the velocity.
void stepSlideMove(const CollisionWorld& w, PlayerState& ps, Pml& pml, bool gravity, const MoveParams& mp) {
    const bool onGroundPlane = pml.groundPlane;
    if (!onGroundPlane && ps.jumping && ps.pmTimeMs != 0) {
        ps.jumping = false;
        ps.jumpOriginZ = 0;
    }
    const Vec startO = ps.origin, startV = ps.velocity;
    const bool hit = slideMove(w, ps, pml, gravity, mp);
    float stepSize = ps.stance == Stance::Prone ? kProneStepSize : kStepSize;
    bool jumpStepped = false;
    if (!ps.onGround) {
        if (ps.jumping && ps.pmTimeMs != 0) {
            ps.jumping = false;
            ps.jumpOriginZ = 0;
        }
        if (!hit || !ps.jumping || !jumpStep(ps, startO[2], stepSize, mp)) return;
        if (stepSize < 1.0f) return;
        jumpStepped = true;
    }
    const Vec downO = ps.origin, downV = ps.velocity;
    float up = 0;
    if (hit) {
        const Vec top{startO[0], startO[1], startO[2] + stepSize + 1.0f};
        const TraceResult tr = pmTrace(w, pml, startO, top);
        up = (stepSize + 1.0f) * tr.fraction - 1.0f;
        if (up >= 1.0f) {
            ps.origin = {startO[0], startO[1], startO[2] + up};
            ps.velocity = startV;
            slideMove(w, ps, pml, gravity, mp);
        } else {
            up = 0;
        }
    }
    if (onGroundPlane || up != 0) {
        Vec down{ps.origin[0], ps.origin[1], ps.origin[2] - up};
        if (onGroundPlane) down[2] -= kStickToGround;
        const TraceResult tr = pmTrace(w, pml, ps.origin, down);
        // The trace's hit id (trace+0x1c, `cmp word [esp+0x90], 1; jnc` at 0x54f5e2) is 0: keep the plain slide.
        // G_Trace (0x44bf00) sets it to 0x7ff when nothing was hit and 0x7fe for the world; an entity clip writes its
        // entity number, so only a hit on entity 0 takes this path. A clear trace lowers the origin by `up` below.
        if (tr.entity == 0) {
            ps.origin = downO;
            ps.velocity = downV;
            return;
        }
        if (tr.fraction >= 1.0f) {
            if (up != 0) ps.origin[2] -= up;
        } else {
            if (tr.normal[2] < kMinStepNormal) {
                ps.origin = downO;
                ps.velocity = downV;
                return;
            }
            ps.origin = lerpTo(ps.origin, down, tr.fraction);
            ps.velocity = clipVelocity(ps.velocity, tr.normal);
        }
    }
    const float stepped = (ps.origin[0] - startO[0]) * ps.velocity[0] + (ps.origin[1] - startO[1]) * ps.velocity[1];
    const float slid = (downO[0] - startO[0]) * ps.velocity[0] + (downO[1] - startO[1]) * ps.velocity[1];
    if (stepped <= slid + 0.001f || (jumpStepped && ps.jumpOriginZ + mp.jumpHeight < ps.origin[2])) {
        ps.origin = downO;
        ps.velocity = downV;
        if (onGroundPlane) {
            const Vec down{ps.origin[0], ps.origin[1], ps.origin[2] - kStickToGround};
            const TraceResult tr = pmTrace(w, pml, ps.origin, down);
            if (tr.fraction < 1.0f) {
                ps.origin = lerpTo(ps.origin, down, tr.fraction);
                ps.velocity = clipVelocity(ps.velocity, tr.normal);
            }
        }
    }
    if (jumpStepped) capJumpStep(ps, startO[2], mp);
    // Stairs (0x54f846): a height change of more than half a unit against the plain slide gives the step event 0x8f
    // (parameter: the rounded height clamped to -16..24, + 0x80) and slows the player down. Not on a ladder.
    // Steps over 3 units also advance the bob cycle by min(|step| / 2, 4) · 1.25 + 7 (a footstep when bit 7 changes,
    // 0x50a830 always allows it). The prone gate 0x4fc000 (called from 0x54e990) returns 1 on every path and only
    // fills the prone body angles ps+0x584..0x58c, so no step is ever undone.
    if (onGroundPlane && !ps.onLadder) {
        const float dz = ps.origin[2] - downO[2];
        if (std::fabs(dz) > 0.5f) {
            const int step = std::lrint(double(dz) + 9.313225746154785e-10);
            if (step != 0) {
                addPlayerEvent(ps, 0x8f, (std::clamp(step, -16, 24) + 0x80) & 0xffff);
                const float f = (1.0f - std::fabs(ps.origin[2] - startO[2]) / stepSize) * 0.8f + 0.2f;
                ps.velocity = scale(ps.velocity, f);
                if (std::abs(step) > 3 && ps.onGround) {
                    const int oldCycle = ps.bobCycle;
                    ps.bobCycle = int(float(std::min(std::abs(step) / 2, 4)) * 1.25f + 7.0f + float(oldCycle)) & 0xff;
                    if (((oldCycle + 0x40) ^ (ps.bobCycle + 0x40)) & 0x80) {
                        const int surf = surfaceTypeOf(pml.groundTrace.surfaceFlags);
                        if (surf != 0) addPlayerEvent(ps, ps.stance == Stance::Prone ? 0x2f + surf : (ps.adsSlow || ps.lean != 0) ? 0x18 + surf : 1 + surf);
                    }
                }
            }
        }
    }
}

// View-height keyframes {percent of the transition, height} (tables at 0x5a9088, 0x5a8fa8, 0x5a9130, 0x5a9018).
struct Key { int percent; float height; };
constexpr Key kToProne[] = {{0, 40}, {11, 38}, {22, 33}, {34, 25}, {45, 16}, {50, 15}, {55, 16}, {70, 18}, {90, 17}, {100, 11}};
constexpr Key kToCrouchFromStand[] = {{0, 60}, {1, 59.5f}, {4, 58.5f}, {30, 56}, {80, 44}, {90, 41.5f}, {95, 40.5f}, {100, 40}};
constexpr Key kToCrouchFromProne[] = {{0, 11}, {5, 10}, {30, 21}, {50, 25}, {67, 31}, {83, 34}, {100, 40}};
constexpr Key kToStand[] = {{0, 40}, {5, 40.5f}, {10, 41.5f}, {20, 44}, {70, 56}, {96, 58.5f}, {99, 59.5f}, {100, 60}};

template <size_t N>
float sampleKeys(const Key (&k)[N], int percent) {
    for (size_t i = 1; i < N; ++i)
        if (percent <= k[i].percent) {
            const float t = float(percent - k[i - 1].percent) / float(k[i].percent - k[i - 1].percent);
            return k[i - 1].height + (k[i].height - k[i - 1].height) * t;
        }
    return k[N - 1].height;
}

// Transition length in ms (0x5096a0): to prone 400; to crouch 200 from standing, 400 from prone; to stand 200.
int transitionMs(int target, bool down) { return target == 11 ? 400 : target == 40 ? (down ? 200 : 400) : 200; }

float viewHeightAt(int target, bool down, int percent) {
    if (target == 11) return sampleKeys(kToProne, percent);
    if (target == 40) return down ? sampleKeys(kToCrouchFromStand, percent) : sampleKeys(kToCrouchFromProne, percent);
    return sampleKeys(kToStand, percent);
}

float maxsZ(Stance s, const MoveParams& mp) { return s == Stance::Prone ? 30.0f : s == Stance::Crouch ? 50.0f : mp.maxs[2]; }

// Stance change (0x509d10): standing up needs room for the taller box; going prone needs ground
// (0x4fc000 always returns 1: it is not a gate). A change starts a view-height transition (0x509840).
void updateStance(const CollisionWorld& w, PlayerState& ps, const PlayerInput& in, const MoveParams& mp) {
    Stance want = in.stance;
    if (want == ps.stance) return;
    // Moves go one step at a time between stand, crouch and prone.
    if (int(want) > int(ps.stance) + 1) want = Stance(int(ps.stance) + 1);
    if (int(want) < int(ps.stance) - 1) want = Stance(int(ps.stance) - 1);
    if (int(want) < int(ps.stance)) {
        // Rising (0x50a0a1 crouch request from prone, 0x50a148 stand request from prone or crouch): the box tested is the
        // requested stance's, but a stand request from prone tests the standing box (0x554 -> 0x100(%ebp), 0x50a150) before
        // stepping up to crouch. A blocked rise adds the event the cgame turns into the invalid-command hint: 0x8e (client
        // prone) with parm 2 (crouch blocked) or 1 (stand blocked) from prone, 0x8d (client crouch) with parm 1 from crouch.
        // The event is skipped while usercmd button 0x2000 (stance key held) is set (0x50a10c, 0x50a1f5, 0x50a28c).
        const bool standRequest = in.stance == Stance::Stand;
        const Vec maxs{mp.maxs[0], mp.maxs[1], maxsZ(standRequest ? Stance::Stand : want, mp)};
        const TraceResult tr = w.trace(ps.origin, ps.origin, mp.mins, maxs, Contents::PlayerSolid);
        if (tr.startSolid || tr.allSolid) {
            if (!in.stanceHeld) addPlayerEvent(ps, ps.stance == Stance::Prone ? 0x8e : 0x8d, standRequest ? 1 : 2);
            return;
        }
    } else if (want == Stance::Prone && !ps.onGround) {
        return;
    }
    const int target = want == Stance::Prone ? 11 : want == Stance::Crouch ? 40 : 60;
    ps.transitionDown = target < ps.viewHeightTarget;
    ps.viewHeightTarget = target;
    ps.transitionStartMs = ps.timeMs;
    ps.stance = want;
}

void updateViewHeight(PlayerState& ps) {
    if (ps.transitionStartMs < 0) {
        ps.viewHeight = float(ps.viewHeightTarget);
        return;
    }
    const int len = transitionMs(ps.viewHeightTarget, ps.transitionDown);
    const int percent = int(std::clamp<int64_t>((ps.timeMs - ps.transitionStartMs) * 100 / len, 0, 100));
    ps.viewHeight = viewHeightAt(ps.viewHeightTarget, ps.transitionDown, percent);
    if (percent >= 100) ps.transitionStartMs = -1;
}

// Command scale (0x508140, 0x5080a0): g_speed · max(|forward| (×backSpeedScale when backwards),
// |right|·strafeSpeedScale) / (|command| · 127), then ×0.65 crouched, ×0.15 prone, × the weapon's
// moveSpeedScale; ×0.4 once while aiming (pm_flags 0x100) or leaning; walk only: ×0.4 with command bit 0x800
// (0x508379; the bit is the shell-shock flag, MoveParams::shellShockSlow). TODO(spec): the blend during crouch/prone
// transitions (0x509760), pm_type 2 (×3) and 3 (×6).
float cmdScale(const PlayerState& ps, float f, float r, const MoveParams& mp, bool walking) {
    const float fb = f < 0 && walking ? f * mp.backSpeedScale : f;
    const float rs = walking ? std::fabs(r * mp.strafeSpeedScale) : std::fabs(r);
    const float m = std::max(std::fabs(fb), rs);
    if (m == 0) return 0;
    float s = float(mp.speed) * m / (std::sqrt(f * f + r * r) * 127.0f);
    if (walking) {
        if (ps.stance == Stance::Prone) s *= 0.15f;
        else if (ps.stance == Stance::Crouch) s *= 0.65f;
        if (mp.weaponSpeedScale > 0) s *= mp.weaponSpeedScale;
        if (mp.shellShockSlow) s *= 0.4f; // 0x508379, after the weapon scale (0x508370)
    }
    if (ps.adsSlow || ps.lean != 0) s *= 0.4f; // pm_flags 0x100 or a nonzero leanf (ps+0x4c) (0x508140, 0x5080a0)
    return s;
}

// Friction (0x507c80): control = max(speed, stopspeed); drop = control · friction · frametime on the ground.
void applyFriction(PlayerState& ps, float dt, const MoveParams& mp) {
    Vec v = ps.velocity;
    if (ps.onGround) v[2] = 0;
    const float speed = std::sqrt(dot(v, v));
    if (speed < 1.0f) {
        ps.velocity = {0, 0, ps.onGround ? 0.0f : ps.velocity[2]};
        return;
    }
    float drop = 0;
    if (ps.onGround) drop = std::max(speed, mp.stopSpeed) * (ps.landTimer ? 0.3f : 1.0f) * mp.friction * dt; // 0x507d10: pm_flags 0x200 x0.3
    const float ns = std::max(0.0f, speed - drop) / speed;
    ps.velocity = scale(ps.velocity, ns);
}

// Accelerate (0x507eb0): accelspeed = max(wishspeed, stopspeed) · frametime · accel, capped at the missing speed.
// TODO(spec): the inertiaMax limit of 0x507e50.
void accelerate(PlayerState& ps, const Vec& wishDir, float wishSpeed, float accel, float dt, const MoveParams& mp) {
    const float add = wishSpeed - dot(ps.velocity, wishDir);
    if (add <= 0) return;
    const float accelSpeed = std::min(add, std::max(wishSpeed, mp.stopSpeed) * dt * accel);
    ps.velocity = ::iw2::game::add(ps.velocity, scale(wishDir, accelSpeed));
}

// Lean (0x50ade0): the target is −1 (left), +1 (right) or 0; only on the ground. The limit is 0.5, 0.25 at
// view height 11. The value moves towards the target by ms · max/350 and back to 0 by ms · max/280. A 16-unit cube
// traced from the eye to the leaned eye limits it to 1 − sqrt(1 − fraction).
// The allowleanleft/right flags (0x2000000/0x4000000, 0x50ae13) cancel the input in the callers.
// TODO(spec): ps flag 0x8000 and 0x20300 in ps+0x7c.
void updateLean(const CollisionWorld& w, PlayerState& ps, const PlayerInput& in, float ms, float yaw) {
    int dir = 0;
    if (ps.onGround) {
        if (in.leanLeft) dir -= 1;
        if (in.leanRight) dir += 1;
    }
    const float maxLean = ps.viewHeightTarget == 11 && ps.viewHeight <= 11.0f ? 0.25f : 0.5f;
    float f = ps.lean;
    if (dir == 0) {
        if (f > 0) f = std::max(0.0f, f - ms * (1.0f / 280.0f) * maxLean);
        else if (f < 0) f = std::min(0.0f, f + ms * (1.0f / 280.0f) * maxLean);
    } else if (dir < 0) {
        if (f > -maxLean) f -= ms * (1.0f / 350.0f) * maxLean;
        f = std::max(f, -maxLean);
    } else {
        if (f < maxLean) f += ms * (1.0f / 350.0f) * maxLean;
        f = std::min(f, maxLean);
    }
    ps.lean = f;
    if (f != 0) {
        const Vec eye{ps.origin[0], ps.origin[1], ps.origin[2] + ps.viewHeight};
        const Vec off = leanOffset(yaw, f);
        const TraceResult tr = w.trace(eye, add(eye, off), {-8, -8, -8}, {8, 8, 8}, Contents::PlayerSolid);
        const float limit = 1.0f - std::sqrt(std::max(0.0f, 1.0f - tr.fraction));
        if (limit < std::fabs(ps.lean)) ps.lean = ps.lean < 0 ? -limit : limit;
    }
}

// Jump (0x5416b0, 0x5414d0): standing on the ground, newly pressed, 500 ms after the previous jump; vertical speed
// sqrt(2 · jump_height · gravity). Leaves the ground and remembers the jump's origin z (ps+0x74).
bool checkJump(PlayerState& ps, Pml& pml, const PlayerInput& in, const MoveParams& mp) {
    const bool jumpPressed = in.jump && !ps.jumpHeld;
    ps.jumpHeld = in.jump;
    if (!jumpPressed || ps.stance != Stance::Stand || ps.viewHeightTarget != 60 || ps.timeMs - ps.lastJumpMs <= 499) return false;
    pml.groundPlane = pml.walking = false;
    ps.onGround = false;
    ps.lastJumpMs = ps.timeMs;
    ps.jumpOriginZ = ps.origin[2];
    ps.pmTimeMs = 0;
    ps.jumping = true;
    ps.velocity[2] = std::sqrt(2.0f * mp.jumpHeight * mp.gravity);
    // 0x541640: the jump event 0x46 + surface of the ground just left (none for surface 0); with pm_flags 0x20 (ladder) it
    // is 0x5b instead.
    if (ps.onLadder) addPlayerEvent(ps, 0x5b);
    else if (const int surf = surfaceTypeOf(pml.groundTrace.surfaceFlags)) addPlayerEvent(ps, 0x46 + surf);
    ps.aimSpread = std::min(ps.aimSpread + mp.jumpSpreadAdd, 255.0f); // jump_spreadAdd (0x54153a)
    return true;
}

// Landing timer (0x541300, called by the walk move while pm_flags 0x80000 is set): 1200 ms after landing at least
// 18 units above the jump's origin, else 1800 ms; the flag is dropped once the timer reads above 1800. The speed
// factors (0.5, 0.65) only apply with jump_slowdownEnable, which defaults to false.
void landingTimer(PlayerState& ps) {
    if (ps.pmTimeMs > 1800) {
        ps.jumping = false;
        ps.jumpOriginZ = 0;
    } else if (ps.pmTimeMs == 0) {
        ps.pmTimeMs = ps.jumpOriginZ + 18.0f <= ps.origin[2] ? 1200 : 1800;
    }
}

Vec wishVelocity(const Vec& fwd, const Vec& right, float f, float r) { return add(scale(fwd, f), scale(right, r)); }

void airMove(const CollisionWorld& w, PlayerState& ps, Pml& pml, float f, float r, const Vec& fwd, const Vec& right,
             const MoveParams& mp) {
    // Air move (0x5085b0): horizontal wish direction, acceleration 1, clipped against a steep ground plane.
    applyFriction(ps, pml.frametime, mp);
    const float s = cmdScale(ps, f, r, mp, false);
    Vec wish = wishVelocity(fwd, right, f, r);
    const float wishSpeed = normalize(wish) * s;
    accelerate(ps, wish, wishSpeed, 1.0f, pml.frametime, mp);
    if (pml.groundPlane) ps.velocity = clipVelocity(ps.velocity, pml.groundTrace.normal);
    stepSlideMove(w, ps, pml, true, mp);
}

void walkMove(const CollisionWorld& w, PlayerState& ps, Pml& pml, const PlayerInput& in, float f, float r, Vec fwd,
              Vec right, const MoveParams& mp) {
    // Walk move (0x5086f0).
    if (ps.jumping) landingTimer(ps);
    if (checkJump(ps, pml, in, mp)) {
        airMove(w, ps, pml, f, r, fwd, right, mp);
        return;
    }
    applyFriction(ps, pml.frametime, mp);
    const float s = cmdScale(ps, f, r, mp, true);
    // The movement axes follow the ground plane.
    const Vec& n = pml.groundTrace.normal;
    fwd = clipVelocity(fwd, n);
    right = clipVelocity(right, n);
    normalize(fwd);
    normalize(right);
    Vec wish = wishVelocity(fwd, right, f, r);
    const float wishSpeed = normalize(wish) * s;
    // Acceleration 9 standing, 12 crouched, 19 prone, 1 on slick ground (surface flag 2), x0.25 with pm_flags 0x200
    // (0x5088d0, set by a damaging landing). pm_flags 0x400 (spawnSlide, ClientSpawn 0x4e925b) also gives 1 and applies gravity.
    const bool slick = (pml.groundTrace.surfaceFlags & 2) != 0 || ps.spawnSlide; // 0x50889b: pm_flags 0x400 acts like slick ground
    float accel = slick ? 1.0f : ps.stance == Stance::Prone ? 19.0f : ps.stance == Stance::Crouch ? 12.0f : 9.0f;
    if (ps.landTimer) accel *= 0.25f;
    accelerate(ps, wish, wishSpeed, accel, pml.frametime, mp);
    if (slick) ps.velocity[2] -= mp.gravity * pml.frametime;
    // Along the ground plane, keeping the speed.
    const float speed = std::sqrt(dot(ps.velocity, ps.velocity));
    const Vec old = ps.velocity;
    ps.velocity = clipVelocity(ps.velocity, n);
    if (dot(old, ps.velocity) > 0) {
        normalize(ps.velocity);
        ps.velocity = scale(ps.velocity, speed);
    }
    if (ps.velocity[0] != 0 || ps.velocity[1] != 0) stepSlideMove(w, ps, pml, false, mp);
}

// Mantle dvars (registered at 0x524a10): mantle_enable 1, mantle_check_range 20, mantle_check_radius 0.1,
// mantle_check_angle 60, mantle_view_yawcap 60.
constexpr float kMantleRange = 20.0f, kMantleRadius = 0.1f, kMantleAngle = 60.0f, kMantleYawCap = 60.0f;
constexpr uint32_t kMantleContents = 0x1000000;                   // 0x525652
constexpr uint32_t kMantleOn = 0x2000000, kMantleOver = 0x4000000; // surface flags
// Table 0x5a94b8 {up anim, over anim, height}.
struct MantleTrans { int up, over; float height; };
constexpr MantleTrans kMantleTrans[7] = {{1, 8, 57}, {2, 8, 51}, {3, 9, 45}, {4, 9, 39}, {5, 9, 33}, {6, 10, 27}, {7, 10, 21}};

// Length in ms (0x524b00): (key count − 1) / frame rate · 1000, truncated (0x565994).
int animMs(const assets::XAnim* a) {
    if (!a || a->frameRate <= 0) return 0;
    return int(float(a->keyCount() - 1) / float(a->frameRate) * 1000.0f);
}

// Delta translation at normalized time t (0x478740).
Vec animDelta(const assets::XAnim* a, float t) {
    if (!a || !a->delta || a->deltaTrans.keys.empty()) return {0, 0, 0};
    const auto p = assets::sampleTrans(a->deltaTrans, t * float(a->keyCount() - 1), a->keyCount());
    return {p[0], p[1], p[2]};
}

int mantleTotalMs(const PlayerState& ps, const MantleAnims& m) {
    const MantleTrans& tr = kMantleTrans[ps.mantleTrans];
    return animMs(m.anims[size_t(tr.up)]) + ((ps.mantleFlags & 1) ? animMs(m.anims[size_t(tr.over)]) : 0);
}

// 0x524bd0: the up animation's delta at t / up length, or past it the whole up delta plus the over animation's at
// (t − up) / over length; turned by the mantle yaw (0x423570).
Vec mantleDelta(const PlayerState& ps, const MantleAnims& m, int t) {
    const MantleTrans& tr = kMantleTrans[ps.mantleTrans];
    const assets::XAnim* up = m.anims[size_t(tr.up)];
    const int upMs = animMs(up);
    Vec d;
    if (t <= upMs) {
        d = animDelta(up, upMs > 0 ? float(t) / float(upMs) : 1.0f);
    } else {
        const assets::XAnim* over = m.anims[size_t(tr.over)];
        const int overMs = animMs(over);
        d = add(animDelta(up, 1.0f), animDelta(over, overMs > 0 ? float(t - upMs) / float(overMs) : 1.0f));
    }
    const float y = ps.mantleYaw * float(M_PI) / 180.0f, c = std::cos(y), sn = std::sin(y);
    return {c * d[0] - sn * d[1], sn * d[0] + c * d[1], d[2]};
}

// Result of the checks (the 0x34-byte block of 0x5257f0): direction, origin, ledge, over point, flags.
struct MantleResult {
    Vec dir{}, origin{}, ledge{}, over{};
    uint32_t flags = 0;
};

// 0x524df0: with MANTLE_OVER the player box (top at 50) is traced 18 down from 31 past the ledge; a clear trace
// makes its end the over point, otherwise the over flag is dropped and the over point is the ledge.
void mantleOverPoint(const CollisionWorld& w, const Pml& pml, MantleResult& r) {
    if (r.flags & 1) {
        const Vec start = add(r.ledge, scale(r.dir, 31.0f));
        const Vec end{start[0], start[1], start[2] - 18.0f};
        const TraceResult tr = w.trace(start, end, pml.mins, {pml.maxs[0], pml.maxs[1], 50.0f}, pml.mask, pml.passEntity);
        if (!tr.startSolid && tr.fraction >= 1.0f) {
            r.over = {end[0], end[1], (end[2] - start[2]) * tr.fraction + start[2]};
            return;
        }
        r.flags &= ~1u;
    }
    r.over = r.ledge;
}

// 0x524d00: the table row whose height is nearest the ledge height above the origin.
int nearestTrans(float originZ, float ledgeZ) {
    const float h = ledgeZ - originZ;
    int best = 0;
    for (int i = 1; i < 7; ++i)
        if (std::fabs(kMantleTrans[i].height - h) < std::fabs(kMantleTrans[best].height - h)) best = i;
    return best;
}

// 0x524f30: start mantling. The origin is put at the over point minus the whole move.
void mantleStart(PlayerState& ps, const MantleAnims& m, const MantleResult& r) {
    ps.mantleYaw = std::atan2(r.dir[1], r.dir[0]) * 180.0f / float(M_PI); // vectoyaw 0x421890
    if (ps.mantleYaw < 0) ps.mantleYaw += 360.0f;
    ps.mantleTimeMs = 0;
    ps.mantleTrans = nearestTrans(r.origin[2], r.ledge[2]);
    ps.mantleFlags = r.flags;
    const Vec d = mantleDelta(ps, m, mantleTotalMs(ps, m));
    ps.origin = {r.over[0] - d[0], r.over[1] - d[1], r.over[2] - d[2]};
    ps.mantling = true;
}

// 0x525010: a ledge `height` above the origin. A box of the player's width (top 2R) moved 16 forward from that
// height must be clear; dropped from there to 18 above the origin it must land on ground (normal z ≥ 0.7). A box
// 50 high must fit on the ledge. Then the hint flag is set; with the mantle button the over point and the stance
// flags are worked out and mantling starts.
bool mantleLedge(const CollisionWorld& w, PlayerState& ps, const Pml& pml, const PlayerInput& in, const MantleAnims& m,
                 MantleResult& r, float height) {
    const float R = pml.maxs[0];
    const Vec mins{-R, -R, 0}, maxs{R, R, R + R};
    const Vec start{r.origin[0], r.origin[1], r.origin[2] + height};
    const Vec fwd = add(start, scale(r.dir, 16.0f));
    TraceResult tr = w.trace(start, fwd, mins, maxs, pml.mask, pml.passEntity);
    if (tr.startSolid || tr.fraction < 1.0f) return false; // ledge too far away
    const Vec down{fwd[0], fwd[1], r.origin[2] + 18.0f};
    tr = w.trace(fwd, down, mins, maxs, pml.mask, pml.passEntity);
    if (tr.startSolid || tr.fraction == 1.0f) return false; // can't find ledge
    if (tr.normal[2] < 0.7f) return false;                  // ledge too steep
    r.ledge = {fwd[0], fwd[1], (down[2] - fwd[2]) * tr.fraction + fwd[2]};
    tr = w.trace(r.ledge, r.ledge, mins, {R, R, 50.0f}, pml.mask, pml.passEntity); // 0x507ac0
    if (tr.startSolid) return false; // can't fit crouched on the ledge
    ps.mantleFlags |= 8;
    r.flags |= 8;
    if (in.jump) {
        mantleOverPoint(w, pml, r);
        if (ps.stance == Stance::Stand) { // eFlags & 4 clear
            if (w.trace(r.ledge, r.ledge, pml.mins, pml.maxs, pml.mask, pml.passEntity).startSolid) r.flags |= 2;
            if (!w.trace(r.over, r.over, pml.mins, pml.maxs, pml.mask, pml.passEntity).startSolid) r.flags |= 4;
        }
        mantleStart(ps, m, r);
    }
    return true;
}

// Mantle check (0x5257f0, 0x5255d0): alive, not mantling, standing, not in the binocular weapon states 0x11–0x16.
// A thin box (radius mantle_check_radius) is traced along the flat view direction against mantle brushes
// (contents 0x1000000) from the back of the player box to mantle_check_range past its front. It must hit a
// MANTLE_ON or MANTLE_OVER surface that the player faces within mantle_check_angle; then ledges 60, 40, 20 above
// the origin are tried.
void mantleCheck(const CollisionWorld& w, PlayerState& ps, const Pml& pml, const PlayerInput& in, const MantleAnims& m) {
    ps.mantleFlags &= ~8u;
    if (ps.mantling || ps.stance != Stance::Stand || (in.weaponState > 0x10 && in.weaponState < 0x17)) return;
    const float yaw = in.yaw * float(M_PI) / 180.0f;
    MantleResult r;
    r.dir = {std::cos(yaw), std::sin(yaw), 0};
    r.origin = ps.origin;
    const float back = pml.maxs[0] - kMantleRadius;
    const TraceResult tr = w.trace(add(ps.origin, scale(r.dir, -back)), add(ps.origin, scale(r.dir, back + kMantleRange)),
                                   {-kMantleRadius, -kMantleRadius, pml.mins[2]}, {kMantleRadius, kMantleRadius, pml.maxs[2]},
                                   kMantleContents, pml.passEntity);
    if (tr.startSolid || tr.allSolid) return;           // mantle brush is too thick
    if (tr.fraction == 1.0f) return;                    // no mantle surface found
    if ((tr.surfaceFlags & (kMantleOn | kMantleOver)) == 0) return;
    Vec n{-tr.normal[0], -tr.normal[1], 0};
    if (normalize(n) < 1e-4f) return;                   // vertical normal
    if (std::acos(std::clamp(dot(r.dir, n), -1.0f, 1.0f)) * 57.2957763671875f > kMantleAngle) return; // not facing it
    if (tr.surfaceFlags & kMantleOver) r.flags |= 1;
    if (!mantleLedge(w, ps, pml, in, m, r, 60.0f) && !mantleLedge(w, ps, pml, in, m, r, 40.0f))
        mantleLedge(w, ps, pml, in, m, r, 20.0f);
}

// Mantle move (0x5259b0): the timer advances by the step, capped at the whole length; the origin follows the
// delta and the velocity is that change over the step. Flag 2 asks the client to crouch (event 0x8d) every step;
// at the end mantling stops and flag 4 asks it to stand (0x8c).
void mantleMove(PlayerState& ps, const MantleAnims& m, int ms) {
    ps.mantleFlags &= ~8u;
    if (ps.mantleFlags & 2) ps.stanceEvent = 1;
    const int total = mantleTotalMs(ps, m);
    const int old = ps.mantleTimeMs;
    ps.mantleTimeMs = std::min(old + ms, total);
    const Vec a = mantleDelta(ps, m, old), b = mantleDelta(ps, m, ps.mantleTimeMs);
    const Vec d{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    ps.origin = add(ps.origin, d);
    // TODO(spec): with no time left the exe divides by zero; the velocity is kept then.
    if (ps.mantleTimeMs != old) ps.velocity = scale(d, 1.0f / (float(ps.mantleTimeMs - old) * 0.001f));
    if (ps.mantleTimeMs == total) {
        ps.mantling = false;
        if (ps.mantleFlags & 4) ps.stanceEvent = 0;
    }
}

// Aiming (0x4dd020, then 0x50bd00): with the aim command and a weapon that may aim (0x4dcf70) the player aims
// standing or crouched; prone only when the aim was just pressed or the player is not moving. A prone aim also sets
// pm_flags 0x800 (0x4dd070, ORed with 0x40): PmoveSingle (0x50c748) keeps the prone player's moves at zero while it is
// set (see proneAimCancel for how it is cleared). Aiming slows movement unless prone or in weapon states 5–9.
void updateAds(PlayerState& ps, const PlayerInput& in, float f, float r) {
    const bool prone = ps.stance == Stance::Prone; // pm_flags 1
    ps.ads = false;
    if (in.adsHeld && in.adsAllowed) {
        if (!prone) ps.ads = true;
        else if (!ps.prevAdsHeld || (f == 0 && r == 0)) ps.ads = ps.proneAimLatch = true;
    }
    ps.prevAdsHeld = in.adsHeld;
    ps.adsSlow = ps.ads && !prone && !(in.weaponState >= 5 && in.weaponState <= 9);
}

// 0x4dd4f0: whether the weapon state can be interrupted by a movement command (false for 3, 0xb and 0xc–0x10; the
// other states, including the unlisted ones after a call to 0x4df0a0, give true).
bool weaponStateInterruptible(int state) { return !(state == 3 || (state >= 0xb && state <= 0x10)); }

// Prone aim cancel (PmoveSingle 0x50c4f6–0x50c5a3, prone only): a larger forward or right command than the previous one
// ends the aim (0x4dd090: pm_flags 0x40 cleared, player event 0x95) and the 0x800 latch when the weapon state allows
// it (0x4dd4f0). Otherwise, with the aim not active, the latch is cleared in weapon states 0, 1, 2 and 5.
void proneAimCancel(PlayerState& ps, float f, float r, int weaponState) {
    if (ps.stance != Stance::Prone) return;
    bool increased = false;
    if (f != ps.prevCmdFwd) increased = std::fabs(ps.prevCmdFwd) < std::fabs(f);
    if (!increased && r != ps.prevCmdRight) increased = std::fabs(ps.prevCmdRight) < std::fabs(r);
    if (increased) {
        if (weaponStateInterruptible(weaponState)) {
            ps.proneAimLatch = false;
            ps.ads = false;
            addPlayerEvent(ps, 0x95);
        }
        return;
    }
    if (!ps.ads && (weaponState == 0 || weaponState == 1 || weaponState == 2 || weaponState == 5)) ps.proneAimLatch = false;
}

// Hold breath (PM_UpdateHoldBreath 0x4dd350). The flag 0x4000 is raised by the +holdbreath command (0x8000) with the
// aim fraction 1 on a reticle weapon, only once the timer has run back to 0; the timer counts up by the frame time
// while held and down otherwise, at the hold time the flag drops and the timer jumps to hold + gasp. The sway scale
// ps+0x590 is tracked (DiffTrack 0x420a20) towards (s - 1) · adsFrac + 1 with s = 0 while holding (rate hold_lerp) or
// 1 + (gasp_scale - 1) · timer / (hold + gasp) otherwise (rate gasp_lerp).
void updateHoldBreath(PlayerState& ps, const PlayerInput& in, const BreathParams& b, int ms, float seconds) {
    const int holdMs = int(std::lrint(b.holdTime * 1000.0f)), gaspMs = int(std::lrint(b.gaspTime * 1000.0f));
    if (holdMs < 1) {
        ps.holdingBreath = false;
        ps.breathScale = 1.0f;
        ps.breathTimerMs = 0;
        return;
    }
    if (in.adsFrac == 1.0f && in.breathWeapon && in.holdBreathHeld) {
        if (ps.breathTimerMs == 0) ps.holdingBreath = true;
    } else {
        ps.holdingBreath = false;
    }
    ps.breathTimerMs += ps.holdingBreath ? ms : -ms;
    if (ps.breathTimerMs < 0) ps.breathTimerMs = 0;
    if (ps.holdingBreath && ps.breathTimerMs > holdMs) {
        ps.breathTimerMs = gaspMs + holdMs;
        ps.holdingBreath = false;
    }
    float s, rate;
    if (ps.holdingBreath) {
        s = 0.0f;
        rate = b.holdLerp;
    } else {
        s = (b.gaspScale - 1.0f) * (float(ps.breathTimerMs) / float(gaspMs + holdMs)) + 1.0f;
        rate = b.gaspLerp;
    }
    const float target = (s - 1.0f) * in.adsFrac + 1.0f;
    const float d = target - ps.breathScale, step = d * rate * seconds;
    ps.breathScale = (std::fabs(d) > 0.001f && !(std::fabs(d) <= std::fabs(step))) ? ps.breathScale + step : target;
}

// Aim spread scale (0x4de540, from PmoveSingle 0x50c6cb before the view angles are taken): decay = hipSpreadDecayRate
// (none, and no increase, when 0), halved in the air (no ground and pm_type ≠ 1), else × hipSpreadProneDecay prone
// or × hipSpreadDuckedDecay crouched (eFlags 8 / 4), × frametime. Unless fully aimed the increase is, per pitch and
// yaw, |AngleDelta(command angle, previous command angle)| · hipSpreadTurnAdd · 0.01 / frametime, plus
// hipSpreadMoveAdd with a forward or right move faster than bg_aimSpreadMoveSpeedThreshold, plus 2 · 1.28 in the
// air, × frametime. scale += (increase − decay) · 255, kept in 0..255.
void updateAimSpread(PlayerState& ps, const PlayerInput& in, float f, float r, float frametime, const MoveParams& mp) {
    const HipSpread& h = mp.hipSpread;
    const bool inAir = !ps.onGround;
    float decay = h.decayRate, inc = 0;
    if (decay == 0) {
        decay = 1.0f;
    } else {
        if (inAir) decay *= 0.5f;
        else if (ps.stance == Stance::Prone) decay *= h.proneDecay;
        else if (ps.stance == Stance::Crouch) decay *= h.duckedDecay;
        decay *= frametime;
        if (in.adsFrac != 1.0f) {
            if (h.turnAdd != 0 && ps.havePrevCmd) {
                const float cur[2] = {in.pitch, in.yaw}, old[2] = {ps.prevCmdPitch, ps.prevCmdYaw};
                for (int i = 0; i < 2; ++i)
                    inc += std::fabs(std::remainder(cur[i] - old[i], 360.0f)) * h.turnAdd * 0.01f * (1.0f / frametime);
            }
            if (h.moveAdd != 0 && (f != 0 || r != 0) &&
                mp.aimSpreadMoveSpeedThreshold * mp.aimSpreadMoveSpeedThreshold < ps.velocity[1] * ps.velocity[1] + ps.velocity[0] * ps.velocity[0])
                inc += h.moveAdd;
            if (inAir) inc += 1.28f + 1.28f;
            inc *= frametime;
        }
    }
    ps.aimSpread = std::clamp(ps.aimSpread + (inc - decay) * 255.0f, 0.0f, 255.0f);
    ps.prevCmdPitch = in.pitch;
    ps.prevCmdYaw = in.yaw;
    ps.havePrevCmd = true;
}

// Bob cycle (PM_Footsteps 0x50a850, after the move): not on a turret; in the air it stays. Slower than
// player_moveThreshhold (or pm_type 1) it is only reset below 1 unit/s. Otherwise bobmove = xy speed / s · k, s =
// g_speed scaled like the command (forward + right: ((strafe − 1)·0.75 + 2)·s/2, then backwards (back + 1)·s/2;
// backwards only: × back; right only: ((strafe − 1)·0.75 + 1)·s), × 0.4 while leaning or with pm_flags 0x100; prone
// × 0.15 and k 0.25 (0.24 with the 0.4), crouched × 0.65 and k 0.34 (0.315), standing k 0.335 (0.305), with
// pm_flags 0x80 0.36 (0.325); the cycle advances by trunc(bobmove · msec + cycle) & 0xff.
// A footstep event (PM_FootstepEvent 0x50a6d0) fires whenever bit 7 of (cycle + 64) changes, on the walking or ground
// plane (0x50abe6): the event of 0x508f20 for the ground's surface type: none for surface 0 (or flag 0x2000), prone
// 0x2f + surface, walking (pm_flags 0x100 or leaning) 0x18 + surface, else the run step 1 + surface.
// TODO(spec): the stance-transition blends (0x509760 and the prone-to-crouch lerp at 0x50aaaf). The ladder case is below.
void updateBobCycle(const CollisionWorld& w, PlayerState& ps, const Pml& pml, float f, float r, int ms, const MoveParams& mp) {
    // Ladder branch (0x50a8c5..0x50a94c, airborne with pm_flags 0x20, pm_type != 1): no bob within 300 ms of a jump, else
    // the cycle advances by the vertical speed · 0.010499 · 0.45 (0.026247 · 0.35 with pm_flags 0x100 or leaning) · msec.
    // A bit-7 change then fires the ladder footstep of PM_FootstepEvent (0x50a6d0): a box like the ladder check's
    // traced 31 units along -vLadder (mask & 0xfdfe3fff): event 1 + surface of the hit, 0x16 when nothing was hit or the
    // surface is 0.
    if (!ps.onGround && ps.onLadder) {
        if (ps.timeMs - ps.lastJumpMs < 300) return;
        const bool slow = ps.adsSlow || ps.lean != 0;
        const float k = slow ? 0.02624671906232834f * 0.3499999940395355f : 0.010498687624931335f * 0.44999998807907104f;
        const int oldCycle = ps.bobCycle;
        ps.bobCycle = int(ps.velocity[2] * k * float(ms) + float(ps.bobCycle)) & 0xff;
        if (((oldCycle + 0x40) ^ (ps.bobCycle + 0x40)) & 0x80) {
            Pml box = pml;
            box.mins = {pml.mins[0] + 6.0f, pml.mins[1] + 6.0f, 8.0f};
            box.maxs = {pml.maxs[0] - 6.0f, pml.maxs[1] - 6.0f, pml.maxs[2] < 8.0f ? 8.0f : pml.maxs[2]};
            box.mask = pml.mask & 0xfdfe3fffu;
            const Vec end{ps.origin[0] - ps.vLadder[0] * 31.0f, ps.origin[1] - ps.vLadder[1] * 31.0f, ps.origin[2] - ps.vLadder[2] * 31.0f};
            const TraceResult tr = pmTrace(w, box, ps.origin, end);
            const int surf = tr.fraction == 1.0f ? 0 : surfaceTypeOf(tr.surfaceFlags);
            addPlayerEvent(ps, surf ? 1 + surf : 0x16);
        }
        return;
    }
    if (!ps.onGround) return;
    const float xy = std::sqrt(ps.velocity[0] * ps.velocity[0] + ps.velocity[1] * ps.velocity[1]);
    if (xy < mp.moveThreshold) {
        if (xy < 1.0f) ps.bobCycle = 0;
        return;
    }
    float s = float(mp.speed);
    if (f != 0) {
        if (r != 0) {
            s = ((mp.strafeSpeedScale - 1.0f) * 0.75f + 1.0f + 1.0f) * s * 0.5f;
            if (f < 0) s = (mp.backSpeedScale + 1.0f) * s * 0.5f;
        } else if (f < 0) {
            s *= mp.backSpeedScale;
        }
    } else if (r != 0) {
        s = ((mp.strafeSpeedScale - 1.0f) * 0.75f + 1.0f) * s;
    }
    const bool slow = ps.adsSlow || ps.lean != 0;
    if (slow) s *= 0.4f;
    float k;
    if (ps.viewHeightTarget == 11) { s *= 0.15f; k = slow ? 0.24f : 0.25f; }
    else if (ps.viewHeightTarget == 40) { s *= 0.65f; k = slow ? 0.315f : 0.34f; }
    else k = ps.movingBack ? (slow ? 0.32499998807907104f : 0.36000001430511475f) : (slow ? 0.305f : 0.335f);
    const float bobmove = xy / s * k;
    const int oldCycle = ps.bobCycle;
    ps.bobCycle = int(bobmove * float(ms) + float(ps.bobCycle)) & 0xff;
    if ((pml.walking || pml.groundPlane) && (((oldCycle + 0x40) ^ (ps.bobCycle + 0x40)) & 0x80)) {
        const int surf = surfaceTypeOf(pml.groundTrace.surfaceFlags);
        if (surf != 0) addPlayerEvent(ps, ps.stance == Stance::Prone ? 0x2f + surf : (ps.adsSlow || ps.lean != 0) ? 0x18 + surf : 1 + surf);
    }
}

// PM_FoliageSounds (0x50ac30, after the bob cycle): below bg_foliagesnd_minspeed the last-event time is cleared once
// resetinterval has passed. From minspeed up the interval between events is slow + (fast - slow) · frac with
// frac = clamp((speed - min) / (max - min), 0, 1); when it has elapsed, a box of 0.75 × the stance box (z 0.9 ×
// maxs.z) at the origin traced with contents mask 2 (foliage) that is overlapping it (start solid) gives event 0x8b.
// The tested result byte is trace+0x23 (frame arithmetic at 0x50ad5e checked, the same byte 0x507b31 tests before its retry);
// by the trace_t layout (fraction 0, normal 4, surfaceFlags 0x10, contents 0x14, hit id 0x1c) that is startsolid.
// The trace's pass entity is ps+0xa8.
void foliageSounds(const CollisionWorld& w, PlayerState& ps, const Pml& pml, const MoveParams& mp) {
    const float speed = std::sqrt(ps.velocity[0] * ps.velocity[0] + ps.velocity[1] * ps.velocity[1]);
    if (speed < mp.foliageMinSpeed) {
        if (ps.lastFoliageMs + mp.foliageResetInterval < ps.timeMs) ps.lastFoliageMs = 0;
        return;
    }
    float frac = (speed - mp.foliageMinSpeed) / (mp.foliageMaxSpeed - mp.foliageMinSpeed);
    if (!(frac <= 1.0f)) frac = 1.0f;
    const int interval = int(float(mp.foliageFastInterval - mp.foliageSlowInterval) * frac + float(mp.foliageSlowInterval));
    if (ps.lastFoliageMs + interval >= ps.timeMs) return;
    const Vec mins{pml.mins[0] * 0.75f, pml.mins[1] * 0.75f, pml.mins[2] * 0.75f};
    const Vec maxs{pml.maxs[0] * 0.75f, pml.maxs[1] * 0.75f, pml.maxs[2] * 0.9f};
    if (!w.trace(ps.origin, ps.origin, mins, maxs, 2u, pml.passEntity).startSolid) return;
    addPlayerEvent(ps, 0x8b);
    ps.lastFoliageMs = ps.timeMs;
}

// One PmoveSingle step (0x50c4a0) for a normal player: ground trace, the walk or air move, ground trace.
// Leaving the ladder (0x50bd70): the flag 0x40000 marks a ladder that was just left.
void clearLadder(PlayerState& ps) {
    if (ps.onLadder) ps.ladderLeft = true;
    ps.onLadder = false;
}

// PM_CheckLadderMove (0x50bd90). A box 6 units smaller than the player's horizontally (mins z 8, maxs z at least 8) is
// traced 8 units (walking) or 30 units along the facing, or along -vLadder when already climbing in the air; a hit on a
// surface with flag 0x8 starts the climb (walking players must press forward), confirmed by a second trace against the
// stored normal. ps+0xd0 is the view height target (0x28 crouch, 0xb prone, as in PM_Footsteps 0x50a88b).
// pm_flags 0x400 is the spawn flag (spawnSlide).
void checkLadder(const CollisionWorld& w, PlayerState& ps, Pml& pml, const PlayerInput& in, float fwdMove, const Vec& fwd) {
    if (pml.walking) ps.ladderLeft = false;
    if (ps.pmTimeMs != 0 && !ps.onLadder && (ps.landTimer || ps.spawnSlide)) { clearLadder(ps); return; }
    const float dist = pml.walking ? 8.0f : 30.0f;
    Vec dir;
    if (ps.onLadder && !ps.onGround) dir = {-ps.vLadder[0], -ps.vLadder[1], -ps.vLadder[2]};
    else {
        dir = {fwd[0], fwd[1], 0};
        normalize(dir);
    }
    if (in.dead) {
        ps.onGround = false;
        pml.walking = pml.groundPlane = false;
        if (ps.onLadder) ps.ladderLeft = true;
        ps.onLadder = false;
        return;
    }
    // 0x50be6f: the ladder is left with 0x40000 set, prone (ps+0xd0 == 11, the view height target), or within 300 ms of a jump.
    if (ps.ladderLeft || ps.viewHeightTarget == 11 || ps.timeMs - ps.lastJumpMs < 300) {
        if (ps.onLadder) ps.ladderLeft = true;
        ps.onLadder = false;
        return;
    }
    const Vec mins{pml.mins[0] + 6.0f, pml.mins[1] + 6.0f, 8.0f};
    const Vec maxs{pml.maxs[0] - 6.0f, pml.maxs[1] - 6.0f, pml.maxs[2] < 8.0f ? 8.0f : pml.maxs[2]};
    auto climbTrace = [&](const Vec& d) {
        const Vec end{ps.origin[0] + d[0] * dist, ps.origin[1] + d[1] * dist, ps.origin[2] + d[2] * dist};
        Pml box = pml;
        box.mins = mins;
        box.maxs = maxs;
        const TraceResult tr = pmTrace(w, box, ps.origin, end);
        pml.mask = box.mask;
        return tr;
    };
    const TraceResult tr = climbTrace(dir);
    if (tr.fraction >= 1.0f || (tr.surfaceFlags & 8) == 0 || (pml.walking && fwdMove <= 0)) { clearLadder(ps); return; }
    if (ps.onLadder) return;
    ps.vLadder = tr.normal;
    const TraceResult t2 = climbTrace({-ps.vLadder[0], -ps.vLadder[1], -ps.vLadder[2]});
    if (t2.fraction < 1.0f && (t2.surfaceFlags & 8)) ps.onLadder = true;
    else clearLadder(ps);
}

// Ladder move (0x50c070). A jump leaves the ladder through the air move. The climb speed is forward · scale · 0.5 · k
// with k = clamp((view forward z + 0.25) · 2.5, -1, 1) (looking up climbs up); strafing runs along the ladder plane;
// accel 9. Without forward the vertical speed decays by gravity · frametime, without strafing the sideways speed is
// removed and gradually re-added (16 · frametime, at least 1); in the air the speed into the wall is removed and a
// mostly vertical climb is pressed 50 units/s into the wall. ps+0x78 (the ladder-relative yaw, clamped to ±75)
// is only copied as a float into entityState+0x74 (BG_PlayerStateToEntityState 0x4e7a9b / 0x4fb42c), i.e. for the body model; no
// first-person reader. TODO(spec): the entityState+0x74 consumer (third-person pose).
void ladderMove(const CollisionWorld& w, PlayerState& ps, Pml& pml, const PlayerInput& in, float f, float r, const MoveParams& mp) {
    if (checkJump(ps, pml, in, mp)) {
        airMove(w, ps, pml, f, r, {std::cos(in.yaw * float(M_PI) / 180.0f), std::sin(in.yaw * float(M_PI) / 180.0f), 0},
                {std::sin(in.yaw * float(M_PI) / 180.0f), -std::cos(in.yaw * float(M_PI) / 180.0f), 0}, mp);
        return;
    }
    const float yawR = in.yaw * float(M_PI) / 180.0f;
    const float fz = -std::sin(in.pitch * float(M_PI) / 180.0f);
    float k = (fz + 0.25f) * 2.5f;
    k = k > 1.0f ? 1.0f : (k < -1.0f ? -1.0f : k);
    Vec rt{std::sin(yawR), -std::cos(yawR), 0};
    const Vec& n = ps.vLadder;
    const float d0 = dot(rt, n);
    rt = {-d0 * n[0] + rt[0], -d0 * n[1] + rt[1], -d0 * n[2] + rt[2]};
    const float s = cmdScale(ps, f, r, mp, false);
    Vec wish{0, 0, 0};
    if (f != 0) wish[2] = f * s * k * 0.5f;
    if (r != 0) {
        const float q = r * s * 0.2f;
        wish[0] = q * rt[0];
        wish[1] = q * rt[1];
        wish[2] += q * rt[2];
    }
    const float wishSpeed = normalize(wish);
    accelerate(ps, wish, wishSpeed, 9.0f, pml.frametime, mp);
    if (f == 0) {
        const float g = mp.gravity * pml.frametime;
        float& vz = ps.velocity[2];
        if (vz > 0) vz = std::max(0.0f, vz - g);
        else vz = std::min(0.0f, vz + g);
    }
    if (r == 0) {
        float lx = rt[0], ly = rt[1];
        const float len = std::sqrt(lx * lx + ly * ly);
        if (len > 0) lx /= len, ly /= len;
        const float d = ly * ps.velocity[1] + lx * ps.velocity[0];
        if (d != 0) {
            ps.velocity[0] -= d * lx;
            ps.velocity[1] -= d * ly;
            float thing = d * pml.frametime * 16.0f;
            const float a = std::fabs(thing);
            if (std::fabs(d) > a) {
                if (a < 1.0f) thing = thing < 0 ? -1.0f : 1.0f;
                const float d2 = d - thing;
                ps.velocity[0] += lx * d2;
                ps.velocity[1] += ly * d2;
            }
        }
    }
    if (!pml.walking) {
        const float t = -(ps.velocity[1] * n[1] + ps.velocity[0] * n[0]);
        ps.velocity[0] += t * n[0];
        ps.velocity[1] += t * n[1];
        const Vec& v = ps.velocity;
        if (!(v[2] * v[2] < v[0] * v[0] + v[1] * v[1])) {
            ps.velocity[0] -= n[0] * 50.0f;
            ps.velocity[1] -= n[1] * 50.0f;
        }
    }
    stepSlideMove(w, ps, pml, false, mp);
}

void moveStep(const CollisionWorld& w, PlayerState& ps, const PlayerInput& in, int ms, const MoveParams& mp) {
    Pml pml;
    pml.frametime = float(ms) * 0.001f;
    pml.mask = mp.traceMask;
    pml.passEntity = mp.passEntity;
    pml.mins = mp.mins;
    pml.maxs = {mp.maxs[0], mp.maxs[1], maxsZ(ps.stance, mp)};
    pml.prevOriginZ = ps.origin[2];
    pml.prevVelZ = ps.velocity[2];
    pml.gravity = mp.gravity;
    pml.dead = in.dead;
    // Aiming while prone (pm_flags 0x40 from the previous step, view height target 11) clears the command's forward
    // and right moves (0x50c5c7).
    float fwdMove = std::clamp(in.forward, -1.0f, 1.0f), rightMove = std::clamp(in.right, -1.0f, 1.0f);
    if (in.dead) fwdMove = rightMove = 0; // 0x50c733: pm_type ≥ 4 clears the command's moves
    proneAimCancel(ps, std::clamp(in.forward, -1.0f, 1.0f), std::clamp(in.right, -1.0f, 1.0f), in.weaponState); // 0x50c4f6
    if ((ps.ads || ps.proneAimLatch) && ps.viewHeightTarget == 11) fwdMove = rightMove = 0; // 0x50c5d4, 0x50c750
    ps.prevCmdFwd = std::clamp(in.forward, -1.0f, 1.0f); // the next step's previous command (raw, before the zeroing above)
    ps.prevCmdRight = std::clamp(in.right, -1.0f, 1.0f);
    updateAimSpread(ps, in, fwdMove, rightMove, pml.frametime, mp);
    // While mantling (pm_flags 4) PmoveSingle skips the stance change and the ground trace (0x50c9b7).
    if (!ps.mantling) groundTrace(w, ps, pml);
    updateLean(w, ps, in, float(ms), in.yaw);
    if (in.dead) {
        // Dead (0x509d10 with pm_type ≥ 4): view height target 8, reached at 180 units/s (0x509840: targets other
        // than 11, 40 and 60 move linearly by frametime · 180).
        ps.viewHeightTarget = 8;
        ps.transitionStartMs = -1;
        const float stepH = pml.frametime * 180.0f;
        ps.viewHeight = ps.viewHeight > 8.0f ? std::max(8.0f, ps.viewHeight - stepH) : std::min(8.0f, ps.viewHeight + stepH);
    } else {
        if (!ps.mantling) updateStance(w, ps, in, mp);
        updateViewHeight(ps);
    }
    pml.maxs[2] = maxsZ(ps.stance, mp);
    // Mantling (PmoveSingle 0x50c9dc → 0x50ca17): checked every step; while mantling the player is off the ground and
    // the mantle move replaces the walk and air moves.
    if (mp.mantle && mp.mantle->loaded()) {
        if (!ps.mantling) mantleCheck(w, ps, pml, in, *mp.mantle);
        if (ps.mantling) {
            ps.onGround = false;
            updateStance(w, ps, in, mp); // 0x50ca29
            updateViewHeight(ps);
            updateAds(ps, in, fwdMove, rightMove);
            updateHoldBreath(ps, in, mp.breath, ms, pml.frametime);
            mantleMove(ps, *mp.mantle, ms);
            return;
        }
    }
    updateAds(ps, in, fwdMove, rightMove);
    updateHoldBreath(ps, in, mp.breath, ms, pml.frametime); // 0x4dd350, right after the aim update (PM_Weapon 0x4dfc6c)

    // pm_time runs down; when it expires pm_flags 0x80000, 0x400 and 0x200 are cleared.
    {
        // pm_flags 0x80 (0x50c70e / 0x50c728): set by a backward command, cleared by a forward one or sideways-only.
    if (fwdMove < 0) ps.movingBack = true;
    else if (fwdMove > 0 || rightMove != 0) ps.movingBack = false;
    // PM_CheckLadderMove runs before the pm_time countdown (0x50ca83).
        const Vec cf{std::cos(in.yaw * float(M_PI) / 180.0f), std::sin(in.yaw * float(M_PI) / 180.0f), 0};
        checkLadder(w, ps, pml, in, fwdMove * 127.0f, cf);
    }
    if (ps.pmTimeMs != 0) {
        if (ms < ps.pmTimeMs) {
            ps.pmTimeMs -= ms;
        } else {
            ps.pmTimeMs = 0;
            ps.jumping = false;
            ps.landTimer = false;
            ps.spawnSlide = false;
        }
    }

    const float yaw = in.yaw * float(M_PI) / 180.0f;
    const Vec fwd{std::cos(yaw), std::sin(yaw), 0}, right{std::sin(yaw), -std::cos(yaw), 0};
    const float f = fwdMove * 127.0f, r = rightMove * 127.0f;
    // Dead move (0x508a10, PmoveSingle 0x50ca6c): on the ground the speed drops by 20 per step, to 0.
    if (in.dead && pml.walking) {
        const float sp = std::sqrt(dot(ps.velocity, ps.velocity)) - 20.0f;
        if (sp <= 0) ps.velocity = {0, 0, 0};
        else { normalize(ps.velocity); ps.velocity = scale(ps.velocity, sp); }
    }
    if (ps.onLadder) ladderMove(w, ps, pml, in, f, r, mp);
    else if (pml.walking) walkMove(w, ps, pml, in, f, r, fwd, right, mp);
    else airMove(w, ps, pml, f, r, fwd, right, mp);
    groundTrace(w, ps, pml);
    updateBobCycle(w, ps, pml, f, r, ms, mp);
    foliageSounds(w, ps, pml, mp);
}

} // namespace

void breathOnFire(PlayerState& ps, const MoveParams& mp, float adsFrac, bool breathWeapon) {
    if (adsFrac != 1.0f || !breathWeapon) return;
    const int holdMs = int(std::lrint(mp.breath.holdTime * 1000.0f));
    if (ps.breathTimerMs < holdMs) {
        ps.breathTimerMs += int(std::lrint(mp.breath.fireDelay * 1000.0f));
        if (ps.breathTimerMs > holdMs) ps.breathTimerMs = holdMs;
    }
    ps.holdingBreath = false;
}

Vec leanOffset(float yawDeg, float lean) {
    if (lean == 0) return {0, 0, 0};
    const float g = (2.0f - std::fabs(lean)) * lean;
    const float roll = g * 16.0f * float(M_PI) / 180.0f;
    const float yaw = yawDeg * float(M_PI) / 180.0f;
    const float d = g * 20.0f;
    return {std::cos(roll) * std::sin(yaw) * d, -std::cos(roll) * std::cos(yaw) * d, -std::sin(roll) * d};
}

std::array<float, 2> hipSpreadRange(const HipSpread& h, float vh) {
    if (vh > 40.0f) {
        const float t = (vh - 40.0f) * 0.05f;
        return {(h.standMin - h.duckedMin) * t + h.duckedMin, (h.max - h.duckedMax) * t + h.duckedMax};
    }
    const float t = (vh - 11.0f) * 0.03448275849223137f;
    return {(h.duckedMin - h.proneMin) * t + h.proneMin, (h.duckedMax - h.proneMax) * t + h.proneMax};
}

void addFireSpread(PlayerState& ps, const HipSpread& h, float adsFrac) {
    if (adsFrac == 1.0f) return;
    ps.aimSpread = std::min(ps.aimSpread + h.fireAdd * 255.0f, 255.0f);
}

bool mantleLowersWeapon(const PlayerState& ps) { return ps.mantling && kMantleTrans[ps.mantleTrans].over != 10; }

bool MantleAnims::loaded() const {
    for (size_t i = 1; i < anims.size(); ++i)
        if (!anims[i]) return false;
    return true;
}

const char* mantleAnimName(int index) {
    static constexpr const char* kNames[11] = {"player_mantle_root", "player_mantle_up_57", "player_mantle_up_51",
                                               "player_mantle_up_45", "player_mantle_up_39", "player_mantle_up_33",
                                               "player_mantle_up_27", "player_mantle_up_21", "player_mantle_over_high",
                                               "player_mantle_over_mid", "player_mantle_over_low"};
    return index >= 0 && index < 11 ? kNames[index] : "";
}

float mantleClampYaw(const PlayerState& ps, float yaw) {
    if (!ps.mantling) return yaw;
    float d = std::remainder(ps.mantleYaw - yaw, 360.0f); // AngleNormalize180 (0x424040)
    if (d >= -kMantleYawCap && d <= kMantleYawCap) return yaw;
    // Past the cap the view is put on the near edge: mantle yaw + cap when the view is to the left, − cap otherwise.
    return ps.mantleYaw + (d <= 0 ? kMantleYawCap : -kMantleYawCap);
}

void playerMove(const CollisionWorld& w, PlayerState& ps, const PlayerInput& in, float dt, const MoveParams& mp) {
    ps.stanceEvent = -1;
    // Pmove (0x50cc10) runs steps of at most 66 ms; PmoveSingle (0x50c4a0) clamps a step to 1..200 ms.
    int ms = std::max(1, int(std::lround(dt * 1000.0f)));
    while (ms > 0) {
        const int step = std::min(ms, 66);
        ps.timeMs += step;
        moveStep(w, ps, in, step, mp);
        ms -= step;
    }
}

} // namespace iw2::game
