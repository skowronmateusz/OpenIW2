// Game client side of the patch 1.3 protocol, implemented from the spec (docs/spec/net.md sections 4-7 and 10):
// a non-blocking connection state machine, gamestate and configstrings (with the `d` / `x` / `y` / `z` updates),
// reliable commands both ways, user commands with a history for prediction, delta snapshots and the snapshot pair
// for interpolation. Knows nothing about worlds or rendering (see src/netgame for that layer).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <utility>

#include "net/delta.h"
#include "net/msg.h"
#include "net/netchan.h"
#include "net/udp.h"

namespace iw2::net {

enum class ClientState { Disconnected, Challenging, Connecting, Primed, Active };
const char* clientStateName(ClientState s);

struct ClientSnapshot {
    bool valid = false;
    int messageNum = 0;
    int serverTime = 0;
    int64_t receivedMs = 0;
    std::vector<uint8_t> ps;                              // kPlayerStateSize bytes
    std::map<int, std::vector<uint8_t>> entities;         // number -> kEntityStateSize bytes
    std::map<int, std::vector<uint8_t>> clients;          // number -> kClientStateSize bytes
};

class GameClient {
public:
    GameClient();
    ~GameClient();

    std::string name = "player";
    std::string error;        // why the client disconnected
    // The reason for the error dialog (docs/spec/net.md section 14): a localization key, parameters behind 0x15, or plain
    // text. dropFromServer: the server ended the connection (`w` server command), else the connection was refused or lost.
    std::string dropReason;
    bool dropFromServer = false;
    std::string password;     // userinfo "password" (the `password` dvar, ui_mp/password.menu); empty: not sent
    // No answer to the handshake (the connection is lost after 4x this). cl_connectTimeout of the original defaults to 200 s
    // (VA 0x410a7d in the patch 1.3 MP executable), for a connect screen that keeps the frame running; this client blocks
    // while it connects, so the bound is shorter on purpose.
    int connectTimeoutMs = 10000;
    // Blocks until primed or disconnected, bounded by the handshake deadline (at most 2x connectTimeoutMs from connect).
    void waitForHandshake(const std::function<int64_t()>& nowMs, int waitMs, const std::function<void()>& idle = {});

    // Opens a socket and starts the handshake (challenging). nowMs: a monotonic clock in milliseconds.
    bool connect(const NetAddress& server, int64_t nowMs);
    // Sends the disconnect command a few times and closes.
    void disconnect();
    // Receives everything that arrived (waiting up to waitMs for the first packet), resends handshake packets and
    // keeps the connection alive while the application sends no user commands.
    void poll(int64_t nowMs, int waitMs = 0);

    ClientState state() const { return state_; }
    const NetAddress& server() const { return server_; }

    // Gamestate.
    std::map<int, std::string> configstrings;
    int clientNum = -1;
    int serverId = 0;
    int checksumFeed = 0;
    std::map<int, std::vector<uint8_t>> baselines;
    int gamestates = 0;  // gamestates received (a map change sends a new one)

    // Server commands in arrival order, taken by the application (the configstring updates are applied already).
    std::vector<std::string> takeServerCommands() { return std::exchange(newCommands_, {}); }
    void addReliableCommand(const std::string& text);
    int reliableSequence() const { return reliableSequence_; }
    int reliableAcknowledged() const { return reliableAcknowledged_; }

    // Client clock (docs/spec/net.md section 10, "Client time"): the client's server time now = real time + the
    // time delta, never earlier than the last value given. It is the serverTime of a user command created now and
    // the time the other entities are drawn at, so the server's antilag (docs/spec/mp-hud.md "Antilag") sees what
    // the player saw. Marks the clock as extrapolating when it reaches the newest snapshot's time - 5 ms.
    int commandTime(int64_t nowMs) const;
    int renderTime(int64_t nowMs) const { return commandTime(nowMs); }
    int timeDelta() const { return serverTimeDelta_; }
    bool extrapolating() const { return extrapolated_; }
    // The time delta rule of the 1.3 client, applied once per received snapshot (docs/spec/net.md section 10):
    // the target delta puts the client time one snapshot interval + 5 ms behind the newest snapshot; a difference
    // over 500 ms resets, over 100 ms averages, otherwise 1 ms per snapshot (2 ms back when extrapolating).
    // Exposed for the tests; parseSnapshot calls it.
    static void adjustTimeDelta(int& delta, bool& extrapolated, int& clientTimeFloor, int snapTime, int oldSnapTime,
                                int64_t nowMs);

    // Stores the command (its serverTime is forced above the previous one) and sends a packet with the newest
    // commands. Ignored before the gamestate.
    void sendCommand(UserCmd cmd, int64_t nowMs);
    // Commands sent so far with serverTime > time, oldest first (at most the last 64).
    std::vector<UserCmd> commandsAfter(int time) const;
    const UserCmd* lastCommand() const;

    // Snapshots.
    const ClientSnapshot* latest() const { return latest_.valid ? &latest_ : nullptr; }
    const ClientSnapshot* previous() const { return prev_.valid ? &prev_ : nullptr; }
    // The two snapshots around time t (the newest one at or before t and the oldest one after it, from the 32 kept)
    // and the fraction between them (0 = a, 1 = b). With one snapshot, t past the newest (the state is held) or t
    // before the oldest, a == b and frac == 0.
    bool interpolation(int t, const ClientSnapshot*& a, const ClientSnapshot*& b, float& frac) const;
    int snapshots = 0;
    int deltaSnapshots = 0;
    int droppedSnapshots = 0;
    bool verbose = false;

    // Tests: sends these commands as they are (a modified client replaying old command times).
    void sendRawCommandsForTest(const std::vector<UserCmd>& cmds, int64_t nowMs) { transmit(cmds, nowMs); }

private:
    void sendOob(const std::string& text);
    void transmit(const std::vector<UserCmd>& cmds, int64_t nowMs);
    void handlePacket(uint8_t* data, int len, int64_t nowMs);
    void parseServerMessage(Msg& msg, int64_t nowMs);
    void parseGamestate(Msg& msg);
    void parseSnapshot(Msg& msg, int64_t nowMs);
    void serverCommand(int seq, const std::string& text);
    void fail(const std::string& why);

    ClientState state_ = ClientState::Disconnected;
    UdpSocket sock_;
    NetAddress server_;
    std::unique_ptr<Netchan> chan_;
    int challenge_ = 0;
    int qport_ = 0;
    int64_t connectStartMs_ = 0;
    int64_t lastOobMs_ = 0;
    int64_t lastSendMs_ = 0;
    int64_t lastReceiveMs_ = 0;
    int messageAcknowledge_ = 0;
    int serverCommandSequence_ = 0;
    std::array<std::string, 128> serverCommands_;
    int reliableSequence_ = 0;
    int reliableAcknowledged_ = 0;
    std::array<std::string, 128> reliableCommands_;
    std::vector<std::string> newCommands_;
    std::string bigConfigstring_;
    std::string dropReason_;  // the server dropped this client (the "w" command), applied after the packet
    bool dropped_ = false;
    std::array<ClientSnapshot, 32> frames_;
    ClientSnapshot latest_, prev_;
    bool haveDelta_ = false;
    int serverTimeDelta_ = 0;
    mutable int clientTimeFloor_ = 0;    // the last client time given out: the clock never goes backwards
    mutable bool extrapolated_ = false;  // the client time reached the newest snapshot (consumed by the next adjust)
    std::array<UserCmd, 64> cmds_{};
    int cmdNumber_ = 0;  // commands stored so far
    int lastCmdTime_ = 0;
    int levelFirstCmd_ = 0; // the first command of the current gamestate (older ones carry the old level's times)
};

} // namespace iw2::net
