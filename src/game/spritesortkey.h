// Draw-list sort key of the 1.3 renderer (gfx_d3d_x86_s.dll writer 0x100247d0, walker 0x1002f830, sorter 0x10024900).
// Each draw record gets an 8-byte list entry {key, record*}; the list is sorted by the key as an unsigned 32-bit number,
// ascending (hybrid quicksort / insertion sort 0x10024900 / 0x100248b0 with unsigned compares; equal keys have no defined order).
#pragma once
#include <cstdint>

namespace iw2::game {

// matIndex: the material's rank in the sorted material table (word at material +0xa, assigned after sorting the table with the
// comparator 0x10016ae0: ascending material byte +0xd, then ascending techset pointer +0x38 in steps of 140 bytes).
// layer: 5 bits (default 0x1f, 0x10024850). entity: scene entity index. recordType: the first dword of the record (low nibble).
// fxSort: dvar fx_sort (0x101ce634, registered at 0x1000b9c1, default 1, flags 0x2000).
inline uint32_t drawSortKey(uint32_t matIndex, uint32_t layer, uint32_t entity, uint32_t recordType, bool fxSort) {
    if (fxSort && recordType == 2) {
        // Sprite records (type 2) with fx_sort: entity-major. The entity index is biased by -0x1000 (entities >= 0x1000, the
        // effect entities, keep a clear sign bit; smaller ones wrap and sort after them as unsigned numbers).
        const uint32_t ent = entity - 0x1000u;
        return ((((ent << 10) + matIndex) << 5) + layer) * 16u + recordType; // 0x100247ff..0x10024816
    }
    // Material-major: material << 21 | layer << 16 | entity << 4 | recordType (0x1002481a..0x1002482b).
    return ((((matIndex << 5) + layer) << 12) + entity) * 16u + recordType;
}

// The fields the walker 0x1002f830 reads back (0x1002f8f9..0x1002f94b).
struct DrawKeyFields {
    uint32_t entity, material, layer;
};
inline DrawKeyFields drawKeyFields(uint32_t key) {
    if (key & 0x80000000u) return {(key >> 0x13) & 0xfff, (key >> 9) & 0x3ff, (key >> 4) & 0x1f};
    return {(key >> 4) & 0xfff, (key >> 0x15) & 0x3ff, (key >> 0x10) & 0x1f};
}

// Order of two materials in the sorted material table (comparator 0x10016ae0): ascending material byte (Material::sortByte), ties by
// the techset record address (ascending, i.e. techset first-load order). `techsetRank` must be that load order; it is NOT reproducible
// from the shipped data alone (TODO(spec): the engine's whole material load sequence), so iw2view does not rank sprites with it.
inline bool materialSortLess(uint8_t byteA, uint32_t techsetRankA, uint8_t byteB, uint32_t techsetRankB) {
    return byteA != byteB ? byteA < byteB : techsetRankA < techsetRankB;
}

} // namespace iw2::game
