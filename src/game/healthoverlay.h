// The multiplayer low-health overlay (docs/spec/mp-hud.md "Low-health overlay"): the full-screen red of the hud menu
// "Low Health" (ownerdraw CG_PLAYER_LOW_HEALTH_OVERLAY 109). Its alpha pulses while health / maxhealth is below
// hud_healthOverlay_pulseStart and fades out after the regeneration pause; a new drop of health below the threshold
// starts the pulses again. Pure state machine; the front end feeds the health fraction and the client time.
#pragma once

#include <algorithm>
#include <cstdint>

namespace iw2::game {

struct HealthOverlayParams {
    float pulseStart = 0.35f;            // hud_healthOverlay_pulseStart
    int regenPauseTime = 5000;           // hud_healthOverlay_regenPauseTime (ms)
    int phaseOneDuration = 150;          // hud_healthOverlay_phaseOne_pulseDuration
    float phaseTwoMultiplier = 0.7f;     // hud_healthOverlay_phaseTwo_toAlphaMultiplier
    int phaseTwoDuration = 320;          // hud_healthOverlay_phaseTwo_pulseDuration
    float phaseThreeMultiplier = 0.6f;   // hud_healthOverlay_phaseThree_toAlphaMultiplier
    int phaseThreeDuration = 400;        // hud_healthOverlay_phaseThree_pulseDuration
    float endAlpha = 0.0f;               // hud_healthOverlay_phaseEnd_toAlpha
    int endDuration = 700;               // hud_healthOverlay_phaseEnd_pulseDuration
};

// The health fraction the overlay reads (MP 0x4c4a00): health / maxhealth clamped to 0..1, 0 when either is 0 or the
// player state is dead (pm_type 6). At 0 nothing is drawn.
inline float healthOverlayFraction(int health, int maxHealth, int pmType) {
    if (health == 0 || maxHealth == 0 || pmType == 6) return 0.0f;
    return std::clamp(float(health) / float(maxHealth), 0.0f, 1.0f);
}

class HealthOverlay {
public:
    // A spawn of the predicted player state (0x4e0190): no pulse, the alpha at the end value, the last fraction 1.
    void reset(const HealthOverlayParams& p) {
        active_ = false;
        duration_ = 0;
        phase_ = 0;
        pulses_ = 0;
        to_ = p.endAlpha;
        last_ = 1.0f;
    }

    // The ownerdraw of one frame (0x4c5250): returns the alpha to draw the overlay with (0: not drawn).
    float frame(float fraction, int64_t nowMs, const HealthOverlayParams& p) {
        if (fraction == 0.0f) return 0.0f;
        update(fraction, nowMs, p);
        return alpha(nowMs);
    }

    // The pulse step (0x4c5000).
    void update(float fraction, int64_t nowMs, const HealthOverlayParams& p) {
        static constexpr float kPulseAlpha[4] = {1.0f, 0.8f, 0.6f, 0.3f};
        if (fraction < last_ && fraction < p.pulseStart) { // a new drop below the threshold: the pulses start again
            dropMs_ = nowMs;
            pulses_ = 0;
        }
        last_ = fraction;
        if (nowMs < startMs_ + duration_) return; // the current step is still running
        if (fraction < p.pulseStart) active_ = true;
        else if (!active_) return;
        startMs_ = nowMs;
        from_ = to_;
        if (pulses_ >= 4) { // the pulses are over: fade to the end value
            active_ = false;
            to_ = p.endAlpha;
            duration_ = p.endDuration;
            phase_ = 0;
            return;
        }
        const float base = kPulseAlpha[pulses_];
        if (phase_ == 0) {
            to_ = std::clamp(base, 0.0f, 1.0f);
            duration_ = p.phaseOneDuration;
            phase_ = 1;
        } else if (phase_ == 1) {
            to_ = std::clamp(base * p.phaseTwoMultiplier, 0.0f, 1.0f);
            duration_ = p.phaseTwoDuration;
            phase_ = 2;
        } else {
            to_ = std::clamp(base * p.phaseThreeMultiplier, 0.0f, 1.0f);
            duration_ = p.phaseThreeDuration;
            phase_ = 0;
            // One pulse level lasts the regeneration pause spread over its pulses: the next level once the pause minus
            // three pulses' worth has passed since the drop.
            if (dropMs_ + p.regenPauseTime - 3 * (p.phaseOneDuration + p.phaseTwoDuration + p.phaseThreeDuration) <= nowMs)
                ++pulses_;
        }
    }

    // The alpha now (0x4c4fb0): linear from the step's start value to its target over its duration.
    float alpha(int64_t nowMs) const {
        const int64_t t = std::max<int64_t>(0, nowMs - startMs_);
        if (duration_ > 0 && t < duration_) return (to_ - from_) * (float(t) / float(duration_)) + from_;
        return to_;
    }

    int pulses() const { return pulses_; }
    bool active() const { return active_; }

private:
    bool active_ = false;
    int phase_ = 0, pulses_ = 0, duration_ = 0;
    int64_t startMs_ = 0, dropMs_ = 0;
    float from_ = 0.0f, to_ = 0.0f, last_ = 1.0f;
};

} // namespace iw2::game
