// Shell shock: the "shock/<name>.shock" records (cgame 0x4b1950 captures the cg_shock_*
// dvars into a 0x84-byte record at 0xf56bdc + index * 0x84) and the per-frame effects computed from a record, the
// shock's start time and its duration (CG_UpdateShellShock 0x4b24a0).
#pragma once

#include <string>

namespace iw2::game {

// The record, in the units the exe stores (times in whole milliseconds, 0x4b1950).
struct ShockRecord {
    int viewKickFadeMs = 3000;        // +0x00 cg_shock_viewKickFadeTime
    float viewKickCyclesPerMs = 0.001f / 0.75f; // +0x04 0.001 / max(cg_shock_viewKickPeriod, 0.001)
    float viewKickRadius = 0.05f;     // +0x08 cg_shock_viewKickRadius
    int blendFadeMs = 1000;           // +0x0c cg_shock_screenBlendFadeTime
    int blendMs = 400;                // +0x10 cg_shock_screenBlendTime
    bool sound = true;                // +0x14 cg_shock_sound
    int soundFadeInMs = 250;          // +0x18 cg_shock_soundFadeInTime
    int soundFadeOutMs = 2500;        // +0x1c cg_shock_soundFadeOutTime
    float dry = 1.0f, wet = 0.5f;     // +0x20 / +0x24 cg_shock_soundDryLevel / WetLevel
    std::string room;                 // +0x28 cg_shock_soundRoomType (15 characters, default empty)
    // +0x38..0x60 cg_shock_volume_*, in the record's order (0..1): auto, auto2d, menu, body, item, weapon, voice,
    // local, music, announcer, shellshock.
    float volume[11] = {0.1f, 0.1f, 1.0f, 0.1f, 0.1f, 0.5f, 0.2f, 1.0f, 1.0f, 1.0f, 1.0f}; // registration defaults
    int soundModEndDelayMs = 2000;     // +0x64 cg_shock_soundModEndDelay
    int loopFadeMs = 1500;            // +0x68 cg_shock_soundLoopFadeTime
    int loopEndDelayMs = -3000;       // +0x6c cg_shock_soundLoopEndDelay
    bool mouse = true;                // +0x70 cg_shock_mouse
    int mouseFadeMs = 2000;           // +0x74 cg_shock_mouse_fadeTime
    float mouseSensitivityScale = 0.5f; // +0x78 cg_shock_mouse_sensitivityscale
    float mouseMaxPitchSpeed = 90.0f; // +0x7c cg_shock_mouse_maxpitchspeed
    float mouseMaxYawSpeed = 90.0f;   // +0x80 cg_shock_mouse_maxyawspeed
};

// Names of the volume channels in ShockRecord::volume order (the alias `type` column values).
extern const char* const kShockChannelNames[11];

// Reads a .shock file (`cg_shock_* "value"` lines executed as dvar sets, so each value is clamped to its dvar's
// registered range, 0x4a20d0) over the defaults of the dvar registrations.
ShockRecord parseShockFile(const std::string& text);

// Seconds to whole milliseconds as the exe does (x87 round to nearest of seconds * 1000 + 9.3e-10).
int shockMs(float seconds);

// View kick (0x4b2380): a smoothly interpolated pseudo-random offset (x: sideways, y: up) in the view's local
// frame, fading out over the record's kick fade time; zero once the shock is over.
struct ShockKick {
    float x = 0, y = 0;
};
ShockKick shockViewKick(const ShockRecord& r, int elapsedMs, int durationMs);

// Rotates a view axis (forward, left, up as 9 floats) by the kick (0x4b1540): the new axis is M * old, M having the
// rows forward' = (1, x, y) normalised, left' = (0, 0, 1) x forward' normalised, up' = forward' x left'.
void applyShockKick(const float old[9], const ShockKick& k, float out[9]);

// Mouse look while shocked (0x4b22c0): the sensitivity scale and the per-frame speed limits (degrees per second,
// 0 = unlimited).
struct ShockMouse {
    float sensitivityScale = 1.0f;
    float maxPitchSpeed = 0, maxYawSpeed = 0;
};
ShockMouse shockMouse(const ShockRecord& r, int elapsedMs, int durationMs);

// Sound filter (0x4b1f90): the effect fraction 0..1 (0 = no filter): fade-in over the first soundFadeInMs, held, then
// fading out so that it reaches zero at duration + soundModEndDelay + soundFadeOut.
float shockSoundFraction(const ShockRecord& r, int elapsedMs, int durationMs);
// The volume factor of a channel at fraction f: (volume - 1) * f + 1.
float shockChannelVolume(const ShockRecord& r, int channel, float f);
// The shellshock_loop sound: plays while duration + loopEndDelay + loopFade lies ahead; the returned blend 0..1 goes
// from the loud loop (0) to its silent twin (1) over the last loopFadeMs. Negative: no loop.
float shockLoopBlend(const ShockRecord& r, int elapsedMs, int durationMs);
// The time (ms after the start) at which the shellshock_end sound starts: duration + loopEndDelay.
int shockEndSoundMs(const ShockRecord& r, int durationMs);

// Screen blend (0x4b16b0): the blend time in ms handed to the renderer, 0 when the shock is over. `remainingMs` is
// start + duration - now.
int shockBlendMs(const ShockRecord& r, int remainingMs);

} // namespace iw2::game
