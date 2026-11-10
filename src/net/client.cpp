// Implemented from the spec (docs/spec/net.md sections 4, 5 and 8).
#include "net/client.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "net/huffman.h"
#include "net/netfields.h"
#include "net/playerstate.h"
#include "net/oob.h"
#include "net/scramble.h"
#include "net/wiredump.h"

namespace iw2::net {

namespace {
int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

int64_t TestClient::now() const { return clock ? clock() : nowMs(); }

bool TestClient::waitOob(const std::string& prefix, std::string& text, int timeoutMs) {
    static uint8_t buf[0x20000];
    int64_t end = now() + timeoutMs;
    while (now() < end) {
        NetAddress from;
        int n = sock_.receive(buf, sizeof buf, from, idle ? 0 : 100);
        if (n <= 0) {
            if (idle) idle();
            continue;
        }
        auto t = outOfBandText(buf, n);
        if (!t) continue;
        if (verbose) std::printf("oob: %s\n", t->c_str());
        if (t->compare(0, prefix.size(), prefix) == 0) {
            text = *t;
            return true;
        }
        if (t->compare(0, 5, "error") == 0) {
            error = *t;
            return false;
        }
    }
    error = "timeout waiting for " + prefix;
    return false;
}

bool TestClient::connect(const NetAddress& server, int timeoutMs) {
    server_ = server;
    if (!sock_.open(0, 0)) {
        error = "cannot open socket";
        return false;
    }
    auto pkt = makeOutOfBand("getchallenge 0 \"\"");
    sock_.sendTo(server_, pkt.data(), static_cast<int>(pkt.size()));
    std::string text;
    if (!waitOob("challengeResponse", text, timeoutMs)) return false;
    auto tok = tokenize(text);
    if (tok.size() < 2) {
        error = "bad challengeResponse";
        return false;
    }
    challenge = std::atoi(tok[1].c_str());
    qport = static_cast<int>(now() & 0xffff);
    std::string info;
    infoSetValueForKey(info, "cl_punkbuster", "0");
    infoSetValueForKey(info, "cl_anonymous", "0");
    infoSetValueForKey(info, "snaps", "20");
    infoSetValueForKey(info, "rate", "25000");
    infoSetValueForKey(info, "name", name);
    infoSetValueForKey(info, "protocol", std::to_string(kProtocolVersion));
    infoSetValueForKey(info, "challenge", std::to_string(challenge));
    infoSetValueForKey(info, "qport", std::to_string(qport));
    pkt = makeOutOfBand("connect \"" + info + "\"");
    sock_.sendTo(server_, pkt.data(), static_cast<int>(pkt.size()));
    if (!waitOob("connectResponse", text, timeoutMs)) return false;
    chan_ = std::make_unique<Netchan>(NetSide::Client, qport, [this](const uint8_t* d, int n) {
        sock_.sendTo(server_, d, n);
    });
    return true;
}

void TestClient::addReliableCommand(const std::string& text) {
    ++reliableSequence;
    reliableCommands_[reliableSequence & 127] = text;
}

UserCmd TestClient::baseCommand() const {
    const Frame* f = lastFrame();
    return f ? usercmdBase(f->ps.data()) : UserCmd{};
}

void TestClient::sendPacket() {
    // Header bytes, then the operation bits that get compressed and scrambled.
    uint8_t ops[2048];
    Msg body(ops, sizeof ops);
    for (int seq = reliableAcknowledged + 1; seq <= reliableSequence; ++seq) {
        body.writeBits(2, 3);  // reliable client command
        body.writeLong(seq);
        body.writeString(reliableCommands_[seq & 127]);
    }
    if (gamestateReceived) {
        const Frame* f = lastFrame();
        int key = checksumFeed ^ messageAcknowledge_ ^ commandHashKey(lastServerCommands_[reliableAcknowledge_ & 127], 32);
        body.writeBits(f ? 0 : 1, 3);  // user commands, with or without a delta snapshot
        body.writeByte(1);
        UserCmd from = baseCommand();
        UserCmd cmd = from;
        cmd.serverTime = f ? f->serverTime + 50 : lastCmdTime_ + 50;
        if (cmd.serverTime <= lastCmdTime_) cmd.serverTime = lastCmdTime_ + 1;
        lastCmdTime_ = cmd.serverTime;
        cmd.forwardmove = static_cast<int8_t>(forwardMove);
        cmd.buttons = buttons;
        writeDeltaUsercmd(body, static_cast<uint32_t>(key), from, cmd);
    }
    body.writeBits(3, 3);  // end of client message
    uint8_t buf[1400];
    Msg m(buf, sizeof buf);
    m.writeByte(serverId_);
    m.writeLong(messageAcknowledge_);
    m.writeLong(reliableAcknowledge_);
    int n = Huffman::protocol().compress(ops, body.cursize(), buf + m.cursize(), m.maxsize() - m.cursize());
    scramble(buf + m.cursize(), n, clientToServerKey(challenge, serverId_, messageAcknowledge_),
             lastServerCommands_[reliableAcknowledge_ & 127]);
    chan_->transmit(buf, m.cursize() + n);
}

void TestClient::run(int ms) {
    static uint8_t buf[0x20000 + 16];
    int64_t end = now() + ms;
    int64_t nextSend = 0;
    while (now() < end) {
        runScript();
        if (now() >= nextSend) {
            sendPacket();
            nextSend = now() + 50;
        }
        NetAddress from;
        int n = sock_.receive(buf, 0x20000, from, idle ? 0 : 10);
        if (n <= 0 && idle) idle();
        if (n <= 0 || !(from == server_)) continue;
        if (n >= 4 && std::memcmp(buf, "\xff\xff\xff\xff", 4) == 0) {
            if (verbose) {
                auto t = outOfBandText(buf, n);
                if (t) std::printf("oob: %s\n", t->c_str());
            }
            continue;
        }
        if (onRawPacket) onRawPacket(buf, n);
        handlePacket(buf, n);
    }
}

void TestClient::handlePacket(uint8_t* data, int len) {
    Msg msg(data, 0x20000 + 16);
    msg.setCursize(len);
    if (!chan_->process(msg)) return;
    messageAcknowledge_ = chan_->incomingSequence();
    // The first 4 bytes after the header are not scrambled or compressed.
    int start = msg.readcount();
    if (len - start < 4) return;
    int32_t firstLong = 0;
    std::memcpy(&firstLong, data + start, 4);
    // The leading long acknowledges our reliable commands; its command text is the scramble string.
    if (firstLong > reliableAcknowledged && firstLong <= reliableSequence) reliableAcknowledged = firstLong;
    int body = start + 4;
    scramble(data + body, msg.cursize() - body, serverToClientKey(challenge, chan_->incomingSequence()),
             reliableCommands_[firstLong & 127]);
    static uint8_t out[0x20000];
    int outLen = Huffman::protocol().decompress(data + body, msg.cursize() - body, out, sizeof out);
    Msg dec(out, sizeof out);
    dec.setCursize(outLen);
    if (verbose) {
        std::printf("packet seq %d: %d bytes -> %d, lead %d\n", chan_->incomingSequence(), len, outLen, firstLong);
        for (int i = 0; i < outLen && i < 48; ++i) std::printf("%02x ", out[i]);
        std::printf("\n");
    }
    parseServerMessage(dec);
}

void TestClient::parseServerMessage(Msg& msg) {
    msg.beginReading();
    while (!msg.overflowed()) {
        int cmd = msg.readByte();
        if (cmd < 0 || cmd == 7) return;
        if (verbose) std::printf("  svc %d\n", cmd);
        switch (cmd) {
        case 0: break;
        case 1: parseGamestate(msg); return;
        case 4: {
            int seq = msg.readLong();
            std::string text = msg.readString();
            if (seq > serverCommandSequence) {
                serverCommandSequence = seq;
                reliableAcknowledge_ = seq;
                lastServerCommands_[seq & 127] = text;
                serverCommands.push_back(text);
                if (dump) std::fprintf(dump, "cmd %d %s\n", seq, text.c_str());
                if (verbose) std::printf("  command %d: %s\n", seq, text.c_str());
            }
            break;
        }
        case 6: parseSnapshot(msg); break;
        default:
            error = "unhandled server message " + std::to_string(cmd);
            return;
        }
    }
}

void TestClient::parseGamestate(Msg& msg) {
    serverCommandSequence = msg.readLong();
    reliableAcknowledge_ = serverCommandSequence;
    std::vector<uint8_t> zero(kEntityStateSize, 0);
    for (;;) {
        int cmd = msg.readByte();
        if (msg.overflowed()) return;
        if (cmd == 7) break;
        if (cmd == 2) {
            int index = msg.readShort();
            configstrings[index] = msg.readBigString();
        } else if (cmd == 3) {
            int number = msg.readBits(kEntityNumberBits) & (kMaxEntities - 1);
            std::vector<uint8_t> state(kEntityStateSize, 0);
            readDeltaStructBody(msg, zero.data(), state.data(), kEntityStateSize, kEntityStateFields, 59);
            baselines_[number] = state;
            ++baselines;
        } else {
            error = "bad gamestate command " + std::to_string(cmd);
            return;
        }
    }
    clientNum = msg.readLong();
    checksumFeed = msg.readLong();
    if (dump) {
        std::fprintf(dump, "gamestate seq %d clientNum %d\n", serverCommandSequence, clientNum);
        for (auto& [k, v] : configstrings) std::fprintf(dump, "cs %d %s\n", k, v.c_str());
        std::vector<uint8_t> z(kEntityStateSize, 0);
        for (auto& [k, v] : baselines_)
            std::fprintf(dump, "baseline %d%s\n", k, dumpFields(v.data(), z.data(), kEntityStateFields, 59, 0).c_str());
    }
    gamestateReceived = true;
    // The server id is a configstring key of the system info (TODO(spec): confirm the source of serverId).
    std::string sid = infoValueForKey(configstrings[1], "sv_serverid");
    serverId_ = std::atoi(sid.c_str()) & 0xff;
}

const TestClient::Frame* TestClient::lastFrame() const {
    if (lastFrameNum_ < 0) return nullptr;
    const Frame& f = frames_[lastFrameNum_ & 31];
    return f.valid && f.messageNum == lastFrameNum_ ? &f : nullptr;
}

void TestClient::parseSnapshot(Msg& msg) {
    Frame frame;
    frame.messageNum = chan_->incomingSequence();
    frame.serverTime = msg.readLong();
    int deltaNum = msg.readByte();
    const int snapFlags = msg.readByte();
    const Frame* old = nullptr;
    if (deltaNum > 0) {
        const Frame& f = frames_[(frame.messageNum - deltaNum) & 31];
        if (!f.valid || f.messageNum != frame.messageNum - deltaNum) {
            error = "delta from an unknown frame";
            return;
        }
        old = &f;
        ++deltaSnapshots;
    }
    frame.ps.assign(kPlayerStateSize, 0);
    readDeltaPlayerState(msg, old ? old->ps.data() : nullptr, frame.ps.data());
    // Entities: 10-bit numbers in ascending order, terminated by kEntityListEnd.
    if (old) frame.entities = old->entities;
    std::vector<uint8_t> zero(kEntityStateSize, 0);
    std::map<int, std::vector<uint8_t>> seen;
    for (;;) {
        int number = msg.readBits(kEntityNumberBits) & (kMaxEntities - 1);
        if (msg.overflowed() || number == kEntityListEnd) break;
        const std::vector<uint8_t>* base = nullptr;
        if (old) {
            auto it = old->entities.find(number);
            if (it != old->entities.end()) base = &it->second;
        }
        if (!base) {
            auto it = baselines_.find(number);
            base = it != baselines_.end() ? &it->second : &zero;
        }
        std::vector<uint8_t> state(kEntityStateSize);
        if (readDeltaStructBody(msg, base->data(), state.data(), kEntityStateSize, kEntityStateFields, 59) ==
            StructDelta::Removed)
            frame.entities.erase(number);
        else
            frame.entities[number] = state;
    }
    // Clients: each frame starts with a 1 bit, a 0 bit ends the list.
    if (old) frame.clients = old->clients;
    std::vector<uint8_t> czero(kClientStateSize, 0);
    while (msg.readBit() == 1 && !msg.overflowed()) {
        int number = msg.readBits(kClientNumberBits) & 63;
        const std::vector<uint8_t>* base = &czero;
        if (old) {
            auto it = old->clients.find(number);
            if (it != old->clients.end()) base = &it->second;
        }
        std::vector<uint8_t> state(kClientStateSize);
        if (readDeltaStructBody(msg, base->data(), state.data(), kClientStateSize, kClientStateFields, 22) ==
            StructDelta::Removed)
            frame.clients.erase(number);
        else
            frame.clients[number] = state;
    }
    if (msg.overflowed()) {
        error = "snapshot overflowed";
        return;
    }
    frame.valid = true;
    entitiesInLastSnapshot = static_cast<int>(frame.entities.size());
    clientsInLastSnapshot = static_cast<int>(frame.clients.size());
    lastServerTime = frame.serverTime;
    float origin[3];
    std::memcpy(origin, frame.ps.data() + 20, 12);
    if (!haveOrigin) std::memcpy(firstOrigin, origin, 12);
    std::memcpy(lastOrigin, origin, 12);
    haveOrigin = true;
    ++snapshots;
    if (verbose)
        std::printf("  snapshot %d time %d delta %d: %d entities, %d clients, origin %.1f %.1f %.1f\n",
                    frame.messageNum, frame.serverTime, deltaNum, entitiesInLastSnapshot, clientsInLastSnapshot,
                    origin[0], origin[1], origin[2]);
    if (firstSnapshotMs_ < 0) {
        firstSnapshotMs_ = now();
        firstServerTime_ = frame.serverTime;
    }
    if (dump) dumpFrame(dump, frame, old, deltaNum, snapFlags);
    if (onFrameView) onFrameView({frame.messageNum, &frame.ps, &frame.entities, &frame.clients});
    lastFrameNum_ = frame.messageNum;
    frames_[frame.messageNum & 31] = std::move(frame);
}

void TestClient::runScript() {
    if (firstSnapshotMs_ < 0) return;
    if (nextStep_ == 0)
        std::stable_sort(script.begin(), script.end(), [](const Step& x, const Step& y) { return x.atMs < y.atMs; });
    const int64_t t = now() - firstSnapshotMs_;
    while (nextStep_ < script.size() && script[nextStep_].atMs <= t) {
        const Step& s = script[nextStep_++];
        if (s.command.empty()) {
            forwardMove = s.forward;
            buttons = s.buttons;
            continue;
        }
        std::string text = s.command;
        const size_t p = text.find("$sid");
        if (p != std::string::npos) text.replace(p, 4, std::to_string(serverId_));
        addReliableCommand(text);
        if (dump) std::fprintf(dump, "send %s\n", text.c_str());
    }
}

void TestClient::addStandardScenario() {
    // Script menu indices of the 1.3 gamestate: 1 the team menu, 2 the British weapon menu (docs/spec/net.md).
    script.push_back({300, "mr $sid 1 allies", 0, 0});
    script.push_back({900, "mr $sid 2 sten_mp", 0, 0});
    script.push_back({1800, "", 127, 0});
    script.push_back({2600, "", 0, 0});
    script.push_back({2800, "", 0, 1});
    script.push_back({3000, "", 0, 0});
    script.push_back({3200, "", 0, 1});
    script.push_back({3400, "", 0, 0});
    script.push_back({3600, "score", 0, 0});
}

void TestClient::dumpFrame(std::FILE* f, const Frame& frame, const Frame* old, int deltaNum, int snapFlags) {
    std::fprintf(f, "snap t %d delta %d flags %d\n", frame.serverTime - firstServerTime_, deltaNum ? 1 : 0, snapFlags);
    (void)old;
    static const std::vector<uint8_t> zes(kEntityStateSize, 0), zcs(kClientStateSize, 0);
    std::fprintf(f, " ps%s\n", dumpPlayerState(frame.ps.data(), firstServerTime_).c_str());
    for (auto& [n, e] : frame.entities) {
        std::fprintf(f, " ent %d%s\n", n, dumpFields(e.data(), zes.data(), kEntityStateFields, 59, firstServerTime_).c_str());
    }
    for (auto& [n, c] : frame.clients)
        std::fprintf(f, " client %d%s\n", n, dumpFields(c.data(), zcs.data(), kClientStateFields, 22, firstServerTime_).c_str());
}

std::string TestClient::summary() const {
    std::string s;
    char line[256];
    std::snprintf(line, sizeof line, "challenge %d qport %d gamestate %s configstrings %zu baselines %d clientNum %d "
                  "snapshots %d (delta %d) entities %d clients %d commands %zu\norigin %.1f %.1f %.1f -> %.1f %.1f %.1f%s%s\n",
                  challenge, qport, gamestateReceived ? "yes" : "no", configstrings.size(), baselines, clientNum,
                  snapshots, deltaSnapshots, entitiesInLastSnapshot, clientsInLastSnapshot, serverCommands.size(),
                  firstOrigin[0], firstOrigin[1], firstOrigin[2], lastOrigin[0], lastOrigin[1], lastOrigin[2],
                  error.empty() ? "" : "\nerror: ", error.c_str());
    s += line;
    for (auto& [k, v] : configstrings) {
        if (!verbose && k > 30) continue;
        std::snprintf(line, sizeof line, "  cs %d: %.150s\n", k, v.c_str());
        s += line;
    }
    return s;
}

bool TestClient::ok() const { return gamestateReceived && snapshots > 0 && error.empty(); }

} // namespace iw2::net
