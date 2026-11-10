// Verification (c) of docs/spec/net.md section 9: a decoder written only from the spec (tests/specdecoder.cpp, no
// openiw2 coder code) reads the stream of iw2server, and of a 1.3 dedicated server when IW2_TEST_LNXDED is set,
// while the test client plays the standard scenario; every configstring, baseline and snapshot it decodes must equal
// what the test client decoded.
//   test_net_specdecoder <iw2server> <main dir>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "net/client.h"
#include "specdecoder.h"

using namespace iw2::net;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "CHECK failed: " << what << "\n";
        ++failures;
    }
}

void verify(const char* address, const char* label) {
    NetAddress to;
    if (!NetAddress::parse(address, to, 28960)) {
        check(false, std::string("address ") + address);
        return;
    }
    TestClient client;
    client.addStandardScenario();
    specdec::Decoder dec;
    client.onRawPacket = [&](const uint8_t* d, int n) { dec.packet(d, n); };
    dec.clientCommandText = [&](int seq) { return client.reliableCommandText(seq); };
    int compared = 0, mismatched = 0;
    client.onFrameView = [&](const TestClientFrameView& f) {
        const auto it = dec.frames.find(f.messageNum);
        if (it == dec.frames.end()) {
            ++mismatched;
            return;
        }
        ++compared;
        if (*f.ps != it->second.ps || *f.entities != it->second.entities || *f.clients != it->second.clients)
            ++mismatched;
    };
    bool ok = false;
    for (int attempt = 0; attempt < 15 && !ok; ++attempt) ok = client.connect(to, 2000);
    check(ok, std::string(label) + ": connect " + client.error);
    if (!ok) return;
    dec.challenge = client.challenge;
    // Run until enough snapshots were compared (condition, not a fixed time window: under load the server
    // may deliver fewer than 50 snapshots in 5 s); give up after 20 s.
    for (int waited = 0; compared <= 60 && waited < 20000; waited += 250) client.run(250);
    client.addReliableCommand("disconnect");
    client.run(300);
    check(dec.err.empty(), std::string(label) + ": spec decoder error " + dec.err);
    check(dec.haveGamestate && dec.configstrings == client.configstrings, std::string(label) + ": configstrings");
    check(dec.baselines == client.baselineStates(), std::string(label) + ": baselines");
    check(dec.clientNum == client.clientNum, std::string(label) + ": client number");
    check(compared > 50 && mismatched == 0, std::string(label) + ": snapshots " + std::to_string(compared) + " equal, " +
                                                std::to_string(mismatched) + " different");
    std::cout << label << ": " << dec.configstrings.size() << " configstrings, " << dec.baselines.size()
              << " baselines, " << compared << " snapshots, " << dec.commandCount << " commands decoded from the spec\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || !*argv[2]) {
        std::cout << "net_specdecoder: no game data, skipped\n";
        return 0;
    }
    // The server is a child with a known pid and is stopped when the test is done, so a run never meets the
    // still-running server of an earlier run on the same port; --seconds is only a safety net.
    const pid_t server = fork();
    if (server < 0) return 1;
    if (server == 0) {
        const int null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, 1);
            dup2(null, 2);
        }
        execl(argv[1], argv[1], argv[2], "mp_toujane", "dm", "--port", "28978", "--bots", "1", "--seconds", "60",
              "--no-console", static_cast<char*>(nullptr));
        _exit(127);
    }
    verify("127.0.0.1:28978", "iw2server");
    kill(server, SIGTERM);
    waitpid(server, nullptr, 0);
    if (const char* t = std::getenv("IW2_TEST_LNXDED"); t && *t) verify(t, "1.3 server");
    return failures ? 1 : 0;
}
