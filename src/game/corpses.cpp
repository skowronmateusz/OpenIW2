#include "game/corpses.h"

namespace iw2::game {

int CorpseSlots::pick(const script::Vec3& eye, const script::Vec3& forward, const OriginFn& origin) const {
    float bestBehind = -1.0f, bestOff = -1.0f, bestAny = -1.0f;
    int behind = 0, off = 0, any = 0;
    for (int i = 0; i < count_; ++i) {
        const int number = slots_[size_t(i)];
        if (number == -1) return i;
        const script::Vec3 o = origin(number);
        const float dx = o[0] - eye[0], dy = o[1] - eye[1], dz = o[2] - eye[2];
        const float distSq = dz * dz + dy * dy + dx * dx;
        const float dot = dz * forward[2] + dy * forward[1] + dx * forward[0];
        if (dot < 0.0f && bestBehind < distSq) {
            bestBehind = distSq;
            behind = i;
        }
        if (distSq * 0.5f >= dot * dot && bestOff < distSq) {
            bestOff = distSq;
            off = i;
        }
        if (bestAny < distSq) {
            bestAny = distSq;
            any = i;
        }
    }
    if (bestBehind != -1.0f) return behind;
    if (bestOff != -1.0f) return off;
    return any;
}

void CorpseSlots::release(int entityNumber) {
    for (int& s : slots_)
        if (s == entityNumber) {
            s = -1;
            return;
        }
}

std::vector<int> CorpseSlots::trim(int dvar) {
    std::vector<int> freed;
    for (int i = dvar; i < count_; ++i) {
        int& s = slots_[size_t(i)];
        if (s < 0) continue;
        freed.push_back(s);
        s = -1;
    }
    count_ = dvar;
    return freed;
}

int CorpseSlots::used() const {
    int n = 0;
    for (int s : slots_) n += s >= 0;
    return n;
}

} // namespace iw2::game
