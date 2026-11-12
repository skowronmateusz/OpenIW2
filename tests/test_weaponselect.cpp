// The network client's weapon selection (docs/spec/net.md section 10 "Weapon selection", src/netgame/weaponselect.h).
// A model of a client and a server on a path with latency and jitter: the client changes its weapon, the server runs
// the change from the commands' weapon byte and lowers the old weapon first, and the snapshots show the server's weapon
// late (an older one than the newest is dropped). The selection must change once per press and never
// follow a snapshot. Also: the server command `a`, a spawn, a new gamestate and a weapon taken away.
#include <cstdio>
#include <deque>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "netgame/weaponselect.h"

using namespace iw2;
using netgame::NetWeaponSelection;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

std::map<int, std::string> weaponList() {
    // The weapon list configstring: the names in weapon number order (docs/spec/net.md section 13).
    std::map<int, std::string> cs;
    cs[netgame::kWeaponListConfigstring] = "kar98k_mp mp40_mp luger_mp";
    return cs;
}

// A weapon in hand that is lowered for dropMs before the selected one comes up (the weapon name changes at the raise).
struct Hand {
    std::string held, target;
    int dropUntil = -1;
    void step(int now, const std::string& selected, int dropMs) {
        if (selected != target) {
            target = selected;
            if (held.empty()) held = target;
            else if (held != target && dropUntil < 0) dropUntil = now + dropMs;
        }
        if (dropUntil >= 0 && now >= dropUntil) {
            held = target;
            dropUntil = held == target ? -1 : now + dropMs;
        }
    }
};

void latencyModel(uint32_t seed, int oneWayMs, int jitterMs, int dropMs) {
    std::mt19937 rng(seed);
    const std::set<std::string> owned{"kar98k_mp", "mp40_mp"};
    NetWeaponSelection sel;
    std::string clientSelect;
    Hand client, server;
    std::string serverSelect, lastCmdWeapon;
    struct Snap { int at; int sent; std::string weapon; };
    int shownSent = -1;
    std::deque<std::pair<int, std::string>> commands;  // arrival time, weapon byte
    std::vector<Snap> snaps;
    std::string shown;  // the newest snapshot's weapon as the client has it
    int changes = 0, presses = 0;
    std::string prev;
    for (int t = 0; t < 20000; t += 8) {
        // Presses: a weapon change every 1.7 s, some as a second press 150 ms after the first (a wheel turned twice).
        if (t % 1700 == 100 || (t % 3400 == 260)) {
            clientSelect = clientSelect == "kar98k_mp" ? "mp40_mp" : "kar98k_mp";
            ++presses;
        }
        // The client frame: the newest snapshot, then the weapon step and the command.
        for (auto it = snaps.begin(); it != snaps.end();)
            if (it->at <= t) {
                if (it->sent > shownSent) shown = it->weapon, shownSent = it->sent; // an older snapshot than the newest is dropped
                it = snaps.erase(it);
            } else {
                ++it;
            }
        if (!shown.empty()) sel.snapshot(0, 1, shown, owned, clientSelect);
        if (clientSelect.empty()) clientSelect = "kar98k_mp";
        if (clientSelect != prev) ++changes, prev = clientSelect;
        client.step(t, clientSelect, dropMs);
        const int up = oneWayMs + int(rng() % unsigned(2 * jitterMs + 1)) - jitterMs;
        commands.emplace_back(t + up, clientSelect);
        // The server: commands in order of arrival, the weapon byte taken on a change.
        while (!commands.empty() && commands.front().first <= t) {
            const std::string w = commands.front().second;
            commands.pop_front();
            if (w != lastCmdWeapon) serverSelect = w;
            lastCmdWeapon = w;
        }
        server.step(t, serverSelect, dropMs);
        if (t % 48 == 0) {
            const int down = oneWayMs + int(rng() % unsigned(2 * jitterMs + 1)) - jitterMs;
            snaps.push_back({t + down + (rng() % 10 == 0 ? 30 : 0), t, server.held}); // some held back behind later ones
        }
    }
    // The first selection counts as a change; every press is one more.
    char what[160];
    std::snprintf(what, sizeof what, "latency %d+-%d ms, drop %d ms: %d selection changes for %d presses", oneWayMs, jitterMs, dropMs, changes, presses);
    std::printf("%s\n", what);
    check(changes == presses + 1, what);
}

} // namespace

int main() {
    const auto cs = weaponList();
    // The server command: the index names a weapon of the list.
    {
        std::string s = "kar98k_mp";
        check(NetWeaponSelection::serverCommand({"a", "2"}, cs, s) && s == "mp40_mp", "a 2 selects the second weapon");
        check(!NetWeaponSelection::serverCommand({"a", "0"}, cs, s) && s == "mp40_mp", "a 0 is ignored");
        check(!NetWeaponSelection::serverCommand({"a", "9"}, cs, s) && s == "mp40_mp", "an unknown index is ignored");
        check(!NetWeaponSelection::serverCommand({"a"}, cs, s), "a without an index is ignored");
    }
    const std::set<std::string> owned{"kar98k_mp", "mp40_mp"};
    // A selection of the player's own differs from the snapshot: it stays.
    {
        NetWeaponSelection n;
        std::string s;
        check(n.snapshot(0, 1, "kar98k_mp", owned, s) && s == "kar98k_mp", "the first snapshot gives the selection");
        s = "mp40_mp";
        for (int i = 0; i < 50; ++i) n.snapshot(0, 1, i < 20 || i > 30 ? "kar98k_mp" : "mp40_mp", owned, s);
        check(s == "mp40_mp", "a snapshot with the old weapon does not change the selection");
    }
    // A spawn (pm_type from dead to 0) takes the player state's weapon, once it is known.
    {
        NetWeaponSelection n;
        std::string s;
        n.snapshot(0, 1, "kar98k_mp", owned, s);
        s = "mp40_mp";
        n.snapshot(6, 1, "", owned, s);
        check(s == "mp40_mp", "dead: no change");
        n.snapshot(0, 1, "", owned, s);
        check(s == "mp40_mp", "spawned without a weapon yet: no change");
        check(n.snapshot(0, 1, "kar98k_mp", owned, s) && s == "kar98k_mp", "the spawn weapon becomes the selection");
        s = "mp40_mp";
        check(!n.snapshot(0, 1, "kar98k_mp", owned, s) && s == "mp40_mp", "only once per spawn");
    }
    // A new gamestate (map change, map_restart) takes it too.
    {
        NetWeaponSelection n;
        std::string s;
        n.snapshot(0, 1, "kar98k_mp", owned, s);
        s = "mp40_mp";
        check(n.snapshot(0, 2, "kar98k_mp", owned, s) && s == "kar98k_mp", "a new gamestate takes the player state's weapon");
    }
    // A selected weapon taken away (no longer owned) gives way to the player state's.
    {
        NetWeaponSelection n;
        std::string s;
        n.snapshot(0, 1, "kar98k_mp", owned, s);
        s = "luger_mp";
        check(n.snapshot(0, 1, "kar98k_mp", owned, s) && s == "kar98k_mp", "a weapon no longer owned is replaced");
    }
    // Under latency and jitter, with the old weapon lowered first.
    for (uint32_t seed = 1; seed <= 20; ++seed) {
        latencyModel(seed, 40, 10, 350);
        latencyModel(seed, 75, 25, 600);
        latencyModel(seed, 120, 40, 150);
    }
    if (failures) {
        std::printf("weaponselect: %d failures\n", failures);
        return 1;
    }
    std::printf("weaponselect: ok\n");
    return 0;
}
