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

std::vector<std::array<float, 3>> LightGrid::points() const {
    std::vector<std::array<float, 3>> out;
    out.reserve(entries_.size());
    for (const LightGridEntry& e : entries_) { // key and sub as find() builds them
        // (x & ~3) << 19 fills bits 21..31, (y & ~3) << 8 bits 10..20, z >> 2 bits 0..9 (indices below 8192 / 8192 / 4096)
        const int x = int(e.key >> 21) << 2 | ((e.sub >> 6) & 3), y = int((e.key >> 10) & 0x7ff) << 2 | ((e.sub >> 4) & 3);
        const int z = int(e.key & 0x3ff) << 2 | ((e.sub >> 2) & 3);
        out.push_back({kOrigin + float(x) * kCellXY, kOrigin + float(y) * kCellXY, kOrigin + float(z) * kCellZ});
    }
    return out;
}

std::vector<std::array<float, 24>> LightGrid::entryCoefficients() const {
    std::vector<std::array<float, 24>> out(entries_.size());
    for (size_t i = 0; i < entries_.size(); ++i)
        for (size_t k = 0; k < 24; ++k) out[i][k] = float(coeffs_[entries_[i].coeffIndex][k]) / 255.0f;
    return out;
}

bool LightGrid::setEntryCoefficients(std::vector<std::array<float, 24>> c) {
    if (c.size() != entries_.size()) return false;
    override_ = std::move(c);
    return true;
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
        if (!override_.empty()) {
            const auto& c = override_[size_t(e - entries_.data())];
            for (int ch = 0; ch < 3; ++ch)
                for (int k = 0; k < 8; ++k) out[size_t(ch)][size_t(k)] += c[size_t(ch * 8 + k)] * w;
        } else {
            const auto& c = coeffs_[e->coeffIndex];
            for (int ch = 0; ch < 3; ++ch)
                for (int k = 0; k < 8; ++k) out[size_t(ch)][size_t(k)] += c[size_t(ch * 8 + k)] / 255.0f * w;
        }
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

std::array<float, 8> LightGrid::smodelWeights(const std::array<float, 3>& n) {
    const float t0 = n[0] * 0.81649655f + n[2] * 0.57735026f;
    const float t1 = n[0] * -0.40824828f + n[1] * 0.70710677f + n[2] * 0.57735026f;
    const float t2 = n[0] * -0.40824828f - n[1] * 0.70710677f + n[2] * 0.57735026f;
    const float m = std::max({std::abs(t0), std::abs(t1), std::abs(t2), 1e-6f});
    const float wx = (t0 / m + 1) * 0.5f, wy = (t1 / m + 1) * 0.5f, wz = (t2 / m + 1) * 0.5f;
    std::array<float, 8> w{};
    for (int k = 0; k < 8; ++k)
        w[size_t(k)] = ((k & 1) ? wx : 1 - wx) * ((k & 2) ? wy : 1 - wy) * ((k & 4) ? wz : 1 - wz);
    return w;
}

void LightGrid::smodelLighting(LightGridCoeffs& c, float& sun, float direct, float indirect) {
    auto byte = [](float v) { return float(std::clamp(int(v * 255.0f + 9.313225746154785e-10f), 0, 255)) / 255.0f; }; // 0x10029b50
    for (auto& ch : c)
        for (float& v : ch) v = byte(v * indirect);
    sun = byte(sun * direct);
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
