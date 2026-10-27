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

namespace iw2::game {
namespace {

FVec add(const FVec& a, const FVec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
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

// The game's linear congruential generator (seed = seed·0x343fd + 0x269ec3, 15 bits from >> 17).
float FxSystem::frand() {
    seed_ = seed_ * 0x343fd + 0x269ec3;
    return float((seed_ >> 17) & 0x7fff) / 32767.0f;
}

float FxSystem::range(const std::vector<float>& v, float fallback) {
    if (v.empty()) return fallback;
    const float lo = v[0], hi = v.size() > 1 ? v[1] : v[0];
    return lo + (hi - lo) * frand();
}

void FxSystem::play(const std::shared_ptr<const assets::EfxEffect>& fx, const FVec& origin, const FVec& forward, int64_t nowMs) {
    if (!fx) return;
    keep_.push_back(fx);
    // Effect axes: forward, a right vector perpendicular to it, and up (inferred construction).
    FVec f = forward;
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    f = fl > 0 ? mul(f, 1.0f / fl) : FVec{0, 0, 1};
    FVec r = std::fabs(f[2]) < 0.99f ? FVec{f[1], -f[0], 0} : FVec{1, 0, 0};
    const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    r = mul(r, 1.0f / rl);
    const FVec up{r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    for (const auto& e : fx->elements) {
        const uint32_t flags = e.flagBits();
        if (e.type == "Decal") {
            spawnDecals(e, flags, origin, f, r, up, nowMs);
            continue;
        }
        if (e.type == "FxRunner") {
            // 0x48dac0: one `playfx` name at random, played at the element's position with the parent's axes (0x4853f0).
            // Count = trunc(random(count) + 0.5), default 1 (loader 0x487160); with the evenDistribution spawn flag the
            // delay is i · |max − min| / count, otherwise random in the range (0x4853f0 loop 0x485566..0x485686); a delay
            // below 1 ms spawns at once. TODO(spec): the spawn shapes (radius / height, 0x48c290), cullrange, and the
            // exact normalisation of the effect name.
            const auto names = e.list("playfx");
            if (names.empty()) continue;
            const int n = std::max(1, int(range(e.numbers("count"), 1) + 0.5f));
            const auto delay = e.numbers("delay");
            const auto sf = e.list("spawnFlags");
            const bool even = std::find(sf.begin(), sf.end(), "evenDistribution") != sf.end();
            for (int i = 0; i < n; ++i) {
                std::string name = names[std::min(names.size() - 1, size_t(frand() * float(names.size())))];
                while (!name.empty() && name[0] == '/') name.erase(0, 1);
                if (name.size() < 4 || name.compare(name.size() - 4, 4, ".efx") != 0) name += ".efx";
                float d = 0;
                if (!delay.empty()) {
                    const float lo = delay[0], hi = delay.size() > 1 ? delay[1] : delay[0];
                    d = even ? float(i) * std::fabs(hi - lo) / float(n) : range(delay, 0);
                }
                pending_.push_back({name, origin, f, nowMs + int64_t(d)});
            }
            continue;
        }
        if (e.type != "Particle" && e.type != "OrientedParticle") continue; // TODO(spec): other element types
        const int count = std::max(1, int(std::lround(range(e.numbers("count"), 1))));
        const auto org = e.numbers("origin");
        for (int i = 0; i < count; ++i) {
            FxParticle p;
            p.element = &e;
            p.startMs = nowMs + int64_t(range(e.numbers("delay"), 0));
            p.endMs = p.startMs + std::max<int64_t>(1, int64_t(range(e.numbers("life"), 1000)));
            FVec off{0, 0, 0};
            if (org.size() >= 6)
                for (int k = 0; k < 3; ++k) off[size_t(k)] = org[size_t(k)] + (org[size_t(k) + 3] - org[size_t(k)]) * frand();
            p.axis[0] = f;
            p.axis[1] = r;
            p.axis[2] = up;
            p.origin = add(origin, add(add(mul(f, off[0]), mul(r, off[1])), mul(up, off[2])));
            const uint32_t randBits[6] = {0x2000, 0x4000, 0x8000, 0x10000, 0x20000, 0x40000};
            for (int k = 0; k < 6; ++k) p.rnd[k] = flags & randBits[k] ? frand() : 0.0f;
            for (int k = 0; k < 3; ++k) p.velRnd[k] = flags & 0x80000 ? frand() : 0.0f;
            for (int k = 0; k < 3; ++k) p.vel2Rnd[k] = flags & 0x100000 ? frand() : 0.0f;
            p.gravity = range(e.numbers("gravity"), 0);
            const char* scaled[8] = {"rgb", "alpha", "size", "size2", "length", "rotationdelta", "velocityx", "velocity2x"};
            for (int k = 0; k < 8; ++k) {
                const auto* g = graph(e, scaled[k]);
                p.scale[k] = g ? g->scaleMin + (g->scaleMax - g->scaleMin) * frand() : 1.0f;
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
            particles_.push_back(p);
        }
    }
}

// Decal spawn (0x485b60) at the graph start (time 0) of each channel: rotation random in the `rotation` range
// (element +0x220); colour (0x4857d0) = rgb · a random scale from its range, lerped towards rgbRand by a random
// with useRandomColors 0x2000; alpha (0x4858c0) the same with useRandomAlpha 0x4000, clamped to 0..1; size
// (0x485a30) with useRandomSize 0x8000; the material picked at random from `shaders` ((seed >> 17)·count >> 15).
// The direction is the effect's forward axis (0x485bd9 passes the effect to 0x4ad430). TODO(spec): the spawn
// position (0x48c290; the particles' origin box is used), count / delay / spawnRange handling, the frustumCull
// spawn flag (spawnFrustumCullRadius), the exact order of the random draws.
void FxSystem::spawnDecals(const assets::EfxElement& e, uint32_t flags, const FVec& origin, const FVec& f, const FVec& r,
                           const FVec& up, int64_t nowMs) {
    const int count = std::max(1, int(std::lround(range(e.numbers("count"), 1))));
    const auto org = e.numbers("origin");
    const auto shaders = e.list("shaders");
    if (std::getenv("IW2_MARK_DEBUG")) std::fprintf(stderr, "decal element %s: %zu shaders\n", e.name.c_str(), shaders.size());
    if (shaders.empty()) return;
    for (int i = 0; i < count; ++i) {
        FxDecal d;
        d.startMs = nowMs + int64_t(range(e.numbers("delay"), 0));
        FVec off{0, 0, 0};
        if (org.size() >= 6)
            for (int k = 0; k < 3; ++k) off[size_t(k)] = org[size_t(k)] + (org[size_t(k) + 3] - org[size_t(k)]) * frand();
        d.origin = add(origin, add(add(mul(f, off[0]), mul(r, off[1])), mul(up, off[2])));
        d.dir = f;
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
    if (pending_.empty() || !loader) return;
    std::vector<PendingRun> due;
    std::erase_if(pending_, [&](const PendingRun& p) {
        if (p.startMs > nowMs) return false;
        due.push_back(p);
        return true;
    });
    for (const PendingRun& p : due) play(loader(p.path), p.origin, p.forward, nowMs);
}

std::vector<FxSprite> FxSystem::sprites(int64_t nowMs) {
    runPending(nowMs);
    std::erase_if(particles_, [&](const FxParticle& p) { return nowMs >= p.endMs; });
    std::vector<FxSprite> out;
    sightBlockers_.clear();
    for (FxParticle& p : particles_) {
        if (nowMs < p.startMs) continue;
        const assets::EfxElement& e = *p.element;
        const uint32_t flags = e.flagBits();
        const float life = float(p.endMs - p.startMs);
        const float t = float(nowMs - p.startMs);
        const float u = std::min(1.0f, t / life);
        const float lifeS = life * 0.001f;
        auto integ = [&](const char* name, const char* rname, bool useRand, float rnd) {
            const auto* g = graph(e, name);
            if (!g) return 0.0f;
            float v = g->integral(u);
            if (useRand)
                if (const auto* rg = graph(e, rname)) v += (rg->integral(u) - v) * rnd;
            return v * g->scaleMin; // TODO(spec): the per-particle velocity scales (+0x1c4…)
        };
        const bool rv = flags & 0x80000, rv2 = flags & 0x100000;
        FVec v1{integ("velocityx", "velocityxrand", rv, p.velRnd[0]), integ("velocityy", "velocityyrand", rv, p.velRnd[1]),
                integ("velocityz", "velocityzrand", rv, p.velRnd[2])};
        FVec v2{integ("velocity2x", "velocity2xrand", rv2, p.vel2Rnd[0]), integ("velocity2y", "velocity2yrand", rv2, p.vel2Rnd[1]),
                integ("velocity2z", "velocity2zrand", rv2, p.vel2Rnd[2])};
        auto toWorld = [&](const FVec& v, bool absolute) {
            return absolute ? v : add(add(mul(p.axis[0], v[0]), mul(p.axis[1], v[1])), mul(p.axis[2], v[2]));
        };
        FVec pos = add(p.origin, add(mul(toWorld(v1, flags & 0x200000), lifeS), mul(toWorld(v2, flags & 0x400000), lifeS)));
        pos[2] += p.gravity * (t * 0.001f) * (t * 0.001f);
        FxSprite s;
        s.position = pos;
        for (int c = 0; c < 3; ++c) s.rgba[c] = std::clamp(channel(e, "rgb", "rgbrand", flags & 0x2000, p.rnd[0], u, c, 1.0f), 0.0f, 1.0f);
        s.rgba[3] = std::clamp(channel(e, "alpha", "alpharand", flags & 0x4000, p.rnd[1], u, 0, 1.0f), 0.0f, 1.0f);
        s.size = channel(e, "size", "sizerand", flags & 0x8000, p.rnd[2], u, 0, 1.0f) * p.scale[2];
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
        out.push_back(s);
    }
    return out;
}

} // namespace iw2::game
