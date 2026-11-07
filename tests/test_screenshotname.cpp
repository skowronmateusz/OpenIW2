// Screenshot file naming proven from the renderer DLL (src/util/screenshot.h, docs/spec/controls.md).
#include <unistd.h>
#include "util/screenshot.h"

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

int main() {
    namespace fs = std::filesystem;
    using iw2::util::screenshotPath;
    const fs::path dir = fs::temp_directory_path() / ("iw2_test_shotname_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);

    // An empty folder starts at 0000.
    CHECK(screenshotPath(dir, "jpg").filename() == "shot0000.jpg");
    CHECK(screenshotPath(dir, "png").filename() == "shot0000.png"); // each extension numbers on its own

    // Existing files are skipped: the first free number wins.
    std::ofstream(dir / "shot0000.jpg") << 'x';
    CHECK(screenshotPath(dir, "jpg").filename() == "shot0001.jpg");
    CHECK(screenshotPath(dir, "png").filename() == "shot0000.png");
    std::ofstream(dir / "shot0002.jpg") << 'x'; // a gap is filled, not jumped over
    CHECK(screenshotPath(dir, "jpg").filename() == "shot0001.jpg");

    // A file of another extension does not block the number.
    std::ofstream(dir / "shot0000.png") << 'x';
    CHECK(screenshotPath(dir, "jpg").filename() == "shot0001.jpg");

    // All numbers 0000..9999 taken: the last one is reused (overwrites), as the original does.
    fs::path full = fs::temp_directory_path() / ("iw2_test_shotfull_" + std::to_string(::getpid()));
    fs::remove_all(full);
    fs::create_directories(full);
    for (int i = 0; i < 10000; ++i) std::ofstream(full / ("shot" + std::to_string(10000 + i).substr(1) + ".jpg")) << 'x';
    CHECK(screenshotPath(full, "jpg").filename() == "shot9999.jpg");
    fs::remove_all(full);

    // The folder need not exist yet (the caller creates it).
    CHECK(screenshotPath(dir / "screenshots", "jpg") == dir / "screenshots" / "shot0000.jpg");

    fs::remove_all(dir);
    return failures == 0 ? 0 : 1;
}
