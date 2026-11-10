// Connectionless (out-of-band) packets, implemented from the spec (docs/spec/net.md section 5).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace iw2::net {

constexpr int kProtocolVersion = 118;
constexpr const char* kProtocolVersionString = "1.3";
constexpr uint16_t kDefaultPort = 28960;

// Splits a command line into tokens (whitespace, // and /* */ comments, double quotes).
std::vector<std::string> tokenize(const std::string& line);

// \key\value info strings.
std::string infoValueForKey(const std::string& info, const std::string& key);
void infoSetValueForKey(std::string& info, const std::string& key, const std::string& value);

// Builds the bytes of an out-of-band packet: FF FF FF FF followed by text, no terminating zero.
std::vector<uint8_t> makeOutOfBand(const std::string& text);
// If the packet is out-of-band, returns its first line as read by the line reader ('%' -> '.', stops at '\n').
std::optional<std::string> parseOutOfBand(const uint8_t* data, int len);
// The whole text after the FF FF FF FF prefix (for replies with several lines).
std::optional<std::string> outOfBandText(const uint8_t* data, int len);

struct PlayerStatus {
    int score = 0;
    int ping = 0;
    std::string name;
};

struct ServerInfo {
    std::string hostname = "openiw2";
    std::string mapname;
    std::string gametype = "dm";
    int clients = 0;
    int maxClients = 20;
    bool pure = false;
    int minPing = 0;
    int maxPing = 0;
    std::string fsGame;
    bool allowAnonymous = false;
    bool conDisabled = false;
    bool password = false;
    int friendlyFire = 0;
    int killcam = 0;
    bool mod = false;
    bool voice = false;
    // Server-info cvars for getstatus (in addition to challenge / pswrd / mod).
    std::vector<std::pair<std::string, std::string>> statusCvars;
    std::vector<PlayerStatus> players;
};

std::string buildInfoResponse(const ServerInfo& info, const std::string& challenge);
std::string buildStatusResponse(const ServerInfo& info, const std::string& challenge);

// Handles getinfo / getstatus / getchallenge / connect (protocol and challenge checks) without authorisation.
class ConnectionlessServer {
public:
    struct Address {
        uint32_t ip = 0;
        uint16_t port = 0;
        bool operator==(const Address& o) const { return ip == o.ip && port == o.port; }
    };
    struct ConnectRequest {
        Address from;
        std::string userinfo;
        int qport = 0;
        int challenge = 0;
    };

    explicit ConnectionlessServer(uint32_t seed = 1) : rng_(seed) {}

    ServerInfo info;
    int time = 0;  // server time in ms, used for the challenge value and slot age

    // Returns the reply text (to send with makeOutOfBand), or nothing. An accepted connect is reported in accepted.
    std::optional<std::string> handle(const Address& from, const std::string& line,
                                      std::optional<ConnectRequest>* accepted = nullptr);

    int challengeFor(const Address& from) const;

private:
    struct Challenge {
        Address adr;
        int challenge = 0;
        int time = 0;
        bool used = false;
        std::string guid;
    };
    int random15();
    std::string handleGetChallenge(const Address& from, const std::vector<std::string>& args);

    std::vector<Challenge> challenges_ = std::vector<Challenge>(1024);
    uint32_t rng_;
};

} // namespace iw2::net
