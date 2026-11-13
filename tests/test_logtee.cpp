// The session log's capture (docs/logging.md "Capture") with the dedicated console window's StdoutTee
// (src/netgame/consoleview.h) nested inside it, as iw2view runs a dedicated server from the menu: a line printed while
// the tee runs reaches the console view, the log file and the original descriptor; stopping the capture while the tee
// still runs returns. Everything is written below a temp folder.
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "netgame/consoleview.h"
#include "util/log.h"

namespace stdfs = std::filesystem;

static int failures = 0;
static int reportFd = 2;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            dprintf(reportFd, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

static std::string readFile(const stdfs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

int main() {
    reportFd = dup(2);
    const stdfs::path root = stdfs::temp_directory_path() / ("iw2_test_logtee_" + std::to_string(getpid()));
    stdfs::remove_all(root);
    stdfs::create_directories(root);
    const stdfs::path orig = root / "orig.txt";
    const int savedOut = dup(1), savedErr = dup(2);
    std::fflush(stdout);
    const int o = ::open(orig.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(o, 1);
    dup2(o, 2);
    ::close(o);

    iw2::log::CaptureOptions opt;
    opt.dir = root / "logs";
    opt.prefix = "openiw2";
    opt.program = "openiw2";
    CHECK(iw2::log::startCapture(opt));
    const stdfs::path log = iw2::log::logPath();
    std::printf("before the tee\n");
    iw2::netgame::StdoutTee tee;
    CHECK(tee.start());
    std::printf("through the tee\n");
    std::fflush(stdout);
    std::string seen;
    for (int i = 0; i < 200 && seen.find("through the tee") == std::string::npos; ++i) {
        seen += tee.take();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(seen.find("through the tee\n") != std::string::npos && seen.find("before the tee") == std::string::npos);
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the tee's thread passes the line on into the capture
    iw2::log::drain();
    // The capture stops (as at exit) while the tee still runs: it returns.
    const auto t0 = std::chrono::steady_clock::now();
    iw2::log::stopCapture();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
    tee.stop();
    std::fflush(stdout);
    dup2(savedOut, 1);
    dup2(savedErr, 2);

    const std::string text = readFile(log), out = readFile(orig);
    CHECK(text.find("] before the tee\n") != std::string::npos && text.find("] through the tee\n") != std::string::npos);
    CHECK(out.find("before the tee\nthrough the tee\n") != std::string::npos);
    stdfs::remove_all(root);
    if (failures) {
        dprintf(reportFd, "%d logtee check(s) failed\n", failures);
        return 1;
    }
    dprintf(reportFd, "logtee tests passed\n");
    return 0;
}
