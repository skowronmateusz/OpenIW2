// Implemented from the spec (docs/spec/net.md sections 4-7 and 10).
#include "net/gameclient.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <random>

#include "net/huffman.h"
#include "net/netfields.h"
#include "net/oob.h"
#include "net/playerstate.h"
#include "net/scramble.h"

namespace iw2::net {

namespace {
constexpr int kResendMs = 3000;       // the handshake resend interval (docs/spec/net.md section 10)
constexpr int kKeepAliveMs = 50;      // a packet at least every 50 ms once the gamestate is in
constexpr int kCommandsPerPacket = 4; // the newest commands go out again in the next packets (lost packets)
} // namespace

const char* clientStateName(ClientState s) {
    switch (s) {
    case ClientState::Disconnected: return "disconnected";
    case ClientState::Challenging: return "challenging";
    case ClientState::Connecting: return "connecting";
    case ClientState::Primed: return "primed";
    case ClientState::Active: return "active";
    }
    return "?";
}

GameClient::GameClient() = default;
GameClient::~GameClient() = default;

void GameClient::fail(const std::string& why) {
    error = why;
    if (dropReason.empty()) dropReason = "EXE_DISCONNECTED_FROM_SERVER";  // TODO(spec): the text of a lost connection
    state_ = ClientState::Disconnected;
    chan_.reset();
}

void GameClient::sendOob(const std::string& text) {
    auto pkt = makeOutOfBand(text);
    sock_.sendTo(server_, pkt.data(), static_cast<int>(pkt.size()));
}

bool GameClient::connect(const NetAddress& server, int64_t nowMs) {
    server_ = server;
    error.clear();
    dropReason.clear();
    dropFromServer = false;
    if (!sock_.isOpen() && !sock_.open(0, 0)) {
        error = "cannot open a UDP socket";
        return false;
    }
    state_ = ClientState::Challenging;
    connectStartMs_ = nowMs;
    lastOobMs_ = nowMs;
    // The server tells clients behind one address apart by the qport: pick a random one.
    qport_ = static_cast<int>(std::random_device{}() & 0xffff);
    sendOob("getchallenge 0 \"\"");
    return true;
}

void GameClient::disconnect() {
    if (chan_ && state_ >= ClientState::Primed) {
        addReliableCommand("disconnect");
        for (int i = 0; i < 3; ++i) transmit({}, lastSendMs_);
    }
    state_ = ClientState::Disconnected;
    chan_.reset();
}

void GameClient::addReliableCommand(const std::string& text) {
    ++reliableSequence_;
    reliableCommands_[static_cast<size_t>(reliableSequence_ & 127)] = text;
}

int GameClient::commandTime(int64_t nowMs) const {
    // docs/spec/net.md section 10 "Client time": real time + delta, never backwards; at or past the newest
    // snapshot's time - 5 the clock is extrapolating (the next adjust pulls it back).
    // Before the first snapshot of a level there is no server time yet: the commands carry the floor (0 after a
    // gamestate), as the original client's cleared state does.
    if (!latest_.valid) return clientTimeFloor_;
    int t = static_cast<int>(nowMs) + serverTimeDelta_;
    if (t < clientTimeFloor_) t = clientTimeFloor_;
    clientTimeFloor_ = t;
    if (latest_.valid && t >= latest_.serverTime - 5) extrapolated_ = true;
    return t;
}

void GameClient::adjustTimeDelta(int& delta, bool& extrapolated, int& clientTimeFloor, int snapTime, int oldSnapTime,
                                 int64_t nowMs) {
    const int period = snapTime - oldSnapTime;
    const int target = snapTime - static_cast<int>(nowMs) - period - 5;
    int diff = target - delta;
    bool thresholds = true;
    if (diff > 0) {
        // Behind the target: the snapshot interval itself is not a drift (a late snapshot moves the target by it).
        if (static_cast<unsigned>(period) <= 500) {
            diff -= period;
            if (diff < 0) thresholds = false;
        }
    } else {
        diff = -diff;
    }
    if (thresholds && diff > 500) {  // a new map, a stall: start again from this snapshot
        delta = target;
        clientTimeFloor = snapTime;
        return;
    }
    if (thresholds && diff > 100) {  // far off: half way
        delta = (delta + target) >> 1;
        return;
    }
    if (extrapolated) {  // the clock ran into the newest snapshot: back off
        extrapolated = false;
        delta -= 2;
    } else if (delta < target) {
        ++delta;
    } else if (delta > target) {
        --delta;
    }
}

void GameClient::sendCommand(UserCmd cmd, int64_t nowMs) {
    if (state_ < ClientState::Primed) return;
    // The command carries the client time as it is (never below the previous command's): two commands in the same
    // client millisecond, or while the clock is held at its floor, carry the same time and the server runs the first
    // only, as in the original. (Raised to the previous + 1 ms instead, they ran ahead of the clock while it was
    // held and the server ran the player in slow motion.)
    if (cmd.serverTime < lastCmdTime_) cmd.serverTime = lastCmdTime_;
    lastCmdTime_ = cmd.serverTime;
    cmds_[static_cast<size_t>(cmdNumber_ & 63)] = cmd;
    ++cmdNumber_;
    std::vector<UserCmd> out;
    // Only this level's commands: one of the old level would set the server's last command time to the old clock.
    for (int n = std::max(levelFirstCmd_, cmdNumber_ - kCommandsPerPacket); n < cmdNumber_; ++n)
        out.push_back(cmds_[static_cast<size_t>(n & 63)]);
    transmit(out, nowMs);
}

std::vector<UserCmd> GameClient::commandsAfter(int time) const {
    std::vector<UserCmd> out;
    for (int n = std::max(levelFirstCmd_, cmdNumber_ - 64); n < cmdNumber_; ++n) {
        const UserCmd& c = cmds_[static_cast<size_t>(n & 63)];
        if (c.serverTime > time) out.push_back(c);
    }
    return out;
}

const UserCmd* GameClient::lastCommand() const {
    return cmdNumber_ > levelFirstCmd_ ? &cmds_[static_cast<size_t>((cmdNumber_ - 1) & 63)] : nullptr;
}

void GameClient::transmit(const std::vector<UserCmd>& cmds, int64_t nowMs) {
    if (!chan_) return;
    lastSendMs_ = nowMs;
    uint8_t ops[4096];
    Msg body(ops, sizeof ops);
    for (int seq = reliableAcknowledged_ + 1; seq <= reliableSequence_; ++seq) {
        body.writeBits(2, 3);  // reliable client command
        body.writeLong(seq);
        body.writeString(reliableCommands_[static_cast<size_t>(seq & 127)]);
    }
    if (!cmds.empty() && state_ >= ClientState::Primed) {
        const uint32_t key = static_cast<uint32_t>(
            checksumFeed ^ messageAcknowledge_ ^
            commandHashKey(serverCommands_[static_cast<size_t>(serverCommandSequence_ & 127)], 32));
        body.writeBits(haveDelta_ ? 0 : 1, 3);  // user commands with / without a delta snapshot
        body.writeByte(static_cast<int>(cmds.size()));
        UserCmd from = latest_.valid ? usercmdBase(latest_.ps.data()) : UserCmd{};
        for (const UserCmd& c : cmds) {
            writeDeltaUsercmd(body, key, from, c);
            from = c;
        }
    }
    body.writeBits(3, 3);  // end
    uint8_t buf[1400];
    Msg m(buf, sizeof buf);
    m.writeByte(serverId);
    m.writeLong(messageAcknowledge_);
    m.writeLong(serverCommandSequence_);
    const int n = Huffman::protocol().compress(ops, body.cursize(), buf + m.cursize(), m.maxsize() - m.cursize());
    scramble(buf + m.cursize(), n, clientToServerKey(challenge_, serverId, messageAcknowledge_),
             serverCommands_[static_cast<size_t>(serverCommandSequence_ & 127)]);
    chan_->transmit(buf, m.cursize() + n);
}

void GameClient::poll(int64_t nowMs, int waitMs) {
    if (state_ == ClientState::Disconnected) return;
    if (!sock_.isOpen()) {
        fail("connection lost");
        return;
    }
    // The whole handshake has a deadline: a server that answers but never gets the client to the gamestate must not hang it.
    if (state_ < ClientState::Primed && nowMs - connectStartMs_ > connectTimeoutMs * 2) {
        fail("connection timed out");
        return;
    }
    static uint8_t buf[0x20000 + 16];
    for (int i = 0; i < 4096; ++i) { // bounded: a flood of packets must not keep the caller here
        NetAddress from;
        const int n = sock_.receive(buf, 0x20000, from, i == 0 ? waitMs : 0);
        if (n <= 0) break;
        if (!(from == server_)) continue;
        if (n >= 4 && std::memcmp(buf, "\xff\xff\xff\xff", 4) == 0) {
            const auto t = outOfBandText(buf, n);
            if (!t) continue;
            const auto tok = tokenize(*t);
            if (tok.empty()) continue;
            if (state_ == ClientState::Challenging && tok[0] == "challengeResponse" && tok.size() >= 2) {
                challenge_ = std::atoi(tok[1].c_str());
                state_ = ClientState::Connecting;
                lastOobMs_ = -kResendMs;  // send the connect now
            } else if (state_ == ClientState::Connecting && tok[0] == "connectResponse") {
                chan_ = std::make_unique<Netchan>(NetSide::Client, qport_, [this](const uint8_t* d, int len) {
                    sock_.sendTo(server_, d, len);
                });
                state_ = ClientState::Connecting;  // stays until the gamestate
                lastReceiveMs_ = nowMs;
            } else if (tok[0] == "error" && state_ <= ClientState::Connecting) {
                // "error\n<reason>": the connection was refused (0x40ef41: an ERR_DROP error with the text).
                std::string why = t->substr(std::min(t->size(), t->find('\n') == std::string::npos ? size_t(5) : t->find('\n') + 1));
                while (!why.empty() && (why.back() == '\n' || why.back() == '\r')) why.pop_back();
                dropReason = why;
                fail(*t);
                return;
            }
            continue;
        }
        if (!chan_) continue;
        lastReceiveMs_ = nowMs;
        handlePacket(buf, n, nowMs);
        if (dropped_) {
            dropped_ = false;
            fail(dropReason_.empty() ? std::string("disconnected by the server") : dropReason_);
        }
        if (state_ == ClientState::Disconnected) return;
    }
    // Handshake resends.
    if (state_ == ClientState::Challenging || (state_ == ClientState::Connecting && !chan_)) {
        if (nowMs - connectStartMs_ > connectTimeoutMs) {
            fail("connection timed out");
            return;
        }
        if (nowMs - lastOobMs_ >= kResendMs) {
            lastOobMs_ = nowMs;
            if (state_ == ClientState::Challenging) {
                sendOob("getchallenge 0 \"\"");
            } else {
                std::string info;
                infoSetValueForKey(info, "cl_punkbuster", "0");
                infoSetValueForKey(info, "cl_anonymous", "0");
                infoSetValueForKey(info, "snaps", "20");
                infoSetValueForKey(info, "rate", "25000");
                infoSetValueForKey(info, "name", name);
                if (!password.empty()) infoSetValueForKey(info, "password", password);
                infoSetValueForKey(info, "protocol", std::to_string(kProtocolVersion));
                infoSetValueForKey(info, "challenge", std::to_string(challenge_));
                infoSetValueForKey(info, "qport", std::to_string(qport_));
                sendOob("connect \"" + info + "\"");
            }
        }
        return;
    }
    if (chan_ && nowMs - lastReceiveMs_ > connectTimeoutMs * 4) {
        fail("server connection timed out");
        return;
    }
    // Keep-alive: acknowledgements (and, once primed, an idle command) while the application sends nothing.
    if (chan_ && nowMs - lastSendMs_ >= kKeepAliveMs) {
        if (state_ >= ClientState::Primed) {
            UserCmd c = lastCommand() ? *lastCommand() : UserCmd{};
            c.serverTime = commandTime(nowMs);
            c.forwardmove = c.rightmove = 0;
            sendCommand(c, nowMs);
        } else {
            transmit({}, nowMs);
        }
    }
}

void GameClient::waitForHandshake(const std::function<int64_t()>& nowMs, int waitMs, const std::function<void()>& idle) {
    while (state_ != ClientState::Disconnected && state_ < ClientState::Primed) {
        poll(nowMs(), waitMs);
        if (idle) idle();
    }
}

void GameClient::handlePacket(uint8_t* data, int len, int64_t nowMs) {
    Msg msg(data, 0x20000 + 16);
    msg.setCursize(len);
    if (!chan_->process(msg)) return;
    messageAcknowledge_ = chan_->incomingSequence();
    const int start = msg.readcount();
    if (len - start < 4) return;
    int32_t firstLong = 0;
    std::memcpy(&firstLong, data + start, 4);
    if (firstLong > reliableAcknowledged_ && firstLong <= reliableSequence_) reliableAcknowledged_ = firstLong;
    const int body = start + 4;
    scramble(data + body, msg.cursize() - body, serverToClientKey(challenge_, chan_->incomingSequence()),
             reliableCommands_[static_cast<size_t>(firstLong & 127)]);
    static uint8_t out[0x20000];
    const int outLen = Huffman::protocol().decompress(data + body, msg.cursize() - body, out, sizeof out);
    Msg dec(out, sizeof out);
    dec.setCursize(outLen);
    parseServerMessage(dec, nowMs);
}

void GameClient::parseServerMessage(Msg& msg, int64_t nowMs) {
    msg.beginReading();
    while (!msg.overflowed()) {
        const int cmd = msg.readByte();
        if (cmd < 0 || cmd == 7) return;
        switch (cmd) {
        case 0: break;
        case 1: parseGamestate(msg); return;
        case 4: {
            const int seq = msg.readLong();
            const std::string text = msg.readString();
            serverCommand(seq, text);
            if (state_ == ClientState::Disconnected) return;
            break;
        }
        case 6: parseSnapshot(msg, nowMs); break;
        default:
            fail("illegible server message " + std::to_string(cmd));
            return;
        }
    }
}

// Server commands (docs/spec/net.md section 10): the configstring updates are applied here, everything goes to the
// application queue.
void GameClient::serverCommand(int seq, const std::string& text) {
    if (seq <= serverCommandSequence_) return;  // already have it
    serverCommandSequence_ = seq;
    serverCommands_[static_cast<size_t>(seq & 127)] = text;
    if (verbose) std::printf("server command %d: %s\n", seq, text.c_str());
    // The server drops this client (kick, VAs in the 1.3 Linux dedicated server 0x808f226 / 0x808f252): letter 'w' with
    // the reason (a localized reference such as EXE_PLAYERKICKED), followed by the name and " PB" in one variant.
    // The client disconnects with that reason and shows it as its error dialog (docs/spec/net.md, client command `w`).
    if (text.size() >= 1 && text[0] == 'w' && (text.size() == 1 || text[1] == ' ')) {
        const auto tok = tokenize(text);
        dropReason_ = tok.size() >= 2 ? tok[1] : std::string();
        dropped_ = true;
    }
    if (text.size() >= 2 && text[1] == ' ' && (text[0] == 'd' || text[0] == 'x' || text[0] == 'y' || text[0] == 'z')) {
        // "<letter> <index> <text>": the text is the rest after the index.
        size_t p = 2;
        while (p < text.size() && text[p] == ' ') ++p;
        const size_t q = text.find(' ', p);
        const int index = std::atoi(text.substr(p, q == std::string::npos ? std::string::npos : q - p).c_str());
        const std::string rest = q == std::string::npos ? std::string() : text.substr(q + 1);
        if (index >= 0 && index < 2048) {
            if (text[0] == 'd') {
                configstrings[index] = rest;
            } else if (text[0] == 'x') {
                bigConfigstring_ = rest;
            } else if (text[0] == 'y') {
                bigConfigstring_ += rest;
            } else {
                configstrings[index] = bigConfigstring_ + rest;
                bigConfigstring_.clear();
            }
        }
    }
    // `w ["<reason>" [PB]]`: the server dropped this client (docs/spec/net.md section 14; client 0x401710 letter 0x77).
    if (!text.empty() && text[0] == 'w' && (text.size() == 1 || text[1] == ' ')) {
        const auto tok = tokenize(text);
        dropReason = tok.size() < 2 ? std::string("EXE_SERVER_DISCONNECTED")
                     : tok.size() > 2 && strcasecmp(tok[2].c_str(), "PB") == 0 ? "EXE_SERVERDISCONNECTREASON\x15" + tok[1]
                                                                                 : tok[1];
        dropFromServer = true;
        fail("disconnected by the server: " + dropReason);
        return;
    }
    newCommands_.push_back(text);
}

void GameClient::parseGamestate(Msg& msg) {
    serverCommandSequence_ = msg.readLong();
    configstrings.clear();
    baselines.clear();
    std::vector<uint8_t> zero(kEntityStateSize, 0);
    for (;;) {
        const int cmd = msg.readByte();
        if (msg.overflowed()) {
            fail("gamestate overflowed");
            return;
        }
        if (cmd == 7) break;
        if (cmd == 2) {
            const int index = msg.readShort();
            if (index < 0 || index >= 2048) {
                fail("configstring index out of range");
                return;
            }
            configstrings[index] = msg.readBigString();
        } else if (cmd == 3) {
            const int number = msg.readBits(kEntityNumberBits) & (kMaxEntities - 1);
            std::vector<uint8_t> state(kEntityStateSize, 0);
            readDeltaStructBody(msg, zero.data(), state.data(), kEntityStateSize, kEntityStateFields, 59);
            baselines[number] = state;
        } else {
            fail("bad gamestate command " + std::to_string(cmd));
            return;
        }
    }
    clientNum = msg.readLong();
    checksumFeed = msg.readLong();
    serverId = std::atoi(infoValueForKey(configstrings[1], "sv_serverid").c_str()) & 0xff;
    for (auto& f : frames_) f.valid = false;
    // A new gamestate (connect, map change, map_restart) starts the server's time again: the client clock, the
    // command time and the prediction's command history start again with it. Kept, a command time
    // of the old level held every new command at "previous + 1 ms", so the server ran the player in slow motion.
    serverTimeDelta_ = 0;
    clientTimeFloor_ = 0;
    extrapolated_ = false;
    lastCmdTime_ = 0;
    levelFirstCmd_ = cmdNumber_;
    latest_ = {};
    prev_ = {};
    haveDelta_ = false;
    ++gamestates;
    state_ = ClientState::Primed;
}

void GameClient::parseSnapshot(Msg& msg, int64_t nowMs) {
    ClientSnapshot frame;
    frame.messageNum = chan_->incomingSequence();
    frame.serverTime = msg.readLong();
    frame.receivedMs = nowMs;
    const int deltaNum = msg.readByte();
    msg.readByte();  // snapshot flags TODO(spec)
    const ClientSnapshot* old = nullptr;
    bool bad = false;
    if (deltaNum > 0) {
        const ClientSnapshot& f = frames_[static_cast<size_t>((frame.messageNum - deltaNum) & 31)];
        if (!f.valid || f.messageNum != frame.messageNum - deltaNum) bad = true;  // "Delta frame too old": dropped
        else old = &f;
        ++deltaSnapshots;
    }
    frame.ps.assign(kPlayerStateSize, 0);
    readDeltaPlayerState(msg, old ? old->ps.data() : nullptr, frame.ps.data());
    if (old) frame.entities = old->entities;
    std::vector<uint8_t> zero(kEntityStateSize, 0);
    for (;;) {
        const int number = msg.readBits(kEntityNumberBits) & (kMaxEntities - 1);
        if (msg.overflowed() || number == kEntityListEnd) break;
        const std::vector<uint8_t>* base = &zero;
        if (old) {
            auto it = old->entities.find(number);
            if (it != old->entities.end()) base = &it->second;
            else if (auto b = baselines.find(number); b != baselines.end()) base = &b->second;
        } else if (auto b = baselines.find(number); b != baselines.end()) {
            base = &b->second;
        }
        std::vector<uint8_t> state(kEntityStateSize);
        if (readDeltaStructBody(msg, base->data(), state.data(), kEntityStateSize, kEntityStateFields, 59) ==
            StructDelta::Removed)
            frame.entities.erase(number);
        else
            frame.entities[number] = std::move(state);
    }
    if (old) frame.clients = old->clients;
    std::vector<uint8_t> czero(kClientStateSize, 0);
    while (!msg.overflowed() && msg.readBit() == 1) {
        const int number = msg.readBits(kClientNumberBits) & 63;
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
            frame.clients[number] = std::move(state);
    }
    if (msg.overflowed() || bad) {
        ++droppedSnapshots;
        return;
    }
    frame.valid = true;
    ++snapshots;
    haveDelta_ = true;
    // Client clock (docs/spec/net.md section 10 "Client time"): the first snapshot sets the delta, every later one
    // adjusts it towards one snapshot interval + 5 ms behind the newest snapshot.
    if (snapshots == 1 || !latest_.valid) {
        serverTimeDelta_ = frame.serverTime - static_cast<int>(nowMs);
        clientTimeFloor_ = frame.serverTime;
        extrapolated_ = false;
    } else {
        // A reset of the clock (a stall of the server, a round restart that holds the server's frames: the client ran
        // ahead of the snapshots) sets the client time back to the snapshot. The commands go on from there, as the
        // original's carry the client time as it is; kept rising from the last command instead, every command of the next
        // half second went out 1 ms after the previous one and the server ran the player in slow motion.
        const int floorBefore = clientTimeFloor_;
        adjustTimeDelta(serverTimeDelta_, extrapolated_, clientTimeFloor_, frame.serverTime, latest_.serverTime, nowMs);
        if (clientTimeFloor_ < floorBefore && clientTimeFloor_ < lastCmdTime_) lastCmdTime_ = clientTimeFloor_;
    }

    if (frame.serverTime > latest_.serverTime || !latest_.valid) {
        prev_ = latest_;
        latest_ = frame;
    }
    frames_[static_cast<size_t>(frame.messageNum & 31)] = std::move(frame);
    if (state_ == ClientState::Primed) state_ = ClientState::Active;
}

bool GameClient::interpolation(int t, const ClientSnapshot*& a, const ClientSnapshot*& b, float& frac) const {
    if (!latest_.valid) return false;
    frac = 0;
    a = b = nullptr;
    // The pair around t from the kept frames (the cgame walks the received snapshots the same way, docs/spec/net.md
    // section 10): frames newer than the newest snapshot's time are stale (a restarted clock).
    for (const ClientSnapshot& f : frames_) {
        if (!f.valid || f.serverTime > latest_.serverTime) continue;
        if (f.serverTime <= t) {
            if (!a || f.serverTime > a->serverTime) a = &f;
        } else if (!b || f.serverTime < b->serverTime) {
            b = &f;
        }
    }
    if (!a && !b) a = b = &latest_;
    else if (!a) a = b;   // t before the oldest kept frame: that frame
    else if (!b) b = a;   // t past the newest: its state is held
    if (a != b) frac = static_cast<float>(t - a->serverTime) / static_cast<float>(b->serverTime - a->serverTime);
    return true;
}

} // namespace iw2::net
