// Server browser of the join-server screen (docs/spec/net.md section 12, docs/spec/mp-ui.md "Join server"): the sources
// (local broadcast, internet, favorites), the filters and sort of the list (FEEDER_SERVERS) and the status query (FEEDER_SERVERSTATUS).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "net/resolve.h"
#include "net/udp.h"

namespace iw2::net {

struct LanServer {
    NetAddress address;
    std::string info;  // the infoResponse info string
    int pingMs = 0;    // 0: no answer yet (a favorite that is not reachable)
    std::string favoriteName, addressText; // favorites: the stored name and the address as entered
    bool favorite = false;
    std::string hostname() const;
    std::string map() const;
    std::string gametype() const;
    int clients() const;
    int maxClients() const;
    bool password() const;
};

enum BrowserSource { kSourceLocal = 0, kSourceInternet = 1, kSourceFavorites = 2 };

// The filter state read from the ui_browser* dvars (the list build, MP 0x5365c0). Defaults are the dvars' registered defaults.
struct BrowserFilter {
    bool showFull = true, showEmpty = true, showPassword = true, showNoPassword = true, showPure = false, showDedicated = false;
    int mod = -1, friendlyFire = -1, killcam = -1, punkBuster = -1;
    std::string gametype; // empty: all
    bool operator==(const BrowserFilter&) const = default;
};
bool passesFilter(const LanServer& s, const BrowserFilter& f);

struct Favorite {
    std::string name, address; // as entered: numeric IP or DNS name, optional port
};

// The favorites of a profile. add() returns as the original's list add (MP 0x4b52f0): 1 added, 0 already listed, -1 full, -2 bad address.
class FavoriteList {
public:
    static constexpr int kMax = 128;
    int add(const std::string& name, const std::string& address, uint16_t defaultPort);
    bool remove(const std::string& address); // case-insensitive
    bool contains(const std::string& address, uint16_t defaultPort) const;
    const std::vector<Favorite>& items() const { return items_; }
    std::string format() const; // one "address\tname" line each
    void parse(const std::string& text, uint16_t defaultPort);
    void clear() { items_.clear(); }

private:
    std::vector<Favorite> items_;
};

// The add path of addFavorite / createFavorite (MP 0x5348b0): returns the @key for ui_favorite_message.
std::string favoriteMessage(FavoriteList& list, const std::string& name, const std::string& address, uint16_t defaultPort);

class LanBrowser {
public:
    static constexpr int kFeederServers = 2;      // FEEDER_SERVERS
    static constexpr int kFeederServerStatus = 13; // FEEDER_SERVERSTATUS
    // Ports asked by the local source (docs/spec/net.md section 12).
    static constexpr uint16_t kFirstPort = 28960;
    static constexpr int kPortCount = 4;

    void setSource(int source) { source_ = source; }
    int source() const { return source_; }
    // Starts a refresh of the current source: clears the list and asks. Local: getinfo broadcast (also to extra, e.g. 127.0.0.1).
    // Internet: nothing, there is no master server. Favorites: one row per favorite, getinfo to each (names are looked up first).
    bool refresh(int64_t nowMs, const std::vector<NetAddress>& extra = {});
    void setFavorites(const std::vector<Favorite>& favorites) { favorites_ = favorites; }
    // Reads replies and finishes name lookups; call every frame.
    void poll(int64_t nowMs);
    bool refreshing(int64_t nowMs) const { return nowMs - refreshMs_ < kRefreshMs; }
    const std::vector<LanServer>& servers() const { return servers_; }

    // The rows shown: the servers that pass the filter, in the sort order.
    void setFilter(const BrowserFilter& f);
    // ServerSort (MP 0x534d60): the same column again reverses the order.
    void sortBy(int column);
    int sortColumn() const { return sortColumn_; }
    bool sortDescending() const { return sortDescending_; }
    int rowCount() const { return int(rows_.size()); }
    const LanServer* rowServer(int row) const;
    // A list column (the column switch of MP 0x537640): 0 password, 1 platform icon (empty), 2 host name or the address until the
    // server answered, 3 map, 4 "clients (max)", 5 game type, 6 voice, 7 pure, 8 mod, 9 PunkBuster, 10 ping.
    std::string column(int row, int column) const;
    int selected = -1;
    void select(int row);
    std::string challenge = "xxx";

    // ServerStatus: getstatus to the selected row; the reply fills statusRows (4 columns, cvars then players).
    bool queryStatus(int64_t nowMs);
    int statusCount() const { return int(status_.size()); }
    std::string statusColumn(int row, int column) const;

private:
    void rebuild();
    bool ensureSocket();
    static constexpr int kRefreshMs = 2000;
    UdpSocket sock_;
    std::vector<LanServer> servers_;
    std::vector<int> rows_;
    std::vector<Favorite> favorites_;
    std::vector<Resolver> lookups_; // one per favorite row while its name is looked up
    std::vector<std::vector<std::string>> status_;
    BrowserFilter filter_;
    NetAddress statusAddress_;
    NetAddress selectedAddress_;
    bool hasSelection_ = false;
    int source_ = kSourceLocal;
    int sortColumn_ = 10;
    bool sortDescending_ = false;
    int64_t refreshMs_ = -1000000;
};

} // namespace iw2::net
