// The voices the client keeps a handle of between frames: the music slot, the ambient track, the shell shock loop and the
// hold-breath heartbeat loop. They belong to the level (or the connection) that started them: a disconnect and a level load
// stop every voice (Mixer::stopAll, the stop-all routine of VA 0x44f940 in the patch 1.3 MP executable, called with no keep
// flags by CL_Disconnect 0x40cd10) and the handles must not outlive that, or the next frame would fade or stop a voice that
// is no longer the level's.
#pragma once

#include <string>

#include "audio/sound.h"

namespace iw2::audio {

struct ClientSoundHandles {
    int music = 0;                 // mu_play voice (music slot 0)
    int ambient = 0;               // ambient_play voice (the dedicated ambient slots)
    std::string ambientAlias;      // the alias the ambient voice plays (the same alias keeps playing)
    int shockLoop = 0;             // the shell shock loop (a blend voice)
    int shockSeenStart = -1;       // start time of the shell shock the loop belongs to (its end sound: game::ShockEndSound)
    int heartbeatLoop = 0;         // the hold-breath heartbeat

    // The ambient track (the slot pair of 0x443080): the same alias keeps playing; another fades the old out and itself in.
    void playAmbient(Mixer& mixer, const std::string& alias, int ms) {
        if (ambient && alias == ambientAlias) return;
        if (ambient) mixer.fadeOut(ambient, ms);
        ambient = mixer.play(alias, std::nullopt, nullptr, {.dedicated = true});
        ambientAlias = ambient ? alias : std::string();
        if (ambient) mixer.fadeIn(ambient, ms);
    }
    void stopAmbient(Mixer& mixer, int ms) {
        if (ambient) mixer.fadeOut(ambient, ms);
        ambient = 0;
        ambientAlias.clear();
    }

    // Stops every voice in the mixer and forgets all handles.
    void stopAll(Mixer& mixer) {
        mixer.stopAll();
        *this = ClientSoundHandles{};
    }
};

} // namespace iw2::audio
