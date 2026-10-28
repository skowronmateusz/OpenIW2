// Particle evaluation following CoD2SP_s.exe 1.3 where known:
// position = ∫velocity graph + ∫velocity2 graph (× scale × life, in effect axes unless absoluteVel) +
// gravity · t² (FUN_004816c0, FUN_004812b0, FUN_004814a0); channels lerp graph → Rand graph by a per-particle
// random when the matching useRandom flag is set, times a per-particle scale (FUN_0048ce70, FUN_004804c0).
// Inferred and marked: spawn shapes other than the origin box, sprite drawing and size meaning.
#include "game/fx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace iw2::game {
namespace {

FVec add(const FVec& a, const FVec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
FVec sub(const FVec& a, const FVec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
FVec mul(const FVec& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }

const assets::EfxGraph* graph(const assets::EfxElement& e, const char* name) {
    auto it = e.graphs.find(name);
    return it == e.graphs.end() ? nullptr : &it->second;
}

float channel(const assets::EfxElement& e, const char* name, const char* randName, bool useRand, float rnd, float u, int c, float fallback) {
    const auto* g = graph(e, name);
    if (!g) return fallback;
    float v = g->sample(u, c);
    if (useRand)
        if (const auto* r = graph(e, randName)) v += (r->sample(u, c) - v) * rnd;
    return v;
}

} // namespace

// The game's linear congruential generator (0x59b2a8: seed = seed·0x343fd + 0x269ec3; the draw is seed >> 17, a 15-bit
// integer, divided by 0x591efc = 32768 in the float helpers 0x48a2c0 / 0x426250).
float FxSystem::frand() {
    seed_ = seed_ * 0x343fd + 0x269ec3;
    return float((seed_ >> 17) & 0x7fff) / 32768.0f;
}

// MSVC CRT rand() (0x553d5d): seed = seed · 0x343fd + 0x269ec3, result (seed >> 16) & 0x7fff.
int FxSystem::crtRand() {
    crtSeed_ = crtSeed_ * 0x343fd + 0x269ec3;
    return int((crtSeed_ >> 16) & 0x7fff);
}

// 0x48a2c0: lo + (hi − lo) · draw; equal bounds return lo WITHOUT consuming a draw.
float FxSystem::range(const std::vector<float>& v, float fallback) {
    if (v.empty()) return fallback;
    const float lo = v[0], hi = v.size() > 1 ? v[1] : v[0];
    if (lo == hi) return lo;
    return lo + (hi - lo) * frand();
}

namespace {

FVec cross(const FVec& a, const FVec& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
FVec normalized(const FVec& v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return l != 0.0f ? mul(v, 1.0f / l) : v; // 0x4210d0: a zero vector is left unchanged
}
// Rotation of v about the unit axis by `deg` degrees (0x421550). TODO(spec): the rotation sense; the angles drawn here
// are uniform over a full turn, so only the exact value for a given draw could differ.
FVec rotateAbout(const FVec& axis, const FVec& v, float deg) {
    const float a = deg * 0.017453292f, c = std::cos(a), s = std::sin(a);
    const float d = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
    return add(add(mul(v, c), mul(cross(axis, v), s)), mul(axis, d * (1.0f - c)));
}
// 0x440600: a world point into tag space, (p − origin) · row k; 0x440650: a direction, d · row k; 0x4404f0: back, origin +
// x·row0 + y·row1 + z·row2.
FVec toTagSpace(const FxTag& t, const FVec& p) {
    const FVec d{p[0] - t.origin[0], p[1] - t.origin[1], p[2] - t.origin[2]};
    FVec o;
    for (int k = 0; k < 3; ++k) o[size_t(k)] = d[0] * t.axes[size_t(k)][0] + d[1] * t.axes[size_t(k)][1] + d[2] * t.axes[size_t(k)][2];
    return o;
}
FVec dirToTagSpace(const FxTag& t, const FVec& d) {
    FVec o;
    for (int k = 0; k < 3; ++k) o[size_t(k)] = d[0] * t.axes[size_t(k)][0] + d[1] * t.axes[size_t(k)][1] + d[2] * t.axes[size_t(k)][2];
    return o;
}
FVec fromTagSpace(const FxTag& t, const FVec& l) {
    FVec o = t.origin;
    for (int k = 0; k < 3; ++k)
        for (int c = 0; c < 3; ++c) o[size_t(c)] += l[size_t(k)] * t.axes[size_t(k)][size_t(c)];
    return o;
}

// 0x4217f0: from the forward axis d build r = (d.z, −d.x, d.y) − d · (d·r), row1 = normalise(r), row2 = row1 × d.
void axesFromForward(FxAxes& ax) {
    const FVec& d = ax[0];
    FVec r{d[2], -d[0], d[1]};
    const float t = -(d[0] * r[0] + d[2] * r[2] + r[1] * d[1]);
    r = normalized(add(r, mul(d, t)));
    ax[1] = r;
    ax[2] = cross(r, d);
}

// An effect name of an element list as a file path ("/fx/x" -> "fx/x.efx"). TODO(spec): the exact normalisation of the name.
std::string fxPath(std::string name) {
    while (!name.empty() && name[0] == '/') name.erase(0, 1);
    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".efx") != 0) name += ".efx";
    return name;
}

// Emitter axes from its angles (0x483e50): 0x421da0 with a = angle · 0.017453292 (0x591f50) gives forward (cp·cy, cp·sy, −sp),
// right (cr·sy − sr·sp·cy, −(sr·sp·sy) − cr·cy, −sr·cp) and up (cr·sp·cy + sr·sy, cr·sp·sy − sr·cy, cr·cp); row1 = 0 − right.
FxAxes angleAxes(const FVec& ang) {
    const float sp = std::sin(ang[0] * 0.017453292f), cp = std::cos(ang[0] * 0.017453292f);
    const float sy = std::sin(ang[1] * 0.017453292f), cy = std::cos(ang[1] * 0.017453292f);
    const float sr = std::sin(ang[2] * 0.017453292f), cr = std::cos(ang[2] * 0.017453292f);
    const FVec right{cr * sy - sr * sp * cy, -(sr * sp * sy) - cr * cy, -sr * cp};
    return {FVec{cp * cy, cp * sy, -sp}, FVec{-right[0], -right[1], -right[2]}, FVec{cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp}};
}

} // namespace

// Spawn position of an element instance (0x48c290), shared by Particle / OrientedParticle / Decal / FxRunner. The random
// ranges are drawn in the order origin z, y, x (0xc8, 0xc0, 0xb8); cheapOrgCalc 0x40 skips the rotation by the effect axes.
// orgOnSphere 0x1: two angles (0..360°, 0..180°), radius from `radius` (0xe8) and the vertical extent from `height`
// (0xf0), added in world axes; axisFromSphere 0x2 then replaces the forward axis by the normalised offset (and rebuilds
// the others with 0x4217f0). orgOnCylinder 0x4: the vector radius · row1 + (random −0.5..0.5) · height · row0 turned by
// 0..360° about row0, added in world axes; axisFromSphere then takes that vector, normalised, as forward and rebuilds
// row1 = normalise(up × forward) (up = z, or y when |forward.z| ≥ 0.999) and row2 = forward × row1 (0x420f30, 0x4210d0).
FVec FxSystem::spawnPoint(const assets::EfxElement& e, const FVec& origin, FxAxes& ax) {
    const uint32_t sf = e.spawnFlagBits();
    // 0x487e70 -> 0x4879b0: `origin` holds 6 numbers (min xyz, max xyz) or 3 (fixed: min = max).
    auto org = e.numbers("origin");
    if (org.size() >= 3 && org.size() < 6) org = {org[0], org[1], org[2], org[0], org[1], org[2]};
    float x = 0, y = 0, z = 0;
    if (org.size() >= 6) {
        z = range({org[2], org[5]}, 0);
        y = range({org[1], org[4]}, 0);
        x = range({org[0], org[3]}, 0);
    }
    FVec pos = sf & 0x40 ? FVec{x, y, z} : add(add(mul(ax[0], x), mul(ax[1], y)), mul(ax[2], z));
    pos = add(pos, origin);
    if (sf & 0x1) {
        const float a1 = frand() * 360.0f * 0.017453292f, a2 = frand() * 180.0f * 0.017453292f;
        const float c1 = std::cos(a1), s1 = std::sin(a1), c2 = std::cos(a2), s2 = std::sin(a2);
        const float ra = range(e.numbers("radius"), 0), rb = range(e.numbers("height"), 0);
        const float k = ra * s2;
        const FVec off{k * s1, k * c1, rb * c2}; // x = r·sin(a2)·sin(a1), y = r·sin(a2)·cos(a1), z = height · cos(a2)
        pos = add(pos, off);
        if (sf & 0x2) {
            const float len = std::sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
            if (len != 0.0f) {
                ax[0] = mul(off, 1.0f / len);
                axesFromForward(ax);
            }
        }
    } else if (sf & 0x4) {
        const float h = range({-0.5f, 0.5f}, 0) * range(e.numbers("height"), 0); // 0x426250(−0.5, 0.5) · height
        const float r = range(e.numbers("radius"), 0);
        const FVec o = rotateAbout(ax[0], add(mul(ax[1], r), mul(ax[0], h)), range({0.0f, 360.0f}, 0));
        pos = add(pos, o);
        if (sf & 0x2) {
            const float len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
            if (len != 0.0f) {
                const FVec f = mul(o, 1.0f / len);
                ax[0] = f;
                const FVec up = std::fabs(f[2]) < 0.999f ? FVec{0, 0, 1} : FVec{0, 1, 0};
                ax[1] = normalized(cross(up, f));
                ax[2] = cross(f, ax[1]);
            }
        }
    }
    return pos;
}

// 0x485300: the per-element gate against the viewer. `spawnRange` parses "min max" into +0x60 / +0x64 and `cullrange` sets
// +0x64; with +0x60 ≠ 0 an element nearer than min · scale is skipped, with +0x64 ≠ 0 one farther than max · scale is
// skipped, scale = tan(fovX / 2) / tan(40°) (0x486e00, the view struct +0xf4) and the distance from the view origin
// (+0x14) to the effect origin. Without a host-provided view the gate passes.
bool FxSystem::inRange(const assets::EfxElement& e, const FVec& origin) const {
    if (!viewValid) return true;
    float lo = 0, hi = 0;
    const auto sr = e.numbers("spawnrange");
    if (!sr.empty()) {
        lo = sr[0];
        hi = sr.size() > 1 ? sr[1] : sr[0];
    }
    const auto cr = e.numbers("cullrange");
    if (!cr.empty()) hi = cr[0];
    if (lo == 0 && hi == 0) return true;
    const FVec d{viewOrigin[0] - origin[0], viewOrigin[1] - origin[1], viewOrigin[2] - origin[2]};
    const float d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (lo != 0 && d2 < (viewScale * lo) * (viewScale * lo)) return false;
    if (hi != 0 && !(d2 <= (viewScale * hi) * (viewScale * hi))) return false;
    return true;
}

// Delay of instance i of n (0x4853f0 loop 0x485566..0x485686): with evenDistribution (0x200) i · |max − min| / n, else a
// random in the range.
float FxSystem::elementDelay(const assets::EfxElement& e, int i, int n) {
    const auto delay = e.numbers("delay");
    if (delay.empty()) return 0;
    if (e.spawnFlagBits() & 0x200) {
        const float hi = delay.size() > 1 ? delay[1] : delay[0];
        return float(i) * std::fabs(hi - delay[0]) / float(n);
    }
    return range(delay, 0);
}

void FxSystem::play(const std::shared_ptr<const assets::EfxEffect>& fx, const FVec& origin, const FVec& forward, int64_t nowMs) {
    // Effect axes: forward, a right vector perpendicular to it, and up (inferred construction).
    FVec f = forward;
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    f = fl > 0 ? mul(f, 1.0f / fl) : FVec{0, 0, 1};
    FVec r = std::fabs(f[2]) < 0.99f ? FVec{f[1], -f[0], 0} : FVec{1, 0, 0};
    const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    r = mul(r, 1.0f / rl);
    const FVec up{r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    playAxes(fx, origin, FxAxes{f, r, up}, nowMs);
}

void FxSystem::playBolted(const std::shared_ptr<const assets::EfxEffect>& fx, std::shared_ptr<FxBolt> bolt, const FxAxes& axes, int64_t nowMs) {
    FxTag at;
    if (!bolt || !(*bolt)(at)) return; // 0x4853f0: an invalid attachment plays nothing
    curBolt_ = std::move(bolt);
    // 0x485479: with an attachment the effect's origin AND axes are the tag's orientation (0x48abb0); the caller's direction
    // is only used when the host does not know the tag axes.
    playAxes(fx, at.origin, at.hasAxes ? at.axes : axes, nowMs);
    curBolt_.reset();
}

void FxSystem::playAxes(const std::shared_ptr<const assets::EfxEffect>& fx, const FVec& origin, const FxAxes& axes, int64_t nowMs) {
    const FVec gatePos = gate_ ? *gate_ : origin; // a bolted play gates on the caller's position (0x4854ea), not the tag
    gate_ = nullptr;
    if (!fx) return;
    const std::shared_ptr<FxBolt> bolt = curBolt_;
    keep_.push_back(fx);
    for (const auto& e : fx->elements) {
        if (!inRange(e, gatePos)) continue;
        if (e.spawnFlagBits() & 0x400) { // 0x4854f9: frustumCull -> 0x486e90 (true = outside, element skipped)
            const auto cr = e.numbers("spawnfrustumcullradius");
            const float radius = cr.empty() ? 0.0f : cr[0];
            bool outside = false;
            for (const auto& pl : viewPlanes)
                if (pl[0] * origin[0] + pl[1] * origin[1] + pl[2] * origin[2] - pl[3] < -radius) outside = true;
            if (outside) continue;
        }
        const uint32_t flags = e.flagBits();
        if (bolt && (flags & 0x2)) { // 0x485bf0: a relative element needs a valid attachment
            FxTag t;
            if (!(*bolt)(t)) continue;
        }
        const bool known = e.type == "Decal" || e.type == "FxRunner" || e.type == "Particle" || e.type == "OrientedParticle" || e.type == "Emitter" ||
                           e.type == "Tail" || e.type == "Light" || e.type == "Line" || e.type == "Cloud" || e.type == "Cylinder" ||
                           e.type == "CameraShake";
        if (!known) { // TODO(spec): other element types
            static std::map<std::string, size_t> skipped; // IW2_FX_HIST survey of elements that were skipped
            if (std::getenv("IW2_FX_HIST") && ++skipped[e.type + " " + e.name] == 1)
                std::fprintf(stderr, "fx skip element type %s name %s\n", e.type.c_str(), e.name.c_str());
            continue;
        }
        // 0x4853f0: count = trunc(random(count) + 0.5) (default 1), the element is skipped when 0.
        const int count = int(range(e.numbers("count"), 1) + 0.5f);
        for (int i = 0; i < count; ++i) {
            // Every instance works on its own copy of the effect axes (0x485e20); randrotaroundfwd 0x100 (0x485c60) first
            // turns row1 about row0 by 0..360° and rebuilds row2 = row0 × row1. TODO(spec): the original draws these at
            // the delayed spawn time; we draw them at play time.
            FxAxes ax = axes;
            if (e.spawnFlagBits() & 0x100) {
                ax[1] = rotateAbout(ax[0], ax[1], range({0.0f, 360.0f}, 0));
                ax[2] = cross(ax[0], ax[1]);
            }
            const int64_t startMs = nowMs + int64_t(elementDelay(e, i, count));
            if (e.type == "Decal") {
                spawnDecals(e, flags, origin, ax, startMs);
                continue;
            }
            if (e.type == "CameraShake") {
                // Type 10 (name 0x57c920, parser 0x4868b7) spawns through 0x48db80: the spawn point (0x48c290), then `life` (+0x58),
                // `radius` (+0xe8) and `bounce` (+0x280) drawn and discarded (fstp). No instance is created and nothing shakes.
                (void)spawnPoint(e, origin, ax);
                (void)range(e.numbers("life"), 0);
                (void)range(e.numbers("radius"), 0);
                (void)range(e.numbers("bounce"), 0);
                continue;
            }
            if (e.type == "FxRunner") {
                // 0x48dac0: one `playfx` name at random, played at the element's spawn point with the instance axes
                // (0x4853f0). TODO(spec): the exact normalisation of the effect name.
                const auto names = e.list("playfx");
                if (names.empty()) continue;
                const FVec pos = spawnPoint(e, origin, ax);
                std::string name = names[std::min(names.size() - 1, size_t(frand() * float(names.size())))];
                while (!name.empty() && name[0] == '/') name.erase(0, 1);
                if (name.size() < 4 || name.compare(name.size() - 4, 4, ".efx") != 0) name += ".efx";
                // A delay below 1 ms spawns at once (0x485e20), else the child is queued for its time (0x4855d0..0x485676).
                if (startMs - nowMs < 1 && loader) {
                    curBolt_ = bolt;
                    playAxes(loader(name), pos, ax, nowMs);
                    curBolt_ = bolt;
                }
                else pending_.push_back({name, pos, ax, startMs, bolt});
                continue;
            }
            spawnParticle(e, flags, origin, ax, startMs);
        }
    }
}

void FxSystem::spawnParticle(const assets::EfxElement& e, uint32_t flags, const FVec& origin, FxAxes& ax, int64_t startMs) {
    FxParticle p;
    p.element = &e;
    if (e.type == "Cloud") {
        // 0x482850 (first thing the Cloud spawn 0x48de90 does): up to 4 tries of three draws x, y, z = draw · 2 − 1 (0x591efc,
        // 0x591e20); a vector with |v|² < 0.01 (0x591ea0) or > 1 is rejected, an accepted one is normalised (0x4210d0); after
        // 4 rejections the axis is (1, 0, 0).
        p.cloud = true;
        bool ok = false;
        for (int tries = 0; tries < 4 && !ok; ++tries) {
            FVec v;
            for (int k = 0; k < 3; ++k) v[size_t(k)] = frand() * 2.0f - 1.0f;
            const float l2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
            if (l2 < 0.01f || l2 > 1.0f) continue;
            p.cloudAxis = normalized(v);
            ok = true;
        }
        if (!ok) p.cloudAxis = {1, 0, 0};
    }
    p.startMs = startMs;
    p.endMs = p.startMs + std::max<int64_t>(1, int64_t(range(e.numbers("life"), 1000)));
    // Common init 0x48cb00 after the life draw: the box (+0x14 = def +0xa0 `min`, +0x20 = def +0xac `max`, 0x487be0 / 0x487c30),
    // then one `deathfx` entry (+0x30, def +0x78 / +0x7c) and one `impactfx` entry (+0x2c, def +0x70 / +0x74), each
    // (seed >> 17) · count >> 15 with a generator draw only when the list is not empty. The rest of the draw order of the
    // spawn is approximate here (see the other comments). deathFx (0x200) plays +0x30 when the element ends (`deathEffect`).
    {
        const auto mn = e.numbers("min"), mx = e.numbers("max");
        if (mn.size() >= 3) p.boxMins = {mn[0], mn[1], mn[2]};
        if (mx.size() >= 3) p.boxMaxs = {mx[0], mx[1], mx[2]};
        auto pick = [&](const char* key) {
            const auto names = e.list(key);
            if (names.empty()) return std::string();
            seed_ = seed_ * 0x343fd + 0x269ec3;
            return fxPath(names[std::min(names.size() - 1, size_t((uint64_t(seed_ >> 17 & 0x7fff) * names.size()) >> 15))]);
        };
        p.deathFx = pick("deathfx");
        p.impactFx = pick("impactfx");
    }
    p.origin = spawnPoint(e, origin, ax);
    if (curBolt_ && (flags & 0x2)) { // relative: the spawn point is stored in tag space (0x48c290 end, 0x440600)
        FxTag t0;
        if ((*curBolt_)(t0)) {
            p.bolt = curBolt_;
            p.bolt0 = t0.origin;
            p.local0 = toTagSpace(t0, p.origin);
            p.tag0 = t0;
        }
    }
    p.axis[0] = ax[0];
    p.axis[1] = ax[1];
    p.axis[2] = ax[2];
    if (e.type == "Particle" || e.type == "OrientedParticle") { // stepped by 0x480bd0 from the spawn point (tag space for relative)
        p.pos = p.bolt ? p.local0 : p.origin;
        p.lastMs = startMs;
    }
    const uint32_t randBits[6] = {0x2000, 0x4000, 0x8000, 0x10000, 0x20000, 0x40000};
    for (int k = 0; k < 6; ++k) p.rnd[k] = flags & randBits[k] ? frand() : 0.0f;
    for (int k = 0; k < 3; ++k) p.velRnd[k] = flags & 0x80000 ? frand() : 0.0f;
    for (int k = 0; k < 3; ++k) p.vel2Rnd[k] = flags & 0x100000 ? frand() : 0.0f;
    p.gravity = range(e.numbers("gravity"), 0);
    p.bounce = range(e.numbers("bounce"), 0); // 0x48ce70: +0x100 from def +0x280 (after gravity, wind and the spawn point)
    // Per-instance channel records, built by the element's init slot (vtable +0x1c): 0x4804c0 (Particle, OrientedParticle,
    // Emitter, Line) fills the 22 groups in key order without length / lengthRand (instance +0x144, stride 0xc: curve, 0,
    // scale); each scale is 0x48a2c0 of the group's `scale` range (no generator draw when min == max; a group without `scale`
    // has 1 1). The Rand groups' scales are drawn but never read: the random lerp 0x47f570 multiplies by the main group's
    // scale. Velocity / velocity2 X, Y, Z each keep their own scale (+0x1c4 / +0x1d0 / +0x1dc and +0x20c / +0x218 / +0x224,
    // read by 0x480e80 / 0x481050). Tail and Cylinder (0x482fa0) and Cloud (0x482970) call 0x4804c0 and then add the length
    // group (def +0x160) and lengthRand; Light (0x4840d0) only rgb, rgbRand, size, sizeRand.
    {
        auto drawScale = [&](const char* name) {
            const auto* g = graph(e, name);
            if (!g || g->scaleMin == g->scaleMax) return g ? g->scaleMin : 1.0f;
            return g->scaleMin + (g->scaleMax - g->scaleMin) * frand();
        };
        for (float& v : p.scale) v = 1.0f;
        if (e.type == "Light") {
            p.scale[0] = drawScale("rgb");
            (void)drawScale("rgbrand");
            p.scale[2] = drawScale("size");
            (void)drawScale("sizerand");
        } else {
            p.scale[0] = drawScale("rgb");
            (void)drawScale("rgbrand");
            p.scale[1] = drawScale("alpha");
            (void)drawScale("alpharand");
            p.scale[2] = drawScale("size");
            (void)drawScale("sizerand");
            p.scale[3] = drawScale("size2");
            (void)drawScale("size2rand");
            p.scale[5] = drawScale("rotationdelta");
            (void)drawScale("rotationdeltarand");
            static const char* const v1[6] = {"velocityx", "velocityy", "velocityz", "velocityxrand", "velocityyrand", "velocityzrand"};
            static const char* const v2[6] = {"velocity2x", "velocity2y", "velocity2z", "velocity2xrand", "velocity2yrand", "velocity2zrand"};
            for (int k = 0; k < 6; ++k) { const float sc = drawScale(v1[k]); if (k < 3) p.velScale[k] = sc; }
            for (int k = 0; k < 6; ++k) { const float sc = drawScale(v2[k]); if (k < 3) p.vel2Scale[k] = sc; }
            p.scale[6] = p.velScale[0];
            p.scale[7] = p.vel2Scale[0];
            if (e.type == "Tail" || e.type == "Cylinder" || e.type == "Cloud") {
                p.scale[4] = drawScale("length");
                (void)drawScale("lengthrand");
            }
        }
    }
    if (e.type == "Line") {
        // 0x48c850 (without org2fromTrace 0x8): origin2 ranges drawn z, y, x (+0xe0, +0xd8, +0xd0; `origin2 a b c d e f` =
        // x [a, d], y [b, e], z [c, f]), taken in world axes with cheapOrg2Calc 0x80, else as x·row0 + y·row1 + z·row2 of the
        // instance axes (0x424a70, after the spawn shape changed them), added to the effect origin. For relative Lines the spawn
        // stores B in tag space (0x440600) and the update maps both points back through the bolt (0x4404f0); here both follow
        // the tag's translation, like relative particles.
        p.line = true;
        const uint32_t sf = e.spawnFlagBits();
        auto o2 = e.numbers("origin2"); // 0x487ed0 -> 0x4879b0: 3 numbers = fixed
        if (o2.size() >= 3 && o2.size() < 6) o2 = {o2[0], o2[1], o2[2], o2[0], o2[1], o2[2]};
        auto org2 = [&] {
            float x = 0, y = 0, z = 0;
            if (o2.size() >= 6) {
                z = range({o2[2], o2[5]}, 0);
                y = range({o2[1], o2[4]}, 0);
                x = range({o2[0], o2[3]}, 0);
            }
            return sf & 0x80 ? FVec{x, y, z} : add(add(mul(ax[0], x), mul(ax[1], y)), mul(ax[2], z));
        };
        if (!(sf & 0x8)) {
            p.lineB = add(origin, org2());
        } else {
            // org2fromTrace 0x8: end = A + row0 · 16384 (0x592270), plus the origin2 sample with org2isOffset 0x20; B = A + (end − A)
            // · fraction of the effect trace (0x486f70, zero box). With traceImpactFx 0x10 one `impactfx` entry at random
            // ((seed >> 17) · count >> 15, none when the list is empty: entry 0 = null) plays at B along the trace normal (0x485220).
            FVec end = add(p.origin, mul(ax[0], 16384.0f));
            if (sf & 0x20) end = add(end, org2());
            float frac = 1.0f;
            FVec n{};
            if (!(trace && trace(p.origin, end, FVec{}, FVec{}, frac, n))) frac = 1.0f;
            p.lineB = add(p.origin, mul(sub(end, p.origin), frac));
            if (sf & 0x10) {
                const auto names = e.list("impactfx");
                if (!names.empty()) {
                    seed_ = seed_ * 0x343fd + 0x269ec3;
                    std::string name = names[std::min(names.size() - 1, size_t((uint64_t(seed_ >> 17 & 0x7fff) * names.size()) >> 15))];
                    while (!name.empty() && name[0] == '/') name.erase(0, 1);
                    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".efx") != 0) name += ".efx";
                    if (loader) impactQueue_.push_back({name, p.lineB, n, startMs});
                }
            }
        }
    }
    // Material and sequence (0x48cd20): the material at random ((seed >> 17)·count >> 15); with a frame
    // count n > 1 the start frame by sequenceStartFrameMode (+0x288; 0 fixed: sequenceFixedFrameValue − 1,
    // 1 random: (seed >> 17)·n >> 15, 2: the caller's value, TODO(spec)) and the rate by
    // sequencePlayRateMode (+0x290; 0: sequenceFixedFpsValue / 1000 frames per ms, 1: n / life), loop mode
    // and times copied (+0x298 / +0x29c). All of these default to 0 (the element is zeroed, 0x4878c0).
    const auto shaders = e.list("shaders");
    if (!shaders.empty()) {
        seed_ = seed_ * 0x343fd + 0x269ec3;
        p.shader = std::min(shaders.size() - 1, size_t((uint64_t(seed_ >> 17 & 0x7fff) * shaders.size()) >> 15));
        const auto [rows, cols] = atlasOf(shaders[p.shader]);
        const int n = rows * cols;
        if (n != 1) {
            const auto num = [&](const char* k) { const auto v = e.numbers(k); return v.empty() ? 0.0f : v[0]; };
            const int startMode = int(num("sequenceStartFrameMode"));
            if (startMode == 0) p.seqStart = int(num("sequenceFixedFrameValue")) - 1;
            else if (startMode == 1) {
                seed_ = seed_ * 0x343fd + 0x269ec3;
                p.seqStart = int((uint64_t(seed_ >> 17 & 0x7fff) * uint64_t(n)) >> 15);
            }
            const int rateMode = int(num("sequencePlayRateMode"));
            p.seqRate = rateMode == 0 ? num("sequenceFixedFpsValue") / 1000.0f
                                      : rateMode == 1 ? float(n) / float(p.endMs - p.startMs) : 0.0f;
            p.seqLoopMode = int(num("sequenceLoopMode"));
            p.seqLoopTimes = int(num("sequenceLoopTimes"));
        }
    }
    if (e.type == "Particle" || e.type == "OrientedParticle") p.rot0 = range(e.numbers("rotation"), 0); // 0x48ce70 (after the material, 0x48cb00)
    if (p.line) { // the Line spawn runs no sequence setup (0x48cd20) and its update no frame step (0x481c30)
        // +0x94 is never written for a Line: the pool (0x47f0a0) reuses freed slots without clearing, so the frame is the value
        // a previous element left there, 0 in never-used pool memory (0xa32380, zero-initialised data). Frame 0 here.
        p.seqStart = 0, p.seqRate = 0;
    }
    if (p.cloud) {
        // 0x48ce70 draws the start angle +0x44 from `rotation` (+0x220); 0x48de90 then draws +0x260 = 0x426250(0, 1) and copies
        // useLength (+0x9d) into +0x25c. The position steps like an Emitter (update 0x482a30 -> 0x480bd0).
        p.pos = p.prev = p.bolt ? p.local0 : p.origin; // relative: tag space
        p.lastMs = p.cloudLastMs = startMs;
        p.cloudAngle = range(e.numbers("rotation"), 0);
        p.lengthRnd = frand();
    }
    if (e.type == "Emitter" || e.type == "Tail") {
        // 0x48d7d0: start position from the spawn shape; the angles are the effect axes' angles plus a random draw in
        // `angle` (z, y, x order), the spin rates a draw in `angleDelta` (z, y, x), then the model at random
        // ((seed >> 17)·count >> 15; without models useModel is cleared). Both keys hold 3 values (fixed, min = max,
        // 0x4879b0) or 6 (min x y z, max x y z). The bounce offset (+0xc4) starts at zero.
        p.emitter = true;
        p.tail = e.type == "Tail";
        p.pos = p.prev = p.bolt ? p.local0 : p.origin; // relative: tag space (0x48c290 end)
        p.tailB = p.origin;
        p.lastMs = startMs;
        auto axisRange = [&](const std::vector<float>& v, int k) -> std::vector<float> {
            if (v.size() >= 6) return {v[size_t(k)], v[size_t(k) + 3]};
            if (v.size() >= 3) return {v[size_t(k)], v[size_t(k)]};
            return {0.0f, 0.0f};
        };
        const auto angleKey = e.numbers("angle"), deltaKey = e.numbers("angledelta");
        float a[3], r[3];
        for (int k = 2; k >= 0; --k) a[k] = range(axisRange(angleKey, k), 0);
        for (int k = 2; k >= 0; --k) r[k] = range(axisRange(deltaKey, k), 0);
        // Angles of the axes (0x421b30; the roll sign convention is inferred). TODO(spec).
        const FVec& f = ax[0];
        const float pitch = std::asin(std::clamp(-f[2], -1.0f, 1.0f)) * 57.2957795f;
        const float yaw = std::abs(f[2]) < 0.9999f ? std::atan2(f[1], f[0]) * 57.2957795f : 0.0f;
        const float roll = std::atan2(-ax[1][2], ax[2][2]) * 57.2957795f;
        p.ang = {pitch + a[0], yaw + a[1], roll + a[2]};
        p.angRate = {r[0], r[1], r[2]};
        const auto models = e.list("models");
        if ((flags & 0x10) && !models.empty()) {
            seed_ = seed_ * 0x343fd + 0x269ec3;
            p.model = models[std::min(models.size() - 1, size_t((uint64_t(seed_ >> 17 & 0x7fff) * models.size()) >> 15))];
        }
        if (!p.tail) {
            // 0x48d7d0 after the model: one `emitfx` entry (+0x290, def +0x80 / +0x84, generator draw only for a non-empty
            // list), `density` (def +0x260 -> +0x294) and `variance` (+0x268 -> +0x298) drawn, +0x24c = the spawn position,
            // +0x264 = the attachment origin for relative (0x47faa0 / 0x48a870) else 0, +0x270 = now, and the first emission
            // distance +0x274 = (2 · rand() / 32768 − 1) · variance + density (CRT rand 0x553d5d).
            const auto names = e.list("emitfx");
            if (!names.empty()) {
                seed_ = seed_ * 0x343fd + 0x269ec3;
                p.emitFx = fxPath(names[std::min(names.size() - 1, size_t((uint64_t(seed_ >> 17 & 0x7fff) * names.size()) >> 15))]);
            }
            p.density = range(e.numbers("density"), 0);
            p.variance = range(e.numbers("variance"), 0);
            p.emitP = p.pos;
            p.emitOff = p.bolt ? p.bolt0 : FVec{};
            p.emitLastMs = startMs;
            p.emitDist = (float(crtRand()) / 32768.0f * 2.0f - 1.0f) * p.variance + p.density;
        }
    }
    particles_.push_back(p);
}

// Velocity of an Emitter at graph position u (0x481810): velocity channels + velocity2 channels + gravity + the bounce
// offset (+0xc4). Velocity (0x480e80 / 0x481050): non-absolute components are taken along the instance axes (+0xd0, 0x424a70),
// absoluteVel 0x200000 / absoluteAccel 0x400000 ones as world vectors. With an attachment orientation (relative element) the
// result is a tag-space vector: non-absolute components are used as they are (0x481004), absolute ones are turned into tag
// space (0x440650, 0x480fd5 / 0x4811a5). Gravity (0x481220): g = trunc(life ms · u) · gravity (+0xf4) · 0.001 on world z,
// i.e. g · (row0.z, row1.z, row2.z) in tag space.
FVec FxSystem::velocityAt(const FxParticle& p, float u, const FxTag* orient) const {
    const assets::EfxElement& e = *p.element;
    const uint32_t flags = e.flagBits();
    // Each component is (curve + (randCurve − curve) · rnd) · its own instance scale (0x47f570 / 0x480efb: record +8).
    auto comp = [&](const char* name, const char* rname, bool useRand, float rnd, float scale) {
        const auto* g = graph(e, name);
        if (!g) return 0.0f;
        float v = g->sample(u, 0);
        if (useRand)
            if (const auto* rg = graph(e, rname)) v += (rg->sample(u, 0) - v) * rnd;
        return v * scale;
    };
    const bool rv = flags & 0x80000, rv2 = flags & 0x100000;
    const FVec v1{comp("velocityx", "velocityxrand", rv, p.velRnd[0], p.velScale[0]), comp("velocityy", "velocityyrand", rv, p.velRnd[1], p.velScale[1]),
                  comp("velocityz", "velocityzrand", rv, p.velRnd[2], p.velScale[2])};
    const FVec v2{comp("velocity2x", "velocity2xrand", rv2, p.vel2Rnd[0], p.vel2Scale[0]),
                  comp("velocity2y", "velocity2yrand", rv2, p.vel2Rnd[1], p.vel2Scale[1]),
                  comp("velocity2z", "velocity2zrand", rv2, p.vel2Rnd[2], p.vel2Scale[2])};
    auto frame = [&](const FVec& v, bool absolute) {
        if (absolute) return orient ? dirToTagSpace(*orient, v) : v;
        return orient ? v : add(add(mul(p.axis[0], v[0]), mul(p.axis[1], v[1])), mul(p.axis[2], v[2]));
    };
    FVec out = add(frame(v1, flags & 0x200000), frame(v2, flags & 0x400000));
    const int64_t ms = int64_t(float(p.endMs - p.startMs) * u);
    const float g = float(ms) * p.gravity * 0.001f;
    out = add(out, orient ? dirToTagSpace(*orient, FVec{0, 0, g}) : FVec{0, 0, g});
    return add(out, p.bounceOff);
}

// One update of an Emitter / Tail / Cloud up to nowMs. The original runs one update per client frame with the effect frame
// time (0x486b30: now − last, clamped to 0..200 ms (0xc8), 0 while paused); here the time since the last update is cut into
// steps of at most 200 ms. Per step: the attachment orientation of a relative element (0x47fcd0; failure ends the element),
// the physics step 0x480bd0 (false ends the element), then for Emitters (0x483630) the spin damping (an unchanged position,
// 0x415c50, multiplies the rates by 0.7, 0x591f30), the angles += frame ms · 0.01 · rate (0x483e50) and emitFx (0x483810).
// A relative element keeps its position in tag space (spawn: 0x48c290 end).
bool FxSystem::stepEmitter(FxParticle& p, int64_t nowMs) {
    while (p.lastMs < nowMs) {
        const int64_t dtMs = std::min<int64_t>(200, nowMs - p.lastMs);
        p.lastMs += dtMs;
        FxTag tag;
        const FxTag* orient = nullptr;
        if (p.bolt) {
            if (!(*p.bolt)(tag)) return false;
            orient = &tag;
        }
        const FVec old = p.pos;
        if (!physicsStep(p, dtMs, orient)) return false;
        if (!p.emitter || p.tail) continue; // Particle 0x480330, OrientedParticle 0x482630, Cloud 0x482a30, Tail 0x483010: no spin
        if (p.pos[0] == old[0] && p.pos[1] == old[1] && p.pos[2] == old[2])
            for (float& x : p.angRate) x *= 0.7f;
        for (int k = 0; k < 3; ++k) p.ang[size_t(k)] += float(dtMs) * 0.01f * p.angRate[size_t(k)];
        // 0x4836e1: a relative Emitter hands emitFx a zero drift, except with useModel when the last emission is older than now:
        // drift = (tag origin − +0x264) · (now − +0x270) · 0.001 (0x483772..0x4837c2, a product, as in the original).
        FVec drift{};
        if (orient && (p.element->flagBits() & 0x10) && !p.model.empty() && p.emitLastMs < p.lastMs)
            drift = mul(sub(orient->origin, p.emitOff), float(p.lastMs - p.emitLastMs) * 0.001f);
        emitFx(p, dtMs, orient, drift);
    }
    return true;
}

// 0x480bd0 (one step of frameMs): nothing at rest (0x1000000) or with frame time 0. u = min((now − spawn) / life, 1), all in
// seconds (0x591e30 = 0.001), with `now` the effect time after this frame; next = pos + velocity(u) · dt. Without usePhysics
// (0x20) the position becomes next. With it the segment pos -> next (both through the bolt for relative, 0x4404f0) is traced
// by 0x486f70 (CM_BoxTrace, model 0, contents 1, box = the instance `min` / `max` with useBBox 0x40, else the zero vector
// 0x56ce9c); a start- or all-solid trace (bytes +0x22 / +0x23) or fraction 1 moves to next. On a hit:
// - impactFx 0x800: the instance's impact effect (+0x2c) plays at start + (end − start) · fraction (0x47f390) along the
//   trace normal (0x485220: axes from 0x4217f0);
// - impactKills 0x400: the step returns false and the element dies (the position is not updated);
// - else 0x480a70 with the hit time uHit = min((fraction · dt + elapsed) / life, 1) (0x4809d0): the element comes to rest
//   (0x480a50: flags &= ~0x820, |= 0x1000000) when the normal points up (n.z > 0, 0x591e00) and |velocity(u)|² < 16
//   (0x591e84), or when the fraction is 0 (0x4809f0); otherwise with D = velocity(uHit) and n the normal (into tag space by
//   0x440650 for relative): offset = b·D − (D − offset) − 2 (b·D · n) n (b = +0x100 bounce, −2 = 0x59216c). The position
//   becomes pos + (next − pos) · fraction (0x47f390, tag space for relative).
bool FxSystem::physicsStep(FxParticle& p, int64_t frameMs, const FxTag* orient) {
    if (p.rest || frameMs == 0) return true;
    const assets::EfxElement& e = *p.element;
    const uint32_t flags = e.flagBits();
    const float dt = float(frameMs) * 0.001f;
    const float curS = float(p.lastMs - p.startMs) * 0.001f;
    const float lifeS = float(p.endMs - p.startMs) * 0.001f;
    float u = curS / lifeS;
    if (!(u <= 1.0f)) u = 1.0f;
    const FVec d = velocityAt(p, u, orient);
    const FVec next = add(p.pos, mul(d, dt));
    if (!(flags & 0x20)) {
        p.pos = next;
        return true;
    }
    const FVec a = orient ? fromTagSpace(*orient, p.pos) : p.pos;
    const FVec b = orient ? fromTagSpace(*orient, next) : next;
    const bool box = flags & 0x40;
    float frac = 1.0f;
    FVec n{};
    if (!(trace && trace(a, b, box ? p.boxMins : FVec{}, box ? p.boxMaxs : FVec{}, frac, n) && frac != 1.0f)) {
        p.pos = next;
        return true;
    }
    if ((flags & 0x800) && !p.impactFx.empty()) {
        DeferredPlay dp;
        dp.path = p.impactFx;
        dp.origin = add(a, mul(sub(b, a), frac));
        dp.axes[0] = n;
        axesFromForward(dp.axes);
        deferred_.push_back(std::move(dp));
    }
    if (flags & 0x400) return false;
    float uHit = (frac * dt + curS) / lifeS;
    if (!(uHit <= 1.0f)) uHit = 1.0f;
    const float speed2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if ((n[2] > 0.0f && speed2 < 16.0f) || frac == 0.0f) {
        p.rest = true;
    } else {
        const FVec nt = orient ? dirToTagSpace(*orient, n) : n;
        const FVec dh = velocityAt(p, uHit, orient);
        const FVec delta = sub(dh, p.bounceOff);
        const FVec r = mul(dh, p.bounce);
        const float k = (r[0] * nt[0] + r[1] * nt[1] + r[2] * nt[2]) * -2.0f;
        p.bounceOff = add(sub(r, delta), mul(nt, k));
    }
    p.pos = add(p.pos, mul(sub(next, p.pos), frac));
    return true;
}

// emitFx (0x100) of an Emitter, 0x483810, after the physics step; nothing with frame time 0. The emitter keeps its own point
// P (+0x24c, starts at the spawn position) that moves with the velocity only (no collision). With t the time of the last
// emission (+0x270), while t < now: k += 12 ms (ka = k · 0.001), u = min((now − spawn + acc) / life, 1) in seconds (acc =
// the seconds emitted this frame), V = velocity(u), Q = P + V · ka (+ drift · ka for relative). When |P + off − Q|² ≥ d²
// (d = +0x274, off = +0x264):
// - play +0x290: without an attachment at Q with row0 = V / |V| completed by 0x4217f0 (the element axes +0x48 when |V| ≤ 0);
//   with one, bolted to the attachment (origin and axes from the tag, 0x485479) with Q through the bolt only as the range
//   gate position (0x4854ea);
// - back-off: a = 2 · ka · |V|²; when a ≠ 0, n = round(((ka² · |V|² − d²) / a) · 1000 + 2^-30) (0x591e50, 0x591f20, fistp),
//   k = max(k − n, (t − +0x270)), ka = k · 0.001, Q = P + velocity(min((ka + now − spawn + acc) / life, 1)) · ka (no drift);
// - P = Q, +0x258 = velocity at that time, off += drift · ka (relative), acc += ka, +0x270 += k, t = +0x270, k = 0, and the
//   next distance d = (2 · rand() / 32768 − 1) · variance + density (CRT rand 0x553d5d, 0x591efc);
// otherwise t += 12. rand() is the MSVC CRT one (0x553d5d: per-thread seed · 0x343fd + 0x269ec3, (seed >> 16) & 0x7fff); the
// exe seeds it once with timeGetTime() (srand 0x553d50 at 0x40e149) and shares it with 25 other callers, so the sequence
// itself cannot be reproduced; `crtRand` keeps the generator and the 0..32767 range with seed 1 (the CRT default).
void FxSystem::emitFx(FxParticle& p, int64_t frameMs, const FxTag* orient, const FVec& drift) {
    const assets::EfxElement& e = *p.element;
    if (!(e.flagBits() & 0x100) || frameMs == 0) return;
    const int64_t now = p.lastMs;
    const float nowS = float(now - p.startMs) * 0.001f;
    const float lifeS = float(p.endMs - p.startMs) * 0.001f;
    float d2 = p.emitDist * p.emitDist;
    float acc = 0.0f;
    int64_t t = p.emitLastMs, k = 0;
    auto uAt = [&](float x) { const float v = x / lifeS; return v <= 1.0f ? v : 1.0f; };
    while (t < now) {
        k += 12;
        float ka = float(k) * 0.001f;
        const FVec target = add(p.emitP, p.emitOff);
        FVec v = velocityAt(p, uAt(nowS + acc), orient);
        FVec q = add(mul(v, ka), p.emitP);
        if (orient) q = add(q, mul(drift, ka));
        const FVec df = sub(target, q);
        if (!(df[0] * df[0] + df[1] * df[1] + df[2] * df[2] >= d2)) {
            t += 12;
            continue;
        }
        if (!p.emitFx.empty()) {
            DeferredPlay dp;
            dp.path = p.emitFx;
            if (orient) {
                dp.origin = fromTagSpace(*orient, q);
                dp.bolt = p.bolt;
            } else {
                dp.origin = q;
                const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (len <= 0.0f) {
                    dp.axes = angleAxes(p.ang);
                } else {
                    dp.axes[0] = mul(v, 1.0f / len);
                    axesFromForward(dp.axes);
                }
            }
            deferred_.push_back(std::move(dp));
        }
        const float v2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        const float a = ka * v2 * 2.0f;
        if (a != 0.0f) {
            const float x = ((ka * ka * v2 - d2) / a) * 1000.0f;
            const int64_t n = int64_t(std::nearbyint(double(x) + 9.313225746154785e-10));
            k -= n;
            if (k < t - p.emitLastMs) k = t - p.emitLastMs;
            ka = float(k) * 0.001f;
            v = velocityAt(p, uAt(ka + nowS + acc), orient);
            q = add(mul(v, ka), p.emitP);
        }
        p.emitP = q;
        if (orient) p.emitOff = add(p.emitOff, mul(drift, ka));
        acc += ka;
        p.emitLastMs += k;
        t = p.emitLastMs;
        k = 0;
        p.emitDist = (float(crtRand()) / 32768.0f * 2.0f - 1.0f) * p.variance + p.density;
        d2 = p.emitDist * p.emitDist;
    }
}

// Decal spawn (0x485b60) at the graph start (time 0) of each channel: rotation random in the `rotation` range
// (element +0x220); colour (0x4857d0) = rgb · a random scale from its range, lerped towards rgbRand by a random
// with useRandomColors 0x2000; alpha (0x4858c0) the same with useRandomAlpha 0x4000, clamped to 0..1; size
// (0x485a30) with useRandomSize 0x8000; the material picked at random from `shaders` ((seed >> 17)·count >> 15).
// The direction is the effect's forward axis (0x485bd9 passes the effect to 0x4ad430). TODO(spec): the spawn
// position (0x48c290; the particles' origin box is used), count / delay / spawnRange handling, the frustumCull
// spawn flag (spawnFrustumCullRadius), the exact order of the random draws.
void FxSystem::spawnDecals(const assets::EfxElement& e, uint32_t flags, const FVec& origin, FxAxes& ax, int64_t startMs) {
    const auto shaders = e.list("shaders");
    if (std::getenv("IW2_MARK_DEBUG")) std::fprintf(stderr, "decal element %s: %zu shaders\n", e.name.c_str(), shaders.size());
    if (shaders.empty()) return;
    {
        FxDecal d;
        d.startMs = startMs;
        d.origin = spawnPoint(e, origin, ax); // 0x48da90 -> 0x48c290
        d.dir = ax[0];
        d.rotation = range(e.numbers("rotation"), 0);
        auto scaled = [&](const char* name, const char* rname, bool useRand, int c, float fallback) {
            const auto* g = graph(e, name);
            const float sc = g ? g->scaleMin + (g->scaleMax - g->scaleMin) * frand() : 1.0f;
            return channel(e, name, rname, useRand, useRand ? frand() : 0.0f, 0.0f, c, fallback) * sc;
        };
        for (int c = 0; c < 3; ++c) d.rgba[size_t(c)] = scaled("rgb", "rgbrand", flags & 0x2000, c, 1.0f);
        d.rgba[3] = std::clamp(scaled("alpha", "alpharand", flags & 0x4000, 0, 1.0f), 0.0f, 1.0f);
        d.size = scaled("size", "sizerand", flags & 0x8000, 0, 1.0f);
        seed_ = seed_ * 0x343fd + 0x269ec3;
        d.material = shaders[std::min(shaders.size() - 1, size_t((uint64_t(seed_ >> 17 & 0x7fff) * shaders.size()) >> 15))];
        decals_.push_back(std::move(d));
    }
}

std::pair<int, int> FxSystem::atlasOf(const std::string& material) {
    return atlas ? atlas(material) : std::pair<int, int>{1, 1};
}

std::vector<FxDecal> FxSystem::takeDecals(int64_t nowMs) {
    std::vector<FxDecal> due;
    std::erase_if(decals_, [&](FxDecal& d) {
        if (d.startMs > nowMs) return false;
        due.push_back(std::move(d));
        return true;
    });
    return due;
}

void FxSystem::runPending(int64_t nowMs) {
    if (!impactQueue_.empty() && loader) { // traceImpactFx of Line spawns (played once their spawn time has come)
        std::vector<ImpactRun> due;
        std::erase_if(impactQueue_, [&](const ImpactRun& r) {
            if (r.startMs > nowMs) return false;
            due.push_back(r);
            return true;
        });
        for (const ImpactRun& r : due) { // 0x485220: row0 = the direction, completed by 0x4217f0
            FxAxes ax{};
            ax[0] = r.dir;
            axesFromForward(ax);
            playAxes(loader(r.path), r.origin, ax, r.startMs);
        }
    }
    if (pending_.empty() || !loader) return;
    std::vector<PendingRun> due;
    std::erase_if(pending_, [&](const PendingRun& p) {
        if (p.startMs > nowMs) return false;
        due.push_back(p);
        return true;
    });
    for (const PendingRun& p : due) {
        curBolt_ = p.bolt;
        playAxes(loader(p.path), p.origin, p.axes, p.startMs);
        curBolt_.reset();
    }
}

namespace {
// Cvar defaults (the 0x432020 registrations at 0x4a1c4f..).
constexpr float kTracerChance = 0.4f, kTracerWidth = 0.8f, kTracerSpeed = 5000.0f, kTracerLength = 300.0f;
constexpr float kTracerScale = 50.0f, kTracerScaleMinDist = 5000.0f, kTracerScaleDistRange = 25000.0f;
// 0x4b7c50: clamp(b / a * c, 1, c); c when a <= 0.
float tracerFactor(float a, float b, float c) {
    if (a <= 0) return c;
    const float v = b / a * c;
    return v < 1.0f ? 1.0f : v <= c ? v : c;
}
} // namespace

void FxSystem::addTracer(const FVec& from, const FVec& to, int64_t nowMs, int frameMs) {
    if (!(frand() < kTracerChance)) return; // rand() < chance * 32768
    FVec d{to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 0)) return;
    Tracer t;
    t.base = from;
    t.dir = {d[0] / len, d[1] / len, d[2] / len};
    t.length = len;
    const int64_t jitter = frameMs > 0 ? int64_t(frand() * 32768.0f) % frameMs / 2 : 0;
    t.startMs = nowMs - jitter;
    t.endMs = t.startMs + int64_t(len / kTracerSpeed * 1000.0f);
    tracers_.push_back(t);
}

// The element death slot (vtable +4, 0x4801d0; in the Particle, OrientedParticle, Emitter, Tail and Cylinder vtables 0x57c810,
// 0x57c7e4, 0x57c760, 0x57c6b0, 0x57c7b8, not Line / Light / Cloud). The update loop 0x48be90 calls it when the system time has
// passed the end (+0xbc; it clears impactKills 0x400 first, 0x48bec9) or when the update slot returned false. With deathFx
// 0x200, without impactKills 0x400 and with an effect in +0x30 it draws a, b, c = 0x426250(-1, 1) in that order, dir = (c, b, a)
// normalised when its length is above 1e-6 (double 0x592310), else (0, 0, 1), and plays +0x30 at the position +0x4 with row 0 =
// dir (0x485220). +0x4 is passed as it is, i.e. in tag space for a relative element. The play goes through the deferred list
// (after the update loop) like impactFx; the exe plays it inside the loop.
void FxSystem::deathEffect(const FxParticle& p, bool expired) {
    const assets::EfxElement& e = *p.element;
    const bool hasSlot = e.type == "Particle" || e.type == "OrientedParticle" || e.type == "Emitter" || e.type == "Tail" ||
                         e.type == "Cylinder";
    const uint32_t flags = e.flagBits() & (expired ? ~0x400u : ~0u);
    if (!hasSlot || !(flags & 0x200) || (flags & 0x400) || p.deathFx.empty()) return;
    auto draw = [&] { return -1.0f + 2.0f * frand(); }; // 0x426250(-1, 1): lo + (hi - lo) · (seed >> 17) / 32768
    const float a = draw(), b = draw(), c = draw();
    const double len = std::sqrt(double(c * c + b * b + a * a));
    FVec dir{0, 0, 1};
    if (len > 1e-6) {
        const float k = 1.0f / float(len);
        dir = {c * k, b * k, a * k};
    }
    DeferredPlay dp;
    dp.path = p.deathFx;
    dp.origin = e.type == "Cylinder" ? (p.bolt ? p.local0 : p.origin) : p.pos; // +0x4
    dp.axes[0] = dir;
    axesFromForward(dp.axes);
    deferred_.push_back(std::move(dp));
}

std::vector<FxSprite> FxSystem::sprites(int64_t nowMs) {
    runPending(nowMs);
    std::erase_if(particles_, [&](const FxParticle& p) {
        if (!p.dead && nowMs < p.endMs) return false;
        deathEffect(p, !p.dead);
        return true;
    });
    std::vector<FxSprite> out;
    sightBlockers_.clear();
    if (std::getenv("IW2_FX_DEBUG")) {
        size_t waiting = 0;
        int64_t maxWait = 0, maxLife = 0;
        for (const FxParticle& p : particles_) {
            if (nowMs < p.startMs) { ++waiting; maxWait = std::max(maxWait, p.startMs - nowMs); }
            maxLife = std::max(maxLife, p.endMs - p.startMs);
        }
        std::fprintf(stderr, "fx: %zu particles, %zu waiting (max %lld ms), max life %lld, pending runs %zu\n", particles_.size(), waiting,
                     (long long)maxWait, (long long)maxLife, pending_.size());
        if (std::getenv("IW2_FX_HIST")) { // per element (type, name, first shader) particle counts, largest first
            std::map<std::string, size_t> hist;
            for (const FxParticle& p : particles_) {
                const auto sh = p.element->list("shaders");
                hist[p.element->type + " " + p.element->name + " " + (sh.empty() ? std::string("-") : sh[0])]++;
            }
            std::vector<std::pair<size_t, std::string>> v;
            for (auto& [k, n] : hist) v.push_back({n, k});
            std::sort(v.rbegin(), v.rend());
            for (size_t i = 0; i < v.size() && i < 12; ++i) std::fprintf(stderr, "fx hist %6zu %s\n", v[i].first, v[i].second.c_str());
        }
    }
    models_.clear();
    lights_.clear();
    clouds_.clear();
    for (FxParticle& p : particles_) {
        if (nowMs < p.startMs) continue;
        const assets::EfxElement& e = *p.element;
        if (p.cloud) {
            // Cloud update 0x482a30: B (+0x9c) = the position before the step, Emitter step (0x480bd0), then the channels:
            // size (0x4818b0 -> +0x88), size2 (0x481940: the size2 channel with nonUniformScale (+0x104 = element +0x9c), else
            // = size), length (0x4829d0, only with useLength: +0x258, useRandomLength 0x20000 lerps by +0x260), rgb / alpha
            // bytes (0x4819b0 / 0x481a40), angle += frame ms · rotationDelta · 0.01 (0x481b90, useRandomRotationDelta 0x40000).
            // Then axis rows = rotation about +0x24c by the angle (0x423a50 quaternion, 0x423680 matrix), refEntity scale
            // (+0x98) = size, width (+0x88) = size2 result, height (+0x8c) = length with useLength else the size2 result; A (+0x7c)
            // = the position (through the bolt with relative, 0x4404f0). TODO(spec): `wind` (+0xf8) and affectedByWind 0x800000
            // have no reader in the exe's effect code.
            const FVec before = p.pos;
            if (!stepEmitter(p, nowMs)) {
                p.dead = true;
                continue;
            }
            const uint32_t flags = e.flagBits();
            const float u = std::min(1.0f, float(nowMs - p.startMs) / float(p.endMs - p.startMs));
            const float size = channel(e, "size", "sizerand", flags & 0x8000, p.rnd[2], u, 0, 1.0f) * p.scale[2];
            const auto nus = e.numbers("nonuniformscale"), ul = e.numbers("uselength");
            const bool nonUniform = !nus.empty() && int(nus[0]) != 0, useLength = !ul.empty() && int(ul[0]) != 0; // _atol != 0
            const float width = nonUniform ? channel(e, "size2", "size2rand", flags & 0x10000, p.rnd[3], u, 0, 1.0f) * p.scale[3] : size;
            const float height = useLength ? channel(e, "length", "lengthrand", flags & 0x20000, p.lengthRnd, u, 0, 1.0f) * p.scale[4] : width;
            const float rotDelta = channel(e, "rotationdelta", "rotationdeltarand", flags & 0x40000, p.rnd[5], u, 0, 0.0f) * p.scale[5];
            p.cloudAngle += float(nowMs - p.cloudLastMs) * rotDelta * 0.01f; // 0x591ea0
            p.cloudLastMs = nowMs;
            // 0x423a50: q = (sin(h) · axis, cos(h)), h = angle · pi / 360 (0x591fa4); 0x423680 with s = 2 / |q|²: rows
            // (1 − s(y² + z²), s(xy + wz), s(xz − wy)), (s(xy − wz), 1 − s(x² + z²), s(yz + wx)), (s(xz + wy), s(yz − wx), 1 − s(x² + y²)).
            const float h = p.cloudAngle * 0.008726646f, sn = std::sin(h);
            const float qx = sn * p.cloudAxis[0], qy = sn * p.cloudAxis[1], qz = sn * p.cloudAxis[2], qw = std::cos(h);
            const float sc = 2.0f / (qx * qx + qy * qy + qz * qz + qw * qw);
            FxCloud c;
            c.axis[0] = {1 - sc * (qy * qy + qz * qz), sc * (qx * qy + qw * qz), sc * (qx * qz - qy * qw)};
            c.axis[1] = {sc * (qx * qy - qw * qz), 1 - sc * (qz * qz + qx * qx), sc * (qy * qz + qx * qw)};
            c.axis[2] = {sc * (qy * qw + qx * qz), sc * (qy * qz - qx * qw), 1 - sc * (qy * qy + qx * qx)};
            c.origin = p.pos;
            c.prev = before;
            if (p.bolt) { // relative: A = the tag-space position through the bolt (0x4404f0); B stays the tag-space value (0x482b2d)
                FxTag now;
                if (!(*p.bolt)(now)) continue; // 0x47fcd0 fails: the update returns false
                c.origin = fromTagSpace(now, p.pos); // p.pos is tag space for relative elements
                c.prev = before;
            }
            c.size = size;
            c.width = width;
            c.height = height;
            // 0x4819b0 / 0x481a40: bytes = clamped channel · 255 truncated; without useAlpha (0x80) the alpha byte is 255 and
            // rgb is scaled by the alpha channel instead (docs/spec/effects.md).
            const float alpha = std::clamp(channel(e, "alpha", "alpharand", flags & 0x4000, p.rnd[1], u, 0, 1.0f), 0.0f, 1.0f);
            for (int k = 0; k < 3; ++k) {
                const float v = std::clamp(channel(e, "rgb", "rgbrand", flags & 0x2000, p.rnd[0], u, k, 1.0f), 0.0f, 1.0f);
                c.rgba[k] = uint8_t((flags & 0x80 ? v : v * alpha) * 255.0f);
            }
            c.rgba[3] = flags & 0x80 ? uint8_t(alpha * 255.0f) : 255;
            const auto shaders = e.list("shaders");
            if (!shaders.empty()) c.shader = shaders[std::min(p.shader, shaders.size() - 1)];
            // Cull slot 0x4827e0 -> 0x486e30: skipped when n·A − d < −r for a view plane (view +0x20, stride 0x10, count +0x80,
            // at most 5 with disableFarPlaneCulling 0x2000000), r = size + max(width, height) (the larger of +0x88 / +0x8c,
            // plus +0x98). TODO(spec): the exe's plane set; viewPlanes holds the four side planes (no far plane).
            bool culled = false;
            if (viewValid) {
                const float r = (width - height < 0.0f ? height : width) + size;
                for (const auto& pl : viewPlanes)
                    if (pl[0] * c.origin[0] + pl[1] * c.origin[1] + pl[2] * c.origin[2] - pl[3] < -r) culled = true;
            }
            if (!culled) clouds_.push_back(std::move(c));
            continue;
        }
        if (p.emitter) {
            p.prev = p.pos; // 0x483010 saves the position (+0x4 -> +0x24c) before the step
            if (!stepEmitter(p, nowMs)) {
                p.dead = true;
                continue;
            }
            if (p.tail) {} // drawn below
            else if (!p.model.empty()) {
                const float u = std::min(1.0f, float(nowMs - p.startMs) / float(p.endMs - p.startMs));
                // 0x483750 / 0x4837eb: refEntity scale +0x98 = the size channel (+0x88, 0x4818b0). TODO(spec): the model draw path
                // (rgb / alpha are not applied).
                FxModelInst mi{p.model, p.pos, p.ang, channel(e, "size", "sizerand", e.flagBits() & 0x8000, p.rnd[2], u, 0, 1.0f) * p.scale[2]};
                if (p.bolt) { // relative: +0x7c = the position through the bolt (0x483719), axis rows turned by 0x440540
                    FxTag now;
                    if (!(*p.bolt)(now)) continue;
                    mi.pos = fromTagSpace(now, p.pos);
                    const FxAxes la = angleAxes(p.ang);
                    mi.hasAxes = true;
                    for (int r = 0; r < 3; ++r) mi.axes[size_t(r)] = sub(fromTagSpace(now, la[size_t(r)]), now.origin);
                }
                models_.push_back(mi);
            }
            if (!p.tail) continue;
        }
        // Particle (0x480330) and OrientedParticle (0x482630) move by the physics step 0x480bd0 every update (false: the element
        // dies). The analytic position (0x4816c0) is only used by the exe for the late part of a spawn (0x48caa0, TODO(spec)).
        const bool moving = e.type == "Particle" || e.type == "OrientedParticle";
        if (moving && !stepEmitter(p, nowMs)) {
            p.dead = true;
            continue;
        }
        const uint32_t flags = e.flagBits();
        const float life = float(p.endMs - p.startMs);
        const float t = float(nowMs - p.startMs);
        const float u = std::min(1.0f, t / life);
        FVec pos = p.origin; // every type reaching here sets its own +0x7c below
        FxTag tagNow;
        const bool relative = p.bolt && !p.tail;
        if (relative) {
            // The current tag orientation (0x47fcd0 -> 0x48abb0); an invalid attachment ends the update.
            if (!(*p.bolt)(tagNow)) continue;
        }
        if (moving) pos = relative ? fromTagSpace(tagNow, p.pos) : p.pos; // +0x7c (0x48037a / 0x48269c)
        FxTag tailTag;
        if (p.tail) { // +0x7c = the position, through the bolt for relative (0x483072)
            if (p.bolt && !(*p.bolt)(tailTag)) continue;
            pos = p.bolt ? fromTagSpace(tailTag, p.pos) : p.pos;
        }
        FVec lineA = p.origin, lineB = p.lineB;
        if (e.type == "Cylinder") pos = relative ? fromTagSpace(tagNow, p.local0) : p.origin; // no motion (0x483450)
        if (e.type == "Light") pos = relative ? fromTagSpace(tagNow, p.local0) : p.origin; // update 0x4841e0: +0x7c = +0x4, no motion
        if (p.line) { // 0x482da0: A (+0x7c) = +0x4, B (+0x9c) = +0x24c, both through the bolt (0x4404f0) for relative
            if (relative) {
                lineA = fromTagSpace(tagNow, p.local0);
                lineB = fromTagSpace(tagNow, toTagSpace(p.tag0, p.lineB)); // B stored in tag space at spawn (0x440600)
            }
            pos = lineA;
        }
        FxSprite s;
        s.position = pos;
        for (int c = 0; c < 3; ++c) s.rgba[c] = std::clamp(channel(e, "rgb", "rgbrand", flags & 0x2000, p.rnd[0], u, c, 1.0f), 0.0f, 1.0f);
        s.rgba[3] = std::clamp(channel(e, "alpha", "alpharand", flags & 0x4000, p.rnd[1], u, 0, 1.0f), 0.0f, 1.0f);
        s.size = channel(e, "size", "sizerand", flags & 0x8000, p.rnd[2], u, 0, 1.0f) * p.scale[2];
        bool hidden = false;
        if (e.type == "Particle" || e.type == "OrientedParticle") {
            // Particle update 0x480330: size (0x4818b0), then (size ≠ 0) size2 (0x481940: the size2 channel with nonUniformScale,
            // else = size) and (size2 ≠ 0) rgb, alpha, angle += frame ms · rotationDelta · 0.01 (0x481b90), frame. The per-frame
            // sum is taken as its continuous limit: rot0 + 0.01 · life ms · ∫rotationDelta du (rand lerp 0x40000, scale).
            const auto nus = e.numbers("nonuniformscale");
            const bool nonUniform = !nus.empty() && int(nus[0]) != 0;
            s.size2 = nonUniform ? channel(e, "size2", "size2rand", flags & 0x10000, p.rnd[3], u, 0, 1.0f) * p.scale[3] : s.size;
            float rd = 0;
            if (const auto* g = graph(e, "rotationdelta")) {
                rd = g->integral(u);
                if (flags & 0x40000)
                    if (const auto* rg = graph(e, "rotationdeltarand")) rd += (rg->integral(u) - rd) * p.rnd[5];
                rd *= p.scale[5];
            }
            s.rotation = p.rot0 + 0.01f * life * rd;
            // Particle draw slot 0x4802f0: nothing is drawn while size (+0x88) or size2 (+0x8c) is 0. OrientedParticle (update
            // 0x482630 runs every channel unconditionally, draw 0x482620 always submits type 7): the quad plane comes from +0x48 =
            // +0x24c, the instance axis row 0 at spawn (0x48dc80; relative: into tag space with 0x440650 at 0x48dd53 and back
            // through the current orientation with 0x440540 at 0x4826ac).
            if (e.type == "Particle" && (s.size == 0.0f || s.size2 == 0.0f)) hidden = true; // still a sight blocker (0x480970)
            if (e.type == "OrientedParticle") {
                s.oriented = true;
                s.orientDir = p.axis[0];
                if (relative) s.orientDir = sub(fromTagSpace(tagNow, dirToTagSpace(p.tag0, p.axis[0])), tagNow.origin);
                // Cull slot 0x4825e0 -> 0x486e30: sphere at +0x7c with radius +0x88 (size), skipped when n·pos − d < −size for a
                // view plane (count system +0x80, at most 5 with disableFarPlaneCulling 0x2000000). viewPlanes: the four side planes.
                if (viewValid)
                    for (const auto& pl : viewPlanes)
                        if (pl[0] * pos[0] + pl[1] * pos[1] + pl[2] * pos[2] - pl[3] < -s.size) hidden = true;
            }
        }
        if (e.type == "Light") { // submitted as a dynamic light (0x4841b0 -> AddLightToScene), not drawn as a sprite
            FxLight l;
            l.origin = pos;
            l.radius = s.size;
            for (int c = 0; c < 3; ++c) l.rgb[c] = channel(e, "rgb", "rgbrand", flags & 0x2000, p.rnd[0], u, c, 1.0f);
            lights_.push_back(l);
            continue;
        }
        const auto shaders = e.list("shaders");
        if (!shaders.empty()) s.shader = shaders[std::min(p.shader, shaders.size() - 1)];
        // Frame (0x481c30): frame = start + _ftol((now − spawn) · rate); n frames; loop mode 0: frame mod n once
        // past the end; mode 1 with loop times > 0: mod n until (times + 1)·n, then the last frame (and times 0);
        // mode 1 with times ≤ 0: held on the last frame. Atlas rectangle (renderer 0x1003fa60): c = byte 0xf,
        // r = byte 0xe; u0 = (frame % c)/c, v0 = (frame / c)/r, u1 = u0 + 1/c, v1 = v0 + 1/r; one frame: 0, 0, 1, 1.
        if (!s.shader.empty()) {
            const auto [rows, cols] = atlasOf(s.shader);
            const int n = rows * cols;
            if (n != 1 && rows > 0 && cols > 0) {
                FxParticle& mp = p;
                int frame = int(t * p.seqRate) + p.seqStart;
                if (p.seqLoopMode == 0) {
                    if (frame >= n) frame %= n;
                } else if (p.seqLoopMode == 1) {
                    if (mp.seqLoopTimes > 0) {
                        if (frame >= n) {
                            if (frame < (mp.seqLoopTimes + 1) * n) frame %= n;
                            else {
                                frame = n - 1;
                                mp.seqLoopTimes = 0;
                            }
                        }
                    } else if (frame >= n) {
                        frame = n - 1;
                    }
                }
                s.uv[0] = float(frame % cols) / float(cols);
                s.uv[1] = float(frame / cols) / float(rows);
                s.uv[2] = s.uv[0] + 1.0f / float(cols);
                s.uv[3] = s.uv[1] + 1.0f / float(rows);
            }
        }
        if (flags & 0x1000) {
            // Alpha byte (0x481a40): the clamped alpha · 255 truncated (_ftol) with useAlpha (0x80), else 255.
            const int alphaByte = flags & 0x80 ? int(s.rgba[3] * 255.0f) : 255;
            sightBlockers_.push_back({pos, s.size * s.size, 1.0f - float(alphaByte) * (1.0f / 255.0f)});
        }
        if (p.line) {
            // Line draw 0x482d90 -> 0x480010 renderer type 8 (the Tail quad, 1.3 DLL 0x10041470 / 0x100409c0): A = +0x7c, B = +0x9c,
            // half width = the size channel (+0x88, 0x4818b0), colour bytes from rgb / alpha (0x4819b0 / 0x481a40).
            s.beam = s.beamTail = true;
            s.beamA = lineA;
            s.beamB = lineB;
            s.widthA = s.widthB = s.size;
            if (!viewValid) continue;
            const FVec a = sub(s.beamA, viewOrigin), b = sub(s.beamB, viewOrigin);
            const FVec c = cross(a, b);
            const float cl = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
            if (!(cl > 0)) continue;
            s.beamPerp = mul(c, 1.0f / cl);
        }
        if (e.type == "Cylinder") {
            // Spawn 0x48d6c0: position = the spawn point (0x48ce70), +0x48 = the instance axis row 0 (tag space for relative).
            // Update 0x483450 (no motion): size -> +0x88, size2 -> +0x8c (0x481940: = size without nonUniformScale), length ->
            // +0x258 (0x4830d0), rgb, alpha; A = position, B = position + length · row0 (both through the bolt). Draw 0x483440:
            // renderer type 9 (A = +0x3c, B = +0x48, +0x64 = size, +0x68 = size2 result).
            // DLL 0x100401f0: n = clamp(round((1 − (|(A + B)/2 − view| · lodScale + lodBias) / 1024) · 32), 8, 32) (0x1019bf58,
            // 0x1019bf20; lodScale / lodBias = r_lodScale (1) / r_lodBias (0) · tan(40°) / tan(fovX / 2), 0x10025b76).
            // TODO(spec): r_lodScale / r_lodBias other than their defaults; the mode-2 builder 0x100405e0 (dvar 0x101ce710 = 2).
            const auto nus = e.numbers("nonuniformscale");
            const bool nonUniform = !nus.empty() && int(nus[0]) != 0;
            const float size2 = nonUniform ? channel(e, "size2", "size2rand", flags & 0x10000, p.rnd[3], u, 0, 1.0f) * p.scale[3] : s.size;
            const float length = channel(e, "length", "lengthrand", flags & 0x20000, p.rnd[4], u, 0, 1.0f) * p.scale[4];
            s.cylinder = true;
            s.beamA = p.origin;
            s.beamB = add(p.origin, mul(p.axis[0], length));
            if (relative) { // row 0 stored in tag space (0x48d786, 0x440650); A and B through the bolt
                s.beamA = fromTagSpace(tagNow, p.local0);
                s.beamB = fromTagSpace(tagNow, add(p.local0, mul(dirToTagSpace(p.tag0, p.axis[0]), length)));
            }
            s.widthA = size2;
            s.widthB = s.size;
            s.position = s.beamA;
            int n = 8;
            if (viewValid) {
                const FVec m = sub(mul(add(s.beamA, s.beamB), 0.5f), viewOrigin);
                const float dist = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                const float lodScale = viewScale != 0 ? 1.0f / viewScale : 1.0f, lodBias = 0.0f;
                const double x = double((1.0f - (dist * lodScale + lodBias) * 0.0009765625f) * 32.0f) + 9.313225746154785e-10;
                n = int(std::nearbyint(x));
                n = n < 8 ? 8 : n > 32 ? 32 : n;
            }
            s.segments = n;
        }
        if (p.tail) {
            // 0x4830d0 length channel (graph "length", random lerp 0x20000, scale +0x1d4), 0x483160 end point: while the
            // position changed (length > 0) B = pos + normalize(prev - pos) * length, else B keeps its last value.
            const float length = channel(e, "length", "lengthrand", flags & 0x20000, p.rnd[4], u, 0, 1.0f) * p.scale[4];
            const FVec d = sub(p.prev, p.pos);
            const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            // +0x9c is world space: for relative the tag-space point goes through the bolt (0x483202); with no movement it keeps the
            // last world value (0x483233).
            if (dl > 0) {
                p.tailB = add(p.pos, mul(d, length / dl));
                if (p.bolt) p.tailB = fromTagSpace(tailTag, p.tailB);
            }
            s.beam = s.beamTail = true;
            s.beamA = pos;
            s.beamB = p.tailB;
            s.widthA = s.widthB = s.size;
            if (!viewValid) continue;
            const FVec a = sub(s.beamA, viewOrigin), b = sub(s.beamB, viewOrigin);
            const FVec c{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
            const float cl = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
            if (!(cl > 0)) continue;
            s.beamPerp = mul(c, 1.0f / cl);
        }
        // Debug: IW2_FX_SKIP=<element type or name> leaves those sprites out of the draw (the element still updates).
        static const char* const fxSkip = std::getenv("IW2_FX_SKIP");
        if (fxSkip && (e.type == fxSkip || e.name == fxSkip)) continue;
        if (!hidden) out.push_back(s);
    }
    // Tracer entities (0x4c0d98): the head P = base + dir * speed * age (TR_LINEAR, 0x4fae80); the streak runs from P
    // ahead to P + dir * min(remaining distance to the impact, cg_tracerlength); 0x4b7ca0 widens each end by distance.
    std::erase_if(tracers_, [&](const Tracer& t) { return nowMs >= t.endMs; });
    for (const Tracer& t : tracers_) {
        if (nowMs < t.startMs) continue;
        const float s = kTracerSpeed * float(nowMs - t.startMs) * 0.001f;
        const float remaining = t.length - s;
        const float l = remaining <= kTracerLength ? remaining : kTracerLength;
        FxSprite b;
        b.beam = true;
        b.shader = "gfx/misc/tracer";
        for (int i = 0; i < 3; ++i) {
            b.beamA[i] = t.base[i] + t.dir[i] * s;
            b.beamB[i] = b.beamA[i] + t.dir[i] * l;
        }
        b.widthA = b.widthB = kTracerWidth;
        if (viewValid && kTracerScale != 1.0f) {
            auto dist = [&](const FVec& p) {
                const float x = p[0] - viewOrigin[0], y = p[1] - viewOrigin[1], z = p[2] - viewOrigin[2];
                return std::sqrt(x * x + y * y + z * z) - (kTracerScaleMinDist != 0 ? kTracerScaleMinDist : 0.0f);
            };
            const float dA = dist(b.beamA), dB = dist(b.beamB);
            if (dA > 0) b.widthA *= tracerFactor(kTracerScaleDistRange, dA, kTracerScale);
            if (dB > 0) b.widthB *= tracerFactor(kTracerScaleDistRange, dB, kTracerScale);
        }
        const FVec d{b.beamB[0] - b.beamA[0], b.beamB[1] - b.beamA[1], b.beamB[2] - b.beamA[2]};
        const float s0 = d[0] * viewRight[0] + d[1] * viewRight[1] + d[2] * viewRight[2];
        const float s1 = d[0] * viewUp[0] + d[1] * viewUp[1] + d[2] * viewUp[2];
        FVec p{viewRight[0] * s1 - viewUp[0] * s0, viewRight[1] * s1 - viewUp[1] * s0, viewRight[2] * s1 - viewUp[2] * s0};
        const float pl = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (!(pl > 0)) continue;
        b.beamPerp = {p[0] / pl, p[1] / pl, p[2] / pl};
        b.rgba[0] = b.rgba[1] = b.rgba[2] = b.rgba[3] = 1.0f;
        out.push_back(b);
    }
    // Effects started by element updates (impactFx, emitFx) spawn after the loop (the particle array cannot grow while iterated).
    std::vector<DeferredPlay> plays;
    plays.swap(deferred_);
    if (loader)
        for (const DeferredPlay& dp : plays) {
            if (dp.bolt) {
                FxTag at;
                if (!(*dp.bolt)(at)) continue; // 0x48abb0 fails: nothing is played
                curBolt_ = dp.bolt;
                gate_ = &dp.origin;
                playAxes(loader(dp.path), at.origin, at.axes, nowMs);
                gate_ = nullptr;
                curBolt_.reset();
            } else
                playAxes(loader(dp.path), dp.origin, dp.axes, nowMs);
        }
    return out;
}

} // namespace iw2::game
