// Corpse slots (docs/spec/ai.md "Corpse limit"): the 64 slots of the corpse table (0x1c7e5b0, stride 0x20), the slot picker with
// its eviction order (0x4fd0d0) and the frame-end trim (0x4d9710). The table only holds entity numbers; World frees the entities.
#pragma once

#include "script/value.h"

#include <array>
#include <functional>
#include <vector>

namespace iw2::game {

constexpr int kCorpseSlots = 64;       // 0x40: the table size and the top of the ai_corpseCount range (0x4d886f)
// DELIBERATE DEVIATION from the 1.3 executable: its ai_corpseCount default is 5 (0x4d8868); openiw2 defaults to 20, approved by the
// project owner. docs/spec/ai.md "Corpse limit" and docs/debug-flags.md list it; the range and all the rules below are the original's.
constexpr int kCorpseCountDefault = 20;

class CorpseSlots {
public:
    using OriginFn = std::function<script::Vec3(int entityNumber)>;

    CorpseSlots() { slots_.fill(-1); }

    // The slot for a new corpse (0x4fd0d0), which may be occupied (the caller frees that entity first). The first free slot below
    // the count wins. With none free the victim is the farthest corpse behind the view (dot < 0); with none behind, the farthest
    // at least 45 degrees off the view (0.5 * dist^2 >= dot^2); else the farthest of all. A tie keeps the lower slot. Count 0: slot 0.
    int pick(const script::Vec3& eye, const script::Vec3& forward, const OriginFn& origin) const;
    void put(int slot, int entityNumber) { slots_[size_t(slot)] = entityNumber; }
    int at(int slot) const { return slots_[size_t(slot)]; }

    // G_FreeEntity's slot release (0x4d7e31).
    void release(int entityNumber);
    // Frame end (0x4d9710): when the dvar is below the cached count the entities of the slots from the dvar up are returned (the
    // caller frees them); either way the dvar becomes the cached count.
    std::vector<int> trim(int dvar);

    int count() const { return count_; }
    void setCount(int count) { count_ = count; }
    int used() const;
    void clear() { slots_.fill(-1); }

private:
    std::array<int, kCorpseSlots> slots_{};
    int count_ = kCorpseCountDefault; // 0x1c60544
};

} // namespace iw2::game
