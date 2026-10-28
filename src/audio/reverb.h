#pragma once
// Client-side environment-effects (reverb) state of the sound system, implemented from CoD2SP_s.exe 1.3:
//   CG_ReverbCmd 0x4a6350 ("reverb <prio> <room> <dry> <wet> <fade s>"), CG_DeactivateReverbCmd 0x4a6500,
//   snd_setEnvironmentEffects 0x441260, snd_deactivateEnvironmentEffects 0x4413d0, activate 0x443ec0,
//   deactivate 0x443fd0, per-frame ramp 0x4442xx..0x4443xx, init 0x444aa4.
// The exe only tracks the state and hands the room type and the wet level to the Miles driver (AIL_set_digital_master_room_type,
// AIL_set_3D_room_type, AIL_set_sample_reverb_levels(handle, 1.0, wet)); the DSP itself lives in mss32.dll and is not reproduced.
#include <string>

namespace iw2::audio {

class EnvReverb {
public:
    static constexpr int kSlots = 3; // 0 = default (always active), 1 = snd_enveffectsprio_level, 2 = ..._shellshock

    EnvReverb();

    // Room type index (0x5a02e8 table: generic, paddedcell, room, ...) or -1 when the name is unknown (0x4411e0 prints an error).
    static int roomIndex(const std::string& name);
    static const char* roomName(int index);
    // Priority index from the name (0x441160 list: "none"=0 unused, "level"=1, "shellshock"=2); -1 when unknown.
    static int priorityIndex(const std::string& name);

    // snd_setEnvironmentEffects. Returns false (nothing changed) for a bad priority/room, dry or wet outside 0..1, fade < 0.
    bool activate(int prio, int room, float dry, float wet, int fadeMs);
    bool activateCommand(int prio, const std::string& room, float dry, float wet, float fadeSeconds);
    // snd_deactivateEnvironmentEffects.
    bool deactivate(int prio, int fadeMs);
    bool deactivateCommand(int prio, float fadeSeconds);
    // Per-frame ramp (0x44429x): current dry/wet move towards the active slot's targets.
    void tick(int dtMs);

    int activeSlot() const { return active_; }
    int roomType() const { return s_[active_].room; }
    float dry() const { return s_[active_].curDry; } // tracked by the exe, no consumer found (only the wet level is applied)
    float wet() const { return s_[active_].curWet; }
    // Wet level applied to one voice: 0 when snd_enableReverb is off or the alias' `reverb` column contains "nowetlevel"
    // (alias flag 0x10, parsed at 0x429130), otherwise the active wet level (0x49e9f0).
    float voiceWet(bool aliasNoWet, bool enableReverb = true) const { return (enableReverb && !aliasNoWet) ? wet() : 0.0f; }

private:
    struct Slot {
        int room = 0;
        float curDry = 1, tgtDry = 1, rateDry = 0; // +4 +8 +0xc
        float curWet = 0, tgtWet = 0, rateWet = 0; // +0x10 +0x14 +0x18
        bool on = false;                           // +0x1c
    };
    Slot s_[kSlots];
    int active_ = 0; // 0x793d60
};

} // namespace iw2::audio
