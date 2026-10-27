#include "video/roq.h"

#include <algorithm>
#include <cstring>

namespace iw2::video {

namespace {
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint8_t clamp8(float v) { return uint8_t(std::clamp(int(v + 0.5f), 0, 255)); }
} // namespace

RoqDecoder::RoqDecoder(std::vector<uint8_t> data) : data_(std::move(data)) {
    if (data_.size() < 8 || rd16(data_.data()) != 0x1084 || rd32(data_.data() + 2) != 0xffffffffu)
        throw std::runtime_error("not a RoQ file");
    fps_ = std::max<int>(rd16(data_.data() + 6), 1);
    pos_ = 8;
}

int RoqDecoder::next2() {
    if (vqFlagPos_ < 0) {
        if (vqp_ + 2 > vqEnd_) { truncated_ = true; return 0; }
        vqFlags_ = rd16(vqp_);
        vqp_ += 2;
        vqFlagPos_ = 7;
    }
    const int v = int(vqFlags_ >> (vqFlagPos_ * 2)) & 3; // the two most significant bits first
    --vqFlagPos_;
    return v;
}

int RoqDecoder::nextByte() {
    if (vqp_ >= vqEnd_) { truncated_ = true; return 0; }
    return *vqp_++;
}

// A codebook chunk: the argument's high byte is the number of 2x2 cells (0 = 256), the low byte the number of 4x4 vectors
// (0 = 256 when bytes remain). A cell is 4 luma values, then U and V (full range BT.601).
void RoqDecoder::codebook(const uint8_t* p, uint32_t size, uint16_t arg) {
    int n1 = arg >> 8, n2 = arg & 0xff;
    if (n1 == 0) n1 = 256;
    if (n2 == 0 && size > uint32_t(n1) * 6) n2 = 256;
    if (uint32_t(n1) * 6 + uint32_t(n2) * 4 > size) throw std::runtime_error("RoQ: codebook larger than its chunk");
    for (int i = 0; i < n1; ++i, p += 6) {
        const float u = float(p[4]) - 128.0f, v = float(p[5]) - 128.0f;
        for (int k = 0; k < 4; ++k) {
            const float y = float(p[k]);
            uint8_t* o = cells_[i] + k * 3;
            o[0] = clamp8(y + 1.402f * v);
            o[1] = clamp8(y - 0.34414f * u - 0.71414f * v);
            o[2] = clamp8(y + 1.772f * u);
        }
    }
    for (int i = 0; i < n2; ++i, p += 4) std::memcpy(vectors_[i], p, 4);
}

// Paints a 2x2 cell at (x, y), each pixel `scale` x `scale` pixels large.
void RoqDecoder::put2x2(int x, int y, int cell, int scale) {
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const uint8_t* c = cells_[cell] + (j * 2 + i) * 3;
            for (int dy = 0; dy < scale; ++dy)
                for (int dx = 0; dx < scale; ++dx) {
                    const int px = x + i * scale + dx, py = y + j * scale + dy;
                    if (px < width_ && py < height_) std::memcpy(&cur_[(size_t(py) * size_t(width_) + size_t(px)) * 3], c, 3);
                }
        }
}

// Paints a 4x4 vector (four cells) at (x, y) magnified by `scale` (1: 4x4 pixels, 2: 8x8 pixels).
void RoqDecoder::put4x4(int x, int y, int vec, int scale) {
    for (int k = 0; k < 4; ++k) put2x2(x + (k & 1) * 2 * scale, y + (k >> 1) * 2 * scale, vectors_[vec][k], scale);
}

// Motion compensation: copies size x size pixels of the previous picture at (sx, sy).
void RoqDecoder::copyBlock(int x, int y, int size, int sx, int sy) {
    for (int j = 0; j < size; ++j) {
        const int py = y + j, qy = sy + j;
        if (py >= height_) break;
        for (int i = 0; i < size; ++i) {
            const int px = x + i, qx = sx + i;
            if (px >= width_) break;
            if (qx < 0 || qy < 0 || qx >= width_ || qy >= height_) continue;
            std::memcpy(&cur_[(size_t(py) * size_t(width_) + size_t(px)) * 3], &prev_[(size_t(qy) * size_t(width_) + size_t(qx)) * 3], 3);
        }
    }
}

// A block of 8, 4 or 2 pixels. Codes: 0 unchanged, 1 motion (a byte: the offset), 2 vector (a byte: the codebook entry),
// 3 split into four sub-blocks. Level 8: vector = a 4x4 vector magnified x2, split = four 4x4 blocks; level 4: vector =
// a 4x4 vector, split = four 2x2 cells with a byte each.
void RoqDecoder::decodeBlock(int x, int y, int size, int level) {
    (void)level;
    if (truncated_) return;
    const int code = next2();
    switch (code) {
    case 0: break;
    case 1: {
        const int mv = nextByte();
        copyBlock(x, y, size, x + 8 - (mv >> 4) - motionX_, y + 8 - (mv & 15) - motionY_);
        break;
    }
    case 2: {
        const int idx = nextByte();
        put4x4(x, y, idx, size / 4);
        break;
    }
    default:
        if (size == 8) {
            for (int k = 0; k < 4; ++k) decodeBlock(x + (k & 1) * 4, y + (k >> 1) * 4, 4, 4);
        } else {
            for (int k = 0; k < 4; ++k) put2x2(x + (k & 1) * 2, y + (k >> 1) * 2, nextByte(), 1);
        }
        break;
    }
}

void RoqDecoder::vq(const uint8_t* p, uint32_t size, uint16_t arg) {
    vqp_ = p;
    vqEnd_ = p + size;
    vqFlagPos_ = -1;
    truncated_ = false;
    motionX_ = int8_t(arg >> 8);
    motionY_ = int8_t(arg & 0xff);
    cur_ = prev_; // unchanged blocks keep the previous picture
    for (int my = 0; my < height_; my += 16)
        for (int mx = 0; mx < width_; mx += 16)
            for (int k = 0; k < 4; ++k) decodeBlock(mx + (k & 1) * 8, my + (k >> 1) * 8, 8, 8);
}

// DPCM: every byte is a signed square step of the channel's predictor (0x00..0x7f add b*b, 0x80..0xff subtract (b-128)^2).
void RoqDecoder::sound(const uint8_t* p, uint32_t size, uint16_t arg, int channels) {
    channels_ = channels;
    int pred[2] = {};
    if (channels == 1) pred[0] = int16_t(arg);
    else pred[0] = int16_t(arg & 0xff00), pred[1] = int16_t((arg & 0xff) << 8);
    for (uint32_t i = 0; i < size; ++i) {
        const int ch = channels == 2 ? int(i & 1) : 0;
        const int b = p[i];
        const int sq = (b & 0x7f) * (b & 0x7f);
        pred[ch] = std::clamp(pred[ch] + (b & 0x80 ? -sq : sq), -32768, 32767);
        audio_.push_back(int16_t(pred[ch]));
    }
}

bool RoqDecoder::nextFrame() {
    while (pos_ + 8 <= data_.size()) {
        const uint8_t* h = data_.data() + pos_;
        const uint16_t id = rd16(h), arg = rd16(h + 6);
        const uint32_t size = rd32(h + 2);
        if (pos_ + 8 + size > data_.size()) return false;
        const uint8_t* p = h + 8;
        pos_ += 8 + size;
        switch (id) {
        case 0x1001: // info: width, height, 8, 4
            if (size >= 4) {
                const int w = rd16(p), hgt = rd16(p + 2);
                if (w <= 0 || hgt <= 0) throw std::runtime_error("RoQ: bad size");
                if (w != width_ || hgt != height_) {
                    width_ = w, height_ = hgt;
                    cur_.assign(size_t(w) * size_t(hgt) * 3, 0);
                    prev_ = cur_;
                    rgba_.assign(size_t(w) * size_t(hgt) * 4, 255);
                }
            }
            break;
        case 0x1002: codebook(p, size, arg); break;
        case 0x1011:
            if (width_ == 0) throw std::runtime_error("RoQ: video before the info chunk");
            vq(p, size, arg);
            for (size_t i = 0, n = size_t(width_) * size_t(height_); i < n; ++i) {
                rgba_[i * 4] = cur_[i * 3], rgba_[i * 4 + 1] = cur_[i * 3 + 1], rgba_[i * 4 + 2] = cur_[i * 3 + 2];
            }
            prev_ = cur_;
            ++frames_;
            return true;
        case 0x1020: sound(p, size, arg, 1); break;
        case 0x1021: sound(p, size, arg, 2); break;
        default: break; // 0x1030 signature and unknown chunks
        }
    }
    return false;
}

} // namespace iw2::video
