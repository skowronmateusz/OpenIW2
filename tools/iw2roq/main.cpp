// iw2roq: inspect and decode a RoQ movie of the game files.
//
//   iw2roq <main-dir> <movie> info                      size, fps, frame and audio counts
//   iw2roq <main-dir> <movie> frames <out-dir> <n>...   writes frame_<n>.png for the listed frame numbers

#include "fs/vfs.h"
#include "util/png.h"
#include "video/roq.h"

#include <filesystem>
#include <iostream>
#include <set>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: iw2roq <main-dir> <movie> info|frames <out-dir> <n>...\n";
        return 2;
    }
    try {
        iw2::fs::Vfs vfs;
        vfs.mountDirectory(argv[1]);
        auto data = vfs.read(std::string("video/") + argv[2] + ".roq");
        if (!data) { std::cerr << "no such movie\n"; return 1; }
        iw2::video::RoqDecoder dec(std::move(*data));
        const std::string cmd = argv[3];
        std::set<int> want;
        std::filesystem::path out;
        if (cmd == "frames" && argc > 5) {
            out = argv[4];
            std::filesystem::create_directories(out);
            for (int i = 5; i < argc; ++i) want.insert(std::atoi(argv[i]));
        }
        size_t samples = 0;
        int frames = 0;
        while (dec.nextFrame()) {
            ++frames;
            samples += dec.takeAudio().size();
            if (want.count(frames - 1))
                iw2::util::writePngRgba(out / ("frame_" + std::to_string(frames - 1) + ".png"), dec.rgba().data(), dec.width(), dec.height());
            if (!want.empty() && frames > *want.rbegin()) break;
        }
        std::cout << dec.width() << "x" << dec.height() << " " << dec.fps() << " fps, " << frames << " frames decoded, " << samples
                  << " audio samples (" << dec.audioChannels() << " ch)\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
