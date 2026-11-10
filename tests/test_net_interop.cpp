// Interoperability check against a running patch 1.3 server (docs/spec/net.md section 9): connect, gamestate,
// delta snapshots, a reliable command and forward movement. Skipped unless IW2_TEST_LNXDED=host:port is set.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "net/client.h"
#include "net/netfields.h"
#include "netgame/worldgame.h"

using namespace iw2::net;

int main() {
    const char* target = std::getenv("IW2_TEST_LNXDED");
    if (!target || !*target) {
        std::cout << "net_interop: IW2_TEST_LNXDED not set, skipped\n";
        return 0;
    }
    NetAddress to;
    if (!NetAddress::parse(target, to, 28960)) {
        std::cerr << "bad IW2_TEST_LNXDED " << target << "\n";
        return 1;
    }
    TestClient client;
    client.forwardMove = 127;
    client.addReliableCommand("say openiw2 interop");
    if (!client.connect(to, 3000)) {
        std::cerr << "connect failed: " << client.error << "\n";
        return 1;
    }
    client.run(3000);
    std::cout << client.summary();
    int failures = 0;
    auto check = [&](bool c, const char* what) {
        if (!c) {
            std::cerr << "CHECK failed: " << what << "\n";
            ++failures;
        }
    };
    check(client.gamestateReceived, "gamestate");
    check(client.configstrings.count(0) && client.configstrings.count(1), "server and system info");
    check(client.snapshots >= 10, "snapshots");
    check(client.deltaSnapshots >= 5, "delta snapshots");
    check(client.error.empty(), "no decode error");
    check(client.reliableAcknowledged == 1, "reliable command acknowledged");
    // The client side reads the 1.3 layout (docs/spec/net.md section 11): after the scenario of net_wirediff the
    // player holds the Sten, weapon numbers name weapons of the weapon list, the client state names player models.
    TestClient join;
    join.addStandardScenario();
    if (!join.connect(to, 3000)) {
        std::cerr << "second connect failed: " << join.error << "\n";
        return 1;
    }
    join.run(5000);
    if (const TestClient::Frame* f = join.lastFrame()) {
        int32_t weapon = 0, offhand = 0;
        std::memcpy(&weapon, f->ps.data() + 212, 4);
        std::memcpy(&offhand, f->ps.data() + 208, 4);
        check(iw2::netgame::weaponNameFromList(join.configstrings, weapon) == "sten_mp", "current weapon sten_mp");
        check(iw2::netgame::weaponNameFromList(join.configstrings, offhand).rfind("frag_grenade", 0) == 0,
              "offhand frag grenade");
        const auto c = f->clients.find(join.clientNum);
        int32_t body = 0;
        if (c != f->clients.end()) std::memcpy(&body, c->second.data() + 8, 4);
        const auto m = join.configstrings.find(iw2::netgame::kModelConfigstrings - 1 + body);
        check(m != join.configstrings.end() && m->second.rfind("xmodel/playerbody", 0) == 0, "body model");
    } else {
        check(false, "snapshot after the scenario");
    }
    join.addReliableCommand("disconnect");
    join.run(300);
    const float dx = client.lastOrigin[0] - client.firstOrigin[0], dy = client.lastOrigin[1] - client.firstOrigin[1];
    check(std::sqrt(dx * dx + dy * dy) > 100.0f, "player moved");
    return failures ? 1 : 0;
}
