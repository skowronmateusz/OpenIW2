#include "assets/efx.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace iw2::assets {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Bytes outside ASCII separate words too (fx/muzzleflashes/panzerschreck_flash.efx has a stray 0xF6 after a
// closing brace; how the original tokenizer treats it is inferred).
bool isSep(char c) { return std::isspace(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80; }

std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && isSep(line[i])) ++i;
        size_t j = i;
        while (j < line.size() && !isSep(line[j])) ++j;
        if (j > i) out.push_back(line.substr(i, j - i));
        i = j;
    }
    return out;
}

// {name, element-flag bit, spawn-flag bit} rows; one table serves both keys.
struct FlagName { const char* name; uint32_t flag, spawn; };
constexpr FlagName kFlagNames[] = {
    {"depthHack", 0x1, 0}, {"setShaderTime", 0x4, 0}, {"useModel", 0x10, 0}, {"useBBox", 0x40, 0},
    {"usePhysics", 0x20, 0}, {"impactKills", 0x400, 0}, {"impactFx", 0x800, 0}, {"deathFx", 0x200, 0},
    {"useAlpha", 0x80, 0}, {"useRandomColors", 0x2000, 0}, {"useRandomAlpha", 0x4000, 0},
    {"useRandomSize", 0x8000, 0}, {"useRandomSize2", 0x10000, 0}, {"useRandomLength", 0x20000, 0},
    {"useRandomRotationDelta", 0x40000, 0}, {"useRandomVelocity", 0x80000, 0}, {"useRandomVelocity2", 0x100000, 0},
    {"absoluteVel", 0x200000, 0}, {"absoluteVel2", 0x400000, 0}, {"affectedByWind", 0x800000, 0},
    {"emitFx", 0x100, 0}, {"relative", 0x2, 0}, {"blocksSight", 0x1000, 0}, {"disableFarPlaneCulling", 0x2000000, 0},
    {"org2fromTrace", 0, 0x8}, {"traceImpactFx", 0, 0x10}, {"org2isOffset", 0, 0x20}, {"cheapOrgCalc", 0, 0x40},
    {"cheapOrg2Calc", 0, 0x80}, {"orgOnSphere", 0, 0x1}, {"orgOnCylinder", 0, 0x4}, {"axisFromSphere", 0, 0x2},
    {"randrotaroundfwd", 0, 0x100}, {"evenDistribution", 0, 0x200}, {"frustumCull", 0, 0x400},
    {"absoluteAccel", 0x400000, 0},
};

uint32_t bitsOf(const std::vector<std::string>& names, bool spawn) {
    uint32_t bits = 0;
    for (const auto& n : names)
        for (const auto& f : kFlagNames)
            if (lower(n) == lower(f.name)) bits |= spawn ? f.spawn : f.flag;
    return bits;
}

// Legacy channel syntax (`start`, `end`, `parm`, `flags` inside a channel group; channel parser 0x489450 with the
// helpers 0x488c30 / 0x488ca0 (3-component start / end: 3 numbers = fixed, 6 = min xyz then max xyz, 0x4879b0, unsorted),
// 0x488d10 / 0x488d90 (one component, "%f %f" sorted min <= max), 0x488e10 (parm) and 0x488e70 (flags, names at 0x57d1d8:
// linear 0x1, random 0x2, nonlinear 0x4, wave 0x8, clamp 0xc)). Every record starts as start = end = parm = 1, flags 0
// (0x4878c0). After the whole element is parsed, 0x487910 turns every channel whose record was touched into a graph
// (0x47eb10), replacing a `curve` graph of the same channel.
struct LegacyChannel {
    float start[3][2] = {{1, 1}, {1, 1}, {1, 1}};
    float end[3][2] = {{1, 1}, {1, 1}, {1, 1}};
    float parm[2] = {1, 1};
    uint32_t flags = 0;
    bool set = false;
};

float mid(const float r[2]) { return (r[1] - r[0]) * 0.5f + r[0]; } // (max - min) * 0.5 + min (0x591e10 = 0.5)

// 0x47eb10: graph and scale range of one legacy channel. dims = 3 for rgb (index 0), else 1; alpha (index 2) uses M = 1,
// other channels M = max(1, max(mid(start), mid(end)) * 4/3) (0x59212c, component 0). Scale range = M -/+ half the start
// spread (component 0). Values are divided by M. p = 0.01 * mid(parm) / M (0x591ea0), w = mid(parm) / M * meanLife *
// 0.0031415929 (0x592158), meanLife = mid(life) (0x487910, element +0x58 / +0x5c).
// - linear only (flags & 1, no 0xc bits, 0x47e780): two rows {0, start}, {1, end}.
// - nonlinear only ((flags & 0xd) == 4, 0x47e7f0): three rows {0, start}, {p, start}, {1, end}.
// - otherwise (0x47e870): 16 rows at t = 0, 1/15, ... (0x5920a4, t clamped to 1). f = 1, or 1 - t with linear; nonlinear:
//   g = t > p ? 1 - (t - p) / (1 - p) : 1; clamp: g = t < p ? (p - t) / p : 0; with linear f = (f + g) / 2 else f = g; wave:
//   f *= cos(t * w) (0x566090). Value = (1 - f) * end + f * start, set to 0 when negative and to 1 when f > 1; random (0x2)
//   multiplies it by a draw of the shared effect generator (0x59b2a8, fxGeneratorSeed).
EfxGraph legacyGraph(const LegacyChannel& r, int dims, bool alpha, float meanLife) {
    float M = 1.0f;
    if (!alpha) {
        const float s = mid(r.start[0]), e = mid(r.end[0]);
        M = (s - e < 0.0f ? e : s) * 1.3333334f; // 0x47eb44: the larger of the two
        if (!(M >= 1.0f)) M = 1.0f;
    }
    EfxGraph g;
    g.dims = dims;
    const float half = (r.start[0][1] - r.start[0][0]) * 0.5f;
    g.scaleMin = M - half;
    g.scaleMax = M + half;
    const float inv = 1.0f / M;
    float s[3], e[3];
    for (int d = 0; d < dims; ++d) s[d] = mid(r.start[d]) * inv, e[d] = mid(r.end[d]) * inv;
    const float parmN = mid(r.parm) * inv;
    const float p = parmN * 0.01f, w = parmN * meanLife * 0.0031415929f;
    auto row = [&](float t, const float* v) {
        std::vector<float> x{t};
        for (int d = 0; d < dims; ++d) x.push_back(v[d]);
        g.points.push_back(std::move(x));
    };
    const uint32_t f = r.flags;
    if ((f & 1) && !(f & 0xc)) {
        row(0, s), row(1, e);
    } else if (!(f & 1) && (f & 0xc) == 4) {
        row(0, s), row(p, s), row(1, e);
    } else {
        uint32_t& seed = fxGeneratorSeed(); // the shared generator; its state at load depends on all earlier draws
        float t = 0;
        for (int i = 0; i < 16; ++i) {
            if (!(t <= 1.0f)) t = 1.0f;
            float k = (f & 1) ? 1.0f - t : 1.0f;
            const uint32_t mode = f & 0xc;
            if (mode == 4 || mode == 0xc) {
                float gg = mode == 4 ? (t > p ? 1.0f - (t - p) / (1.0f - p) : 1.0f) : (t < p ? (p - t) / p : 0.0f);
                k = (f & 1) ? (gg + k) * 0.5f : gg;
            } else if (mode == 8) {
                k *= float(std::cos(double(t * w)));
            }
            float v[3];
            for (int d = 0; d < dims; ++d) {
                float x = (1.0f - k) * e[d] + k * s[d];
                if (x < 0.0f) x = 0.0f;
                else if (k > 1.0f) x = 1.0f;
                if (f & 2) {
                    seed = seed * 0x343fd + 0x269ec3;
                    x = x * float((seed >> 17) & 0x7fff) / 32768.0f;
                }
                v[d] = x;
            }
            row(t, v);
            t += 0.06666667f;
        }
    }
    return g;
}

} // namespace

uint32_t& fxGeneratorSeed() {
    static uint32_t seed = 0x89abcdef;
    return seed;
}

uint32_t EfxElement::flagBits() const {
    if (flagBitsCache_ < 0) flagBitsCache_ = bitsOf(list("flags"), false);
    return uint32_t(flagBitsCache_);
}
uint32_t EfxElement::spawnFlagBits() const {
    if (spawnFlagBitsCache_ < 0) spawnFlagBitsCache_ = bitsOf(list("spawnflags"), true);
    return uint32_t(spawnFlagBitsCache_);
}

float EfxGraph::sample(float t, int c) const {
    if (points.empty()) return 0;
    const size_t k = size_t(c) + 1;
    if (t <= points.front()[0]) return points.front()[k];
    for (size_t i = 1; i < points.size(); ++i)
        if (t <= points[i][0]) {
            const float t0 = points[i - 1][0], t1 = points[i][0];
            const float f = t1 > t0 ? (t - t0) / (t1 - t0) : 0.0f;
            return points[i - 1][k] + (points[i][k] - points[i - 1][k]) * f;
        }
    return points.back()[k];
}

float EfxGraph::integral(float t, int c) const {
    if (points.size() < 2) return 0;
    const size_t k = size_t(c) + 1;
    float area = 0;
    for (size_t i = 1; i < points.size(); ++i) {
        const float t0 = points[i - 1][0], t1 = points[i][0];
        if (t <= t0) break;
        const float v0 = points[i - 1][k];
        if (t < t1) {
            const float v = v0 + (points[i][k] - v0) * (t - t0) / (t1 - t0);
            area += (t - t0) * (v0 + v) * 0.5f;
            break;
        }
        area += (t1 - t0) * (v0 + points[i][k]) * 0.5f;
    }
    return area;
}

std::vector<std::string> EfxElement::list(const std::string& key) const {
    auto it = keys.find(lower(key));
    return it == keys.end() ? std::vector<std::string>{} : it->second;
}

std::vector<float> EfxElement::numbers(const std::string& key) const {
    std::vector<float> out;
    for (const auto& s : list(key)) out.push_back(float(std::atof(s.c_str())));
    return out;
}

EfxEffect parseEfx(std::string_view text) {
    std::vector<std::vector<std::string>> lines;
    {
        std::string cur;
        for (char ch : text) {
            if (ch == '\n') { lines.push_back(words(cur)); cur.clear(); }
            else if (ch != '\r') cur += ch;
        }
        lines.push_back(words(cur));
    }
    EfxEffect fx;
    size_t i = 0;
    auto next = [&]() -> std::vector<std::string>& {
        while (i < lines.size() && lines[i].empty()) ++i;
        if (i >= lines.size()) throw std::runtime_error("efx: unexpected end");
        return lines[i++];
    };
    auto expect = [&](const char* tok) {
        auto& l = next();
        if (l.size() != 1 || l[0] != tok) throw std::runtime_error(std::string("efx: expected ") + tok + " at line " + std::to_string(i) + ", got " + (l.empty() ? std::string() : l[0]));
    };
    while (true) {
        while (i < lines.size() && lines[i].empty()) ++i;
        if (i >= lines.size()) break;
        auto& head = lines[i++];
        if (head.size() != 1) throw std::runtime_error("efx: expected an element type");
        EfxElement el;
        el.type = head[0];
        std::vector<std::pair<std::string, LegacyChannel>> legacy;
        expect("{");
        while (true) {
            auto& l = next();
            if (l.size() == 1 && l[0] == "}") break;
            const std::string key = lower(l[0]);
            if (l.size() == 1 && i < lines.size()) {
                size_t j = i;
                while (j < lines.size() && lines[j].empty()) ++j;
                if (j < lines.size() && lines[j].size() == 1 && lines[j][0] == "[") {
                    // A name list: key [ name name … ] (shaders, models, effect references).
                    i = j + 1;
                    std::vector<std::string> names;
                    while (true) {
                        auto& row = next();
                        if (row.size() == 1 && row[0] == "]") break;
                        names.insert(names.end(), row.begin(), row.end());
                    }
                    el.keys[key] = std::move(names);
                    continue;
                }
            }
            if (l.size() == 1) {
                // A channel group: { curve [ rows ] scale a [b] … }.
                EfxGraph g;
                g.dims = key == "rgb" || key == "rgbrand" ? 3 : 1;
                LegacyChannel leg;
                const bool rgb3 = key == "rgb"; // channel index 0 (0x489450: indices 0 and 1 read three components)
                auto pairOf = [&](const std::vector<std::string>& gl, float out[2], bool sorted) {
                    if (gl.size() < 2) return false;
                    out[0] = float(std::atof(gl[1].c_str()));
                    out[1] = gl.size() > 2 ? float(std::atof(gl[2].c_str())) : out[0];
                    if (sorted && out[1] < out[0]) std::swap(out[0], out[1]);
                    return true;
                };
                auto triple = [&](const std::vector<std::string>& gl, float out[3][2]) {
                    if (gl.size() == 4 || gl.size() >= 7) { // 0x4879b0: 3 numbers (fixed) or 6 (min xyz, max xyz)
                        const bool six = gl.size() >= 7;
                        for (int d = 0; d < 3; ++d) {
                            out[d][0] = float(std::atof(gl[size_t(1 + d)].c_str()));
                            out[d][1] = six ? float(std::atof(gl[size_t(4 + d)].c_str())) : out[d][0];
                        }
                        return true;
                    }
                    return false;
                };
                expect("{");
                while (true) {
                    auto& gl = next();
                    if (gl.size() == 1 && gl[0] == "}") break;
                    const std::string gk = lower(gl[0]);
                    if (gk == "curve") {
                        expect("[");
                        while (true) {
                            auto& row = next();
                            if (row.size() == 1 && row[0] == "]") break;
                            std::vector<float> p;
                            for (size_t c = 0; c < row.size() && c < size_t(g.dims) + 1; ++c) p.push_back(float(std::atof(row[c].c_str())));
                            while (p.size() < size_t(g.dims) + 1) p.push_back(0);
                            g.points.push_back(p);
                        }
                    } else if (gk == "start" || gk == "end") {
                        auto& dst = gk == "start" ? leg.start : leg.end;
                        if (rgb3 || key == "rgbrand") { if (triple(gl, dst)) leg.set = true; }
                        else if (pairOf(gl, dst[0], true)) leg.set = true;
                    } else if (gk == "parm" || gk == "parms") {
                        if (pairOf(gl, leg.parm, false)) leg.set = true; // 0x488e10: one number = both, not sorted
                    } else if (gk == "flags" || gk == "flag") {
                        for (size_t w = 1; w < gl.size() && w <= 4; ++w) { // "%s %s %s %s" (0x57d1e0)
                            const std::string fl = lower(gl[w]);
                            if (fl == "linear") leg.flags |= 1;
                            else if (fl == "nonlinear") leg.flags |= 4;
                            else if (fl == "wave") leg.flags |= 8;
                            else if (fl == "random") leg.flags |= 2;
                            else if (fl == "clamp") leg.flags |= 0xc;
                        }
                        leg.set = true;
                    } else if (gk == "scale" && gl.size() > 1) {
                        g.scaleMin = float(std::atof(gl[1].c_str()));
                        g.scaleMax = gl.size() > 2 ? float(std::atof(gl[2].c_str())) : g.scaleMin;
                    }
                }
                if (!g.points.empty()) {
                    // Missing end points at t = 0 and t = 1 are added (FUN_0047ed80); both copy the values of the
                    // file's first row (the copy for t = 1 reads from the start of the input, as in the original).
                    const auto first = g.points.front();
                    if (g.points.back()[0] != 1) { auto p = first; p[0] = 1; g.points.push_back(p); }
                    if (first[0] != 0) { auto p = first; p[0] = 0; g.points.insert(g.points.begin(), p); }
                }
                if (leg.set) legacy.emplace_back(key, leg);
                el.graphs[key] = std::move(g);
                continue;
            }
            if (key == "name") el.name = l[1];
            el.keys[key] = std::vector<std::string>(l.begin() + 1, l.end());
        }
        // 0x487910: legacy records become graphs once the element is complete (mean life from `life`, element +0x58/+0x5c).
        // rgbRand (index 1) gets one component there, which the rgb updater (0x47f4a0, fixed 16-byte rows) reads out of bounds:
        // undefined in the original, no shipped file has it; left as parsed here.
        if (!legacy.empty()) {
            const auto life = el.numbers("life");
            float lr[2] = {life.empty() ? 0.0f : life[0], life.size() > 1 ? life[1] : (life.empty() ? 0.0f : life[0])};
            const float meanLife = mid(lr);
            for (auto& [name, rec] : legacy) {
                if (name == "rgbrand") continue;
                el.graphs[name] = legacyGraph(rec, name == "rgb" ? 3 : 1, name == "alpha", meanLife);
            }
        }
        fx.elements.push_back(std::move(el));
    }
    return fx;
}

} // namespace iw2::assets
