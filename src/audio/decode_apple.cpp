// MP3 decoding through the macOS AudioToolbox framework (AudioFile + ExtAudioFile), reading from memory.

#include "audio/decode.h"

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace iw2::audio {
namespace {

struct Source {
    std::span<const uint8_t> data;
};

OSStatus readProc(void* user, SInt64 pos, UInt32 count, void* buffer, UInt32* actual) {
    const auto& s = *static_cast<Source*>(user);
    if (pos < 0 || uint64_t(pos) > s.data.size()) {
        *actual = 0;
        return kAudioFileInvalidPacketOffsetError;
    }
    const size_t n = std::min<size_t>(count, s.data.size() - size_t(pos));
    std::memcpy(buffer, s.data.data() + pos, n);
    *actual = UInt32(n);
    return noErr;
}

SInt64 sizeProc(void* user) { return SInt64(static_cast<Source*>(user)->data.size()); }

std::runtime_error fail(const char* what, OSStatus st) {
    return std::runtime_error(std::string("mp3: ") + what + " failed (OSStatus " + std::to_string(int(st)) + ")");
}

} // namespace

Wav decodeMp3(std::span<const uint8_t> data) {
    Source src{data};
    AudioFileID file = nullptr;
    OSStatus st = AudioFileOpenWithCallbacks(&src, readProc, nullptr, sizeProc, nullptr, kAudioFileMP3Type, &file);
    if (st != noErr) throw fail("AudioFileOpenWithCallbacks", st);

    ExtAudioFileRef ext = nullptr;
    st = ExtAudioFileWrapAudioFileID(file, false, &ext);
    if (st != noErr) {
        AudioFileClose(file);
        throw fail("ExtAudioFileWrapAudioFileID", st);
    }
    struct Closer {
        AudioFileID file;
        ExtAudioFileRef ext;
        ~Closer() {
            ExtAudioFileDispose(ext);
            AudioFileClose(file);
        }
    } closer{file, ext};

    AudioStreamBasicDescription in{};
    UInt32 size = sizeof(in);
    st = ExtAudioFileGetProperty(ext, kExtAudioFileProperty_FileDataFormat, &size, &in);
    if (st != noErr) throw fail("reading the file format", st);
    if (in.mChannelsPerFrame < 1 || in.mChannelsPerFrame > 2 || in.mSampleRate <= 0)
        throw std::runtime_error("mp3: unsupported channel count or rate");

    Wav w;
    w.rate = int(in.mSampleRate);
    w.channels = int(in.mChannelsPerFrame);

    AudioStreamBasicDescription out{};
    out.mSampleRate = in.mSampleRate;
    out.mFormatID = kAudioFormatLinearPCM;
    out.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    out.mChannelsPerFrame = UInt32(w.channels);
    out.mBitsPerChannel = 16;
    out.mFramesPerPacket = 1;
    out.mBytesPerFrame = 2 * UInt32(w.channels);
    out.mBytesPerPacket = out.mBytesPerFrame;
    st = ExtAudioFileSetProperty(ext, kExtAudioFileProperty_ClientDataFormat, sizeof(out), &out);
    if (st != noErr) throw fail("setting the PCM output format", st);

    SInt64 total = 0;
    size = sizeof(total);
    if (ExtAudioFileGetProperty(ext, kExtAudioFileProperty_FileLengthFrames, &size, &total) == noErr && total > 0)
        w.samples.reserve(size_t(total) * size_t(w.channels));

    constexpr UInt32 chunkFrames = 4096;
    std::vector<int16_t> chunk(chunkFrames * size_t(w.channels));
    for (;;) {
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = UInt32(w.channels);
        list.mBuffers[0].mDataByteSize = UInt32(chunk.size() * sizeof(int16_t));
        list.mBuffers[0].mData = chunk.data();
        UInt32 frames = chunkFrames;
        st = ExtAudioFileRead(ext, &frames, &list);
        if (st != noErr) throw fail("ExtAudioFileRead", st);
        if (frames == 0) break;
        w.samples.insert(w.samples.end(), chunk.begin(), chunk.begin() + ptrdiff_t(frames) * w.channels);
    }
    return w;
}

} // namespace iw2::audio
