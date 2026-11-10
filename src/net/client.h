// Minimal protocol test client (connect, gamestate, user commands, snapshots), implemented from the spec
// (docs/spec/net.md). Used by tools/iw2netclient and the interoperability tests; not a game client.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "net/delta.h"
#include "net/netchan.h"
#include "net/udp.h"

namespace iw2::net {

struct TestClientFrameView {
    int messageNum = 0;
    const std::vector<uint8_t>* ps = nullptr;
    const std::map<int, std::vector<uint8_t>>* entities = nullptr;
    const std::map<int, std::vector<uint8_t>>* clients = nullptr;
};

class TestClient {
public:
    bool verbose = false;
    std::string error;
    std::string name = "openiw2probe";
    // For tests with an injected clock: the time (default: the steady clock) and what runs when no packet is waiting
    // (default: wait on the socket).
    std::function<int64_t()> clock;
    std::function<void()> idle;

    bool connect(const NetAddress& server, int timeoutMs);
    // Queues a reliable client command (sent until the server acknowledges it).
    void addReliableCommand(const std::string& text);
    int reliableSequence = 0;     // last queued client command
    int reliableAcknowledged = 0; // last client command the server acknowledged
    void run(int ms);
    std::string summary() const;
    bool ok() const;

    // State learned from the server.
    int challenge = 0;
    int qport = 0;
    int serverCommandSequence = 0;
    int clientNum = -1;
    int checksumFeed = 0;
    bool gamestateReceived = false;
    std::map<int, std::string> configstrings;
    int baselines = 0;
    int snapshots = 0;
    std::vector<std::string> serverCommands;
    int entitiesInLastSnapshot = 0;
    int clientsInLastSnapshot = 0;
    int deltaSnapshots = 0;
    int lastServerTime = 0;
    float firstOrigin[3] = {0, 0, 0};
    float lastOrigin[3] = {0, 0, 0};
    bool haveOrigin = false;
    int forwardMove = 0;  // movement sent in user commands (-127..127)
    int buttons = 0;      // buttons sent in user commands

    // Structured dump of everything decoded (one record per line, times relative to the first snapshot) for the
    // differential wire tests: configstrings, baselines, snapshots with every non-zero field, server commands.
    std::FILE* dump = nullptr;
    // A scripted scenario, relative to the first snapshot: reliable commands ("$sid" becomes the server id) and
    // movement / button changes.
    struct Step {
        int atMs = 0;
        std::string command;  // empty: only movement / buttons
        int forward = 0;
        int buttons = 0;
    };
    std::vector<Step> script;
    // Observers for verification: every netchan packet from the server as received, and every decoded frame.
    std::function<void(const uint8_t*, int)> onRawPacket;
    std::function<void(const struct TestClientFrameView&)> onFrameView;
    // The text of the client's reliable command with this sequence number.
    std::string reliableCommandText(int seq) const { return reliableCommands_[seq & 127]; }
    const std::map<int, std::vector<uint8_t>>& baselineStates() const { return baselines_; }
    // The scenario used by the differential tests: team and weapon by script menu response, walk, fire.
    void addStandardScenario();

    struct Frame {
        bool valid = false;
        int messageNum = 0;
        int serverTime = 0;
        std::vector<uint8_t> ps;
        std::map<int, std::vector<uint8_t>> entities;
        std::map<int, std::vector<uint8_t>> clients;
    };
    const Frame* lastFrame() const;

private:
    bool waitOob(const std::string& prefix, std::string& text, int timeoutMs);
    void sendPacket();
    void handlePacket(uint8_t* data, int len);
    void parseServerMessage(Msg& msg);
    void parseGamestate(Msg& msg);
    void parseSnapshot(Msg& msg);
    UserCmd baseCommand() const;

    UdpSocket sock_;
    NetAddress server_;
    std::unique_ptr<Netchan> chan_;
    int serverId_ = 0;
    int messageAcknowledge_ = 0;
    int reliableAcknowledge_ = 0;
    std::string lastServerCommands_[128];
    std::string reliableCommands_[128];
    std::map<int, std::vector<uint8_t>> baselines_;
    Frame frames_[32];
    int lastFrameNum_ = -1;
    int lastCmdTime_ = 0;
    int64_t firstSnapshotMs_ = -1;
    int firstServerTime_ = 0;
    size_t nextStep_ = 0;
    void runScript();
    int64_t now() const;
    void dumpFrame(std::FILE* f, const Frame& frame, const Frame* old, int deltaNum, int snapFlags);
};

} // namespace iw2::net
