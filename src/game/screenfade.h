// The "scr_fade <alpha> <start> <duration>" client command (0x4a6f3c -> 0x4bec70) and its per-frame update (0x4bda70): a black
// full-screen quad whose alpha moves to the target by dt / duration per frame (dt: real milliseconds since the previous update,
// steps of 500 ms or more are skipped); once start + duration has passed (game time) the target is set at once.
#pragma once

#include <cstdint>

namespace iw2::game {

class ScreenFade {
public:
    // alpha is the command's first argument (the command stores it times 1000 and the setter scales by 0.001 again).
    void command(float alpha, int startMs, int durationMs, int64_t nowMs) {
        target_ = alpha;
        start_ = startMs;
        duration_ = durationMs;
        if (int64_t(startMs) + durationMs <= nowMs) current_ = target_; // 0x4bec8c: jg skips when start + duration > now
    }
    // gameMs: cg.time (0xf708dc); realMs: the real-time clock.
    void update(int64_t gameMs, int64_t realMs) {
        if (int64_t(start_) + duration_ < gameMs) {
            current_ = target_;
        } else if (current_ != target_) {
            const int64_t dt = realMs - lastReal_;
            lastReal_ = realMs;
            if (dt < 500 && dt > 0 && duration_ > 0) {
                const float delta = float(dt) / float(duration_);
                if (current_ <= target_) {
                    current_ += delta;
                    if (current_ > target_) current_ = target_;
                } else {
                    current_ -= delta;
                    if (current_ < target_) current_ = target_;
                }
            }
        } else {
            lastReal_ = realMs;
        }
    }
    float alpha() const { return current_; }
    void clear() { *this = ScreenFade(); }

private:
    float target_ = 0, current_ = 0;
    int start_ = 0, duration_ = 0;
    int64_t lastReal_ = 0;
};

} // namespace iw2::game
