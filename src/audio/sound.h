// Sounds: PCM .wav files, sound aliases (soundaliases/*.csv) and a small software mixer (docs/spec/sound.md).
#pragma once

#include "fs/vfs.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace iw2::audio {

struct Wav {
    int rate = 0;
    int channels = 0;
    std::vector<int16_t> samples; // interleaved
    size_t frames() const { return channels ? samples.size() / size_t(channels) : 0; }
};

// Parses a RIFF WAVE file with 16-bit PCM data. Throws std::runtime_error otherwise.
Wav parseWav(std::span<const uint8_t> data);

struct SoundAlias {
    std::string name;
    std::string file;          // relative to sound/
    float volMin = 1, volMax = 1;
    float pitchMin = 1, pitchMax = 1;
    float distMin = 120, distMax = 0; // distMax 0: not given
    std::string channel;       // "local" plays without position
    bool loop = false;
    std::string secondary;     // secondaryaliasname
    std::string subtitle;      // `subtitle` column: localised caption key (e.g. SUBTITLE_MOSCOW_RS1_PICKUPRIFLE), original case
};

class SoundAliases {
public:
    // Loads every soundaliases/*.csv; rows whose loadspec excludes `level` (or names other levels) are skipped.
    // Adds the aliases allowed for `level`. With `menuOnly` only rows whose loadspec names "menu" are taken (the front-end and
    // pause-menu sounds).
    void load(const fs::Vfs& vfs, const std::string& level, bool menuOnly = false);
    void clear() { aliases_.clear(); }
    // One of the variants with this name (random), nullptr if unknown.
    const SoundAlias* pick(const std::string& name, std::mt19937& rng) const;
    bool exists(const std::string& name) const;
    size_t size() const { return aliases_.size(); }

private:
    std::map<std::string, std::vector<SoundAlias>> aliases_; // lower-case names
};

using Vec3 = std::array<float, 3>;

// Mixes playing sounds into interleaved stereo float samples. play()/stop()/setListener() run on the game
// thread, mix() on the audio thread.
class Mixer {
public:
    Mixer(const fs::Vfs& vfs, const SoundAliases& aliases, int outputRate);

    // Starts an alias; `origin` empty plays it at full volume without panning. Returns a handle (0 = nothing).
    int play(const std::string& alias, std::optional<Vec3> origin = std::nullopt, float* duration = nullptr);
    // The subtitle key of the alias started by the last play() call (empty when none).
    const std::string& lastSubtitle() const { return lastSubtitle_; }
    void stop(int handle);
    void fadeOut(int handle, int ms);
    void fadeIn(int handle, int ms);
    // A looping low/high alias pair blended by fraction f (vehicle engine, cgame 0x4ac8a0 -> 0x4423d0): volume =
    // lerp(low.volMin, high.volMin, f), pitch lerped only at the start; plays the high alias' file (identical to the
    // low one for the shipped vehicle pairs). Returns a handle (0 = nothing).
    int playBlend(const std::string& low, const std::string& high, float f, const Vec3& origin);
    // Updates a live blend voice (0x441f80 / SetVolume 0x442114) and its position.
    void setBlend(int handle, float f, const Vec3& origin);
    void setListener(const Vec3& origin, const Vec3& forward, const Vec3& right);
    void mix(float* out, int frames);

    int active() const;
    float masterVolume = 0.75f; // snd_volume (default 1.0, 0..1) * 0.75 (0x443ab3 -> 0x793a24)
    // snd_fade (0x4431a0): volume scale moving linearly to `fadeTarget` at `fadeRate` per ms (fadeStep advances it).
    float fadeVolume = 1.0f, fadeTarget = 1.0f, fadeRate = 0.0f;
    void fade(float target, int ms);
    void fadeStep(float ms);

private:
    std::string lastSubtitle_;
    struct Voice {
        int handle = 0;
        std::shared_ptr<const Wav> wav;
        double pos = 0;
        double step = 1; // source frames per output frame
        float volume = 1;
        std::optional<Vec3> origin;
        float distMin = 0, distMax = 0;
        bool loop = false;
        bool blend = false;
        float gain = 1, fadeRate = 0; // per-voice fade (fadeRate = gain lost per output frame, negative = fade in)
        float volLow = 0, volHigh = 0;
    };
    std::shared_ptr<const Wav> load(const std::string& file);
    void gains(const Voice& v, float& left, float& right) const;

    const fs::Vfs& vfs_;
    const SoundAliases& aliases_;
    int rate_;
    std::mt19937 rng_{1234};
    std::map<std::string, std::shared_ptr<const Wav>> cache_;
    mutable std::mutex mutex_;
    std::vector<Voice> voices_;
    Vec3 listener_{0, 0, 0}, forward_{1, 0, 0}, right_{0, -1, 0};
    int nextHandle_ = 1;
};

} // namespace iw2::audio
