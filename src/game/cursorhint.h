// The cursor hint's hold and fade (cgame cursor-hint draw, VA 0x4a9b50 in the patch 1.3 SP executable, which calls the colour fade
// 0x4b0900 with cg_hintFadeTime): while a hint is set its start time is the current frame time; once it is gone the last hint stays for
// the fade time with an alpha that falls over the last `span` milliseconds, then it is dropped. The icon and the text are drawn from the
// same held hint, so they come and go together.
//
// The times are the client's frame clock (clienttime.h), which restarts with every level: a held hint belongs to the level that set it
// and must be forgotten with it (reset), or its start time from the old clock keeps it drawn until the new clock catches up.
#pragma once

#include <cstdint>
#include <string>

#include "game/hudfade.h"

namespace iw2::game {

class CursorHintFade {
public:
    struct Hint {
        int hint = 0;        // HINT_* class (2 usable, 3 health, 4 friendly), 0 none
        std::string weapon;  // a weapon hint: the weapon whose icon is drawn
        std::string text;    // the hint string (localization key, may be empty)
        bool active() const { return hint != 0 || !weapon.empty(); }
    };

    // cg_hintFadeTime (default 100 ms) and the 0x4b0900 span argument (100).
    static constexpr int64_t kFadeTimeMs = 100, kFadeSpanMs = 100;

    // One drawn frame at `nowMs`: the current hint (empty when none). Returns the alpha to draw the held hint with (0..1), or a
    // negative value when nothing is drawn.
    float update(const Hint& current, int64_t nowMs, int64_t fadeTimeMs = kFadeTimeMs, int64_t spanMs = kFadeSpanMs) {
        if (current.active()) {
            start_ = nowMs;
            held_ = current;
            valid_ = true;
            return 1.0f;
        }
        if (!valid_) return -1.0f;
        // A clock that restarted under a held hint (nowMs before its start) cannot be the same level's: drop it.
        const float a = nowMs < start_ ? -1.0f : hudFadeAlpha(start_, nowMs, fadeTimeMs, spanMs);
        if (a < 0.0f) {
            valid_ = false;
            return -1.0f;
        }
        return a;
    }

    const Hint& held() const { return held_; }
    bool valid() const { return valid_; }
    void reset() { *this = CursorHintFade{}; }

private:
    Hint held_;
    int64_t start_ = 0;
    bool valid_ = false;
};

} // namespace iw2::game
