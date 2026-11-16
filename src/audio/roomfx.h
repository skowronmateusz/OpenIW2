#pragma once
// The audible room effect of the environment-effects state (audio/reverb.h). openiw2 approximation, not the original DSP:
// the exe hands the active room type and wet level to the Miles driver (AIL_set_digital_master_room_type, sample reverb
// levels (1.0, wet)), and the reverb itself lives in mss32.dll, a third-party library that is not analysed (docs/spec/sound.md
// "Round 5"). What is kept from the exe is the structure: the dry signal is untouched (the exe always passes 1.0 as the dry
// level) and each voice feeds the room by its wet level (0 for aliases with `nowetlevel`). The reverb here is a mono
// Schroeder design (four damped feedback combs into two allpasses) added equally to both output channels.
// Only the rooms listed in roomFxParams are rendered; every other room type stays silent as before.
#include <optional>
#include <vector>

namespace iw2::audio {

struct RoomFxParams {
    float decaySeconds = 1.0f; // time for the tail to fall by 60 dB
    float damping = 0.5f;      // 0..1: high-frequency loss per pass through a comb (one-pole low-pass in the loop)
    float predelaySeconds = 0; // delay before the tail starts
    float gain = 1.0f;         // tail level relative to the wet send
};

// Parameters of a room type index (EnvReverb::roomIndex), nullopt when the room is not rendered.
std::optional<RoomFxParams> roomFxParams(int room);

class RoomFx {
public:
    explicit RoomFx(int sampleRate = 48000);
    // Adds the room's response to `send` (mono, frames samples) into the interleaved stereo `out`.
    void process(const float* send, float* out, int frames, const RoomFxParams& p);
    // Clears the delay lines (the tail is dropped).
    void reset();

private:
    struct Comb {
        std::vector<float> buf;
        size_t pos = 0;
        float store = 0;
    };
    struct Allpass {
        std::vector<float> buf;
        size_t pos = 0;
    };
    int rate_;
    std::vector<float> pre_;
    size_t prePos_ = 0;
    Comb combs_[4];
    Allpass allpasses_[2];
};

} // namespace iw2::audio
