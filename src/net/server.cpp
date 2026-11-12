// Implemented from the spec (docs/spec/net.md sections 4-7).
#include "net/server.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "net/huffman.h"
#include "net/netfields.h"
#include "net/playerstate.h"
#include "net/scramble.h"

namespace iw2::net {

namespace {
constexpr int kOpGamestate = 1;
constexpr int kOpServerCommand = 4;
constexpr int kOpSnapshot = 6;
constexpr int kOpEnd = 7;
constexpr int kGsConfigstring = 2;
constexpr int kGsBaseline = 3;

constexpr int kClcMoveDelta = 0;
constexpr int kClcMoveNoDelta = 1;
constexpr int kClcCommand = 2;
constexpr int kClcEnd = 3;
} // namespace

Server::Server(ServerGame& game, uint32_t seed) : game_(game), oob_(seed) {
    if (std::getenv("IW2_CMD_PLAIN")) commandPlayout = false;
}

bool Server::open(uint16_t port) {
    clients_.clear();
    clients_.resize(static_cast<size_t>(maxClients));
    oob_.info.maxClients = maxClients;
    return sock_.open(0, port);
}

uint16_t Server::openFirstFree(uint16_t port, int tries) {
    for (int i = 0; i < tries && int(port) + i <= 65535; ++i)
        if (open(uint16_t(port + i))) return this->port();
    return 0;
}

void Server::sendServerCommand(int slot, const std::string& text) {
    for (int i = 0; i < maxClients; ++i) {
        if (slot >= 0 && i != slot) continue;
        Client& c = clients_[static_cast<size_t>(i)];
        if (c.state == ServerClientState::Free) continue;
        ++c.reliableSequence;
        // Exactly 129 unacknowledged commands: the client is told out of band and dropped at the next send; the slot
        // gets the drop command in place of this one (docs/spec/net.md section 4a).
        if (c.reliableSequence - c.reliableAcknowledge == 129) {
            sendOutOfBand(c.address, "disconnect");
            if (c.pendingDrop.empty()) c.pendingDrop = "EXE_SERVERCOMMANDOVERFLOW";
            c.serverCommands[static_cast<size_t>(c.reliableSequence & 127)] = "w \"EXE_SERVERCOMMANDOVERFLOW\"";
            continue;
        }
        c.serverCommands[static_cast<size_t>(c.reliableSequence & 127)] = text;
    }
}

void Server::shutdown(const std::string& reason) {
    for (size_t i = 0; i < clients_.size(); ++i)
        if (clients_[i].state != ServerClientState::Free) dropClient(static_cast<int>(i), reason);
}

bool isLanAddress(const NetAddress& a) {
    // IW2_NET_WAN=1 (docs/debug-flags.md): every peer counts as remote, so a test through a local relay gets the rate
    // and snaps limits an internet client gets.
    if (std::getenv("IW2_NET_WAN")) return false;
    const uint32_t ip = a.ip;
    const uint32_t b0 = ip >> 24;
    const uint32_t b1 = (ip >> 16) & 255;
    return b0 == 10 || b0 == 127 || (b0 == 169 && b1 == 254) || (b0 == 172 && (b1 & 0xf0) == 16) ||
           (b0 == 192 && b1 == 168);
}

std::string gamePasswordRejection(const std::string& gPassword, const std::string& userinfo) {
    std::string low = gPassword;
    for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (!gPassword.empty() && low != "none" && infoValueForKey(userinfo, "password") != gPassword) return "GAME_INVALIDPASSWORD";
    return {};
}

void Server::dropClient(int slot, const std::string& reason) {
    Client& c = clients_[static_cast<size_t>(slot)];
    if (c.state == ServerClientState::Free) return;
    // Tell the client once, as the server command `w` with the reason (VAs in the 1.3 Linux dedicated server 0x808f252;
    // the other variant at 0x808f226 puts the client's name before the reason and " PB" after it; docs/spec/net.md section 14).
    sendServerCommand(slot, "w \"" + reason + "\"");
    uint8_t buf[2048];
    Msg ops(buf, sizeof buf);
    writeServerCommands(slot, ops);
    ops.writeByte(kOpEnd);
    transmit(slot, ops);
    game_.clientDisconnect(slot);
    // The slot stays a zombie for sv_zombietime (docs/spec/net.md section 4a); a kick counts from now.
    const NetAddress addr = c.address;
    const int qport = c.qport;
    const int64_t base = reason == "EXE_PLAYERKICKED" ? lastNowMs_ : c.lastPacketMs;
    c = Client{};
    c.address = addr;
    c.qport = qport;
    c.zombieUntilMs = base + zombieMs;
}

void Server::applyUserinfo(Client& c) {
    // docs/spec/net.md section 4a "Rate and snaps".
    const std::string r = infoValueForKey(c.userinfo, "rate");
    c.rate = r.empty() ? 5000 : std::clamp(std::atoi(r.c_str()), 1000, 90000);
    if (isLanAddress(c.address) && !publicServer) c.rate = 99999;
    const std::string s = infoValueForKey(c.userinfo, "snaps");
    c.snapshotMs = s.empty() ? 50 : 1000 / std::clamp(std::atoi(s.c_str()), 1, 30);
}

void Server::updatePing(Client& c) {
    // docs/spec/net.md section 4a "Ping".
    if (c.state != ServerClientState::Active) {
        c.ping = 999;
        return;
    }
    int64_t sum = 0;
    int n = 0;
    for (const auto& fr : c.frames)
        if (fr.ackMs > 0) {
            sum += fr.ackMs - fr.sentMs;
            ++n;
        }
    c.ping = n ? static_cast<int>(std::min<int64_t>(sum / n, 999)) : 999;
}

void Server::sendOutOfBand(const NetAddress& to, const std::string& text) {
    auto pkt = makeOutOfBand(text);
    sock_.sendTo(to, pkt.data(), static_cast<int>(pkt.size()));
}

void Server::newMap(int newServerId) {
    serverId = newServerId;
    if (mapStarted_) snapFlags ^= 4;
    mapStarted_ = true;
    for (Client& c : clients_) {
        if (c.state == ServerClientState::Free) continue;
        c.state = ServerClientState::Connected;
        c.gamestateMessageNum = -1;
        c.deltaMessage = -1;
        c.lastUsercmdTime = 0;
        c.clockResetAllowed = false;
        c.playout.clear();
        c.currentPs.clear();
        c.lastSnapshotServerTime = -1;
        for (auto& f : c.frames) f = Client::Frame{};
    }
}

void Server::poll(int64_t nowMs, int waitMs) {
    if (startMs_ < 0) {
        startMs_ = nowMs;
        nextFrameMs_ = nowMs;
    }
    static uint8_t buf[kMaxPacketLen + 64];
    NetAddress from;
    // The wait for packets ends at the next frame: nowMs is the time before the wait, and the frame and its snapshots
    // run with it, so a longer wait would start the frame late by up to the rest of the wait (docs/spec/net.md section
    // 10 "Server cadence"; the 1.3 server sleeps until its next frame is due).
    if (waitMs > 0 && nextFrameMs_ - nowMs < waitMs) waitMs = static_cast<int>(std::max<int64_t>(0, nextFrameMs_ - nowMs));
    for (int wait = waitMs;; wait = 0) {
        int n = sock_.receive(buf, kMaxPacketLen, from, wait);
        if (n <= 0) break;
        if (n >= 4 && std::memcmp(buf, "\xff\xff\xff\xff", 4) == 0)
            handleOutOfBand(from, buf, n, nowMs);
        else
            handlePacket(from, buf, n, nowMs);
    }
    lastNowMs_ = nowMs;
    // A long frame (a fast restart rebuilds the level, a map load) leaves the server behind its frame schedule.
    bool timeJump = nowMs - nextFrameMs_ > kTimeJumpMs;
    bool framesRan = false;
    if (timeJump) flushCommands();
    while (nowMs >= nextFrameMs_) {
        runDueCommands(nextFrameMs_);
        game_.runFrame(frameMs);
        nextFrameMs_ += frameMs;
        framesRan = true;
    }
    // Frames that did not advance the time leave the server's time behind the real time.
    const int64_t drift = (nowMs - startMs_) - game_.serverTime();
    if (drift - timeDriftBase_ > kTimeJumpMs) timeJump = true;
    if (timeJump || drift < timeDriftBase_) timeDriftBase_ = drift;
    // After such a jump the clients' clocks are reset back (docs/spec/net.md section 10 "Client time"): each client
    // may run one older command within the window (parseMove).
    if (timeJump) {
        flushCommands();
        for (Client& c : clients_) c.clockResetAllowed = true;
        clockResetUntilMs_ = nowMs + kClockResetWindowMs;
    } else if (clockResetUntilMs_ >= 0 && nowMs >= clockResetUntilMs_) {
        for (Client& c : clients_) c.clockResetAllowed = false;
        clockResetUntilMs_ = -1;
    }
    for (int i = 0; i < maxClients; ++i) {
        Client& c = clients_[static_cast<size_t>(i)];
        if (c.state == ServerClientState::Free) continue;
        updatePing(c);
        if (c.lastPacketMs > nowMs) c.lastPacketMs = nowMs;
        // Once per server frame: more than five consecutive failing checks (docs/spec/net.md section 4a).
        if (framesRan) {
            if (nowMs - c.lastPacketMs > timeoutMs) {
                if (++c.timeoutCount > 5) {
                    dropClient(i, "EXE_TIMEDOUT");
                    clients_[static_cast<size_t>(i)].zombieUntilMs = 0;  // freed at once, without the zombie state
                    continue;
                }
            } else {
                c.timeoutCount = 0;
            }
        }
        if (!c.pendingDrop.empty()) {
            dropClient(i, c.pendingDrop);
            continue;
        }
        // A snapshot follows every game frame (the message loop runs after the frame; docs/spec/net.md section 10
        // "Server cadence"): one per new server time, so the snapshot times keep the frame spacing whatever the poll
        // rate is. A host that stops stepping its world still gets a message every second (its server commands).
        const int serverTime = game_.serverTime();
        const bool lan = isLanAddress(c.address);
        // Delay until the next message: nothing on LAN, otherwise the rate and snaps limits (section 4a).
        auto nextDelay = [&](int bytes) {
            if (lan) return 0;
            int rate = c.rate;
            if (maxRate != 0) rate = std::min(rate, std::max(maxRate, 1000));
            return std::max((std::min(bytes, 1500) * 1000 + 48000) / rate, c.snapshotMs);
        };
        // The delay counts in server time, as the 1.3 server's due time is its frame-stepped time plus the delay
        // (0x809ad66; the time advances by whole frames at 0x8096dc3): a delay of one frame is due at the next frame.
        // Counted in real time it fell a few ms after the frame now and then and the snapshot waited a whole frame
        // more. A server time that went back (a new level) is due at once.
        if (serverTime < c.lastSnapshotServerTime) c.nextSnapshotServerTime = serverTime;
        if (c.state == ServerClientState::Active &&
            ((serverTime >= c.nextSnapshotServerTime && serverTime != c.lastSnapshotServerTime) || nowMs - c.lastSnapshotMs >= 1000)) {
            const int messageNum = c.chan->outgoingSequence();
            const int bytes = sendSnapshot(i);
            if (c.shotReadMs >= 0) {
                static const bool shotTrace = std::getenv("IW2_NET_TRACE") != nullptr;
                if (shotTrace)
                    std::printf("NETTRACE shot slot=%d read->snapshot %lld ms (server time %d%s)\n", i, (long long)(nowMs - c.shotReadMs), serverTime,
                                c.shotHeld ? ", held" : "");
                c.shotReadMs = -1;
            }
            Client::Frame& sent = c.frames[static_cast<size_t>(messageNum & 31)];
            sent.sentMs = nowMs;
            sent.ackMs = -1;
            c.lastSnapshotServerTime = serverTime;
            c.lastSnapshotMs = nowMs;
            c.nextSnapshotServerTime = serverTime + nextDelay(bytes);
        } else if (c.state != ServerClientState::Active && nowMs >= c.nextSnapshotMs) {
            // A client without the world gets a message with no operation but the end mark: a client that lost the
            // gamestate acknowledges a later message and so gets it again (0x809adea). The pending server commands
            // wait for the active state.
            static uint8_t mbuf[kMaxMsgLen];
            Msg ops(mbuf, sizeof mbuf);
            ops.writeByte(kOpEnd);
            const int bytes = transmit(i, ops);
            c.nextSnapshotMs = nowMs + (lan ? frameMs : std::max(nextDelay(bytes), 1000));
        }
    }
}

void Server::handleOutOfBand(const NetAddress& from, const uint8_t* data, int len, int64_t nowMs) {
    auto line = parseOutOfBand(data, len);
    if (!line) return;
    oob_.time = static_cast<int>(nowMs - startMs_);
    if (oobHook) {
        if (oobHook(from, *line, nowMs)) return;
    }
    oob_.info.clients = 0;
    for (const Client& c : clients_)
        if (c.state != ServerClientState::Free) ++oob_.info.clients;
    if (statusHook) statusHook(oob_.info);
    std::optional<ConnectionlessServer::ConnectRequest> req;
    auto reply = oob_.handle({from.ip, from.port}, *line, &req);
    if (req) {
        // Reuse the slot of the same address and qport (a reconnect, also a zombie), else take a free one.
        int slot = -1;
        for (int i = 0; i < maxClients && slot < 0; ++i) {
            const Client& c = clients_[static_cast<size_t>(i)];
            if ((c.state != ServerClientState::Free || c.zombieUntilMs > nowMs) && c.address.ip == from.ip &&
                c.qport == req->qport)
                slot = i;
        }
        if (slot >= 0) {
            game_.clientDisconnect(slot);
            clients_[static_cast<size_t>(slot)] = Client{};
        }
        // Private slots (docs/spec/dedicated.md section 5): the search starts at 0 only with the private password.
        const int first = infoValueForKey(req->userinfo, "password") == privatePassword ? 0 : privateClients;
        for (int i = std::max(0, first); i < maxClients && slot < 0; ++i) {
            const Client& c = clients_[static_cast<size_t>(i)];
            if (c.state == ServerClientState::Free && c.zombieUntilMs <= nowMs) slot = i;
        }
        const std::string rejected = connectFilter ? connectFilter(req->userinfo) : std::string();
        if (slot < 0) {
            reply = std::string("error\nEXE_SERVERISFULL");
        } else if (!rejected.empty()) {
            std::printf("Game rejected a connection: %s.\n", rejected.c_str());
            reply = "error\n" + rejected;
        } else if (!game_.clientConnect(slot, req->userinfo)) {
            reply = std::string("error\nEXE_CANNOTJOIN");  // TODO(spec): the game's rejection text
        } else {
            Client& c = clients_[static_cast<size_t>(slot)];
            c.state = ServerClientState::Connected;
            c.address = from;
            c.qport = req->qport;
            c.challenge = req->challenge;
            c.userinfo = req->userinfo;
            c.lastPacketMs = nowMs;
            applyUserinfo(c);
            c.chan = std::make_unique<Netchan>(NetSide::Server, c.qport, [this, slot](const uint8_t* d, int n) {
                sock_.sendTo(clients_[static_cast<size_t>(slot)].address, d, n);
            });
        }
    }
    if (reply) {
        auto pkt = makeOutOfBand(*reply);
        sock_.sendTo(from, pkt.data(), static_cast<int>(pkt.size()));
    }
}

void Server::handlePacket(const NetAddress& from, uint8_t* data, int len, int64_t nowMs) {
    if (len < 6) return;
    int qport = data[4] | (data[5] << 8);
    for (int i = 0; i < maxClients; ++i) {
        Client& c = clients_[static_cast<size_t>(i)];
        if (c.state == ServerClientState::Free || c.address.ip != from.ip || c.qport != qport) continue;
        c.address.port = from.port;  // a changed source port moves the client (docs/spec/net.md section 4a)
        Msg msg(data, kMaxPacketLen + 64);
        msg.setCursize(len);
        if (!c.chan->process(msg)) return;
        c.lastPacketMs = nowMs;
        parseClientMessage(i, msg, nowMs);
        return;
    }
}

void Server::parseClientMessage(int slot, Msg& msg, int64_t nowMs) {
    Client& c = clients_[static_cast<size_t>(slot)];
    const int sid = msg.readByte();
    c.messageAcknowledge = msg.readLong();
    const int relAck = msg.readLong();
    if (msg.overflowed() || relAck < 0 || c.reliableSequence - relAck > 127) return;
    c.reliableAcknowledge = relAck;
    // The acknowledged frame's ack time is overwritten by every packet naming it (section 4a "Ping").
    {
        Client::Frame& f = c.frames[static_cast<size_t>(c.messageAcknowledge & 31)];
        if (f.messageNum == c.messageAcknowledge) f.ackMs = nowMs;
    }
    // A packet of another server id (an older map, or no gamestate yet) carries nothing the server uses: its client
    // commands and user commands are dropped (the client sends the commands again). The gamestate goes out again only
    // when the client acknowledged a message sent after it, i.e. it lost it (0x809114e). A client that has not yet been
    // sent the gamestate of this map is handled the same way even if its stale id happens to match.
    if (sid != serverId || c.state == ServerClientState::Connected) {
        if (c.gamestateMessageNum < c.messageAcknowledge) sendGamestate(slot);
        return;
    }
    const int start = msg.readcount();
    const int len = msg.cursize() - start;
    if (len <= 0) return;
    scramble(msg.data() + start, len, clientToServerKey(c.challenge, sid, c.messageAcknowledge),
             c.serverCommands[static_cast<size_t>(c.reliableAcknowledge & 127)]);
    static uint8_t out[kMaxMsgLen];
    int n = Huffman::protocol().decompress(msg.data() + start, len, out, sizeof out);
    Msg body(out, sizeof out);
    body.setCursize(n);
    for (;;) {
        int op = body.readBits(3) & 7;
        if (body.overflowed() || op == kClcEnd) break;
        if (op == kClcCommand) {
            int seq = body.readLong();
            std::string text = body.readString();
            if (seq <= c.lastClientCommand) continue;  // already executed
            if (seq > c.lastClientCommand + 1) {
                dropClient(slot, "EXE_LOSTRELIABLECOMMANDS");
                return;
            }
            // Flood protection (section 4a): team, score and mr commands never count.
            const bool floodable = text.rfind("team ", 0) != 0 && text.rfind("score ", 0) != 0 && text.rfind("mr ", 0) != 0;
            bool clientOk = true;
            if (c.state == ServerClientState::Active && floodProtect && nowMs < c.nextReliableMs && floodable) clientOk = false;
            if (floodable) c.nextReliableMs = nowMs + 800;
            c.lastClientCommand = seq;
            c.lastClientCommandString = text;
            if (text == "disconnect") {
                dropClient(slot, "EXE_DISCONNECTED");
                return;
            }
            if (text.rfind("userinfo ", 0) == 0) {
                const auto args = tokenize(text);
                if (args.size() > 1) {
                    c.userinfo = args[1];
                    applyUserinfo(c);
                }
            }
            if (clientOk) game_.clientCommand(slot, text);
        } else if (op == kClcMoveDelta || op == kClcMoveNoDelta) {
            parseMove(slot, body, op == kClcMoveDelta, nowMs);
            if (clients_[static_cast<size_t>(slot)].state == ServerClientState::Free) return;
        } else {
            std::fprintf(stderr, "WARNING: bad command byte %i for client %i\n", op, slot);
            break;
        }
    }
}

void Server::parseMove(int slot, Msg& msg, bool delta, int64_t nowMs) {
    Client& c = clients_[static_cast<size_t>(slot)];
    c.deltaMessage = delta ? c.messageAcknowledge : -1;
    int count = msg.readByte();
    if (count < 1 || count > 32) return;
    const int32_t key = checksumFeed ^ c.messageAcknowledge ^
                        commandHashKey(c.serverCommands[static_cast<size_t>(c.reliableAcknowledge & 127)]);
    UserCmd from = c.currentPs.empty() ? UserCmd{} : usercmdBase(c.currentPs.data());
    std::vector<UserCmd> cmds;
    for (int i = 0; i < count; ++i) {
        UserCmd cmd;
        readDeltaUsercmd(msg, static_cast<uint32_t>(key), from, cmd);
        cmds.push_back(cmd);
        from = cmd;
    }
    if (msg.overflowed()) return;
    if (c.state == ServerClientState::Primed) {
        c.state = ServerClientState::Active;
        c.deltaMessage = -1;
        game_.clientBegin(slot);
    }
    // 0x8091091..0x8091147: a command newer than the packet's last one is skipped (sent before the client set its
    // clock back), and so is one not newer than the last command run (sent again, or in the same client millisecond).
    // openiw2: after a jump of the server's own time (a frame that ran over kTimeJumpMs, docs/spec/net.md section 10
    // "Client time") the client's clock is reset back and its commands fall behind the last one run. The first command
    // more than kClockResetMs older than the last one runs then, once per client and jump; otherwise the 1.3 rule holds,
    // so old commands sent again never run twice.
    // openiw2: on a bursty path the accepted commands wait for their turn (runDueCommands) instead of running now.
    bool hold = false;
    if (commandPlayout) {
        // A packet read after a pause of the server's own socket reads (a long frame) says nothing about the path.
        if (lastNowMs_ > 0 && nowMs - lastNowMs_ <= kLateReadMs) c.playout.packet(nowMs, cmds.back().serverTime);
        hold = c.playout.holding();
    }
    for (const UserCmd& cmd : cmds) {
        if (cmd.serverTime > cmds.back().serverTime) continue;
        if (cmd.serverTime <= c.lastUsercmdTime) {
            static const bool netTrace = std::getenv("IW2_NET_TRACE") != nullptr;
            if (netTrace && cmd.serverTime < c.lastUsercmdTime - 60 && cmd.serverTime == cmds.back().serverTime)
                std::printf("NETTRACE stale slot=%d cmd=%d last=%d\n", slot, cmd.serverTime, c.lastUsercmdTime);
            if (!c.clockResetAllowed || c.lastUsercmdTime - cmd.serverTime <= kClockResetMs) continue;
            c.clockResetAllowed = false;
        }
        c.lastUsercmdTime = cmd.serverTime;
        if (hold) c.playout.push(cmd);
        else game_.clientThink(slot, cmd);
        // IW2_NET_TRACE: an attack press (the button's rising edge) is timed from the read of its packet to the snapshot
        // that carries its result (docs/debug-flags.md).
        if ((cmd.buttons & 1) && !(c.traceButtons & 1)) c.shotReadMs = nowMs, c.shotHeld = hold;
        c.traceButtons = cmd.buttons;
    }
}

void Server::runDueCommands(int64_t frameMs) {
    static const bool netTrace = std::getenv("IW2_NET_TRACE") != nullptr;
    for (size_t n = 0; n < clients_.size(); ++n) {
        Client& c = clients_[n];
        const int i = static_cast<int>(n);
        if (c.state != ServerClientState::Active) continue;
        const int before = c.playout.holdMs();
        for (const UserCmd& cmd : c.playout.due(frameMs)) {
            if (c.state != ServerClientState::Active) break;  // dropped by one of its own commands
            game_.clientThink(i, cmd);
        }
        if (netTrace && (before == 0) != (c.playout.holdMs() == 0))
            std::printf("NETTRACE %s slot=%d\n", before == 0 ? "hold" : "release", i);
    }
}

void Server::flushCommands() {
    for (size_t n = 0; n < clients_.size(); ++n) {
        Client& c = clients_[n];
        for (const UserCmd& cmd : c.playout.flush()) {
            if (c.state != ServerClientState::Active) break;
            game_.clientThink(static_cast<int>(n), cmd);
        }
        c.playout.reset();
    }
}

int Server::transmit(int slot, Msg& ops) {
    Client& c = clients_[static_cast<size_t>(slot)];
    static uint8_t buf[kMaxMsgLen + 8];
    std::memcpy(buf, &c.lastClientCommand, 4);
    int n = Huffman::protocol().compress(ops.data(), ops.cursize(), buf + 4, kMaxMsgLen);
    scramble(buf + 4, n, serverToClientKey(c.challenge, c.chan->outgoingSequence()), c.lastClientCommandString);
    c.chan->transmit(buf, n + 4);
    while (c.chan->hasPendingFragments()) c.chan->transmitNextFragment();
    return n + 4;
}

void Server::writeServerCommands(int slot, Msg& ops) {
    Client& c = clients_[static_cast<size_t>(slot)];
    for (int seq = c.reliableAcknowledge + 1; seq <= c.reliableSequence; ++seq) {
        ops.writeByte(kOpServerCommand);
        ops.writeLong(seq);
        ops.writeString(c.serverCommands[static_cast<size_t>(seq & 127)]);
    }
}

void Server::sendGamestate(int slot) {
    Client& c = clients_[static_cast<size_t>(slot)];
    static uint8_t buf[kMaxMsgLen];
    Msg ops(buf, sizeof buf);
    // The pending commands go first (their texts key the client's next packets, section 6), then the gamestate,
    // whose command sequence covers them.
    writeServerCommands(slot, ops);
    c.reliableAcknowledge = c.reliableSequence;
    ops.writeByte(kOpGamestate);
    ops.writeLong(c.reliableSequence);
    for (const auto& [index, text] : game_.configstrings()) {
        if (text.empty()) continue;
        ops.writeByte(kGsConfigstring);
        ops.writeShort(index);
        ops.writeBigString(text);
    }
    std::vector<uint8_t> zero(kEntityStateSize, 0);
    baselines_ = game_.baselines();
    for (const auto& [number, state] : baselines_) {
        ops.writeByte(kGsBaseline);
        ops.writeBits(number, kEntityNumberBits);
        writeDeltaStruct(ops, zero.data(), state.data(), true, kEntityStateFields, 59, number, 0, false);
    }
    ops.writeByte(kOpEnd);
    ops.writeLong(game_.clientNumber(slot));
    ops.writeLong(checksumFeed);
    ops.writeByte(kOpEnd);
    c.gamestateMessageNum = c.chan->outgoingSequence();
    c.state = ServerClientState::Primed;
    transmit(slot, ops);
}

int Server::sendSnapshot(int slot) {
    Client& c = clients_[static_cast<size_t>(slot)];
    const int messageNum = c.chan->outgoingSequence();
    Client::Frame& frame = c.frames[static_cast<size_t>(messageNum & 31)];
    frame.messageNum = messageNum;
    frame.view = game_.snapshot(slot);
    frame.view.serverTime = game_.serverTime();
    c.currentPs = frame.view.ps;

    const Client::Frame* old = nullptr;
    // A delta request older than 28 messages is ignored (section 4a); the frames kept are full copies, so the
    // original's check against its shared entity ring has no equivalent here.
    if (c.deltaMessage > 0 && messageNum - c.deltaMessage <= 28 && messageNum - c.deltaMessage > 0) {
        const Client::Frame& f = c.frames[static_cast<size_t>(c.deltaMessage & 31)];
        if (f.messageNum == c.deltaMessage) old = &f;
    }
    static uint8_t buf[kMaxMsgLen];
    Msg ops(buf, sizeof buf);
    writeServerCommands(slot, ops);
    ops.writeByte(kOpSnapshot);
    ops.writeLong(frame.view.serverTime);
    ops.writeByte(old ? messageNum - old->messageNum : 0);
    ops.writeByte(snapFlags);  // docs/spec/net.md section 11, "Player state"
    writeDeltaPlayerState(ops, old ? old->view.ps.data() : nullptr, frame.view.ps.data());

    // Entities against the delta frame, or the baseline / zero for new ones; removed ones are named with the
    // removed bit; 1023 ends the list.
    static const std::vector<uint8_t> zeroEnt(kEntityStateSize, 0);
    const auto& now = frame.view.entities;
    const std::map<int, std::vector<uint8_t>> none;
    const auto& before = old ? old->view.entities : none;
    auto itNew = now.begin();
    auto itOld = before.begin();
    while (itNew != now.end() || itOld != before.end()) {
        int nNew = itNew != now.end() ? itNew->first : 99999;
        int nOld = itOld != before.end() ? itOld->first : 99999;
        if (nNew == nOld) {
            writeDeltaStruct(ops, itOld->second.data(), itNew->second.data(), false, kEntityStateFields, 59, nNew,
                             kEntityNumberBits, false);
            ++itNew;
            ++itOld;
        } else if (nNew < nOld) {
            auto base = baselines_.find(nNew);
            writeDeltaStruct(ops, base != baselines_.end() ? base->second.data() : zeroEnt.data(),
                             itNew->second.data(), true, kEntityStateFields, 59, nNew, kEntityNumberBits, false);
            ++itNew;
        } else {
            writeDeltaStruct(ops, itOld->second.data(), nullptr, true, kEntityStateFields, 59, nOld,
                             kEntityNumberBits, false);
            ++itOld;
        }
    }
    ops.writeBits(kEntityListEnd, kEntityNumberBits);

    static const std::vector<uint8_t> zeroCl(kClientStateSize, 0);
    const auto& cnow = frame.view.clients;
    const auto& cbefore = old ? old->view.clients : none;
    auto cn = cnow.begin();
    auto co = cbefore.begin();
    while (cn != cnow.end() || co != cbefore.end()) {
        int a = cn != cnow.end() ? cn->first : 99999;
        int b = co != cbefore.end() ? co->first : 99999;
        if (a == b) {
            writeDeltaStruct(ops, co->second.data(), cn->second.data(), false, kClientStateFields, 22, a,
                             kClientNumberBits, true);
            ++cn;
            ++co;
        } else if (a < b) {
            writeDeltaStruct(ops, zeroCl.data(), cn->second.data(), true, kClientStateFields, 22, a,
                             kClientNumberBits, true);
            ++cn;
        } else {
            writeDeltaStruct(ops, co->second.data(), nullptr, true, kClientStateFields, 22, b, kClientNumberBits,
                             true);
            ++co;
        }
    }
    ops.writeBit0();
    ops.writeByte(kOpEnd);
    return transmit(slot, ops);
}

} // namespace iw2::net
