// Loopback UDP test: a connectionless server answers getinfo / getstatus / getchallenge over 127.0.0.1.

#include <cstdio>
#include <iostream>
#include <string>

#include "net/oob.h"
#include "net/udp.h"

using namespace iw2::net;

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

constexpr uint32_t kLoopback = 0x7f000001;

std::string ask(UdpSocket& client, UdpSocket& server, ConnectionlessServer& srv, const std::string& request) {
    NetAddress to{kLoopback, server.localPort()};
    auto pkt = makeOutOfBand(request);
    client.sendTo(to, pkt.data(), int(pkt.size()));
    uint8_t buf[4096];
    NetAddress from;
    int n = server.receive(buf, sizeof buf, from, 2000);
    if (n <= 0) return "<no request>";
    auto line = parseOutOfBand(buf, n);
    if (!line) return "<not oob>";
    auto reply = srv.handle({from.ip, from.port}, *line);
    if (!reply) return "<ignored>";
    auto out = makeOutOfBand(*reply);
    server.sendTo(from, out.data(), int(out.size()));
    n = client.receive(buf, sizeof buf, from, 2000);
    if (n <= 0) return "<no reply>";
    auto text = outOfBandText(buf, n);
    return text ? *text : "<bad reply>";
}
} // namespace

int main() {
    UdpSocket server, client;
    CHECK(server.open(kLoopback, 0));
    CHECK(client.open(kLoopback, 0));
    CHECK(server.localPort() != 0);
    ConnectionlessServer srv(7);
    srv.info.hostname = "loop";
    srv.info.mapname = "mp_harbor";
    srv.info.players = {{1, 2, "p"}};

    std::string info = ask(client, server, srv, "getinfo 99");
    CHECK(info.rfind("infoResponse\n", 0) == 0);
    CHECK(infoValueForKey(info.substr(13), "challenge") == "99");
    CHECK(infoValueForKey(info.substr(13), "protocol") == "118");
    CHECK(infoValueForKey(info.substr(13), "mapname") == "mp_harbor");

    std::string status = ask(client, server, srv, "getstatus 5");
    CHECK(status.rfind("statusResponse\n", 0) == 0);
    CHECK(status.find("\n1 2 \"p\"\n") != std::string::npos);

    std::string ch = ask(client, server, srv, "getchallenge");
    CHECK(ch.rfind("challengeResponse ", 0) == 0);

    NetAddress parsed;
    CHECK(NetAddress::parse("127.0.0.1:28961", parsed, kDefaultPort) && parsed.ip == kLoopback && parsed.port == 28961);
    CHECK(NetAddress::parse("localhost", parsed, kDefaultPort) && parsed.port == 28960);
    CHECK(parsed.toString() == "127.0.0.1:28960");

    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "net_udp: all tests passed\n";
    return 0;
}
