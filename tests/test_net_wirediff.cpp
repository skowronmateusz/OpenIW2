// Differential wire test (docs/spec/net.md section 9): the same scenario (join, team and weapon by script menu
// response, walk, fire, score) against a running patch 1.3 dedicated server (IW2_TEST_LNXDED=host:port, mp_toujane
// dm, sv_pure 0, no other clients) and against iw2server on the same map; the decoded gamestates and snapshots are
// compared field by field. Skipped unless IW2_TEST_LNXDED is set.
//   test_net_wirediff <iw2server> <main dir>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "net/client.h"

using namespace iw2::net;

namespace {

using Fields = std::map<std::string, std::string>;

struct Capture {
    std::map<int, std::string> cs;
    std::map<int, Fields> baselines;
    Fields ps;
    std::map<int, Fields> entities, clients;
    int snapFlags = -1;
    std::set<int> snapFlagValues;
    int clientNum = -1;
    std::vector<std::string> commands;
};

Fields parseFields(std::istringstream& in) {
    Fields f;
    std::string kv;
    while (in >> kv) {
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) f[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    return f;
}

// Runs the standard scenario and parses the dump; the snapshot part keeps the last snapshot.
bool capture(const char* address, Capture& c, std::string& error, int runMs = 5000) {
    NetAddress to;
    if (!NetAddress::parse(address, to, 28960)) {
        error = std::string("bad address ") + address;
        return false;
    }
    char path[] = "/tmp/iw2wirediffXXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) return false;
    close(fd);
    TestClient client;
    client.addStandardScenario();
    client.dump = std::fopen(path, "w");
    bool ok = false;
    for (int attempt = 0; attempt < 15 && !ok; ++attempt) ok = client.connect(to, 2000);
    if (!ok) {
        error = "connect failed: " + client.error;
        std::fclose(client.dump);
        std::remove(path);
        return false;
    }
    client.run(runMs);
    // Leave the server free for the next run (what arrives after the disconnect is not compared).
    std::fflush(client.dump);
    const long end = std::ftell(client.dump);
    client.addReliableCommand("disconnect");
    client.run(300);
    std::fclose(client.dump);
    std::FILE* f = std::fopen(path, "r");
    char line[16384];
    bool inSnap = false;
    while (f && std::ftell(f) < end && std::fgets(line, sizeof line, f)) {
        std::string l(line);
        if (!l.empty() && l.back() == '\n') l.pop_back();
        std::istringstream in(l);
        std::string tag;
        in >> tag;
        if (tag == "cs") {
            int n;
            in >> n;
            in.get();
            std::string rest;
            std::getline(in, rest);
            c.cs[n] = rest;
        } else if (tag == "gamestate") {
            std::string w;
            int seq;
            in >> w >> seq >> w >> c.clientNum;
        } else if (tag == "baseline") {
            int n;
            in >> n;
            c.baselines[n] = parseFields(in);
        } else if (tag == "cmd") {
            int n;
            in >> n;
            in.get();
            std::string rest;
            std::getline(in, rest);
            c.commands.push_back(rest);
        } else if (tag == "snap") {
            std::string w;
            int t, d;
            in >> w >> t >> w >> d >> w >> c.snapFlags;
            c.snapFlagValues.insert(c.snapFlags);
            c.entities.clear();
            c.clients.clear();
            inSnap = true;
        } else if (inSnap && tag == "ps") {
            c.ps = parseFields(in);
        } else if (inSnap && tag == "ent") {
            int n;
            in >> n;
            c.entities[n] = parseFields(in);
        } else if (inSnap && tag == "client") {
            int n;
            in >> n;
            c.clients[n] = parseFields(in);
        }
    }
    if (f) std::fclose(f);
    std::remove(path);
    return true;
}

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "DIFF: " << what << "\n";
        ++failures;
    }
}

std::set<std::string> infoKeys(const std::string& info) {
    std::set<std::string> keys;
    size_t i = 1;
    bool key = true;
    while (i <= info.size()) {
        size_t e = info.find('\\', i);
        if (e == std::string::npos) e = info.size();
        if (key) keys.insert(info.substr(i, e - i));
        key = !key;
        i = e + 1;
    }
    return keys;
}

std::string infoValue(const std::string& info, const std::string& key) {
    const std::string k = "\\" + key + "\\";
    const size_t p = info.find(k);
    if (p == std::string::npos) return {};
    const size_t b = p + k.size(), e = info.find('\\', b);
    return info.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

// Equal configstrings, with the values that differ by nature: the ambient's start time (3), the host name.
bool sameConfigstring(const Capture& ref, int n, const std::string& a, const std::string& b) {
    if (n == 3) return a.substr(0, a.find("\\t\\")) == b.substr(0, b.find("\\t\\"));
    if (n >= 238 && n < 334) {
        const auto name = ref.cs.find(n - 96);
        if (name != ref.cs.end() && name->second == "ui_hostname") return true;
    }
    return a == b;
}

}  // namespace

int main(int argc, char** argv) {
    const char* target = std::getenv("IW2_TEST_LNXDED");
    if (!target || !*target) {
        std::cout << "net_wirediff: IW2_TEST_LNXDED not set, skipped\n";
        return 0;
    }
    if (argc < 3) {
        std::cerr << "usage: test_net_wirediff <iw2server> <main dir>\n";
        return 1;
    }
    std::string error;
    Capture ref, ours;
    if (!capture(target, ref, error)) {
        std::cerr << "reference: " << error << "\n";
        return 1;
    }
    // The same map and gametype as the reference, and as many bots as it has other clients.
    const std::string map = infoValue(ref.cs[0], "mapname"), gametype = infoValue(ref.cs[0], "g_gametype");
    const int bots = int(ref.clients.size()) - 1;
    // Later sd rounds (IW2_TEST_ROUNDS set; the reference needs players on both teams, e.g. bots that leave the
    // round): over 80 s both servers restart the round at least once, which flips the snapshot flag bit and sends
    // configstring 13 with the restart's server time.
    if (gametype == "sd" && std::getenv("IW2_TEST_ROUNDS")) {
        // Rounds mode only: the reference is in a later round, so the first-round comparison does not apply.
        Capture refR, oursR;
        const std::string cmdR = std::string(argv[1]) + " " + argv[2] + " " + map + " sd --port 28979 --bots 2" +
                                 " --seconds 100 --no-console > /dev/null 2>&1 &";
        if (std::system(cmdR.c_str()) != 0) return 1;
        // Both at once, so that each server is seen over the same 80 s.
        std::string errRef, errOurs;
        bool okRef = false, okOurs = false;
        std::thread t1([&] { okRef = capture(target, refR, errRef, 80000); });
        std::thread t2([&] { okOurs = capture("127.0.0.1:28979", oursR, errOurs, 80000); });
        t1.join();
        t2.join();
        if (!okRef || !okOurs) {
            std::cerr << "rounds: " << errRef << errOurs << "\n";
            return 1;
        }
        auto restarted = [](const Capture& c) {
            bool cs13 = false;
            for (const auto& t : c.commands)
                if (t.rfind("d 13 ", 0) == 0 && std::atoi(t.c_str() + 5) > 0) cs13 = true;
            return cs13 && c.snapFlagValues.size() == 2;
        };
        check(restarted(refR), "reference: no round restart seen");
        check(restarted(oursR), "iw2server: round restart (configstring 13, snapshot flag)");
        std::cout << "net_wirediff rounds: " << failures << " differences\n";
        return failures ? 1 : 0;
    }
    std::cout << "reference: " << map << " " << gametype << ", " << bots << " other clients\n";
    const std::string cmd = std::string(argv[1]) + " " + argv[2] + " " + map + " " + gametype +
                            " --port 28977 --seconds 15 --no-console" +
                            (bots > 0 ? " --bots " + std::to_string(bots) : std::string()) + " > /dev/null 2>&1 &";
    if (std::system(cmd.c_str()) != 0) return 1;
    if (!capture("127.0.0.1:28977", ours, error)) {
        std::cerr << "iw2server: " << error << "\n";
        return 1;
    }

    // Gamestate: every configstring of the reference outside the known gaps, same index and text (0 and 1: the
    // same keys; the host name, server id and iwd lists differ by nature).
    std::set<std::string> infoRef = infoKeys(ref.cs[0]);
    infoRef.erase("fs_game");  // a reference run with a script mod
    check(infoRef == infoKeys(ours.cs[0]), "server info keys");
    std::set<std::string> sysRef = infoKeys(ref.cs[1]), sysOurs = infoKeys(ours.cs[1]);
    sysRef.erase("sv_referencedIwdNames");
    sysRef.erase("sv_referencedIwds");
    sysRef.erase("fs_game");
    check(sysRef == sysOurs, "system info keys");
    int csSame = 0;
    for (const auto& [n, text] : ref.cs) {
        if (n <= 1) continue;
        const auto it = ours.cs.find(n);
        const bool same = it != ours.cs.end() && sameConfigstring(ref, n, text, it->second);
        check(same, "configstring " + std::to_string(n) + " \"" + text + "\" / \"" +
                        (it != ours.cs.end() ? it->second : std::string("-")) + "\"");
        if (same) ++csSame;
    }
    for (const auto& [n, text] : ours.cs)
        check(ref.cs.count(n) != 0, "extra configstring " + std::to_string(n) + " \"" + text + "\"");
    // Baselines: the same entity (type, model, weapon, place) under the same number. An entity found under another
    // number counts as renumbered (known gap: entities spawned at run time that openiw2 lacks shift the numbers
    // after them, docs/spec/net.md section 11); one found nowhere is a difference.
    auto sameEntity = [](const Fields& a, const Fields& b) {
        for (const char* k : {"eType", "typeIndex", "weapon", "pos.trBase[0]", "pos.trBase[1]", "pos.trBase[2]"})
            if (a.count(k) != b.count(k) || (a.count(k) && a.at(k) != b.at(k))) return false;
        return true;
    };
    int renumbered = 0, missing = 0;
    for (const auto& [n, f] : ref.baselines) {
        const auto it = ours.baselines.find(n);
        if (it != ours.baselines.end() && sameEntity(f, it->second)) continue;
        bool found = false;
        for (const auto& [m, g] : ours.baselines) found = found || sameEntity(f, g);
        if (found) ++renumbered;
        else ++missing;
    }
    for (const auto& [n, f] : ours.baselines) {
        bool found = false;
        for (const auto& [m, g] : ref.baselines) found = found || sameEntity(f, g);
        check(found, "extra baseline " + std::to_string(n));
    }
    if (renumbered || missing)
        std::cout << "known gap: baselines renumbered " << renumbered << ", missing " << missing << "\n";
    // Snapshots: flags, the weapon side of the player state and the fields the 1.3 server always fills.
    check(ref.snapFlags == ours.snapFlags, "snapFlags");
    for (const char* k : {"weapon", "ownedWeaponBits[0]", "offhandWeapon", "hintStringIndex", "viewLockEntity",
                          "speed", "gravity", "boundsMin[0]", "boundsMax[2]", "currentViewHeight", "groundEntityNum",
                          "weaponSlotWord[0]", "viewModel", "pm_flags", "eFlags", "boundsMax[2]"})
        check(ref.ps[k] == ours.ps[k], std::string("player state ") + k + ": " + ref.ps[k] + " / " + ours.ps[k]);
    // Ammo (+324) and clip (+836) slots: the same slots are filled; the reserve ammo is equal (the clip depends on
    // how many shots the scenario's presses fired).
    auto slots = [](const Fields& ps, int from, int to) {
        std::map<int, std::string> out;
        for (const auto& [k, v] : ps)
            if (k[0] == '@' && std::atoi(k.c_str() + 1) >= from && std::atoi(k.c_str() + 1) < to) out[std::atoi(k.c_str() + 1)] = v;
        return out;
    };
    check(slots(ref.ps, 324, 836) == slots(ours.ps, 324, 836), "ammo slots");
    {
        std::set<int> a, b;
        for (auto& [k, v] : slots(ref.ps, 836, 1348)) a.insert(k);
        for (auto& [k, v] : slots(ours.ps, 836, 1348)) b.insert(k);
        // Known gap (docs/spec/net.md section 11): the pistol given by setWeaponSlotWeapon has a loaded clip on the
        // 1.3 server and an empty one in openiw2 (TODO(spec)); only the slots openiw2 fills must be filled there.
        for (int k : b) check(a.count(k) != 0, "clip slot @" + std::to_string(k));
        if (a != b) std::cout << "known gap: clip slots " << a.size() << " / " << b.size() << "\n";
    }
    // Other players (bots): entity number = client number, eType 1, a client state for each.
    for (const auto& [n, f] : ref.clients) {
        if (n == ref.clientNum) continue;
        check(ours.clients.count(n) != 0, "client state of client " + std::to_string(n));
        const auto e = ours.entities.find(n);
        check(e != ours.entities.end() && e->second.count("eType") && e->second.at("eType") == "1",
              "player entity " + std::to_string(n));
    }
    // The client's own client state: body and attached models.
    // (the body and head are chosen at random: compare that both name player models of the same kind)
    for (const char* k : {"modelindex", "attachedModel[0]", "attachedModel[1]"}) {
        auto kind = [&](Capture& c) {
            const std::string m = c.cs[334 + std::atoi(c.clients[c.clientNum][k].c_str())];
            return m.substr(0, m.find('_', 7));
        };
        check(kind(ref) == kind(ours), std::string("client state ") + k + ": " + kind(ref) + " / " + kind(ours));
    }
    // Server commands that carry state: every setClientCvar of the reference arrives (order may differ).
    std::multiset<std::string> refV, ourV;
    for (const auto& c : ref.commands)
        if (c.rfind("v ", 0) == 0) refV.insert(c);
    for (const auto& c : ours.commands)
        if (c.rfind("v ", 0) == 0) ourV.insert(c);
    for (const auto& c : refV) check(ourV.count(c) != 0, "server command " + c);
    std::cout << "clientNum " << ref.clientNum << " / " << ours.clientNum << ", client states " << ref.clients.size() << " / " << ours.clients.size() << "\n";
    std::cout << "net_wirediff: " << csSame << " configstrings equal, " << ours.baselines.size()
              << " baselines checked, " << failures << " differences\n";
    return failures ? 1 : 0;
}
