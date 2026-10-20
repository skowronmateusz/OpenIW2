#include "assets/lightgrid.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace iw2::assets {
namespace {

constexpr float kOrigin = -131072.0f;
constexpr float kCellXY = 32.0f, kCellZ = 64.0f;

float smooth(float t) { return (3.0f - 2.0f * t) * t * t; }

} // namespace

LightGrid::LightGrid(std::span<const uint8_t> hashLump, std::span<const uint8_t> coeffLump) {
    if (hashLump.size() % 8 || coeffLump.size() % 24) throw std::runtime_error("lightgrid: bad lump size");
    entries_.resize(hashLump.size() / 8);
    for (size_t i = 0; i < entries_.size(); ++i) {
        const uint8_t* p = hashLump.data() + i * 8;
        LightGridEntry& e = entries_[i];
        std::memcpy(&e.key, p, 4);
        e.sub = p[4];
        e.unknown = p[5];
        std::memcpy(&e.coeffIndex, p + 6, 2);
    }
    coeffs_.resize(coeffLump.size() / 24);
    for (size_t i = 0; i < coeffs_.size(); ++i) std::memcpy(coeffs_[i].data(), coeffLump.data() + i * 24, 24);
    for (const auto& e : entries_)
        if (e.coeffIndex >= coeffs_.size()) throw std::runtime_error("lightgrid: coefficient index out of range");
}

const LightGridEntry* LightGrid::find(int x, int y, int z) const {
    const uint32_t key = ((uint32_t(x & ~3) << 11 | uint32_t(y & ~3)) << 8) | uint32_t(z >> 2);
    const uint8_t sub = uint8_t((((x & 3) << 2 | (y & 3)) << 2 | (z & 3)) << 2);
    // Entries are sorted by (key, sub with bit 0 ignored).
    auto it = std::lower_bound(entries_.begin(), entries_.end(), std::pair{key, sub}, [](const LightGridEntry& e, const std::pair<uint32_t, uint8_t>& k) {
        return e.key != k.first ? e.key < k.first : (e.sub & ~1) < k.second;
    });
    if (it == entries_.end() || it->key != key || (it->sub & ~1) != sub) return nullptr;
    return &*it;
}

bool LightGrid::sample(const std::array<float, 3>& pos, LightGridCoeffs& out, float& sunVisibility) const {
    for (auto& ch : out) ch.fill(0);
    sunVisibility = 0;
    const float fx = (pos[0] - kOrigin) / kCellXY, fy = (pos[1] - kOrigin) / kCellXY, fz = (pos[2] - kOrigin) / kCellZ;
    const int x = int(std::floor(fx)), y = int(std::floor(fy)), z = int(std::floor(fz));
    const float tx = fx - float(x), ty = fy - float(y), tz = fz - float(z);
    float total = 0;
    for (int corner = 0; corner < 8; ++corner) {
        const LightGridEntry* e = find(x + (corner & 1), y + ((corner >> 1) & 1), z + ((corner >> 2) & 1));
        if (!e) continue;
        const float w = ((corner & 1) ? tx : 1 - tx) * (((corner >> 1) & 1) ? ty : 1 - ty) * (((corner >> 2) & 1) ? tz : 1 - tz);
        total += w;
        const auto& c = coeffs_[e->coeffIndex];
        for (int ch = 0; ch < 3; ++ch)
            for (int k = 0; k < 8; ++k) out[size_t(ch)][size_t(k)] += c[size_t(ch * 8 + k)] / 255.0f * w;
        if (e->sub & 1) sunVisibility += w;
    }
    if (total <= 0) return false;
    if (total < 0.98f) {
        for (auto& ch : out)
            for (auto& v : ch) v /= total;
        sunVisibility /= total;
    }
    return true;
}

std::array<float, 8> LightGrid::weights(const std::array<float, 3>& n) {
    // Project onto three axes tilted 54.7 degrees from vertical, spaced 120 degrees apart.
    const float t0 = n[0] * 0.81649655f + n[2] * 0.57735026f;
    const float t1 = n[0] * -0.40824828f + n[1] * 0.70710677f + n[2] * 0.57735026f;
    const float t2 = n[0] * -0.40824828f - n[1] * 0.70710677f + n[2] * 0.57735026f;
    const float m = std::max({std::abs(t0), std::abs(t1), std::abs(t2), 1e-6f});
    const float u = smooth((t0 / m + 1) * 0.5f), v = smooth((t1 / m + 1) * 0.5f);
    std::array<float, 8> w{};
    for (int half = 0; half < 2; ++half) {
        const float s = smooth(half == 0 ? 0.5f - t2 / m * 0.5f : (t2 / m + 1) * 0.5f);
        w[size_t(half * 4 + 0)] = (1 - u) * (1 - v) * s;
        w[size_t(half * 4 + 1)] = (1 - v) * s * u;
        w[size_t(half * 4 + 2)] = (1 - u) * s * v;
        w[size_t(half * 4 + 3)] = s * v * u;
    }
    return w;
}

std::array<float, 3> LightGrid::evaluate(const LightGridCoeffs& c, const std::array<float, 3>& normal) {
    const auto w = weights(normal);
    std::array<float, 3> out{};
    for (int ch = 0; ch < 3; ++ch)
        for (int k = 0; k < 8; ++k) out[size_t(ch)] += w[size_t(k)] * c[size_t(ch)][size_t(k)];
    return out;
}

} // namespace iw2::assets
