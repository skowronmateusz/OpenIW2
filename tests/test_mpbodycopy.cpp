// A network client's bodies keep the model their client had when they appeared (docs/spec/net.md section 11 "Entities",
// bodies; netgame::BodyInfos): after the client leaves, after it joins again without a body model, and not for a body
// first seen once its client is gone. No game files needed.
#include "net/gameclient.h"
#include "netgame/remoteview.h"
#include "netgame/worldgame.h"

#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

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

void putI(std::vector<uint8_t>& b, size_t off, int32_t v) {
    if (b.size() < off + 4) b.resize(off + 4, 0);
    std::memcpy(b.data() + off, &v, 4);
}

// A body entity state: eType 2, eFlags, the client number (+0x90).
std::vector<uint8_t> body(int client, int flags) {
    std::vector<uint8_t> es(256, 0);
    putI(es, 4, netgame::kEtCorpse);
    putI(es, 8, flags);
    putI(es, 144, client);
    return es;
}

// A client state with the body model index (+8) and attached model indices (+12..).
std::vector<uint8_t> clientState(int model, std::vector<int> attachments) {
    std::vector<uint8_t> cs(96, 0);
    putI(cs, 8, model);
    for (size_t i = 0; i < attachments.size(); ++i) putI(cs, 12 + 4 * i, attachments[i]);
    return cs;
}

} // namespace

int main() {
    // Model configstrings: index i is configstring kModelConfigstrings + i - 1.
    std::map<int, std::string> cfg;
    cfg[netgame::kModelConfigstrings + 0] = "xmodel/playerbody_american_normandy01";
    cfg[netgame::kModelConfigstrings + 1] = "xmodel/head_us_braeburn";
    cfg[netgame::kModelConfigstrings + 2] = "xmodel/playerbody_german_normandy01";
    cfg[netgame::kModelConfigstrings + 3] = "xmodel/head_german_heinz";
    constexpr int kBody = 64 + 3; // a body of the queue
    constexpr int kNew = netgame::kEfBodyNew;

    {
        netgame::BodyInfos infos;
        net::ClientSnapshot s;
        s.valid = true;
        // The body appears while its client (5) is still connected.
        s.clients[5] = clientState(1, {2});
        s.entities[kBody] = body(5, 0x20000 | kNew | 2);
        infos.update(s, cfg);
        const netgame::BodyInfos::Info* bi = infos.find(kBody);
        CHECK(bi && bi->valid && bi->client == 5 && bi->model == "xmodel/playerbody_american_normandy01");
        CHECK(bi && bi->attachments.size() == 1 && bi->attachments[0] == "xmodel/head_us_braeburn");
        // The client leaves: no client state any more; the body keeps its copy.
        s.clients.clear();
        s.entities[kBody] = body(5, 0x20000 | 2);
        infos.update(s, cfg);
        bi = infos.find(kBody);
        CHECK(bi && bi->valid && bi->model == "xmodel/playerbody_american_normandy01");
        // It joins again and has not spawned: a client state without a body model; still the copy.
        s.clients[5] = clientState(0, {});
        infos.update(s, cfg);
        bi = infos.find(kBody);
        CHECK(bi && bi->model == "xmodel/playerbody_american_normandy01" && bi->attachments.size() == 1);
        // It spawns in the other team: the body is not reset (same bit 2), so it keeps the old look.
        s.clients[5] = clientState(3, {4});
        infos.update(s, cfg);
        bi = infos.find(kBody);
        CHECK(bi && bi->model == "xmodel/playerbody_american_normandy01");
        // The slot is reused by a new body of another client (bit 2 toggled, eFlags 0x80000): a new copy.
        s.clients[6] = clientState(3, {4});
        s.entities[kBody] = body(6, 0x20000 | kNew);
        infos.update(s, cfg);
        bi = infos.find(kBody);
        CHECK(bi && bi->client == 6 && bi->model == "xmodel/playerbody_german_normandy01" && bi->attachments.size() == 1 &&
              bi->attachments[0] == "xmodel/head_german_heinz");
        // The body leaves the snapshot and comes back after its death animation (no 0x80000): its copy holds this
        // client and a model, so it is kept even though the client state changed meanwhile.
        s.entities.erase(kBody);
        infos.update(s, cfg);
        s.clients[6] = clientState(1, {2});
        s.entities[kBody] = body(6, 0x20000);
        infos.update(s, cfg);
        bi = infos.find(kBody);
        CHECK(bi && bi->model == "xmodel/playerbody_german_normandy01");
        // A body coming back with eFlags 0x80000 is copied again.
        s.entities.erase(kBody);
        infos.update(s, cfg);
        s.entities[kBody] = body(6, 0x20000 | kNew);
        infos.update(s, cfg);
        bi = infos.find(kBody);
        CHECK(bi && bi->model == "xmodel/playerbody_american_normandy01");
    }
    {
        // A client that connects after the body's client left: the first copy finds no client state, so the body has no
        // model; it gets one only when a reset finds its client again.
        netgame::BodyInfos infos;
        net::ClientSnapshot s;
        s.valid = true;
        s.entities[kBody] = body(7, 0x20000 | 2);
        infos.update(s, cfg);
        const netgame::BodyInfos::Info* bi = infos.find(kBody);
        CHECK(bi && !bi->valid && bi->model.empty());
        s.clients[7] = clientState(1, {2});
        infos.update(s, cfg); // no reset: nothing copied
        bi = infos.find(kBody);
        CHECK(bi && bi->model.empty());
        s.entities.erase(kBody);
        infos.update(s, cfg);
        s.entities[kBody] = body(7, 0x20000 | 2); // back in the snapshot: a reset, the copy has no model, so it is taken
        infos.update(s, cfg);
        bi = infos.find(kBody);
        CHECK(bi && bi->valid && bi->model == "xmodel/playerbody_american_normandy01");
    }
    std::cout << (failures ? "FAILED" : "ok") << "\n";
    return failures ? 1 : 0;
}
