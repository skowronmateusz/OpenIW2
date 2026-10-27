// RoQ video decoder (the format of video/*.roq, e.g. cod_intro.roq; docs/spec/video.md).
// Chunks: 8-byte header (id u16, size u32, argument u16). File header 0x1084, 0xFFFFFFFF, fps u16.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace iw2::video {

class RoqDecoder {
public:
    // Parses the file header. Throws std::runtime_error when the data is not a RoQ file.
    explicit RoqDecoder(std::vector<uint8_t> data);

    int width() const { return width_; }
    int height() const { return height_; }
    int fps() const { return fps_; }
    // Audio is 22050 Hz, 1 (0x1020) or 2 (0x1021) channels (the channel count of the latest sound chunk).
    static constexpr int kAudioRate = 22050;
    int audioChannels() const { return channels_; }

    // Decodes chunks up to and including the next video frame (a 0x1011 chunk). Sound chunks met on the way are appended
    // to takeAudio(). Returns false at the end of the data (no new frame).
    bool nextFrame();
    // The latest frame as width*height RGBA bytes.
    const std::vector<uint8_t>& rgba() const { return rgba_; }
    // Interleaved 16-bit samples decoded since the last call.
    std::vector<int16_t> takeAudio() { return std::move(audio_); }
    int frameIndex() const { return frames_; }

private:
    void codebook(const uint8_t* p, uint32_t size, uint16_t arg);
    void vq(const uint8_t* p, uint32_t size, uint16_t arg);
    void sound(const uint8_t* p, uint32_t size, uint16_t arg, int channels);
    void decodeBlock(int x, int y, int size, int level);
    void put2x2(int x, int y, int cell, int scale);
    void put4x4(int x, int y, int vec, int scale);
    void copyBlock(int x, int y, int size, int sx, int sy);

    std::vector<uint8_t> data_;
    size_t pos_ = 8;
    std::vector<int16_t> audio_;
    int width_ = 0, height_ = 0, fps_ = 30, channels_ = 2, frames_ = 0;
    // Planar frames: RGB (3 bytes per pixel) of the current and the previous picture.
    std::vector<uint8_t> cur_, prev_, rgba_;
    // 2x2 cells as 4 RGB pixels (12 bytes); 4x4 vectors as 4 cell indices.
    uint8_t cells_[256][12] = {};
    uint8_t vectors_[256][4] = {};
    // State of the VQ stream.
    const uint8_t* vqp_ = nullptr;
    const uint8_t* vqEnd_ = nullptr;
    uint32_t vqFlags_ = 0;
    int vqFlagPos_ = -1;
    int motionX_ = 0, motionY_ = 0;
    bool truncated_ = false;
    int next2();
    int nextByte();
};

} // namespace iw2::video
