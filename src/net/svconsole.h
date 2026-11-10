// Dedicated-server console: commands, cvars, map rotation, status and the remote console. Implemented from the spec
// (docs/spec/dedicated.md). The console knows nothing about worlds or sockets; the server behind it is a ConsoleHost.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace iw2::net {

// One used client slot as the status command and the server queries show it.
struct ConsoleClient {
    enum class State { Connecting, Active, Zombie };
    int num = 0;
    int score = 0;
    int ping = 0;
    State state = State::Active;
    std::string name;
    int lastMsgMs = 0;
    std::string address;
    int qport = 0;
    int rate = 0;
    std::string userinfo;
    bool bot = false;
};

class ConsoleHost {
public:
    virtual ~ConsoleHost() = default;
    virtual bool running() const = 0;
    virtual std::string mapName() const = 0;
    // Loads a map with the console's g_gametype; false when the map does not exist (the current map keeps running).
    virtual bool loadMap(const std::string& map) = 0;
    virtual void restartMap(bool fast) = 0;
    virtual std::vector<ConsoleClient> clients() = 0;
    virtual void kick(int num, const std::string& reason) = 0;
    // Reliable server command to one client (-1: all clients).
    virtual void serverCommand(int num, const std::string& text) = 0;
    virtual bool readFile(const std::string& name, std::string& out) = 0;
    virtual void quit() = 0;
    virtual bool addTestClient() { return false; }
    // Debug (env IW2_DEBUG_DAMAGE=1, docs/debug-flags.md): damages client victim through the MP damage path.
    virtual bool debugDamage(int victim, int attacker, int amount) { (void)victim, (void)attacker, (void)amount; return false; }
    virtual void cvarChanged(const std::string& name, const std::string& value) { (void)name, (void)value; }
};

struct CaseLess {
    bool operator()(const std::string& a, const std::string& b) const;
};

// Removes ^N colour codes.
std::string stripColors(const std::string& s);

// The status table of docs/spec/dedicated.md section 3.
std::string formatStatus(const std::string& mapName, const std::vector<ConsoleClient>& clients);

// Result of one rotation step (docs/spec/dedicated.md section 2): the map to load, or none (map_restart).
struct RotationStep {
    std::optional<std::string> map;
    std::optional<std::string> gametype;  // set when a gametype keyword was read
    std::string log;                      // the lines the original prints
};
// Advances current (sv_mapRotationCurrent) through rotation (sv_mapRotation).
RotationStep rotateMap(std::string& current, const std::string& rotation);

class ServerConsole {
public:
    explicit ServerConsole(ConsoleHost& host);

    // Output of commands (stdout by default); while an rcon request runs it goes to the requester.
    std::function<void(const std::string&)> print;

    std::map<std::string, std::string, CaseLess> cvars;
    std::string cvar(const std::string& name) const;
    void setCvar(const std::string& name, const std::string& value);

    // Runs a command buffer: commands separated by newlines or ';' outside quotes.
    void execute(const std::string& text);
    // Runs text and returns what it printed.
    std::string capture(const std::string& text);
    void mapRotate();

    // A connectionless `rcon <password> <command…>` line (docs/spec/dedicated.md section 4). Returns the reply
    // texts to send back (each is already `print\n…`); nothing when the request is dropped by the rate limit.
    std::optional<std::vector<std::string>> rcon(const std::string& line, const std::string& from, int64_t nowMs);

    // The info string of the server-info cvars (serverinfo command, getstatus).
    std::string serverInfo() const;

private:
    void command(const std::vector<std::string>& args, const std::string& argsText);
    void out(const std::string& s) { print(s); }
    ConsoleHost& host_;
    int64_t lastRconMs_ = 0;
    bool haveRcon_ = false;
    int execDepth_ = 0;
};

} // namespace iw2::net
