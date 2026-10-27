#include "game/shellshock.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace iw2::game {

const char* const kShockChannelNames[11] = {"auto", "auto2d", "menu", "body", "item", "weapon",
                                            "voice", "local", "music", "announcer", "shellshock"};

namespace {
// 0x56e640: 128 view kick points (x, y); the exe repeats the first three after them, so that the 4-point window never
// leaves the table (the index is masked with 0x7f).
const float kKick[128][2] = {
    {-0.563551009f, -0.0044300002f}, {-0.282061994f, -0.757933021f}, {0.413046986f, 0.244246006f}, {0.527894974f, -0.72389698f},
    {-0.329777002f, 0.669799984f}, {-0.394248009f, -0.763090014f}, {0.126206994f, 0.497768998f}, {0.00498599978f, -0.01413f},
    {0.55991298f, 0.112824999f}, {-0.333088994f, -0.573283017f}, {0.335404009f, -0.107175998f}, {-0.569060028f, -0.213140994f},
    {-0.166676f, 0.785084009f}, {0.299591988f, 0.0375929996f}, {-0.516866982f, 0.510758996f}, {0.138008997f, 0.0348230004f},
    {-0.156167001f, 0.829047978f}, {-0.999458015f, 0.0203169994f}, {0.30002901f, 0.252943993f}, {0.0302150007f, -0.295731992f},
    {-0.917361975f, -0.0507109985f}, {0.0441769995f, -0.269288987f}, {0.588424027f, 0.362576991f}, {-0.379913002f, 0.619213998f},
    {0.204431996f, -0.0194230005f}, {0.018499f, 0.468079001f}, {0.916186988f, -0.247878f}, {0.00379900006f, 0.108209997f},
    {0.0573629998f, 0.606239974f}, {0.324595004f, 0.158732995f}, {-0.130529001f, -0.183387995f}, {0.715672016f, -0.363858014f},
    {0.984257996f, 0.106095999f}, {-0.00331300008f, 0.34553501f}, {-0.320351005f, -0.573935986f}, {0.0634550005f, -0.00323899998f},
    {-0.570173025f, -0.759312987f}, {0.106455997f, 0.283726007f}, {-0.668163002f, 0.142388001f}, {-0.501119018f, -0.720005989f},
    {-0.253280997f, 0.524031997f}, {-0.0640840009f, -0.165942997f}, {-0.194672003f, 0.43355f}, {-0.281800002f, -0.417744011f},
    {0.0457860008f, 0.40298599f}, {0.105063997f, -0.558937013f}, {0.312243998f, 0.688318014f}, {-0.263294011f, -0.256810993f},
    {0.659186006f, 0.070671998f}, {0.0936250016f, -0.0468120016f}, {-0.875020027f, 0.288509011f}, {0.329358995f, 0.105940998f},
    {-0.181309f, 0.259864986f}, {0.261597008f, -0.0740699992f}, {-0.29608199f, 0.0318580009f}, {0.0385840014f, 0.565946996f},
    {-0.253444999f, -0.71786499f}, {-0.211835995f, 0.336521f}, {0.89012301f, 0.00494999997f}, {-0.97982502f, -0.170790002f},
    {0.0453459993f, 0.0222399998f}, {-0.345795989f, 0.522711992f}, {0.108525001f, 0.165424004f}, {-0.572795987f, -0.473399013f},
    {0.368604988f, -0.865844011f}, {0.0755710006f, -0.327702999f}, {-0.466352999f, -0.565594018f}, {-0.358837008f, 0.610301971f},
    {0.603883982f, 0.440023005f}, {0.00246499991f, -0.144448996f}, {-0.294914991f, 0.799969971f}, {-0.0283470005f, -0.112071f},
    {-0.00947200041f, 0.686061025f}, {0.0711499974f, 0.0199100003f}, {0.962689996f, 0.0249259993f}, {0.309208006f, 0.87154901f},
    {-0.123782001f, -0.31230101f}, {-0.433055013f, -0.895981014f}, {0.962495029f, -0.263776988f}, {-0.511460006f, -0.359477997f},
    {-0.044013001f, 0.0202099998f}, {-0.109339997f, -0.761229992f}, {0.171002999f, -0.107460998f}, {0.418911994f, 0.435294002f},
    {0.444940001f, -0.139642999f}, {0.518573999f, 0.365965009f}, {-0.506996989f, 0.655596972f}, {0.510524988f, 0.508961022f},
    {-0.296173006f, -0.67583698f}, {0.851332009f, 0.307191998f}, {-0.0084739998f, -0.188743994f}, {0.552703023f, 0.427085996f},
    {0.0803340003f, -0.00280499994f}, {0.0356560014f, 0.610991001f}, {0.770592988f, 0.398874015f}, {-0.522136986f, 0.32436201f},
    {0.00604500016f, 0.0427879989f}, {0.482455999f, 0.848994017f}, {0.226058006f, -0.522367001f}, {-0.674606025f, -0.547814012f},
    {-0.441998005f, 0.598839998f}, {-0.183956996f, -0.270233989f}, {0.518850029f, 0.634945989f}, {0.430386007f, 0.125257f},
    {-0.185496002f, -0.264459014f}, {0.0236900002f, 0.312977999f}, {-0.444287002f, 0.849928021f}, {0.291978002f, -0.897678971f},
    {-0.0458259992f, -0.0471279994f}, {-0.114246003f, 0.51197499f}, {0.738133013f, 0.607667029f}, {-0.786889017f, -0.384056985f},
    {0.182992995f, 0.265085995f}, {-0.399450004f, -0.30903101f}, {-0.482894987f, 0.265662014f}, {0.0596709996f, 0.0977599993f},
    {0.793174028f, -0.0159719996f}, {0.201657996f, 0.492444992f}, {-0.707370996f, -0.0261899997f}, {-0.320881993f, 0.372280002f},
    {0.572812974f, -0.537254989f}, {0.337619007f, 0.116292998f}, {-0.606536984f, 0.173372999f}, {-0.166593f, -0.335112005f},
    {-0.583993018f, 0.182916f}, {-0.573518991f, -0.623347998f}, {-0.39270699f, 0.449474007f}, {0.151473999f, 0.840400994f},
};

float cubic(float p0, float p1, float p2, float p3, float t) {
    const float a = p3 - p2 + p1 - p0;
    const float b = p0 - p1 - a;
    return p1 + t * ((p2 - p0) + t * (b + a * t));
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
} // namespace

int shockMs(float seconds) {
    const float t = seconds * 1000.0f;
    return int(std::lrint(double(t) + 9.313225746154785e-10));
}

ShockRecord parseShockFile(const std::string& text) {
    ShockRecord r;
    double period = 0.75;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string key, value;
        if (!(ls >> key)) continue;
        std::getline(ls >> std::ws, value);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        if (key.rfind("cg_shock_", 0) != 0) continue;
        key = key.substr(9);
        const float v = float(std::atof(value.c_str()));
        auto time = [&](int& ms) { ms = shockMs(clampf(v, 0.001f, 10.0f)); };
        auto unit = [&](float& f) { f = clampf(v, 0.0f, 1.0f); };
        if (key == "screenBlendTime") time(r.blendMs);
        else if (key == "screenBlendFadeTime") time(r.blendFadeMs);
        else if (key == "viewKickPeriod") period = clampf(v, 0.001f, 10.0f);
        else if (key == "viewKickRadius") unit(r.viewKickRadius);
        else if (key == "viewKickFadeTime") time(r.viewKickFadeMs);
        else if (key == "sound") r.sound = std::atoi(value.c_str()) != 0;
        else if (key == "soundFadeInTime") time(r.soundFadeInMs);
        else if (key == "soundFadeOutTime") time(r.soundFadeOutMs);
        else if (key == "soundLoopFadeTime") time(r.loopFadeMs);
        else if (key == "soundLoopEndDelay") r.loopEndDelayMs = shockMs(clampf(v, -10.0f, 10.0f));
        else if (key == "soundRoomType") r.room = value.substr(0, 15);
        else if (key == "soundDryLevel") unit(r.dry);
        else if (key == "soundWetLevel") unit(r.wet);
        else if (key == "soundModEndDelay") r.soundModEndDelayMs = shockMs(clampf(v, -10.0f, 10.0f));
        else if (key.rfind("volume_", 0) == 0) {
            for (int i = 0; i < 11; ++i)
                if (key.substr(7) == kShockChannelNames[i]) unit(r.volume[i]);
        } else if (key == "mouse") r.mouse = std::atoi(value.c_str()) != 0;
        else if (key == "mouse_maxpitchspeed") r.mouseMaxPitchSpeed = std::max(v, 0.0f);
        else if (key == "mouse_maxyawspeed") r.mouseMaxYawSpeed = std::max(v, 0.0f);
        else if (key == "mouse_sensitivityscale") r.mouseSensitivityScale = clampf(v, 0.0f, 2.0f);
        else if (key == "mouse_fadeTime") time(r.mouseFadeMs);
    }
    r.viewKickCyclesPerMs = 0.001f / float(period);
    return r;
}

ShockKick shockViewKick(const ShockRecord& r, int elapsedMs, int durationMs) {
    const int remaining = durationMs - elapsedMs;
    if (remaining <= 0) return {};
    float ratio = 1.0f;
    if (remaining < r.viewKickFadeMs) ratio = float(remaining) / float(r.viewKickFadeMs);
    const float amp = ratio * ratio * (3.0f - 2.0f * ratio) * r.viewKickRadius; // smoothstep
    const float cycles = float(elapsedMs) * r.viewKickCyclesPerMs;
    const int idx = int(std::lrint(double(cycles) - 0.4999999990686774));
    const float t = cycles - float(idx);
    const int base = (durationMs * 0x3d + idx) & 0x7f;
    float p[4][2];
    for (int k = 0; k < 4; ++k) {
        p[k][0] = kKick[(base + k) & 0x7f][0];
        p[k][1] = kKick[(base + k) & 0x7f][1];
    }
    ShockKick out;
    out.x = amp * cubic(p[0][0], p[1][0], p[2][0], p[3][0], t);
    out.y = amp * cubic(p[0][1], p[1][1], p[2][1], p[3][1], t);
    return out;
}

void applyShockKick(const float old[9], const ShockKick& k, float out[9]) {
    auto norm = [](float v[3]) {
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 0) for (int i = 0; i < 3; ++i) v[i] /= l;
    };
    float f[3] = {1.0f, k.x, k.y};
    norm(f);
    const float u[3] = {0, 0, 1};
    float c[3] = {u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]};
    norm(c);
    const float d[3] = {f[1] * c[2] - f[2] * c[1], f[2] * c[0] - f[0] * c[2], f[0] * c[1] - f[1] * c[0]};
    const float* m[3] = {f, c, d};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i * 3 + j] = m[i][0] * old[j] + m[i][1] * old[3 + j] + m[i][2] * old[6 + j];
}

ShockMouse shockMouse(const ShockRecord& r, int elapsedMs, int durationMs) {
    ShockMouse out;
    if (!r.mouse) return out;
    const int remaining = durationMs - elapsedMs;
    if (remaining >= r.mouseFadeMs) {
        out.sensitivityScale = r.mouseSensitivityScale;
        out.maxPitchSpeed = r.mouseMaxPitchSpeed;
        out.maxYawSpeed = r.mouseMaxYawSpeed;
    } else if (remaining > 0) {
        const float t = float(remaining) / float(r.mouseFadeMs);
        out.sensitivityScale = (r.mouseSensitivityScale - 1.0f) * t + 1.0f;
        const float inv = 1.0f / t;
        out.maxPitchSpeed = inv * r.mouseMaxPitchSpeed;
        out.maxYawSpeed = inv * r.mouseMaxYawSpeed;
    }
    return out;
}

float shockSoundFraction(const ShockRecord& r, int elapsedMs, int durationMs) {
    if (!r.sound) return 0.0f;
    const int x = r.soundModEndDelayMs - elapsedMs + r.soundFadeOutMs + durationMs;
    if (x < r.soundFadeOutMs) {
        const float f = float(x) / float(r.soundFadeOutMs);
        return f > 0.0f ? f : 0.0f;
    }
    if (elapsedMs < r.soundFadeInMs) return float(elapsedMs) / float(r.soundFadeInMs);
    return 1.0f;
}

float shockChannelVolume(const ShockRecord& r, int channel, float f) {
    if (channel < 0 || channel >= 11) return 1.0f;
    return (r.volume[channel] - 1.0f) * f + 1.0f;
}

float shockLoopBlend(const ShockRecord& r, int elapsedMs, int durationMs) {
    const int z = r.loopEndDelayMs - elapsedMs + durationMs + r.loopFadeMs;
    if (z <= 0) return -1.0f;
    if (r.loopFadeMs == 0) return 0.0f;
    const float f = 1.0f - float(z) / float(r.loopFadeMs);
    return f > 0.0f ? f : 0.0f;
}

int shockEndSoundMs(const ShockRecord& r, int durationMs) { return durationMs + r.loopEndDelayMs; }

int shockBlendMs(const ShockRecord& r, int remainingMs) {
    if (remainingMs <= 0) return 0;
    if (remainingMs >= r.blendFadeMs) return r.blendMs;
    const float v = float(remainingMs) / float(r.blendFadeMs) * float(r.blendMs);
    return int(std::lrint(double(v) + 9.313225746154785e-10));
}

} // namespace iw2::game
