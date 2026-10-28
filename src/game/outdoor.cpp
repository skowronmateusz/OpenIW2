#include "game/outdoor.h"

#include "fs/vfs.h"

#include <algorithm>
#include <cfenv>
#include <cmath>

namespace iw2::game {

void OutdoorMap::setBounds(const assets::D3dBsp& bsp, const fs::Vfs& vfs) {
    std::vector<int> skip(bsp.materials().size(), 0);
    for (size_t i = 0; i < bsp.materials().size(); ++i) {
        const auto md = vfs.read("materials/" + bsp.materials()[i].name);
        skip[i] = md && md->size() > 0xc && ((*md)[0xc] & 8) ? 1 : 0; // the runtime material is the relocated file
    }
    std::array<float, 3> mins{131072.0f, 131072.0f, 131072.0f}, maxs{-131072.0f, -131072.0f, -131072.0f};
    const auto& verts = bsp.vertices();
    const auto& idx = bsp.indices();
    // World surfaces only: model 0's soups (lump 35; brush models are entities and stored in model space).
    const auto [worldFirst, worldCount] = bsp.modelSoups(0);
    for (size_t si = worldFirst; si < worldFirst + worldCount; ++si) {
        const auto& soup = bsp.soups()[si];
        if (soup.material < skip.size() && skip[soup.material]) continue;
        // Surface bounds (the surface reference's bounds pointer, +8): the soup's vertex bounds.
        std::array<float, 3> smin{1e30f, 1e30f, 1e30f}, smax{-1e30f, -1e30f, -1e30f};
        bool any = false;
        for (uint32_t k = 0; k < soup.indexCount; ++k) {
            if (soup.firstIndex + k >= idx.size()) break;
            const size_t vi = soup.firstVertex + idx[soup.firstIndex + k];
            if (vi >= verts.size()) continue;
            any = true;
            for (size_t c = 0; c < 3; ++c) {
                smin[c] = std::min(smin[c], verts[vi].position[c]);
                smax[c] = std::max(smax[c], verts[vi].position[c]);
            }
        }
        if (!any) continue;
        for (size_t c = 0; c < 3; ++c) {
            if (mins[c] > smin[c]) mins[c] = smin[c];
            if (maxs[c] < smax[c]) maxs[c] = smax[c];
        }
    }
    setBounds(mins, maxs);
}

void OutdoorMap::setBounds(const std::array<float, 3>& mins, const std::array<float, 3>& maxs) {
    mins_ = mins;
    maxs_ = maxs;
    for (int c = 0; c < 3; ++c) {
        if (mins_[c] == 131072.0f) mins_[c] = 0, maxs_[c] = 0;
        if (!(maxs_[c] - mins_[c] >= 1.0f)) mins_[c] -= 0.5f, maxs_[c] += 0.5f;
        scale_[c] = float(kSize[c] - 1) / (maxs_[c] - mins_[c]);
        bias_[c] = -(scale_[c] * mins_[c]);
    }
    image_.clear();
}

void OutdoorMap::build(const std::function<float(float x, float y, float zTop, float zBottom)>& hitZ) {
    image_.assign(size_t(kSize[0]) * size_t(kSize[1]), 0);
    const float invX = 1.0f / scale_[0], invY = 1.0f / scale_[1];
    const int round = std::fegetround();
    std::fesetround(FE_TONEAREST); // fistp in the default x87 mode
    for (int row = 0; row < kSize[1]; ++row) {
        const float y = (float(row) + 0.5f - bias_[1]) * invY;
        for (int col = 0; col < kSize[0]; ++col) {
            const float x = (float(col) + 0.5f - bias_[0]) * invX;
            const float z = hitZ(x, y, maxs_[2] + 1.0f, mins_[2] - 1.0f);
            const long v = std::lrint(double(z * scale_[2] + bias_[2]) - 0.4999999990686774);
            image_[size_t(row) * size_t(kSize[0]) + size_t(col)] = uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
    std::fesetround(round);
}

bool OutdoorMap::covered(const std::array<float, 3>& p) const {
    if (image_.empty()) return false;
    float pos[3];
    for (int c = 0; c < 3; ++c) pos[c] = (p[size_t(c)] - mins_[size_t(c)]) / (maxs_[size_t(c)] - mins_[size_t(c)]);
    const int col = std::clamp(int(std::floor(pos[0] * float(kSize[0]))), 0, kSize[0] - 1);
    const int row = std::clamp(int(std::floor(pos[1] * float(kSize[1]))), 0, kSize[1] - 1);
    return pos[2] < float(image_[size_t(row) * size_t(kSize[0]) + size_t(col)]) / 255.0f;
}

} // namespace iw2::game
