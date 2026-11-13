// Dedicated-server protocol core, implemented from the spec (docs/spec/net.md sections 4-7): connectionless
// handshake, per-client netchan, gamestate, reliable commands in both directions, user commands, delta snapshots,
// timeouts. The game itself sits behind ServerGame; the core knows nothing about worlds or scripts.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "net/commandplayout.h"
#include "net/delta.h"
#include "net/netchan.h"
#include "net/oob.h"
#include "net/udp.h"
#include "util/logwatch.h"

namespace iw2::net {

// What one client sees in one snapshot.
struct SnapshotView {
    int serverTime = 0;
    std::vector<uint8_t> ps;                              // kPlayerStateSize bytes
    std::map<int, std::vector<uint8_t>> entities;         // number -> kEntityStateSize bytes
    std::map<int, std::vector<uint8_t>> clients;          // number -> kClientStateSize bytes
};

class ServerGame {
public:
    virtual ~ServerGame() = default;
    virtual std::map<int, std::string> configstrings() = 0;
    virtual std::map<int, std::vector<uint8_t>> baselines() { return {}; }
    // A client took a slot (false rejects it); begin: it acknowledged the gamestate and sent user commands.
    virtual bool clientConnect(int slot, const std::string& userinfo) = 0;
    virtual void clientBegin(int slot) = 0;
    virtual void clientCommand(int slot, const std::string& text) = 0;
    virtual void clientThink(int slot, const UserCmd& cmd) = 0;
    virtual void clientDisconnect(int slot) = 0;
    // Advances the game by one server frame of frameMs.
    virtual void runFrame(int frameMs) = 0;
    virtual int serverTime() const = 0;
    virtual SnapshotView snapshot(int slot) = 0;
    // Snapshots between two game frames (docs/spec/net.md section 10 "Snapshot rate"): prepareSubFrame is called once
    // for every snapshot time `serverTime() + subMs` (0 <= subMs < frame) before the snapshots of that time are built
    // with snapshotAt. subFrameAllowed: false while this client must get one snapshot per game frame.
    virtual void prepareSubFrame(int subMs) { (void)subMs; }
    virtual SnapshotView snapshotAt(int slot, int subMs) { (void)subMs; return snapshot(slot); }
    virtual bool subFrameAllowed(int slot) { (void)slot; return true; }
    // The time passed since the game's last frame when the game steps on its own clock (a listen host's world); -1:
    // the server's frame schedule tells.
    virtual int subFrameMs(int64_t nowMs) { (void)nowMs; return -1; }
    // The client number the gamestate tells the client (its own entity and client state number); the slot by default.
    virtual int clientNumber(int slot) { return slot; }
};

// Connected: waiting for a gamestate; Primed: the gamestate was sent, the client has not yet sent user commands of
// the current server id; Active: in the world (docs/spec/net.md section 4, "Map change").
enum class ServerClientState { Free, Connected, Primed, Active };

class Server {
public:
    explicit Server(ServerGame& game, uint32_t seed = 1);

    bool open(uint16_t port);
    // NET_OpenIP's port rule (docs/spec/net.md section 11, "Server socket"): every interface, port .. port + tries - 1, the
    // first one free. Returns the port bound, 0 when none was free (port 0 binds any free port).
    uint16_t openFirstFree(uint16_t port, int tries = 10);
    uint16_t port() const { return sock_.localPort(); }
    uint32_t boundIp() const { return sock_.localIp(); }

    // Receives and handles packets for up to waitMs, then runs the game frames that are due and sends snapshots.
    // nowMs is a monotonic clock in milliseconds supplied by the caller.
    void poll(int64_t nowMs, int waitMs);

    // Queues a reliable server command for one client (-1: all connected clients).
    void sendServerCommand(int slot, const std::string& text);
    void dropClient(int slot, const std::string& reason);
    // The server ends: every client is dropped with `reason` (EXE_SERVERQUIT, ...; docs/spec/net.md section 14).
    void shutdown(const std::string& reason);

    ServerInfo& info() { return oob_.info; }

    // Dedicated-server hooks (docs/spec/dedicated.md). oobHook sees every connectionless line first and returns true
    // when it handled it (rcon); statusHook fills the live player list and counts before getinfo / getstatus;
    // connectFilter returns a rejection reason ("" accepts) before a slot is given to the game.
    std::function<bool(const NetAddress& from, const std::string& line, int64_t nowMs)> oobHook;
    std::function<void(ServerInfo& info)> statusHook;
    std::function<std::string(const std::string& userinfo)> connectFilter;
    int privateClients = 0;        // sv_privateClients: the first slots, kept for sv_privatePassword
    std::string privatePassword;   // sv_privatePassword
    void sendOutOfBand(const NetAddress& to, const std::string& text);
    // A new map: every client goes back to connected and gets the gamestate of the new serverId.
    void newMap(int newServerId);
    int maxClients = 8;
    int frameMs = 50;            // sv_fps 20
    // sv_snaps (openiw2): the snapshots a second offered to a client that asks for more than 30 (userinfo `snaps`).
    // Above the frame rate the frame is cut into steps, and the snapshots between two game frames carry the time
    // passed since the frame (docs/spec/net.md section 10 "Snapshot rate").
    int snapshotHz = 60;
    int subSteps() const { return std::max(1, snapshotHz * frameMs / 1000); }
    int timeoutMs = 240000;      // sv_timeout 240 s (docs/spec/net.md section 4a)
    int zombieMs = 2000;         // sv_zombietime 2 s
    bool publicServer = false;   // dedicated 2: a LAN address does not lift the rate
    int maxRate = 0;             // sv_maxRate (0: no cap)
    bool floodProtect = true;    // sv_floodProtect
    // Commands of a client on a bursty path run on an even schedule (docs/spec/net.md section 10 "Commands on a bursty
    // path"); false (IW2_CMD_PLAIN=1): every command runs when its packet is read, as in the original.
    bool commandPlayout = true;
    int checksumFeed = 0;
    int serverId = 0;            // low byte of sv_serverid in configstring 1
    // Snapshot flags: 4 on the first map (observed with the 1.3 server); flipped on every new map as the classic
    // server-count bit. TODO(spec): the flip and the other bits.
    int snapFlags = 4;
    bool mapStarted_ = false;

    struct Client {
        int traceCmds = 0, tracePackets = 0; // IW2_NET_TRACE=2: commands accepted and packets read since the last snapshot
        uint32_t traceButtons = 0;   // IW2_NET_TRACE shot timing
        int64_t shotReadMs = -1;
        bool shotHeld = false;
        ServerClientState state = ServerClientState::Free;
        NetAddress address;
        int qport = 0;
        int challenge = 0;
        std::string userinfo;
        std::unique_ptr<Netchan> chan;
        int64_t lastPacketMs = 0;
        // Reliable server commands.
        std::array<std::string, 128> serverCommands;
        int reliableSequence = 0;
        int reliableAcknowledge = 0;
        // Reliable client commands.
        int lastClientCommand = 0;
        std::string lastClientCommandString;
        // Snapshots.
        int messageAcknowledge = 0;
        int deltaMessage = -1;
        int gamestateMessageNum = -1;
        int lastUsercmdTime = 0;
        bool clockResetAllowed = false;  // a server time jump: one older command may run (parseMove)
        CommandPlayout playout;          // the commands of a bursty path wait here (docs/spec/net.md section 10)
        std::vector<uint8_t> currentPs;
        struct Frame {
            int messageNum = -1;
            SnapshotView view;
            int64_t sentMs = 0;
            int64_t ackMs = -1;
        };
        std::array<Frame, 32> frames;
        int64_t nextSnapshotMs = 0;  // the next message of a client that is not active (real time)
        int nextSnapshotServerTime = 0; // the next snapshot of an active client (server time; docs/spec/net.md section 4a)
        int64_t nextReliableMs = 0;  // flood protection of client commands
        int64_t zombieUntilMs = 0;   // a dropped slot: reusable from then on, by others than the same address
        int timeoutCount = 0;        // consecutive failing timeout checks
        int rate = 5000;
        int snapshotMs = 50;         // 1000 / snaps
        bool subFrame = false;       // gets snapshots between the game frames
        std::string pendingDrop;     // dropped at the next send (a server command overflow)
        int lastSnapshotServerTime = -1;  // the server time of the last snapshot sent (one per game frame)
        int64_t lastSnapshotMs = 0;
        int ping = 999;  // mean round trip of the acknowledged snapshots; 999 when not active or none (section 4a)
    };
    const Client& client(int slot) const { return clients_[static_cast<size_t>(slot)]; }

private:
    void handleOutOfBand(const NetAddress& from, const uint8_t* data, int len, int64_t nowMs);
    void handlePacket(const NetAddress& from, uint8_t* data, int len, int64_t nowMs);
    void parseClientMessage(int slot, Msg& msg, int64_t nowMs);
    void parseMove(int slot, Msg& msg, bool delta, int64_t nowMs);
    void runDueCommands(int64_t frameMs);  // the held commands due before the game frame at frameMs
    void flushCommands();                  // every held command runs now and the holds start again
    int transmit(int slot, Msg& ops);  // returns the sent size (compressed + 4)
    void applyUserinfo(Client& c);
    void updatePing(Client& c);
    void sendGamestate(int slot);
    int sendSnapshot(int slot, int subMs = -1);
    void writeServerCommands(int slot, Msg& ops);

    ServerGame& game_;
    iw2::log::ServerWatch logWatch_; // the 10 s summary of the event lines (docs/logging.md)
    UdpSocket sock_;
    ConnectionlessServer oob_;
    std::vector<Client> clients_;
    // The entity baselines of the level: built once, for the first gamestate after newMap, and sent unchanged in every
    // gamestate of that level; the snapshots code a new entity against them (docs/spec/net.md section 4, "Gamestate").
    std::map<int, std::vector<uint8_t>> baselines_;
    bool baselinesBuilt_ = false;
    int64_t lastNowMs_ = 0;
    const bool frameTrace_ = [] { const char* e = std::getenv("IW2_NET_TRACE"); return e && std::atoi(e) >= 2; }();
    int64_t startMs_ = -1;
    int64_t nextFrameMs_ = 0;
    int lastSubTime_ = -1;  // the last snapshot time prepared between two frames
    int64_t clockResetUntilMs_ = -1;  // end of the window in which a client may run one older command
    int64_t timeDriftBase_ = INT64_MAX;  // real time minus server time at the last jump (or its lowest since)
    static constexpr int kTimeJumpMs = 500;           // a server behind its frame schedule by more: a time jump
    static constexpr int kClockResetMs = 400;         // a command older than the last one run by more: a clock reset
    static constexpr int kLateReadMs = 30;            // socket reads further apart: the receive time is the server's own delay
    static constexpr int kClockResetWindowMs = 3000;  // how long after a time jump that command is accepted
};

// docs/spec/net.md section 4a: loopback and private ranges (the original also counts local interface subnets).
bool isLanAddress(const NetAddress& a);

// The game's connect check (docs/spec/dedicated.md section 5): GAME_INVALIDPASSWORD when g_password is set, not `none`
// (any case) and differs from the userinfo "password" (case-sensitive); else empty.
std::string gamePasswordRejection(const std::string& gPassword, const std::string& userinfo);

} // namespace iw2::net
