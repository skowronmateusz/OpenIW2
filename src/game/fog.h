// World fog (script setcullfog -> server command "fog" -> client 0x4a5770 -> gfx dll R_SetFog 0x1000bb90 / R_SetFogTransition 0x1000bc20,
// per-frame blend 0x100244d0, constants 0x10034db0 + lib/fogcalc.hlsl).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace iw2::game {

// Argument validation of the shared fog script command 0x4f53f0 (setCullFog 0x4f5580 calls it with the script's near / far / r / g / b / time,
// setExpFog 0x4f5630 with near 0, far 1). Returns the script error text (strings 0x585c8c, 0x585c58, 0x585c18, 0x585bec; "%s" is the
// command name) or an empty string. The checks run in this order; plain float compares against 0.0 / 1.0 (0x591e00, 0x591e20).
inline std::string fogArgError(const char* name, float nearD, float farD, float r, float g, float b, float timeSec) {
    const std::string n = name;
    if (nearD < 0.0f) return n + ": near distance must be >= 0";
    if (farD <= nearD) return n + ": near distance must be less than far distance";
    for (float c : {r, g, b})
        if (c < 0.0f || c > 1.0f) return n + ": red/green/blue color components must be in the range [0, 1]";
    if (timeSec < 0.0f) return n + ": transition time must be >= 0 seconds";
    return {};
}
// setExpFog's own density check (0x4f5697..0x4f56ca, string 0x585b8c): the density must be > 0 and < 1.
inline bool fogDensityValid(float density) { return density > 0.0f && density < 1.0f; }
// setshadowhint (0x4ef700): the entity-state bits 0x2000..0xe000 per argument string (script strings 0x1c7f0a6 "normal" -> 0,
// 0x1c7f132 "never" -> 0x2000, 0x1c7f134 "low_priority" -> 0x4000, 0x1c7f136 "high_priority" -> 0x6000, 0x1c7f130 "always" -> 0x8000,
// 0x1c7f138 "receiver" -> 0xa000). Any other string is a script error (0x587058).
inline bool shadowHintBits(const std::string& s, uint32_t& bits) {
    static const struct { const char* n; uint32_t b; } k[] = {{"normal", 0},       {"never", 0x2000},   {"low_priority", 0x4000},
                                                               {"high_priority", 0x6000}, {"always", 0x8000}, {"receiver", 0xa000}};
    for (const auto& e : k)
        if (s == e.n) return bits = e.b, true;
    return false;
}

// Far-plane distance of the renderer (gfx dll 0x1000aaa0, exe export slot 0x617af0; set from configstring 8 by 0x1000ab00): the r_zfar
// dvar when nonzero, else the linear fog far distance while a linear fog is active, else 0; then the larger of that and the setculldist
// value. 0 = no far plane. TODO(spec): the exponential-fog case (only linear fog is modelled as contributing).
// The plane culls by depth along the view direction (see FxSystem::beyondFar), it is not the projection far clip.
inline float farPlaneDistance(bool linearFogActive, float fogFar, float rZfar, float cullDist) {
    float d = rZfar != 0.0f ? rZfar : (linearFogActive ? fogFar : 0.0f);
    return std::max(d, cullDist);
}
// Shadow-caster score of the gfx dll's selection (0x10028150): distance / largest bounds extent, scaled by the entity hint (always 0,
// high_priority x0.25, low_priority x4); never and the receiver-less skip rules live in the caller. Lower scores win (top 24).
inline float shadowCasterScore(uint32_t hintBits, float dist, float maxExtent) {
    if (hintBits == 0x8000) return 0.0f;
    const float s = dist / maxExtent;
    return hintBits == 0x6000 ? s * 0.25f : hintBits == 0x4000 ? s * 4.0f : s;
}

struct FogParams {
    bool active = false;
    bool exponential = false; // R_SetFog type != 1 (client 0x4a5770: third value < 1 is the density)
    float density = 0;
    float nearDist = 0, farDist = 0;
    uint8_t color[3] = {0, 0, 0}; // 0..255, rounded from the command's 0..1 floats (0x4a5870: * 255, fistp)
};

// Shader constants (0x10034e5f..): scale = -1 / (far - near), shift = far / (far - near); linear factor = dist * scale + shift,
// clamped to 0..1 by the hardware fog register; out = lerp(fogColor, color, factor), dist = radial eye distance.
struct FogConsts {
    float scale = 0, shift = 1;
    float exponent = 0; // fogConsts[FOG_EXPONENT] = -density (0x10034db0: _DAT_1070c1c8 = -[+0x16bfc4]); nonzero selects Fog_ExponentialFactor
    bool exponential = false;
    float color[3] = {0, 0, 0};
    bool active = false;
};

class FogState {
public:
    // "fog <near> <far> <1> <r> <g> <b> <ms>" handled at the client time nowMs (0x4a5770: R_SetFog + transition from nowMs for ms).
    void command(float nearD, float farD, float r, float g, float b, int ms, int64_t nowMs) {
        from_ = current(nowMs);
        to_.active = true;
        to_.exponential = false;
        to_.nearDist = nearD;
        to_.farDist = farD;
        const float v[3] = {r, g, b};
        for (int i = 0; i < 3; ++i) to_.color[i] = uint8_t(std::clamp(std::nearbyint(v[i] * 255.0f), 0.0f, 255.0f));
        start_ = nowMs;
        end_ = nowMs + ms;
        // 0x100244fa: when the current state is not an active fog (type differs) the target is taken immediately.
        if (!from_.active) end_ = start_;
    }
    // Exponential fog: "fog <near> <far> <density> ..." with density < 1 (setexpfog sends "fog 0 1 <density> r g b ms"). When the current fog is of
    // another type the target is taken at once (0x100244fa). TODO(spec): density interpolation between two exponential fogs (assumed linear).
    void commandExp(float density, float r, float g, float b, int ms, int64_t nowMs) {
        command(0, 1, r, g, b, ms, nowMs);
        to_.exponential = true;
        to_.density = density;
        if (!from_.exponential) end_ = start_;
    }
    void clear() { *this = FogState{}; }

    // Blend of 0x100244d0: t = (now - start) / (end - start) clamped, near / far lerped as floats, colour bytes lerped.
    FogParams current(int64_t nowMs) const {
        if (!to_.active) return {};
        if (nowMs >= end_) return to_;
        const float t = std::clamp(float(nowMs - start_) / float(std::max<int64_t>(end_ - start_, 1)), 0.0f, 1.0f);
        FogParams p;
        p.active = true;
        p.nearDist = (to_.nearDist - from_.nearDist) * t + from_.nearDist;
        p.farDist = (to_.farDist - from_.farDist) * t + from_.farDist;
        p.exponential = to_.exponential;
        p.density = (to_.density - from_.density) * t + from_.density;
        for (int i = 0; i < 3; ++i)
            p.color[i] = uint8_t(std::nearbyint(float(int(to_.color[i]) - int(from_.color[i])) * t + float(from_.color[i])));
        return p;
    }
    FogConsts consts(int64_t nowMs) const {
        const FogParams p = current(nowMs);
        FogConsts c;
        if (p.active && p.exponential) {
            c.active = c.exponential = true;
            c.exponent = -p.density;
            for (int i = 0; i < 3; ++i) c.color[i] = float(p.color[i]) / 255.0f;
            return c;
        }
        if (!p.active || p.farDist <= p.nearDist) return c;
        const float s = 1.0f / (p.farDist - p.nearDist);
        c.active = true;
        c.scale = -s;
        c.shift = s * p.farDist;
        for (int i = 0; i < 3; ++i) c.color[i] = float(p.color[i]) / 255.0f;
        return c;
    }

private:
    FogParams from_, to_;
    int64_t start_ = 0, end_ = 0;
};

} // namespace iw2::game
