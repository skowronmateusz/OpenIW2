// Tests for WAV parsing and the mixer's distance attenuation, with synthetic data.

#include "audio/decode.h"
#include "audio/reverb.h"
#include "audio/sound.h"

#include "fs/vfs.h"

#include <cmath>
#include <cstdlib>
#include <string>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <iostream>

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }

std::vector<uint8_t> makeWav(const std::vector<int16_t>& samples, int rate) {
    std::vector<uint8_t> v{'R', 'I', 'F', 'F'};
    put32(v, uint32_t(36 + samples.size() * 2));
    for (char c : std::string("WAVEfmt ")) v.push_back(uint8_t(c));
    put32(v, 16);
    put16(v, 1); put16(v, 1); put32(v, uint32_t(rate)); put32(v, uint32_t(rate * 2)); put16(v, 2); put16(v, 16);
    for (char c : std::string("data")) v.push_back(uint8_t(c));
    put32(v, uint32_t(samples.size() * 2));
    for (int16_t s : samples) put16(v, uint16_t(s));
    return v;
}

void writeFile(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// Distance falloff curves (0x441590 / 0x420b40) and master/slave ducking (0x443bb0 / 0x4a0cf0) through the real mixer.
void testMixer() {
    namespace fsys = std::filesystem;
    const fsys::path dir = fsys::temp_directory_path() / "iw2_test_sound_main";
    fsys::remove_all(dir);
    const auto wav = makeWav(std::vector<int16_t>(48000, 16384), 48000);
    fsys::create_directories(dir / "sound");
    std::ofstream(dir / "sound" / "c.wav", std::ios::binary).write(reinterpret_cast<const char*>(wav.data()), std::streamsize(wav.size()));
    writeFile(dir / "soundaliases" / "t.vfcurve", "SNDCURVE\n3\n0 1\n0.5 0.2\n1 0\n");
    writeFile(dir / "soundaliases" / "t.csv",
              "name,file,vol_min,vol_max,dist_min,dist_max,volumefalloffcurve,masterslave\n"
              "curved,c.wav,1,1,100,300,t,\n"
              "linear,c.wav,1,1,100,300,,\n"
              "boss,c.wav,1,1,,,,master\n"
              "minion,c.wav,1,1,,,,0.5\n");
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(dir);
    iw2::audio::SoundAliases al;
    al.load(vfs, "test");
    CHECK(al.exists("curved") && al.exists("minion"));

    auto level = [&](const char* alias, float distance) {
        iw2::audio::Mixer m(vfs, al, 48000);
        m.masterVolume = 1.0f;
        m.setListener({0, 0, 0}, {1, 0, 0}, {0, -1, 0});
        m.play(alias, iw2::audio::Vec3{distance, 0, 0});
        std::vector<float> out(2 * 64);
        m.mix(out.data(), 64);
        return out[2 * 63] / 0.25f; // 0.5 full scale wav, mono centre pan = half per channel
    };
    CHECK(std::fabs(level("curved", 50) - 1.0f) < 1e-3f);   // inside dist_min
    CHECK(std::fabs(level("curved", 200) - 0.2f) < 1e-3f);  // t = 0.5 -> knot (0.5, 0.2)
    CHECK(std::fabs(level("curved", 150) - 0.6f) < 1e-3f);  // t = 0.25 -> halfway between 1 and 0.2
    CHECK(level("curved", 300) == 0.0f);                    // t >= 1
    CHECK(std::fabs(level("linear", 200) - 0.5f) < 1e-3f);  // default curve (0,1)-(1,0)

    // Pan: a source on the left (listener right = -y, so +y is left) plays only in the left channel at full level.
    {
        iw2::audio::Mixer m(vfs, al, 48000);
        m.masterVolume = 1.0f;
        m.setListener({0, 0, 0}, {1, 0, 0}, {0, -1, 0});
        m.play("curved", iw2::audio::Vec3{0, 50, 0});
        std::vector<float> out(2 * 64);
        m.mix(out.data(), 64);
        CHECK(std::fabs(out[2 * 63] - 0.5f) < 1e-3f && std::fabs(out[2 * 63 + 1]) < 1e-3f);
    }

    // Ducking: a slave (0.5) is lowered by 1 - duck * 0.5 while a master plays; the duck builds over snd_slaveFadeTime.
    iw2::audio::Mixer m(vfs, al, 48000);
    m.masterVolume = 1.0f;
    m.slaveFadeMs = 100;
    m.play("minion");
    std::vector<float> out(2 * 2400);
    m.mix(out.data(), 2400);
    CHECK(std::fabs(out[2 * 2399] - 0.25f) < 1e-3f); // no master yet
    m.play("boss");
    m.mix(out.data(), 2400); // half the fade time: duck 0.5 -> slave factor 0.75, plus the master at 0.5
    CHECK(std::fabs(out[2 * 2399] - (0.25f * 0.75f + 0.25f)) < 1e-3f);
    m.mix(out.data(), 2400); // full duck: slave factor 0.5
    CHECK(std::fabs(out[2 * 2399] - (0.25f * 0.5f + 0.25f)) < 1e-3f);
    // snd_fade (0x4431a0 / 0x443a21): the master scale moves linearly to the target over the given ms and stops there.
    m.fade(0.0f, 1000);
    m.mix(out.data(), 2400); // 50 ms of 1000
    CHECK(std::fabs(m.fadeVolume - 0.95f) < 1e-3f);
    for (int i = 0; i < 25; i++) m.mix(out.data(), 2400);
    CHECK(m.fadeVolume == 0.0f);
    m.fade(1.0f, 0); // 0 ms sets it at once
    CHECK(m.fadeVolume == 1.0f);
    fsys::remove_all(dir);
}

} // namespace

int main() {
    const auto bytes = makeWav({100, -200, 16384}, 22050);
    const auto w = iw2::audio::parseWav(bytes);
    CHECK(w.rate == 22050 && w.channels == 1 && w.frames() == 3);
    CHECK(w.samples[2] == 16384);

    bool threw = false;
    try { iw2::audio::parseWav(std::vector<uint8_t>{1, 2, 3}); } catch (const std::exception&) { threw = true; }
    CHECK(threw);

    // decodeSound picks the parser by extension: WAV data under a .wav name, the MP3 decoder for .mp3.
    CHECK(iw2::audio::decodeSound(bytes, "a/b.WAV").frames() == 3);
    threw = false;
    try { iw2::audio::decodeSound(std::vector<uint8_t>(64, 0), "music/x.MP3"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);

    testMixer();

    {
        using iw2::audio::EnvReverb;
        CHECK(EnvReverb::roomIndex("generic") == 0 && EnvReverb::roomIndex("psychotic") == 25 && EnvReverb::roomIndex("x") == -1);
        EnvReverb r;
        CHECK(r.wet() == 0 && r.activeSlot() == 0);
        // level slot: wet ramps 0 -> 0.5 over 1000 ms
        CHECK(r.activateCommand(1, "cave", 1.0f, 0.5f, 1.0f));
        CHECK(r.activeSlot() == 1 && r.roomType() == 8);
        r.tick(500);
        CHECK(std::fabs(r.wet() - 0.25f) < 1e-4f);
        // shellshock outranks level; the level one stays stored
        CHECK(r.activate(2, 22, 1.0f, 1.0f, 0));
        CHECK(r.activeSlot() == 2 && r.roomType() == 22);
        r.tick(1);
        CHECK(r.wet() == 1.0f);
        // a lower slot while a higher is active only stores
        CHECK(r.activate(1, 3, 1.0f, 0.1f, 0) && r.activeSlot() == 2);
        // deactivating the active slot falls back to the level slot, wet moves to its target
        CHECK(r.deactivate(2, 100) && r.activeSlot() == 1 && r.roomType() == 3);
        r.tick(200);
        CHECK(std::fabs(r.wet() - 0.1f) < 1e-4f);
        CHECK(r.deactivate(1, 0) && r.activeSlot() == 0);
        r.tick(5);
        CHECK(r.wet() == 0.0f);
        CHECK(!r.activate(1, 0, 1.5f, 0.5f, 0) && !r.activate(1, 99, 1, 0.5f, 0) && !r.activate(0, 0, 1, 0.5f, 0));
        CHECK(r.voiceWet(true) == 0.0f && r.voiceWet(false, false) == 0.0f);
    }

    // Real data (IW2_MAIN): one-shot player sounds end; nothing stays in the mixer.
    if (const char* main = std::getenv("IW2_MAIN")) {
        iw2::fs::Vfs vfs;
        vfs.mountDirectory(main);
        iw2::audio::SoundAliases al;
        al.load(vfs, "toujane");
        iw2::audio::Mixer m(vfs, al, 48000);
        for (const char* a : {"land_plr_dirt", "step_run_plr_dirt", "gear_rattle_plr_run", "land_damage"}) {
            const int h = m.play(a);
            CHECK(h != 0 || std::string(a) == "gear_rattle_plr_run");
        }
        std::vector<float> buf(2 * 1024);
        for (int i = 0; i < 48000 * 4 / 1024; ++i) m.mix(buf.data(), 1024);
        std::cout << "voices after 4 s: " << m.active() << "\n";
        CHECK(m.active() == 0);
        // Pause (0x442a30): a game-channel voice holds its position while paused and finishes after the resume.
        CHECK(m.play("land_plr_dirt") != 0);
        m.setPaused(true);
        for (int i = 0; i < 48000 * 2 / 1024; ++i) m.mix(buf.data(), 1024);
        CHECK(m.active() == 1);
        m.setPaused(false);
        for (int i = 0; i < 48000 * 2 / 1024; ++i) m.mix(buf.data(), 1024);
        CHECK(m.active() == 0);
    }
    if (failures) return 1;
    std::cout << "sound tests passed\n";
    return 0;
}
