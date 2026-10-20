// iw2fs: inspect the virtual filesystem of an installed game directory.
//
//   iw2fs <main-dir> packs                 list packs in search order
//   iw2fs <main-dir> list [prefix]         list visible files
//   iw2fs <main-dir> stats                 file counts and sizes per top-level folder / extension
//   iw2fs <main-dir> which <path>          show which pack serves a file
//   iw2fs <main-dir> cat <path>            write a file to stdout
//
// Options: --lang <language> keeps only localized packs for that language.

#include "fs/vfs.h"

#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> args;
    std::string language;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--lang" && i + 1 < argc) language = argv[++i];
        else args.push_back(a);
    }
    if (args.size() < 2) {
        std::cerr << "usage: iw2fs <main-dir> packs|list [prefix]|stats|which <path>|cat <path> [--lang <language>]\n";
        return 2;
    }

    try {
        iw2::fs::Vfs vfs;
        vfs.mountDirectory(args[0], language);
        const std::string& cmd = args[1];

        if (cmd == "packs") {
            for (const auto& p : vfs.packs())
                std::cout << p.path.filename().string() << "\t" << p.fileCount << " files"
                          << (p.localized ? "\tlocalized:" + p.language : "") << "\n";
        } else if (cmd == "list") {
            for (const auto& name : vfs.list(args.size() > 2 ? args[2] : "")) std::cout << name << "\n";
        } else if (cmd == "stats") {
            std::map<std::string, size_t> counts;
            for (const auto& name : vfs.list()) {
                const auto slash = name.find('/');
                const std::string top = slash == std::string::npos ? "(root)" : name.substr(0, slash);
                const auto dot = name.rfind('.');
                const std::string ext = (dot == std::string::npos || dot < name.rfind('/') + 1) ? "(none)" : name.substr(dot);
                ++counts[top + "\t" + ext];
            }
            for (const auto& [k, v] : counts) std::cout << v << "\t" << k << "\n";
        } else if (cmd == "which" && args.size() > 2) {
            const std::string src = vfs.sourceOf(args[2]);
            if (src.empty()) { std::cerr << "not found: " << args[2] << "\n"; return 1; }
            std::cout << src << "\n";
        } else if (cmd == "cat" && args.size() > 2) {
            auto data = vfs.read(args[2]);
            if (!data) { std::cerr << "not found: " << args[2] << "\n"; return 1; }
            std::fwrite(data->data(), 1, data->size(), stdout);
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
