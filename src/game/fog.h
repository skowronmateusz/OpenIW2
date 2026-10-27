// World fog (script setcullfog -> server command "fog" -> client 0x4a5770 -> gfx dll R_SetFog 0x1000bb90 / R_SetFogTransition 0x1000bc20,
// per-frame blend 0x100244d0, constants 0x10034db0 + lib/fogcalc.hlsl).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace iw2::game {

struct FogParams {
    bool active = false;
    float nearDist = 0, farDist = 0;
    uint8_t color[3] = {0, 0, 0}; // 0..255, rounded from the command's 0..1 floats (0x4a5870: * 255, fistp)
};

// Shader constants (0x10034e5f..): scale = -1 / (far - near), shift = far / (far - near); linear factor = dist * scale + shift,
// clamped to 0..1 by the hardware fog register; out = lerp(fogColor, color, factor), dist = radial eye distance.
struct FogConsts {
    float scale = 0, shift = 1;
    float color[3] = {0, 0, 0};
    bool active = false;
};

class FogState {
public:
    // "fog <near> <far> <1> <r> <g> <b> <ms>" handled at the client time nowMs (0x4a5770: R_SetFog + transition from nowMs for ms).
    void command(float nearD, float farD, float r, float g, float b, int ms, int64_t nowMs) {
        from_ = current(nowMs);
        to_.active = true;
        to_.nearDist = nearD;
        to_.farDist = farD;
        const float v[3] = {r, g, b};
        for (int i = 0; i < 3; ++i) to_.color[i] = uint8_t(std::clamp(std::nearbyint(v[i] * 255.0f), 0.0f, 255.0f));
        start_ = nowMs;
        end_ = nowMs + ms;
        // 0x100244fa: when the current state is not an active fog (type differs) the target is taken immediately.
        if (!from_.active) end_ = start_;
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
        for (int i = 0; i < 3; ++i)
            p.color[i] = uint8_t(std::nearbyint(float(int(to_.color[i]) - int(from_.color[i])) * t + float(from_.color[i])));
        return p;
    }
    FogConsts consts(int64_t nowMs) const {
        const FogParams p = current(nowMs);
        FogConsts c;
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
