// Game folder validation and app settings/log helpers (src/fs/gamedir.h), on a temp folder of empty files.
#include <unistd.h>
#include "fs/gamedir.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n";    \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

namespace stdfs = std::filesystem;

static std::string nn(int i) { char b[8]; std::snprintf(b, sizeof b, "%02d", i); return b; }
static void touch(const stdfs::path& p) { std::ofstream(p).put('\0'); }
static bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

static stdfs::path fullDir(const stdfs::path& root, const char* name) {
    const stdfs::path d = root / name;
    stdfs::create_directories(d);
    for (int i = 0; i <= 15; ++i) touch(d / ("iw_" + nn(i) + ".iwd"));
    for (int i = 0; i <= 10; ++i) touch(d / ("localized_english_iw" + nn(i) + ".iwd"));
    return d;
}

int main() {
    using namespace iw2::fs;
    const stdfs::path root = stdfs::temp_directory_path() / ("iw2_test_gamedir_" + std::to_string(::getpid()));
    stdfs::remove_all(root);

    { // complete set
        const auto r = checkGameDir(fullDir(root, "ok"));
        CHECK(r.ok());
        CHECK(r.missing.empty());
        CHECK(r.language == "english");
    }
    { // a missing pack
        const auto d = fullDir(root, "missing");
        stdfs::remove(d / "iw_07.iwd");
        const auto r = checkGameDir(d);
        CHECK(!r.ok());
        CHECK(r.missing.size() == 1 && r.missing[0] == "iw_07.iwd");
    }
    { // wrong case is accepted
        const auto d = fullDir(root, "case");
        stdfs::rename(d / "iw_03.iwd", d / "IW_03.IWD");
        stdfs::rename(d / "localized_english_iw04.iwd", d / "Localized_English_IW04.iwd");
        const auto r = checkGameDir(d);
        CHECK(r.ok());
        CHECK(r.language == "english");
    }
    { // no localized set at all
        const auto d = root / "nolocal";
        stdfs::create_directories(d);
        for (int i = 0; i <= 15; ++i) touch(d / ("iw_" + nn(i) + ".iwd"));
        const auto r = checkGameDir(d);
        CHECK(!r.ok());
        CHECK(r.missing.size() == 11);
        CHECK(has(r.missing, "localized_<language>_iw00.iwd"));
    }
    { // an incomplete localized set names its gaps
        const auto d = fullDir(root, "partial");
        stdfs::remove(d / "localized_english_iw10.iwd");
        const auto r = checkGameDir(d);
        CHECK(!r.ok());
        CHECK(r.missing.size() == 1 && r.missing[0] == "localized_english_iw10.iwd");
    }
    { // a second, complete language is chosen when the first is incomplete
        const auto d = fullDir(root, "twolang");
        stdfs::remove(d / "localized_english_iw02.iwd");
        for (int i = 0; i <= 10; ++i) touch(d / ("localized_german_iw" + nn(i) + ".iwd"));
        const auto r = checkGameDir(d);
        CHECK(r.ok());
        CHECK(r.language == "german");
    }
    { // a folder that does not exist
        const auto r = checkGameDir(root / "nope");
        CHECK(!r.ok());
        CHECK(r.missing.size() == 16 + 11);
    }
    { // settings: remembered path survives other lines
        const stdfs::path s = root / "pref" / "settings.cfg";
        CHECK(!readMainPath(s));
        CHECK(writeMainPath(s, "/some dir/main"));
        CHECK(readMainPath(s) && *readMainPath(s) == "/some dir/main");
        { std::ofstream(s, std::ios::app) << "other=1\n"; }
        CHECK(writeMainPath(s, "/b/main"));
        CHECK(readMainPath(s) && *readMainPath(s) == "/b/main");
        std::ifstream f(s);
        const std::string text((std::istreambuf_iterator<char>(f)), {});
        CHECK(text.find("other=1") != std::string::npos);
    }
    { // log rotation keeps one previous log
        const stdfs::path log = root / "logs" / "openiw2.log";
        CHECK(rotateLog(log));
        CHECK(!stdfs::exists(log));
        std::ofstream(log) << "first";
        CHECK(rotateLog(log));
        std::ofstream(log) << "second";
        CHECK(rotateLog(log));
        std::ifstream f(root / "logs" / "openiw2.log.1");
        std::string t;
        std::getline(f, t);
        CHECK(t == "second");
        CHECK(!stdfs::exists(log));
    }

    stdfs::remove_all(root);
    if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
    std::cout << "gamedir: ok\n";
    return 0;
}
