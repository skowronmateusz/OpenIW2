// Draw order helper for the 2D HUD: the scope / binocular overlay is drawn by the crosshair step of the 2D pass (VA 0x4bd760 ->
// 0x4bcdd0 in the patch 1.3 SP executable), before the script hud elements (0x4c3030, layer 0 then layer 1), the HUD menus and the
// subtitles; iw2view builds those into one list in a different order, so the overlay quads are collected apart and spliced in.
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace iw2::game {

// Inserts `quads` into `list` at `pos` (clamped to the list size), keeping their order.
template <class T>
void insertBelow(std::vector<T>& list, size_t pos, const std::vector<T>& quads) {
    list.insert(list.begin() + std::ptrdiff_t(std::min(pos, list.size())), quads.begin(), quads.end());
}

} // namespace iw2::game
