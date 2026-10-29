// Shadow-cookie caster budget controller of the gfx renderer (dll 1.3 0x10028390; candidate scores 0x10028150, see shadowCasterScore in
// fog.h). The 24 lowest-score casters (sorted ascending by score, comparator 0x100275c0) are walked in order; casters with score <= T
// are rendered by 0x10027b20 with a fade factor, and T (state 0x106751d0, reset to 10.0 with last-time -1 by 0x10028570) adapts every
// frame so that about sc_wantCount casters pass. See docs/spec/render-hints.md.
#pragma once
#include <algorithm>
#include <cstdint>

namespace iw2::game {

struct ShadowCookieParams {
    int wantCount = 12;      // sc_wantCount (0x101ce720): int 0..24, default 12
    int wantCountMargin = 1; // sc_wantCountMargin (0x101ce734): int 0..24, default 1
    float fadeRange = 0.25f; // sc_fadeRange (0x101ce774): float 0..1, default 0.25
    float inRate = 2.0f;     // sc_shadowInRate (0x101ce620): float 0..20, default 2 (T falls at this rate while too many casters pass)
    float outRate = 5.0f;    // sc_shadowOutRate (0x101ce7d8): float 0..20, default 5 (T rises at this rate while too few pass)
};

struct ShadowCookieCaster {
    int entity = -1; // -1 ends the list
    float score = 0.0f;
};

// Fade factor of a caster with score s <= T (0x100283ed..0x10028440): 1 up to (1 - fadeRange) * T (and for score exactly 0), then the
// linear ramp (T - s) / (T * fadeRange) down to 0 at T.
inline float shadowCookieFade(float s, float T, const ShadowCookieParams& p) {
    const float limit = (1.0f - p.fadeRange) * T;
    if (s <= limit || s == 0.0f) return 1.0f;
    return (T - s) * (1.0f / (T * p.fadeRange));
}

struct ShadowCookieState {
    float threshold = 10.0f; // 0x106751d0, reset value 0x41200000
    int64_t lastMs = -1;     // 0x106751d4 (-1 = none): the frame time of the previous call
};

struct ShadowCookieResult {
    int count = 0; // casters rendered
    int entity[24] = {};
    float fade[24] = {};
};

// One call of 0x10028390 over the sorted candidates (at most 24 entries, entity -1 terminates). nowMs = frame time (0x10656b04).
inline ShadowCookieResult stepShadowCookies(ShadowCookieState& st, const ShadowCookieCaster* sorted, int n, int64_t nowMs,
                                            const ShadowCookieParams& p) {
    ShadowCookieResult r;
    int flag = 0;
    int i = 0;
    for (; i < n && i < 24; ++i) {
        if (sorted[i].entity == -1) break; // list ended: the budget is left alone
        if (sorted[i].score > st.threshold) {
            if (i > p.wantCount + p.wantCountMargin) flag = -1;
            else if (i < p.wantCount - p.wantCountMargin) flag = 1;
            break;
        }
        r.entity[r.count] = sorted[i].entity;
        r.fade[r.count] = shadowCookieFade(sorted[i].score, st.threshold, p);
        ++r.count;
    }
    if (i == 24) flag = -1; // all 24 slots used
    float dt = 0.0f;
    if (st.lastMs != -1) dt = std::max(float(nowMs - st.lastMs) * 0.001f, 0.0f);
    st.lastMs = nowMs;
    if (flag > 0) st.threshold = dt * p.outRate + st.threshold;
    else if (flag < 0) st.threshold = st.threshold - dt * p.inRate;
    if (st.threshold < 1.0e-4f) st.threshold = 1.0e-4f; // 0x38d1b717
    return r;
}

} // namespace iw2::game
