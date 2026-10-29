#include <sstream>
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
int channelIndex(const std::string& c) {
    static const char* const kChannels[11] = {"auto", "auto2d", "menu", "body", "item", "weapon",
                                              "voice", "local", "music", "announcer", "shellshock"};
    for (int i = 0; i < 11; ++i)
        if (c == kChannels[i]) return i;
    return -1;
}
} // namespace
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

// loadspec (0x428f40, callers pass the mode tag "all_sp" 0x56cb6c): lower-cased; a leading '!' makes the whole list exclusive.
// The level matches when its name occurs as a word (preceded by the start or a character below '"', followed by one below
// '!', so "!libya" lists libya). With flag = (exclusive ? level == "menu" : 1): a match returns flag; no match returns
// !flag for the "menu" level, otherwise flag when the whole spec equals the mode tag (so a row marked "all_sp" loads in
// every single-player level) and !flag when it does not. An empty spec allows every level.
bool loadspecAllows(const std::string& spec, const std::string& level) {
    const std::string s = lower(spec);
    if (s.empty()) return true;
    const bool exclusive = s[0] == '!';
    const bool flag = exclusive ? level == "menu" : true;
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
    if (listed) return flag;
    if (level == "menu") return !flag;
    return s == "all_sp" ? flag : !flag;
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

void SoundAliases::load(const fs::Vfs& vfs, const std::string& level, bool menuOnly) {
    const std::string lvl = lower(level);
    // soundaliases/volumemodgroups.def: "<group> <volume>" lines under a VOLUMEMODGROUPS header, "//" comments (0x429870).
    std::map<std::string, float> groups;
    if (const auto def = vfs.read("soundaliases/volumemodgroups.def")) {
        std::istringstream in(std::string(def->begin(), def->end()));
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            std::string name;
            float vol = 0;
            if ((ls >> name >> vol) && name.rfind("//", 0) != 0) groups[lower(name)] = vol;
        }
    }
    // soundaliases/<name>.vfcurve (parser 0x42a9f0 / 0x42a6e0): "SNDCURVE", a knot count (2..8) and that many "x y" pairs with x in
    // 0..1. The first knot must be (0,1) and the last (1,0); otherwise the exe warns and overwrites the endpoints.
    std::map<std::string, std::vector<std::array<float, 2>>> curves;
    for (const std::string& path : vfs.list("soundaliases/")) {
        if (path.size() < 8 || lower(path.substr(path.size() - 8)) != ".vfcurve") continue;
        const auto data = vfs.read(path);
        if (!data) continue;
        std::istringstream in(std::string(data->begin(), data->end()));
        std::string magic;
        int n = 0;
        if (!(in >> magic >> n) || magic != "SNDCURVE" || n < 2 || n > 8) continue;
        std::vector<std::array<float, 2>> k;
        float x, y;
        while (int(k.size()) < n && (in >> x >> y)) {
            if (x < 0 || x > 1) { k.clear(); break; }
            k.push_back({x, y});
        }
        if (int(k.size()) != n) continue;
        k.front() = {0, 1};
        k.back() = {1, 0};
        std::string name = path.substr(path.find_last_of('/') + 1);
        curves[lower(name.substr(0, name.size() - 8))] = std::move(k);
    }
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
            if (menuOnly && (get("loadspec").empty() || !loadspecAllows(get("loadspec"), "menu"))) continue;
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
            // masterslave (0x4290a0): "master" or a slave percentage 0..1 (out of range is a load error in the exe).
            if (const std::string ms = lower(get("masterslave")); ms == "master") a.master = true;
            else if (!ms.empty()) {
                const float pct = num(ms, -1);
                if (pct >= 0 && pct <= 1) a.slavePct = pct;
            }
            // reverb column (0x429130): substring "nowetlevel" sets alias flag 0x10, "fulldrylevel" flag 0x8.
            {
                const std::string rv = lower(get("reverb"));
                a.fullDryLevel = rv.find("fulldrylevel") != std::string::npos;
                a.noWetLevel = rv.find("nowetlevel") != std::string::npos;
            }
            if (const std::string c = lower(get("volumefalloffcurve")); !c.empty()) {
                if (const auto it = curves.find(c); it != curves.end()) a.curve = it->second;
            }
            a.streamed = lower(get("type")) == "streamed"; // 0x428e70
            a.startDelayMs = int(num(get("startdelay"), 0));
            a.lfePct = std::clamp(num(get("lfe percentage"), 0), 0.0f, 1.0f);
            a.secondary = lower(get("secondaryaliasname"));
            a.subtitle = get("subtitle");
            // vol_mod group (0x428bf0, applied by the post-parse check 0x429783): vol_min / vol_max are multiplied by the group
            // volume and clamped to 0..1; an unknown group gives 0. An empty cell leaves the volumes alone.
            if (const std::string g = lower(get("vol_mod")); !g.empty()) {
                const auto it = groups.find(g);
                const float gv = it == groups.end() ? 0.0f : it->second;
                if (gv != 1.0f) {
                    a.volMin = std::clamp(a.volMin * gv, 0.0f, 1.0f);
                    a.volMax = std::clamp(a.volMax * gv, 0.0f, 1.0f);
                }
            }
            if (a.file.empty()) continue;
            aliases_[a.name].push_back(std::move(a));
        }
    }
}

bool SoundAliases::exists(const std::string& name) const {
    auto it = aliases_.find(lower(name));
    return it != aliases_.end() && !it->second.empty();
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

namespace {
// The exe's channel numbers (alias flags >> 7 & 0xf, name table 0x59b334): auto 0, auto2d 1, menu 2, weapon 3, voice 4, item 5, body 6, local 7,
// music 8, announcer 9, shellshock 10. A slot's channel doubles as its steal priority (0x4417d1 stores it at slot +4, 0x441910 compares it).
int exeChannel(const std::string& c) {
    static const char* const kNames[11] = {"auto", "auto2d", "menu", "weapon", "voice", "item", "body", "local", "music", "announcer", "shellshock"};
    for (int i = 0; i < 11; ++i)
        if (c == kNames[i]) return i;
    return 0;
}
// 0x441d1c jump table (bytes 0x441dfc): auto, weapon, voice, item, body are 3D channels (0x49f740, culled beyond dist_max by 0x4423d0
// before anything starts); the rest are 2D (0x49f470).
bool is3DChannel(int ch) { return ch == 0 || (ch >= 3 && ch <= 6); }
} // namespace

// Pool bookkeeping of the generic play (0x4423d0): free slot (0x441990 / 0x441a30 / 0x441ad0) or else the steal choice of 0x441910: only slots
// whose priority (channel) is <= the new sound's are candidates; a slot of the new sound's owner wins over another owner's; otherwise the lowest
// priority, then the earliest end time (a loop ends "now", 0x49f6db) goes. Returns 0 (play fails) when nothing can be stolen.
int Mixer::allocSlot(Voice& v) {
    if (v.dedicated) return 1;
    const int cap = v.pool == 2 ? kPoolStream : v.pool == 1 ? kPool3D : kPool2D;
    int count = 0;
    for (const Voice& o : voices_) count += (!o.dedicated && o.pool == v.pool) ? 1 : 0;
    if (count < cap) return 1;
    auto endMs = [&](const Voice& o) {
        if (o.loop) return 0.0;
        return (double(o.wav->frames()) - o.pos) / o.step / double(rate_) * 1000.0 + o.delayFrames * 1000.0 / double(rate_);
    };
    int best = -1;
    for (int i = 0; i < int(voices_.size()); ++i) {
        const Voice& o = voices_[size_t(i)];
        if (o.dedicated || o.pool != v.pool || o.prio > v.prio) continue;
        if (best < 0) { best = i; continue; }
        const Voice& b = voices_[size_t(best)];
        if (o.owner != b.owner) {
            if (o.owner == v.owner) { best = i; continue; }
            if (b.owner == v.owner) continue;
        }
        int d = o.prio - b.prio;
        if (d == 0) {
            const double e = endMs(o) - endMs(b);
            d = e < 0 ? -1 : e > 0 ? 1 : 0;
            if (d == 0) continue;
        }
        if (d < 0) best = i;
    }
    if (best < 0) return 0;
    voices_.erase(voices_.begin() + best);
    return 1;
}

int Mixer::play(const std::string& name, std::optional<Vec3> origin, float* duration, PlayOptions opt) {
    lastSubtitle_.clear();
    const SoundAlias* a = aliases_.pick(name, rng_);
    if (!a) return 0;
    // The secondary alias starts first (0x442401..0x44243e) and may itself have none.
    const std::string sub = a->subtitle;
    if (!a->secondary.empty() && a->secondary != a->name) play(a->secondary, origin, nullptr, opt);
    lastSubtitle_ = sub;
    const int exch = exeChannel(a->channel);
    const float distMax = a->distMax > a->distMin ? a->distMax : a->distMin * 5.0f; // dist_max 0 -> 5 * dist_min (0x429701)
    if (is3DChannel(exch) && origin && a->channel != "local" && !opt.dedicated && (!a->loop || opt.refresh)) {
        std::lock_guard lock(mutex_);
        const float dx = (*origin)[0] - listener_[0], dy = (*origin)[1] - listener_[1], dz = (*origin)[2] - listener_[2];
        if (distMax > 0 && dx * dx + dy * dy + dz * dz > distMax * distMax) return 0; // 0x4424e6: culled when farther than dist_max
    }
    if (a->loop && opt.refresh) { // 0x441f80: a running loop of the same alias and owner is refreshed instead of restarted
        std::lock_guard lock(mutex_);
        for (Voice& o : voices_)
            if (o.refresh && o.loop && o.owner == opt.owner && o.name == a->name) {
                o.stamp = stampNow_;
                if (a->channel != "local") o.origin = origin;
                return o.handle;
            }
    }
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
    v.distMax = distMax;
    v.curve = a->curve;
    v.loop = a->loop;
    v.chan = channelIndex(a->channel);
    v.master = a->master;
    v.slavePct = a->slavePct;
    v.noWet = a->noWetLevel;
    v.owner = opt.owner;
    v.prio = exch;
    v.pool = a->streamed ? 2 : is3DChannel(exch) ? 1 : 0;
    v.dedicated = opt.dedicated;
    v.refresh = opt.refresh;
    v.name = a->name;
    v.delayFrames = double(a->startDelayMs) * double(rate_) / 1000.0;
    int handle;
    {
        std::lock_guard lock(mutex_);
        // Channel exclusivity (0x44250a..0x442531): outside auto / auto2d / menu, an alias without a secondary first stops the owner's
        // running sounds of the same channel.
        if (opt.owner >= 0 && exch > 2 && a->secondary.empty())
            std::erase_if(voices_, [&](const Voice& o) { return o.owner == opt.owner && o.prio == exch && !o.dedicated; });
        v.stamp = stampNow_;
        if (!allocSlot(v)) return 0;
        v.handle = handle = nextHandle_++;
        voices_.push_back(std::move(v));
    }
    return handle;
}

void Mixer::sweepLoops(int soundTimeMs) {
    std::lock_guard lock(mutex_);
    if (paused_) return; // 0x443522: the sweep does not run while paused
    std::erase_if(voices_, [&](const Voice& v) { return v.refresh && v.loop && v.stamp != stampNow_; });
    stampNow_ = soundTimeMs;
}

int Mixer::playBlend(const std::string& low, const std::string& high, float f, const Vec3& origin) {
    const SoundAlias* lo = aliases_.pick(low, rng_);
    const SoundAlias* hi = aliases_.pick(high, rng_);
    if (!lo || !hi) return 0;
    auto wav = load(hi->file);
    if (!wav || wav->frames() == 0) return 0;
    Voice v;
    v.wav = wav;
    v.blend = true;
    v.volLow = lo->volMin;
    v.volHigh = hi->volMin;
    v.volume = lo->volMin + (hi->volMin - lo->volMin) * f;
    const float pitch = lo->pitchMin + (hi->pitchMin - lo->pitchMin) * f;
    v.step = double(wav->rate) * pitch / double(rate_);
    v.origin = origin;
    v.distMin = lo->distMin + (hi->distMin - lo->distMin) * f;
    const float dmax = hi->distMax > hi->distMin ? hi->distMax : hi->distMin * 5.0f; // 0x429701
    v.distMax = dmax;
    v.curve = lo->curve; // 0x4a0e85: the curve of the alias weighted by (1 - f)
    v.loop = true;
    v.chan = channelIndex(lo->channel);
    std::lock_guard lock(mutex_);
    {
        // Culled when the listener is farther than lerp(distMax) (0x44247b); a running loop is not culled again.
        const float dx = origin[0] - listener_[0], dy = origin[1] - listener_[1], dz = origin[2] - listener_[2];
        const float dm = lo->distMax + ((hi->distMax > 0 ? hi->distMax : v.distMax) - lo->distMax) * f;
        if (dm > 0 && dx * dx + dy * dy + dz * dz > dm * dm) return 0;
    }
    v.handle = nextHandle_++;
    const int h = v.handle;
    voices_.push_back(std::move(v));
    return h;
}

void Mixer::setBlend(int handle, float f, const Vec3& origin) {
    std::lock_guard lock(mutex_);
    for (Voice& v : voices_)
        if (v.handle == handle && v.blend) {
            v.volume = v.volLow + (v.volHigh - v.volLow) * f;
            v.origin = origin;
        }
}

void Mixer::stop(int handle) {
    std::lock_guard lock(mutex_);
    std::erase_if(voices_, [&](const Voice& v) { return v.handle == handle; });
}

// mu_stop <ms> (0x442fb0): ms 0 stops the voice at once, otherwise its volume falls linearly to zero over `ms`.
void Mixer::fadeOut(int handle, int ms) {
    if (ms <= 0) { stop(handle); return; }
    std::lock_guard lock(mutex_);
    for (Voice& v : voices_)
        if (v.handle == handle) v.fadeRate = 1.0f / (float(ms) * 0.001f * float(rate_));
}

// Fade-in of a freshly started voice (ambient track, 0x442df0): gain 0 rising linearly to full over `ms`.
void Mixer::fadeIn(int handle, int ms) {
    if (ms <= 0) return;
    std::lock_guard lock(mutex_);
    for (Voice& v : voices_)
        if (v.handle == handle) v.gain = 0, v.fadeRate = -1.0f / (float(ms) * 0.001f * float(rate_));
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

// Distance gain (0x441590): 1 up to dist_min, 0 from dist_max (also when dist_min == dist_max), in between the alias'
// .vfcurve evaluated at t = (dist - dist_min) / (dist_max - dist_min) (0x420b40: linear between the knots around t).
float Mixer::falloff(const Voice& v, float dist) {
    float t = dist - v.distMin;
    if (t <= 0) return 1;
    if (v.distMin == v.distMax) return 0;
    t /= v.distMax - v.distMin;
    if (!(t < 1)) return 0;
    const auto& k = v.curve;
    for (size_t i = 1; i < k.size(); i++)
        if (t <= k[i][0]) return k[i - 1][1] + (t - k[i - 1][0]) / (k[i][0] - k[i - 1][0]) * (k[i][1] - k[i - 1][1]);
    return -1; // the exe returns -1 when no knot matches
}

// Pan law (0x49f000 / 0x49ff80, backend call "set levels(handle, left, right)"): a = volume x falloff. A stereo wave plays (a, a). A
// mono wave plays (a (1 - p), a p) with p = (1 - dot) / 2, dot = unit direction to the source . listener axis[1] (left); a mono
// wave without a position uses p = 0.5, so mono is half as loud per channel at the centre (0x49f5b9 halves a mono voice).
void Mixer::gains(const Voice& v, float& left, float& right) const {
    float a = v.volume, p = 0.5f;
    if (v.origin) {
        const Vec3 d{(*v.origin)[0] - listener_[0], (*v.origin)[1] - listener_[1], (*v.origin)[2] - listener_[2]};
        const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        a *= std::max(0.0f, falloff(v, dist));
        float side = 0; // the direction's component along "right" (= -dot); 0 when the source is at the listener (0x4210d0)
        if (dist > 0) side = (d[0] * right_[0] + d[1] * right_[1] + d[2] * right_[2]) / dist;
        p = (1 + side) * 0.5f;
    }
    if (v.wav->channels == 2) {
        left = right = a;
    } else {
        left = a * (1 - p);
        right = a * p;
    }
}

// snd_fade <volume> <ms> (0x4431a0): the rate is (target − current) / ms; ms 0 sets the volume at once.
void Mixer::fade(float target, int ms) {
    std::lock_guard lock(mutex_);
    fadeTarget = target;
    if (ms > 0) fadeRate = (target - fadeVolume) / float(ms);
    else fadeVolume = target, fadeRate = 0;
}

// Per-frame step (0x4437a0 takes the elapsed ms as an int; 0x443a21: vol += dt * rate, then clamped at the target, rate cleared): the
// same dt as every other fade (ambient slots, reverb), so the step is linear in elapsed milliseconds.
void Mixer::fadeStep(float ms) {
    if (fadeRate == 0) return;
    fadeVolume += fadeRate * ms;
    if ((fadeRate < 0 && fadeVolume <= fadeTarget) || (fadeRate > 0 && fadeVolume >= fadeTarget)) {
        fadeVolume = fadeTarget;
        fadeRate = 0;
    }
}

void Mixer::reverbCommand(int prio, const std::string& room, float dry, float wet, float fadeSeconds) {
    std::lock_guard lock(mutex_);
    reverb.activateCommand(prio, room, dry, wet, fadeSeconds);
}
void Mixer::deactivateReverbCommand(int prio, float fadeSeconds) {
    std::lock_guard lock(mutex_);
    reverb.deactivateCommand(prio, fadeSeconds);
}
float Mixer::voiceWetLevel(int handle) const {
    std::lock_guard lock(mutex_);
    for (const Voice& v : voices_)
        if (v.handle == handle) return reverb.voiceWet(v.noWet, enableReverb);
    return 0.0f;
}

void Mixer::setPaused(bool paused) {
    std::lock_guard lock(mutex_);
    if (paused == paused_) return;
    paused_ = paused;
    // Only the channels whose flag 0x793d6f[channel] is set pause (0x442a30); the defaults leave out "menu" and openvictoryscreen
    // (0x4a6d7f) also clears "music", so the level-end theme keeps playing under the victory screen.
    for (Voice& v : voices_) v.held = paused && (v.chan < 0 || channelPausable[v.chan]);
}

void Mixer::mix(float* out, int frames) {
    std::fill(out, out + size_t(frames) * 2, 0.0f);
    std::lock_guard lock(mutex_);
    // Reverb ramp (0x4442xx): advances the active slot's dry/wet by whole milliseconds.
    reverbMs_ += double(frames) * 1000.0 / double(rate_);
    if (reverbMs_ >= 1.0) {
        reverb.tick(int(reverbMs_));
        reverbMs_ -= std::floor(reverbMs_);
    }
    fadeMs_ += double(frames) * 1000.0 / double(rate_);
    if (fadeMs_ >= 1.0) {
        fadeStep(float(int(fadeMs_)));
        fadeMs_ -= std::floor(fadeMs_);
    }
    // Duck state (0x443bb0): rises while any master voice plays, falls otherwise, over snd_slaveFadeTime; 0 ms switches at once.
    {
        bool masterPlaying = false;
        for (const Voice& v : voices_) masterPlaying = masterPlaying || v.master;
        if (slaveFadeMs <= 0) duck_ = masterPlaying ? 1.0f : 0.0f;
        else duck_ = std::clamp(duck_ + (masterPlaying ? 1.0f : -1.0f) * float(frames) * 1000.0f / (float(rate_) * float(slaveFadeMs)), 0.0f, 1.0f);
    }
    for (Voice& v : voices_) {
        if (v.held) continue;
        if (pausedChannels && v.chan >= 0 && channelPausable[v.chan]) continue; // paused samples keep their position
        float gl, gr;
        gains(v, gl, gr);
        const float cs = v.chan >= 0 ? channelScale[v.chan] : 1.0f;
        // Slaves are ducked to slavePct while masters play: volume x (1 - duck x (1 - pct)) (0x4a0cf0).
        const float sl = (duck_ > 0 && !v.master && v.slavePct >= 0) ? 1.0f - duck_ * (1.0f - v.slavePct) : 1.0f;
        gl *= masterVolume * fadeVolume * cs * sl;
        gr *= masterVolume * fadeVolume * cs * sl;
        const float baseL = gl, baseR = gr;
        const Wav& w = *v.wav;
        const size_t n = w.frames();
        for (int i = 0; i < frames; ++i) {
            if (v.fadeRate != 0) { // fade out (rate > 0): linear to silence, then the voice ends; fade in (rate < 0): up to full
                v.gain -= v.fadeRate;
                if (v.fadeRate < 0 && v.gain >= 1) v.gain = 1, v.fadeRate = 0;
                if (v.gain <= 0) { v.pos = double(n); v.loop = false; break; }
            }
            if (v.gain != 1) {
                gl = baseL * v.gain;
                gr = baseR * v.gain;
            }
            if (v.delayFrames > 0) { v.delayFrames -= 1; continue; } // alias startdelay: silent until it has passed
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
