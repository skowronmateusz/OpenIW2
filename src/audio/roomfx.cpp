#include "audio/roomfx.h"

#include "audio/reverb.h"

#include <algorithm>
#include <cmath>

namespace iw2::audio {

std::optional<RoomFxParams> roomFxParams(int room) {
    // "underwater" is the room of the shipped shell shocks (shock/default.shock, shock/duhoc_boatexplosion.shock); no level
    // script uses it. TODO(spec): every value below is an openiw2 choice (a dark tail of about 1.5 s), not read from the Miles
    // DSP; the other 25 room types are not rendered.
    if (room == EnvReverb::roomIndex("underwater")) return RoomFxParams{1.5f, 0.85f, 0.011f, 1.0f};
    return std::nullopt;
}

RoomFx::RoomFx(int sampleRate) : rate_(sampleRate > 0 ? sampleRate : 48000) {
    // Delay lengths of the classic Schroeder / Freeverb tuning at 44.1 kHz (mutually prime), scaled to the output rate.
    // TODO(spec): openiw2 choice.
    static const int kComb[4] = {1116, 1188, 1277, 1356};
    static const int kAllpass[2] = {556, 441};
    const double scale = double(rate_) / 44100.0;
    for (int i = 0; i < 4; ++i) combs_[i].buf.assign(size_t(std::max(1L, std::lround(kComb[i] * scale))), 0.0f);
    for (int i = 0; i < 2; ++i) allpasses_[i].buf.assign(size_t(std::max(1L, std::lround(kAllpass[i] * scale))), 0.0f);
    pre_.assign(size_t(rate_) / 10 + 1, 0.0f); // up to 0.1 s of predelay
}

void RoomFx::reset() {
    for (auto& c : combs_) std::fill(c.buf.begin(), c.buf.end(), 0.0f), c.pos = 0, c.store = 0;
    for (auto& a : allpasses_) std::fill(a.buf.begin(), a.buf.end(), 0.0f), a.pos = 0;
    std::fill(pre_.begin(), pre_.end(), 0.0f);
    prePos_ = 0;
}

void RoomFx::process(const float* send, float* out, int frames, const RoomFxParams& p) {
    const size_t preLen = std::min(pre_.size() - 1, size_t(std::max(0L, std::lround(double(p.predelaySeconds) * rate_))));
    float feedback[4];
    for (int i = 0; i < 4; ++i) // -60 dB after decaySeconds: g^(decay * rate / length) = 0.001
        feedback[i] = p.decaySeconds > 0 ? float(std::pow(0.001, double(combs_[i].buf.size()) / (double(p.decaySeconds) * rate_))) : 0.0f;
    const float damp = std::clamp(p.damping, 0.0f, 0.99f);
    for (int n = 0; n < frames; ++n) {
        // Predelay line.
        pre_[prePos_] = send[n];
        const size_t readPos = (prePos_ + pre_.size() - preLen) % pre_.size();
        const float in = preLen ? pre_[readPos] : send[n];
        prePos_ = (prePos_ + 1) % pre_.size();
        // Parallel damped combs.
        float acc = 0;
        for (int i = 0; i < 4; ++i) {
            Comb& c = combs_[i];
            const float y = c.buf[c.pos];
            c.store = y * (1.0f - damp) + c.store * damp;
            c.buf[c.pos] = in + c.store * feedback[i];
            c.pos = (c.pos + 1) % c.buf.size();
            acc += y;
        }
        acc *= 0.25f;
        // Series allpasses (gain 0.5).
        for (auto& a : allpasses_) {
            const float b = a.buf[a.pos];
            a.buf[a.pos] = acc + b * 0.5f;
            acc = b - acc * 0.5f;
            a.pos = (a.pos + 1) % a.buf.size();
        }
        out[n * 2] += acc * p.gain;
        out[n * 2 + 1] += acc * p.gain;
    }
}

} // namespace iw2::audio
