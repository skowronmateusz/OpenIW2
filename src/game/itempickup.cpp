#include "game/itempickup.h"

#include <utility>

#include <algorithm>
#include <cmath>

namespace iw2::game {

bool playerTouchesItem(const std::array<float, 3>& p, const std::array<float, 3>& it) {
    const float dx = p[0] - it[0], dy = p[1] - it[1], dz = p[2] - it[2];
    // Broad phase of G_TouchTriggers.
    if (std::fabs(dx) > 40.f || std::fabs(dy) > 40.f || std::fabs(dz) > 52.f) return false;
    return std::fabs(dx) <= 36.f && std::fabs(dy) <= 36.f && dz >= -88.f && dz <= 18.f;
}

bool canGrabWeaponItem(bool offhandOrItemClass, bool owned, bool touch, int room) {
    if (touch && !owned && !offhandOrItemClass) return false;
    return room > 0;
}

static int nextRand(uint32_t& seed) {
    seed = seed * 0x343fdu + 0x269ec3u;
    return static_cast<int>(seed >> 17); // 15 bits
}

ItemAmmo resolveItemAmmo(ItemAmmo item, int dropMin, int dropMax, int clipSize, int maxAmmo, uint32_t& seed) {
    if (item.stock == 0) {
        if (dropMin != 0 || dropMax != 0) {
            item.stock = dropMax == dropMin ? dropMin : nextRand(seed) % (dropMax - dropMin) + dropMin;
        } else {
            const float r = static_cast<float>(nextRand(seed)) / 32768.f;
            item.stock = static_cast<int>(std::lround((r + 1.f) * static_cast<float>(clipSize - 1) * 0.5f)) + 1;
        }
    }
    item.stock = std::min(item.stock, maxAmmo);
    if (item.clip == 0 && item.stock >= 0) {
        item.clip = std::min(clipSize, item.stock);
        item.stock -= item.clip;
    }
    item.clip = std::min(item.clip, clipSize);
    return item;
}

ItemAmmo actorDropAmmo(int dropMin, int dropMax, int clipSize, uint32_t& seed) {
    int hi = dropMax, lo = dropMin;
    if (hi < lo) std::swap(hi, lo); // 0x522198
    ItemAmmo a;
    if (hi == 0 && lo == 0) { // 0x5221b4..0x522272
        const float r1 = static_cast<float>(nextRand(seed)) * 3.0517578125e-05f;
        const int total = static_cast<int>(std::lround((r1 + 1.0f) * static_cast<float>(clipSize - 1) * 0.5f)) + 1;
        const float r2 = static_cast<float>(nextRand(seed)) * 1.52587890625e-05f;
        a.clip = static_cast<int>(std::lround(static_cast<float>(total) * (r2 + 0.25f)));
        a.stock = total - a.clip;
        return a;
    }
    if (hi < 0) return a; // 0x522277
    const int total = hi == lo ? lo : nextRand(seed) % (hi - lo) + lo;
    if (total <= 0) return a; // 0x5222c6
    const int clip = clipSize != 0 ? nextRand(seed) % clipSize : 0;
    if (clip >= total) a.clip = total; // 0x522311
    else a.clip = clip, a.stock = total - clip;
    return a;
}

ItemAmmo itemAmmoAfterPartial(ItemAmmo item, int added) {
    // The stock is reduced first, the rest comes off the clip.
    const int fromStock = std::min(added, std::max(item.stock, 0));
    item.stock -= fromStock;
    item.clip = std::max(0, item.clip - (added - fromStock));
    return item;
}

} // namespace iw2::game
