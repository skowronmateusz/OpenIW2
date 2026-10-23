#include "audio/sound.h"
#include "audio/decode.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace iw2::audio {
namespace {

uint32_t u32(std::span<const uint8_t> d, size_t at) {
    if (at + 4 > d.size()) throw std::runtime_error("wav: truncated");
    uint32_t v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}

uint16_t u16(std::span<const uint8_t> d, size_t at) {
    if (at + 2 > d.size()) throw std::runtime_error("wav: truncated");
    uint16_t v;
    std::memcpy(&v, d.data() + at, 2);
    return v;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Splits one CSV line; fields may be quoted with "" escapes.
std::vector<std::string> csvFields(const std::string& line) {
    std::vector<std::string> out(1);
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') { out.back() += '"'; ++i; }
            else if (c == '"') quoted = false;
            else out.back() += c;
        } else if (c == '"') quoted = true;
        else if (c == ',') out.emplace_back();
        else if (c != '\r') out.back() += c;
    }
    return out;
}

float num(const std::string& s, float fallback) {
    if (s.empty()) return fallback;
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    return end == s.c_str() ? fallback : v;
}

// loadspec (0x428f40): lower-cased; a leading '!' makes the whole list exclusive. The level matches when its name
// occurs as a word (preceded by the start or a character below '"', followed by one below '!', so "!libya" lists
// libya). Plain list: allowed when listed; '!' list: allowed when not listed. An empty spec allows every level.
// TODO(spec): the "menu" level special case and the compare against the first argument when nothing matched.
bool loadspecAllows(const std::string& spec, const std::string& level) {
    const std::string s = lower(spec);
    if (s.empty()) return true;
    const bool exclusive = s[0] == '!';
    bool listed = false;
    for (size_t at = s.find(level); !level.empty() && at != std::string::npos; at = s.find(level, at + 1)) {
        const bool startOk = at == 0 || static_cast<unsigned char>(s[at - 1]) < '"';
        const size_t end = at + level.size();
        const bool endOk = end >= s.size() || static_cast<unsigned char>(s[end]) < '!';
        if (startOk && endOk) {
            listed = true;
            break;
        }
    }
    return exclusive ? !listed : listed;
}

} // namespace

Wav parseWav(std::span<const uint8_t> d) {
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error("wav: not a RIFF WAVE file");
    Wav w;
    bool haveFmt = false;
    size_t at = 12;
    while (at + 8 <= d.size()) {
        const uint32_t size = u32(d, at + 4);
        const size_t body = at + 8;
        if (std::memcmp(d.data() + at, "fmt ", 4) == 0) {
            if (u16(d, body) != 1 || u16(d, body + 14) != 16) throw std::runtime_error("wav: only 16-bit PCM is supported");
            w.channels = u16(d, body + 2);
            w.rate = int(u32(d, body + 4));
            haveFmt = true;
        } else if (std::memcmp(d.data() + at, "data", 4) == 0) {
            if (!haveFmt) throw std::runtime_error("wav: data before fmt");
            const size_t n = std::min<size_t>(size, d.size() - body) / 2;
            w.samples.resize(n);
            std::memcpy(w.samples.data(), d.data() + body, n * 2);
            if (w.channels < 1 || w.channels > 2 || w.rate <= 0) throw std::runtime_error("wav: bad format");
            return w;
        }
        at = body + size + (size & 1);
    }
    throw std::runtime_error("wav: no data chunk");
}

void SoundAliases::load(const fs::Vfs& vfs, const std::string& level) {
    const std::string lvl = lower(level);
    for (const std::string& path : vfs.list("soundaliases/")) {
        if (path.size() < 4 || lower(path.substr(path.size() - 4)) != ".csv") continue;
        const auto data = vfs.read(path);
        if (!data) continue;
        std::string text(data->begin(), data->end());
        std::vector<std::string> keys;
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            const auto f = csvFields(text.substr(start, end - start));
            start = end + 1;
            if (f.empty() || f[0].empty() || f[0][0] == '#') continue;
            if (keys.empty()) { // the first non-comment row names the columns
                for (const auto& k : f) keys.push_back(lower(k));
                continue;
            }
            auto get = [&](const char* key) -> std::string {
                for (size_t i = 0; i < keys.size() && i < f.size(); ++i)
                    if (keys[i] == key) return f[i];
                return {};
            };
            if (!loadspecAllows(get("loadspec"), lvl)) continue;
            SoundAlias a;
            a.name = lower(f[0]);
            a.file = get("file");
            a.volMin = num(get("vol_min"), 1);
            a.volMax = num(get("vol_max"), a.volMin);
            a.pitchMin = num(get("pitch_min"), 1);
            a.pitchMax = num(get("pitch_max"), a.pitchMin);
            a.distMin = num(get("dist_min"), 120);
            a.distMax = num(get("dist_max"), 0);
            a.channel = lower(get("channel"));
            a.loop = lower(get("loop")) == "looping";
            a.secondary = lower(get("secondaryaliasname"));
            if (a.file.empty()) continue;
            aliases_[a.name].push_back(std::move(a));
        }
    }
}

const SoundAlias* SoundAliases::pick(const std::string& name, std::mt19937& rng) const {
    auto it = aliases_.find(lower(name));
    if (it == aliases_.end() || it->second.empty()) return nullptr;
    return &it->second[std::uniform_int_distribution<size_t>(0, it->second.size() - 1)(rng)];
}

Mixer::Mixer(const fs::Vfs& vfs, const SoundAliases& aliases, int outputRate) : vfs_(vfs), aliases_(aliases), rate_(outputRate) {}

std::shared_ptr<const Wav> Mixer::load(const std::string& file) {
    const std::string key = lower(file);
    if (auto it = cache_.find(key); it != cache_.end()) return it->second;
    std::shared_ptr<const Wav> w;
    try {
        if (const auto d = vfs_.read("sound/" + key)) w = std::make_shared<Wav>(decodeSound(*d, key));
        else std::cerr << "warning: missing sound " << file << "\n";
    } catch (const std::exception& e) {
        std::cerr << "warning: sound " << file << ": " << e.what() << "\n";
    }
    cache_[key] = w;
    return w;
}

int Mixer::play(const std::string& name, std::optional<Vec3> origin, float* duration) {
    const SoundAlias* a = aliases_.pick(name, rng_);
    if (!a) return 0;
    auto wav = load(a->file);
    if (!wav || wav->frames() == 0) return 0;
    Voice v;
    v.wav = wav;
    std::uniform_real_distribution<float> u(0, 1);
    v.volume = a->volMin + (a->volMax - a->volMin) * u(rng_);
    const float pitch = a->pitchMin + (a->pitchMax - a->pitchMin) * u(rng_);
    v.step = double(wav->rate) * pitch / double(rate_);
    if (duration) *duration = a->loop ? 0.0f : float(double(wav->frames()) / (double(wav->rate) * pitch));
    if (a->channel != "local") v.origin = origin;
    v.distMin = a->distMin;
    v.distMax = a->distMax > a->distMin ? a->distMax : a->distMin * 10.0f; // inferred default
    v.loop = a->loop;
    int handle;
    {
        std::lock_guard lock(mutex_);
        v.handle = handle = nextHandle_++;
        voices_.push_back(std::move(v));
    }
    if (!a->secondary.empty() && a->secondary != a->name) play(a->secondary, origin, nullptr);
    return handle;
}

void Mixer::stop(int handle) {
    std::lock_guard lock(mutex_);
    std::erase_if(voices_, [&](const Voice& v) { return v.handle == handle; });
}

void Mixer::setListener(const Vec3& origin, const Vec3& forward, const Vec3& right) {
    std::lock_guard lock(mutex_);
    listener_ = origin;
    forward_ = forward;
    right_ = right;
}

int Mixer::active() const {
    std::lock_guard lock(mutex_);
    return int(voices_.size());
}

// Linear fall-off between dist_min and dist_max (inferred; the .vfcurve files are not used yet) and
// constant-power panning by the direction's side component.
void Mixer::gains(const Voice& v, float& left, float& right) const {
    left = right = v.volume;
    if (!v.origin) return;
    const Vec3 d{(*v.origin)[0] - listener_[0], (*v.origin)[1] - listener_[1], (*v.origin)[2] - listener_[2]};
    const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float att = 1;
    if (dist > v.distMin) att = std::max(0.0f, 1.0f - (dist - v.distMin) / std::max(1.0f, v.distMax - v.distMin));
    float side = 0;
    if (dist > 1) side = (d[0] * right_[0] + d[1] * right_[1] + d[2] * right_[2]) / dist;
    const float angle = (side + 1) * 0.25f * float(M_PI); // 0 = left, pi/2 = right
    left = v.volume * att * std::cos(angle) * float(M_SQRT2);
    right = v.volume * att * std::sin(angle) * float(M_SQRT2);
    left = std::min(left, v.volume);
    right = std::min(right, v.volume);
}

// snd_fade <volume> <ms> (0x4431a0): the rate is (target − current) / ms; ms 0 sets the volume at once.
void Mixer::fade(float target, int ms) {
    fadeTarget = target;
    if (ms > 0) fadeRate = (target - fadeVolume) / float(ms);
    else fadeVolume = target, fadeRate = 0;
}

// TODO(spec): the per-frame step of the volume (0x793a28 by 0x793a30) is not read; linear in real ms is inferred.
void Mixer::fadeStep(float ms) {
    if (fadeRate == 0) return;
    fadeVolume += fadeRate * ms;
    if ((fadeRate < 0 && fadeVolume <= fadeTarget) || (fadeRate > 0 && fadeVolume >= fadeTarget)) {
        fadeVolume = fadeTarget;
        fadeRate = 0;
    }
}

void Mixer::mix(float* out, int frames) {
    std::fill(out, out + size_t(frames) * 2, 0.0f);
    std::lock_guard lock(mutex_);
    for (Voice& v : voices_) {
        float gl, gr;
        gains(v, gl, gr);
        gl *= masterVolume * fadeVolume;
        gr *= masterVolume * fadeVolume;
        const Wav& w = *v.wav;
        const size_t n = w.frames();
        for (int i = 0; i < frames; ++i) {
            if (v.pos >= double(n)) {
                if (!v.loop) break;
                v.pos = std::fmod(v.pos, double(n));
            }
            const size_t i0 = size_t(v.pos);
            const size_t i1 = i0 + 1 < n ? i0 + 1 : (v.loop ? 0 : i0);
            const float t = float(v.pos - double(i0));
            float l, r;
            if (w.channels == 1) {
                l = r = (w.samples[i0] * (1 - t) + w.samples[i1] * t) / 32768.0f;
            } else {
                l = (w.samples[i0 * 2] * (1 - t) + w.samples[i1 * 2] * t) / 32768.0f;
                r = (w.samples[i0 * 2 + 1] * (1 - t) + w.samples[i1 * 2 + 1] * t) / 32768.0f;
            }
            out[i * 2] += l * gl;
            out[i * 2 + 1] += r * gr;
            v.pos += v.step;
        }
    }
    std::erase_if(voices_, [](const Voice& v) { return !v.loop && v.pos >= double(v.wav->frames()); });
    for (int i = 0; i < frames * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

} // namespace iw2::audio
