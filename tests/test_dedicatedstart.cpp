// The dedicated start of the create-server screen (docs/spec/mp-ui.md "Dedicated start"), headless: the dedicated value
// check, the server settings taken from the menu, the console window's model (log, command line, history, scroll),
// the stdout tee, and the shared server loop (src/netgame/dedicated.cpp) started with those settings: it answers
// getinfo and getstatus, a network client joins, the console front end runs commands and "quit" (the window's close)
// ends it. The server part needs IW2_MAIN.
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fs/vfs.h"
#include "net/gameclient.h"
#include "net/lanbrowser.h"
#include "net/oob.h"
#include "net/udp.h"
#include "netgame/consoleview.h"
#include "netgame/dedicated.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

using namespace iw2;

void menuSettings() {
    CHECK(!netgame::menuStartIsDedicated("0"));
    CHECK(!netgame::menuStartIsDedicated(""));
    CHECK(netgame::menuStartIsDedicated("1")); // LAN
    CHECK(netgame::menuStartIsDedicated("2")); // internet: the same in openiw2 (no master server)
    const std::map<std::string, std::string> cv = {{"g_gametype", "tdm"}, {"sv_hostname", "Menu host"},
                                                   {"g_password", "secret"}, {"scr_allow_kar98k", "0"}};
    const auto s = netgame::dedicatedFromMenu(cv, "wait ; wait ; map mp_toujane\n", 3, 2, 28970);
    CHECK(s.map == "mp_toujane");
    CHECK(s.gametype == "tdm");
    CHECK(s.bots == 3 && s.botSkill == 2 && s.port == 28970);
    CHECK(!s.readStdin);
    bool host = false, pw = false, allow = false;
    for (const auto& [k, v] : s.cvars) {
        host = host || (k == "sv_hostname" && v == "Menu host");
        pw = pw || (k == "g_password" && v == "secret");
        allow = allow || (k == "scr_allow_kar98k" && v == "0");
    }
    CHECK(host && pw && allow);
}

void consoleView() {
    netgame::ConsoleView v(5);
    v.append("one\ntw");
    v.append("o\nthree\n");
    CHECK(v.lines().size() == 3 && v.lines()[1] == "two" && v.lines()[2] == "three");
    v.append("4\n5\n6\n7\n");
    CHECK(v.lines().size() == 5 && v.lines().front() == "three"); // capped, oldest dropped
    CHECK(v.visible(2) == (std::vector<std::string>{"6", "7"}));
    v.scroll(2);
    CHECK(v.visible(2) == (std::vector<std::string>{"4", "5"}));
    v.scroll(100);
    CHECK(v.scrollOffset() == 5);
    v.scroll(-100);
    CHECK(v.scrollOffset() == 0);

    v.insert("stat\x01us");
    CHECK(v.input() == "status" && v.cursor() == 6);
    v.left();
    v.left();
    v.backspace();
    CHECK(v.input() == "staus" && v.cursor() == 3);
    v.insert("t");
    v.end();
    auto c = v.enter();
    CHECK(c && *c == "status");
    CHECK(v.lines().back() == "]status"); // echo
    CHECK(v.input().empty());
    CHECK(!v.enter()); // empty line: nothing
    v.insert("map mp_dawnville");
    v.enter();
    v.insert("map mp_dawnville");
    v.enter();
    CHECK(v.history().size() == 2); // the same command twice in a row is kept once
    v.historyUp();
    CHECK(v.input() == "map mp_dawnville");
    v.historyUp();
    CHECK(v.input() == "status");
    v.historyUp();
    CHECK(v.input() == "status"); // oldest
    v.historyDown();
    CHECK(v.input() == "map mp_dawnville");
    v.historyDown();
    CHECK(v.input().empty());
    v.insert("abc");
    v.home();
    v.del();
    CHECK(v.input() == "bc");
}

void stdoutTee() {
    netgame::StdoutTee tee;
    CHECK(tee.start());
    std::printf("tee line one\n");
    std::cout << "tee line two" << std::endl;
    std::fflush(stdout);
    std::string got;
    for (int i = 0; i < 100 && got.find("two") == std::string::npos; ++i) {
        usleep(10000);
        got += tee.take();
    }
    tee.stop();
    CHECK(got.find("tee line one\n") != std::string::npos);
    CHECK(got.find("tee line two\n") != std::string::npos);
}

std::string query(int port, const std::string& what) {
    net::UdpSocket sock;
    if (!sock.open(0, 0)) return {};
    net::NetAddress to;
    net::NetAddress::parse("127.0.0.1:" + std::to_string(port), to, 28960);
    auto pkt = net::makeOutOfBand(what + " xxx");
    sock.sendTo(to, pkt.data(), static_cast<int>(pkt.size()));
    static uint8_t buf[0x20000];
    net::NetAddress from;
    const int n = sock.receive(buf, sizeof buf, from, 2000);
    if (n <= 0) return {};
    auto t = net::outOfBandText(buf, n);
    return t ? *t : std::string();
}

void server(const char* mainDir) {
    fs::Vfs vfs;
    vfs.mountDirectory(mainDir);
    std::map<std::string, std::string> cv = {{"g_gametype", "dm"}, {"sv_hostname", "Dedicated from menu"}};
    auto s = netgame::dedicatedFromMenu(cv, "wait ; wait ; map mp_toujane\n", 1, 1, 0);
    std::atomic<int> port{0};
    std::mutex m;
    std::vector<std::string> typed; // lines the console window would hand over
    std::atomic<int> frames{0};
    s.onStarted = [&](int p) { port = p; };
    s.frontEnd = [&] {
        ++frames;
        std::lock_guard<std::mutex> lock(m);
        return std::exchange(typed, {});
    };
    int rc = -1;
    std::thread t([&] { rc = netgame::runDedicated(vfs, s); });
    for (int i = 0; i < 1200 && port == 0; ++i) usleep(50000);
    CHECK(port != 0);
    if (port != 0) {
        const std::string info = query(port, "getinfo");
        CHECK(info.find("infoResponse") == 0);
        CHECK(net::infoValueForKey(info.substr(info.find('\n') + 1), "hostname") == "Dedicated from menu");
        CHECK(net::infoValueForKey(info.substr(info.find('\n') + 1), "mapname") == "mp_toujane");
        const std::string st = query(port, "getstatus");
        CHECK(st.find("statusResponse") == 0);
        CHECK(st.find("\"bot0\"") != std::string::npos);

        net::GameClient client;
        client.name = "joiner";
        net::NetAddress to;
        net::NetAddress::parse("127.0.0.1:" + std::to_string(port.load()), to, 28960);
        int64_t now = 1000;
        CHECK(client.connect(to, now));
        for (int i = 0; i < 400 && client.state() < net::ClientState::Primed && client.state() != net::ClientState::Disconnected; ++i)
            client.poll(now += 20, 20);
        CHECK(client.state() >= net::ClientState::Primed);
        CHECK(net::infoValueForKey(client.configstrings[0], "mapname") == "mp_toujane");
        // Console commands from the window, then its close.
        {
            std::lock_guard<std::mutex> lock(m);
            typed = {"set sv_hostname Renamed"};
        }
        for (int i = 0; i < 100; ++i) {
            client.poll(now += 20, 20);
            if (net::infoValueForKey(query(port, "getinfo").substr(13), "hostname") == "Renamed") break;
        }
        CHECK(net::infoValueForKey(query(port, "getinfo").substr(13), "hostname") == "Renamed");
        {
            std::lock_guard<std::mutex> lock(m);
            typed = {"quit"};
        }
        for (int i = 0; i < 200 && client.state() != net::ClientState::Disconnected; ++i) client.poll(now += 20, 20);
        CHECK(client.state() == net::ClientState::Disconnected); // the server quit and told the client
    }
    t.join();
    CHECK(rc == 0);
    CHECK(frames > 0);
}

} // namespace

int main() {
    menuSettings();
    consoleView();
    stdoutTee();
    if (const char* main = std::getenv("IW2_MAIN")) server(main);
    else std::cout << "IW2_MAIN not set: server part skipped\n";
    if (failures) return 1;
    std::cout << "dedicatedstart: ok\n";
    return 0;
}
