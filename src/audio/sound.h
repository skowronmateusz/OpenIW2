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
};

class SoundAliases {
public:
    // Loads every soundaliases/*.csv; rows whose loadspec excludes `level` (or names other levels) are skipped.
    void load(const fs::Vfs& vfs, const std::string& level);
    // One of the variants with this name (random), nullptr if unknown.
    const SoundAlias* pick(const std::string& name, std::mt19937& rng) const;
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
    void stop(int handle);
    void setListener(const Vec3& origin, const Vec3& forward, const Vec3& right);
    void mix(float* out, int frames);

    int active() const;
    float masterVolume = 0.8f;

private:
    struct Voice {
        int handle = 0;
        std::shared_ptr<const Wav> wav;
        double pos = 0;
        double step = 1; // source frames per output frame
        float volume = 1;
        std::optional<Vec3> origin;
        float distMin = 0, distMax = 0;
        bool loop = false;
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
