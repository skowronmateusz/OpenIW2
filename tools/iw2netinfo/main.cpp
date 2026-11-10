// iw2netinfo: answers or sends connectionless queries of the patch 1.3 protocol (docs/spec/net.md section 5).
//   iw2netinfo serve [port] [seconds]      answer getinfo / getstatus / getchallenge (no game, no authorisation)
//   iw2netinfo query host[:port] getinfo|getstatus [challenge]
//   iw2netinfo rcon host[:port] <password> <command...>   remote console (docs/spec/dedicated.md section 4)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "net/oob.h"
#include "net/udp.h"

using namespace iw2::net;

namespace {

int serve(uint16_t port, int seconds) {
    UdpSocket sock;
    if (!sock.open(0, port)) {
        std::fprintf(stderr, "cannot bind UDP port %u\n", port);
        return 1;
    }
    std::printf("listening on UDP %u\n", sock.localPort());
    ConnectionlessServer srv(static_cast<uint32_t>(std::time(nullptr)));
    srv.info.hostname = "openiw2";
    srv.info.mapname = "mp_toujane";
    srv.info.gametype = "dm";
    srv.info.statusCvars = {{"sv_hostname", "openiw2"}, {"mapname", "mp_toujane"}, {"g_gametype", "dm"},
                            {"protocol", "118"}, {"shortversion", "1.3"}};
    auto start = std::chrono::steady_clock::now();
    uint8_t buf[1400];
    for (;;) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        int ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
        if (seconds > 0 && ms >= seconds * 1000) return 0;
        NetAddress from;
        int n = sock.receive(buf, sizeof buf, from, 100);
        if (n <= 0) continue;
        auto line = parseOutOfBand(buf, n);
        if (!line) continue;
        srv.time = ms;
        auto reply = srv.handle({from.ip, from.port}, *line);
        std::printf("%s: %s%s\n", from.toString().c_str(), line->c_str(), reply ? "" : " (ignored)");
        if (reply) {
            auto pkt = makeOutOfBand(*reply);
            sock.sendTo(from, pkt.data(), static_cast<int>(pkt.size()));
        }
    }
}

int query(const std::string& host, const std::string& what, const std::string& challenge) {
    NetAddress to;
    if (!NetAddress::parse(host, to, kDefaultPort)) {
        std::fprintf(stderr, "bad address %s\n", host.c_str());
        return 1;
    }
    UdpSocket sock;
    if (!sock.open(0, 0)) return 1;
    auto pkt = makeOutOfBand(what + " " + challenge);
    sock.sendTo(to, pkt.data(), static_cast<int>(pkt.size()));
    static uint8_t buf[0x20000];
    NetAddress from;
    int n = sock.receive(buf, sizeof buf, from, 2000);
    if (n <= 0) {
        std::fprintf(stderr, "no reply\n");
        return 2;
    }
    auto text = outOfBandText(buf, n);
    if (!text) return 2;
    std::fwrite(text->data(), 1, text->size(), stdout);
    std::printf("\n");
    return 0;
}

// Sends `rcon <password> <command>` and prints the text of every `print` reply that arrives within a second.
int rcon(const std::string& host, const std::string& password, const std::string& command) {
    NetAddress to;
    if (!NetAddress::parse(host, to, kDefaultPort)) {
        std::fprintf(stderr, "bad address %s\n", host.c_str());
        return 1;
    }
    UdpSocket sock;
    if (!sock.open(0, 0)) return 1;
    auto pkt = makeOutOfBand("rcon \"" + password + "\" " + command);
    sock.sendTo(to, pkt.data(), static_cast<int>(pkt.size()));
    static uint8_t buf[0x20000];
    NetAddress from;
    int replies = 0;
    for (int wait = 2000;; wait = 1000) {
        const int n = sock.receive(buf, sizeof buf, from, wait);
        if (n <= 0) break;
        auto text = outOfBandText(buf, n);
        if (!text || text->rfind("print\n", 0) != 0) continue;
        std::fwrite(text->data() + 6, 1, text->size() - 6, stdout);
        ++replies;
    }
    if (!replies) std::fprintf(stderr, "no reply\n");
    return replies ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "serve") == 0) {
        uint16_t port = argc >= 3 ? static_cast<uint16_t>(std::atoi(argv[2])) : kDefaultPort;
        int seconds = argc >= 4 ? std::atoi(argv[3]) : 0;
        return serve(port, seconds);
    }
    if (argc >= 4 && std::strcmp(argv[1], "query") == 0)
        return query(argv[2], argv[3], argc >= 5 ? argv[4] : "xxx");
    if (argc >= 5 && std::strcmp(argv[1], "rcon") == 0) {
        std::string command;
        for (int i = 4; i < argc; ++i) {
            const std::string a = argv[i];
            if (i > 4) command += ' ';
            command += a.find_first_of(" \t;") != std::string::npos ? "\"" + a + "\"" : a;
        }
        return rcon(argv[2], argv[3], command);
    }
    std::fprintf(stderr, "usage: iw2netinfo serve [port] [seconds] | query host[:port] getinfo|getstatus [challenge] | "
                         "rcon host[:port] <password> <command...>\n");
    return 1;
}
