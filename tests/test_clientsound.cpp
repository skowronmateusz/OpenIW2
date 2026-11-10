// Stopping every sound of a client (Mixer::stopAll, audio::ClientSoundHandles) at a disconnect or a level load.
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "audio/clientsound.h"
#include "audio/sound.h"
#include "fs/vfs.h"
#include "netgame/ambientconfig.h"

namespace {
int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
std::vector<uint8_t> makeWav(int frames) {
    std::vector<uint8_t> v{'R', 'I', 'F', 'F'};
    put32(v, uint32_t(36 + frames * 2));
    for (char c : std::string("WAVEfmt ")) v.push_back(uint8_t(c));
    put32(v, 16); put16(v, 1); put16(v, 1); put32(v, 48000); put32(v, 96000); put16(v, 2); put16(v, 16);
    for (char c : std::string("data")) v.push_back(uint8_t(c));
    put32(v, uint32_t(frames * 2));
    for (int i = 0; i < frames; ++i) put16(v, 8000);
    return v;
}
} // namespace

int main() {
    namespace fsys = std::filesystem;
    const fsys::path dir = fsys::temp_directory_path() / ("iw2_test_clientsound_" + std::to_string(::getpid()));
    fsys::remove_all(dir);
    fsys::create_directories(dir / "sound");
    fsys::create_directories(dir / "soundaliases");
    const auto wav = makeWav(48000);
    std::ofstream(dir / "sound" / "c.wav", std::ios::binary).write(reinterpret_cast<const char*>(wav.data()), std::streamsize(wav.size()));
    std::ofstream(dir / "soundaliases" / "t.csv")
        << "name,file,vol_min,vol_max,dist_min,dist_max,channel,type,loop,startdelay,secondaryaliasname,lfe percentage,speakermap\n"
           "amb,c.wav,1,1,,,music,streamed,looping,,,,\n"
           "mus,c.wav,1,1,,,music,streamed,,,,,\n"
           "loop3d,c.wav,1,1,100,200,weapon,,looping,,,,\n"
           "shot,c.wav,1,1,100,200,weapon,,,,,,\n";
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(dir);
    iw2::audio::SoundAliases al;
    al.load(vfs, "t");
    iw2::audio::Mixer m(vfs, al, 48000);
    m.masterVolume = 1.0f;

    // The ambient track and the music of a level, a looping effect, a one-shot and the shell shock style blend loop.
    iw2::audio::ClientSoundHandles snd;
    snd.ambient = m.play("amb", std::nullopt, nullptr, {.dedicated = true});
    snd.ambientAlias = "amb";
    snd.music = m.play("mus", std::nullopt, nullptr, {.dedicated = true});
    snd.shockLoop = m.playBlend("loop3d", "loop3d", 0.5f, {0, 0, 0});
    snd.heartbeatLoop = m.play("loop3d");
    snd.shockSeenStart = 1234;
    const int loose = m.play("loop3d", iw2::audio::Vec3{10, 0, 0});
    CHECK(snd.ambient && snd.music && snd.shockLoop && snd.heartbeatLoop && loose);
    CHECK(m.active() >= 5);

    // The disconnect: every voice goes, music and ambient included, and the handles are forgotten with them.
    snd.stopAll(m);
    CHECK(m.active() == 0);
    CHECK(snd.ambient == 0 && snd.music == 0 && snd.shockLoop == 0 && snd.heartbeatLoop == 0);
    CHECK(snd.ambientAlias.empty() && snd.shockSeenStart == -1);
    std::vector<float> out(2 * 256, 1.0f);
    m.mix(out.data(), 256);
    bool silent = true;
    for (float s : out) silent = silent && s == 0.0f;
    CHECK(silent);  // nothing plays on in the main menu

    // The mixer is usable afterwards, and the old handles stay dead (a late stop of one is harmless).
    const int h = m.play("shot", iw2::audio::Vec3{10, 0, 0});
    CHECK(h != 0 && m.active() == 1);
    m.stop(loose);
    CHECK(m.active() == 1);
    m.stopAll();
    CHECK(m.active() == 0);
    // Configstring 3 of a network game: the gamestate's track, then changes; once per value; a late joiner has no fade.
    {
        using iw2::netgame::AmbientConfigTracker;
        using iw2::netgame::parseAmbientConfig;
        AmbientConfigTracker t;
        CHECK(!t.update("", 100));  // no track on the map
        auto c = t.update("n\\amb", 100);
        CHECK(c && c->play && c->alias == "amb" && c->fadeMs == 0);
        CHECK(!t.update("n\\amb", 150));
        c = t.update("n\\other\\t\\2000", 1500);
        CHECK(c && c->play && c->alias == "other" && c->fadeMs == 500);
        c = t.update("t\\2400", 2000);
        CHECK(c && !c->play && c->fadeMs == 400);
        CHECK(parseAmbientConfig("n\\amb\\t\\100", 5000).fadeMs == 0);
        t.reset();
        c = t.update("n\\amb", 9000);  // after the level change the same value starts the track again
        CHECK(c && c->play);
        // The voice follows, and a disconnect ends a fading track too: no ambient in the menu.
        iw2::audio::ClientSoundHandles a;
        a.playAmbient(m, "amb", 500);
        const int voice = a.ambient;
        CHECK(voice != 0 && m.active() == 1);
        a.playAmbient(m, "amb", 500);
        CHECK(a.ambient == voice && m.active() == 1);  // the same alias keeps playing
        a.stopAmbient(m, 400);
        CHECK(a.ambient == 0 && m.active() == 1);  // still fading out
        a.stopAll(m);
        CHECK(m.active() == 0);
    }
    fsys::remove_all(dir);
    if (failures) std::cerr << failures << " check(s) failed\n";
    else std::cout << "clientsound: ok\n";
    return failures ? 1 : 0;
}
