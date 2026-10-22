#include "assets/efx.h"

#include <algorithm>
#include <cctype>
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

} // namespace

uint32_t EfxElement::flagBits() const { return bitsOf(list("flags"), false); }
uint32_t EfxElement::spawnFlagBits() const { return bitsOf(list("spawnflags"), true); }

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
                el.graphs[key] = std::move(g);
                continue;
            }
            if (key == "name") el.name = l[1];
            el.keys[key] = std::vector<std::string>(l.begin() + 1, l.end());
        }
        fx.elements.push_back(std::move(el));
    }
    return fx;
}

} // namespace iw2::assets
