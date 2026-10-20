// iw2asset: decode game assets into open formats.
//
//   iw2asset <main-dir> iwi-info <images/name.iwi>
//   iw2asset <main-dir> iwi-png  <images/name.iwi> <out.png> [--face N]
//   iw2asset <main-dir> iwi-check                    decode every .iwi and report per-format results
//   iw2asset <main-dir> iwi-export <out-dir>         convert every .iwi (mip 0) to PNG

#include "assets/iwi.h"
#include "fs/vfs.h"
#include "util/png.h"

#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace iw2;

namespace {

std::vector<uint8_t> mustRead(const fs::Vfs& vfs, const std::string& path) {
    auto data = vfs.read(path);
    if (!data) throw std::runtime_error("not found: " + path);
    return std::move(*data);
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    int face = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--face" && i + 1 < argc) face = std::stoi(argv[++i]);
        else args.push_back(a);
    }
    if (args.size() < 2) {
        std::cerr << "usage: iw2asset <main-dir> iwi-info <path> | iwi-png <path> <out.png> [--face N] | iwi-check | iwi-export <out-dir>\n";
        return 2;
    }

    try {
        fs::Vfs vfs;
        vfs.mountDirectory(args[0]);
        const std::string& cmd = args[1];

        if (cmd == "iwi-info" && args.size() > 2) {
            const auto data = mustRead(vfs, args[2]);
            const auto img = assets::parseIwi(data);
            const auto& h = img.header;
            std::cout << args[2] << ": " << assets::formatName(h.format) << " " << h.width << "x" << h.height
                      << " flags=0x" << std::hex << int(h.flags) << std::dec << " faces=" << img.faceCount
                      << " mips=" << img.levels.size() << "\n";
        } else if (cmd == "iwi-png" && args.size() > 3) {
            const auto data = mustRead(vfs, args[2]);
            const auto img = assets::parseIwi(data);
            const auto rgba = assets::decodeToRgba(img, 0, face);
            util::writePngRgba(args[3], rgba.data(), img.levels[0].width, img.levels[0].height);
        } else if (cmd == "iwi-check" || (cmd == "iwi-export" && args.size() > 2)) {
            const bool exportPng = cmd == "iwi-export";
            if (exportPng) std::filesystem::create_directories(args[2]);
            std::map<std::string, std::pair<int, int>> stats; // format -> (ok, failed)
            int ok = 0, failed = 0;
            for (const auto& name : vfs.list("images/")) {
                if (name.size() < 4 || name.substr(name.size() - 4) != ".iwi") continue;
                std::string fmt = "?";
                try {
                    const auto data = mustRead(vfs, name);
                    const auto img = assets::parseIwi(data);
                    fmt = assets::formatName(img.header.format);
                    for (size_t l = 0; l < img.levels.size(); ++l)
                        for (int f = 0; f < img.faceCount; ++f) {
                            const auto rgba = assets::decodeToRgba(img, int(l), f);
                            if (exportPng && l == 0) {
                                std::string out = name.substr(7, name.size() - 11);
                                if (img.faceCount > 1) out += "_face" + std::to_string(f);
                                util::writePngRgba(std::filesystem::path(args[2]) / (out + ".png"), rgba.data(),
                                                   img.levels[0].width, img.levels[0].height);
                            }
                        }
                    ++stats[fmt].first;
                    ++ok;
                } catch (const std::exception& e) {
                    ++stats[fmt].second;
                    ++failed;
                    std::cerr << name << ": " << e.what() << "\n";
                }
            }
            for (const auto& [fmt, s] : stats) std::cout << fmt << ": " << s.first << " ok, " << s.second << " failed\n";
            std::cout << "total: " << ok << " ok, " << failed << " failed\n";
            return failed ? 1 : 0;
        } else {
            std::cerr << "unknown command: " << cmd << "\n";
            return 2;
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
