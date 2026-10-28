#include "audio/reverb.h"

#include <algorithm>
#include <cmath>

namespace iw2::audio {

namespace {
const char* const kRooms[] = {"generic", "paddedcell", "room", "bathroom", "livingroom", "stoneroom", "auditorium",
                              "concerthall", "cave", "arena", "hangar", "carpetedhallway", "hallway", "stonecorridor",
                              "alley", "forest", "city", "mountains", "quarry", "plain", "parkinglot", "sewerpipe",
                              "underwater", "drugged", "dizzy", "psychotic"};
constexpr int kRoomCount = int(sizeof kRooms / sizeof *kRooms);
} // namespace

EnvReverb::EnvReverb() { s_[0].on = true; } // 0x444aa4: slot 0 room 0, dry 1/1, wet 0/0, active

int EnvReverb::roomIndex(const std::string& name) {
    for (int i = 0; i < kRoomCount; ++i)
        if (name == kRooms[i]) return i;
    return -1;
}
const char* EnvReverb::roomName(int i) { return i >= 0 && i < kRoomCount ? kRooms[i] : ""; }
int EnvReverb::priorityIndex(const std::string& name) {
    if (name == "level") return 1;
    if (name == "shellshock") return 2;
    return -1;
}

bool EnvReverb::activate(int prio, int room, float dry, float wet, int fadeMs) {
    if (prio < 1 || prio >= kSlots || room < 0 || room >= kRoomCount) return false;
    if (!(dry >= 0.0f && dry <= 1.0f) || !(wet >= 0.0f && wet <= 1.0f) || fadeMs < 0) return false;
    Slot& s = s_[prio];
    const Slot cur = s_[active_]; // 0x793d60
    s.on = true;
    s.room = room;
    const float fade = float(std::max(fadeMs, 1)); // 0x443ee9: forced >= 1
    s.tgtDry = dry;
    s.tgtWet = wet;
    s.curDry = cur.curDry; // the new slot continues from the currently audible values
    s.curWet = cur.curWet;
    s.rateDry = (dry - cur.curDry) / fade;
    s.rateWet = (wet - cur.curWet) / fade;
    if (&s == &s_[active_]) return true; // same slot: only re-applies the room type (0x49fe90)
    for (int i = prio + 1; i < kSlots; ++i)
        if (s_[i].on) return true; // a higher priority slot is active: stored only
    active_ = prio;
    return true;
}

bool EnvReverb::activateCommand(int prio, const std::string& room, float dry, float wet, float fadeSeconds) {
    // CG_ReverbCmd 0x4a6350: fade = max(0, int(fadeSeconds * 1000)).
    const long ms = std::lround(double(fadeSeconds) * 1000.0);
    return activate(prio, roomIndex(room), dry, wet, int(std::max(ms, 0L)));
}

bool EnvReverb::deactivate(int prio, int fadeMs) {
    if (prio < 1 || prio >= kSlots) return false;
    Slot& s = s_[prio];
    s.on = false; // 0x443fdd
    if (&s != &s_[active_]) return true;
    int n = prio - 1; // next lower active slot (slot 0 is always on)
    while (n > 0 && !s_[n].on) --n;
    const Slot old = s;
    active_ = n;
    Slot& t = s_[n];
    const float fade = float(std::max(fadeMs, 1));
    t.curDry = old.curDry;
    t.curWet = old.curWet;
    t.rateDry = (t.tgtDry - t.curDry) / fade;
    t.rateWet = (t.tgtWet - t.curWet) / fade;
    return true;
}

bool EnvReverb::deactivateCommand(int prio, float fadeSeconds) {
    const long ms = std::lround(double(fadeSeconds) * 1000.0);
    return deactivate(prio, int(std::max(ms, 0L)));
}

void EnvReverb::tick(int dtMs) {
    Slot& s = s_[active_];
    auto step = [&](float& cur, float tgt, float& rate) {
        if (rate == 0.0f) return;
        cur += float(dtMs) * rate;
        if ((rate > 0.0f) ? (cur >= tgt) : (cur <= tgt)) {
            cur = tgt;
            rate = 0.0f;
        }
    };
    step(s.curDry, s.tgtDry, s.rateDry);
    step(s.curWet, s.tgtWet, s.rateWet);
}

} // namespace iw2::audio
