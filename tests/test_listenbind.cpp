// Listen server socket (docs/spec/net.md section 11, "Server socket"): the next free port of ten when the first is taken,
// bound to every interface, and the getinfo answer to the LAN address and to the broadcast, not only to loopback.
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "net/server.h"

using namespace iw2::net;

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #c "\n"; ++failures; } } while (0)

struct NoGame : ServerGame {
    std::map<int, std::string> configstrings() override { return {}; }
    bool clientConnect(int, const std::string&) override { return false; }
    void clientBegin(int) override {}
    void clientCommand(int, const std::string&) override {}
    void clientThink(int, const UserCmd&) override {}
    void clientDisconnect(int) override {}
    void runFrame(int) override {}
    int serverTime() const override { return 0; }
    SnapshotView snapshot(int) override { return {}; }
};

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The first IPv4 address of an interface that is up, not loopback; 0 when there is none.
uint32_t lanAddress(uint32_t* broadcast) {
    ifaddrs* list = nullptr;
    uint32_t ip = 0;
    if (getifaddrs(&list) != 0) return 0;
    for (ifaddrs* i = list; i && !ip; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_BROADCAST)) continue;
        ip = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
        if (broadcast && i->ifa_broadaddr) *broadcast = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_broadaddr)->sin_addr.s_addr);
    }
    freeifaddrs(list);
    return ip;
}

// Sends getinfo to ip:port from a fresh socket, polls the server until an infoResponse arrives (2 s at most).
bool asks(Server& server, uint32_t ip, uint16_t port, bool broadcast) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return false;
    int on = 1;
    if (broadcast) setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    timeval tv{0, 20000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(ip);
    to.sin_port = htons(port);
    const char req[] = "\xff\xff\xff\xffgetinfo xxx";
    bool got = false;
    if (::sendto(fd, req, sizeof req - 1, 0, reinterpret_cast<sockaddr*>(&to), sizeof to) > 0) {
        const int64_t t0 = nowMs();
        while (!got && nowMs() - t0 < 2000) {
            server.poll(nowMs(), 0);
            char buf[2048];
            const ssize_t n = ::recv(fd, buf, sizeof buf - 1, 0);
            if (n > 4) { buf[n] = 0; got = std::strstr(buf + 4, "infoResponse") == buf + 4; }
        }
    }
    ::close(fd);
    return got;
}
} // namespace

int main() {
    NoGame game;
    // A port someone else holds (any free one, so the test never touches 28960).
    UdpSocket holder;
    CHECK(holder.open(0, 0));
    const uint16_t taken = holder.localPort();
    CHECK(taken != 0);

    Server server(game);
    const uint16_t got = server.openFirstFree(taken);
    CHECK(got > taken && got < taken + 10); // the next free one of the ten
    CHECK(server.port() == got);
    CHECK(server.boundIp() == 0); // every interface (INADDR_ANY), not loopback

    // Every port of the ten taken: no socket.
    {
        UdpSocket h[10];
        uint16_t base = 0;
        for (int attempt = 0; attempt < 20 && !base; ++attempt) {
            UdpSocket probe;
            if (!probe.open(0, 0)) break;
            const uint16_t p = probe.localPort();
            probe.close();
            if (p > 65525) continue;
            int ok = 0;
            for (int i = 0; i < 10; ++i) ok += h[i].open(0, uint16_t(p + i));
            if (ok == 10) base = p;
            else for (auto& s : h) s.close();
        }
        if (base) {
            Server s2(game);
            CHECK(s2.openFirstFree(base) == 0);
        }
    }

    CHECK(asks(server, 0x7f000001, got, false));
    uint32_t bcast = 0;
    const uint32_t lan = lanAddress(&bcast);
    // The LAN address and the broadcasts: the macOS application firewall drops incoming LAN packets to a binary it has not
    // been told to allow (loopback passes), so these are hard checks only with IW2_TEST_LAN=1 (the binary allowed).
    const char* lanEnv = std::getenv("IW2_TEST_LAN");
    const bool strict = lanEnv && std::atoi(lanEnv) == 1;
    if (!lan) {
        std::cout << "no LAN interface: LAN address and broadcast not checked\n";
    } else {
        const bool a = asks(server, lan, got, false), b = asks(server, bcast, got, true), c = asks(server, 0xffffffffu, got, true);
        std::cout << "LAN address " << a << ", subnet broadcast " << b << ", limited broadcast " << c << "\n";
        if (strict) { CHECK(a); CHECK(b); CHECK(c); }
        else if (!(a && b && c)) std::cout << "LAN answers missing (firewall?); IW2_TEST_LAN=1 makes them a failure\n";
    }
    if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
    std::cout << "listen bind ok (port " << got << ")\n";
    return 0;
}
