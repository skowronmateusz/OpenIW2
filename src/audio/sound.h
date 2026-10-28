// Sounds: PCM .wav files, sound aliases (soundaliases/*.csv) and a small software mixer (docs/spec/sound.md).
#pragma once

#include "audio/reverb.h"
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
    bool master = false;       // masterslave column "master" (alias flag 0x2)
    float slavePct = -1;       // masterslave column number 0..1 (alias flag 0x4, +0x30); -1 = not a slave
    bool noWetLevel = false;   // `reverb` column contains "nowetlevel" (alias flag 0x10, 0x429130): the voice gets no reverb send
    bool fullDryLevel = false; // `reverb` column contains "fulldrylevel" (alias flag 0x8); TODO(spec): no consumer found in the exe
    // volumefalloffcurve (soundaliases/<name>.vfcurve, alias +0x40): knots (x = normalised distance 0..1, y = gain); an empty
    // column keeps the built-in default curve (0,1)-(1,0) (0x428d22).
    std::vector<std::array<float, 2>> curve{{0, 1}, {1, 0}};
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
    // snd_slaveFadeTime (default 500 ms, 0..5000): how long the duck of slave aliases takes to build up / release (0x443bb0).
    int slaveFadeMs = 500;
    float masterVolume = 0.75f; // snd_volume (default 1.0, 0..1) * 0.75 (0x443ab3 -> 0x793a24)
    // Per-channel volume scales in the order auto, auto2d, menu, body, item, weapon, voice, local, music, announcer,
    // shellshock (the shell shock channel volumes, shellshock.h); 1 = unchanged.
    float channelScale[11] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    // Pause (0x442dc0 / 0x442df0 / 0x444690: when cl_paused changes, 0x442a30 pauses and 0x442bf0 resumes the playing samples whose
    // channel flag 0x793d6f[channel] is set). The flags are reset to the defaults 0x793d64 (every channel but menu) at sound init and
    // client (re)connects (0x444ba5, 0x40cc80, 0x401610, 0x402940); `openvictoryscreen` (0x4a6d7f) also clears music.
    bool channelPausable[11] = {true, true, false, true, true, true, true, true, true, true, true};
    bool paused = false;
    void resetPausable() { for (int c = 0; c < 11; ++c) channelPausable[c] = c != 2; }
    // snd_fade (0x4431a0): volume scale moving linearly to `fadeTarget` at `fadeRate` per ms (fadeStep advances it).
    float fadeVolume = 1.0f, fadeTarget = 1.0f, fadeRate = 0.0f;
    void fade(float target, int ms);
    void fadeStep(float ms);
    // Environment effects (reverb) state, see audio/reverb.h. `enableReverb` is the dvar snd_enableReverb (default 1).
    // TODO(spec): the reverb DSP is Miles' (mss32.dll), so only the state is modelled.
    void reverbCommand(int prio, const std::string& room, float dry, float wet, float fadeSeconds);
    void deactivateReverbCommand(int prio, float fadeSeconds);
    // Reverb send of a voice (0 without the handle's wet level): the exe passes this to AIL_set_sample_reverb_levels.
    float voiceWetLevel(int handle) const;
    EnvReverb reverb;
    bool enableReverb = true;

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
        bool master = false;
        float slavePct = -1;
        bool noWet = false;
        int chan = -1; // index into kChannels (the alias `channel` column), -1 = unknown
        float volLow = 0, volHigh = 0;
        std::vector<std::array<float, 2>> curve{{0, 1}, {1, 0}};
    };
    std::shared_ptr<const Wav> load(const std::string& file);
    void gains(const Voice& v, float& left, float& right) const;
    static float falloff(const Voice& v, float dist);

    const fs::Vfs& vfs_;
    const SoundAliases& aliases_;
    int rate_;
    std::mt19937 rng_{1234};
    std::map<std::string, std::shared_ptr<const Wav>> cache_;
    mutable std::mutex mutex_;
    std::vector<Voice> voices_;
    Vec3 listener_{0, 0, 0}, forward_{1, 0, 0}, right_{0, -1, 0};
    int nextHandle_ = 1;
    double reverbMs_ = 0;
    float duck_ = 0; // 0x793cfc: 0..1, rises while a master voice plays
};

} // namespace iw2::audio
