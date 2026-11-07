// Source guard: the iw2view frame code draws at the client time cg.time (cgNowMs, game::clientRenderTimeMs), which advances
// every drawn frame. A visual evaluated at the 20 Hz server time (World::timeMs) moves in 50 ms steps, so every remaining
// server-time read in the file must say why with "[server time]" on its line (simulation input, cache keys, debug output).

#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: test_renderclock <apps/iw2view/main.cpp>\n";
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 2;
    }
    int failures = 0, lineNo = 0, allowed = 0;
    for (std::string line; std::getline(in, line);) {
        ++lineNo;
        if (line.find("timeMs()") == std::string::npos) continue;
        if (line.find("auto cgNowMs") != std::string::npos || line.find("[server time]") != std::string::npos) {
            ++allowed;
            continue;
        }
        std::cerr << argv[1] << ":" << lineNo << ": server time in the frame code; use cgNowMs() or mark it [server time]\n";
        ++failures;
    }
    if (allowed == 0) {
        std::cerr << "no cgNowMs definition found\n";
        ++failures;
    }
    if (failures == 0) std::cout << "renderclock tests passed\n";
    return failures ? 1 : 0;
}
