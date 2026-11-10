// Multiplayer spectating: allowSpectateTeam, following another client, the free spectator camera's speed and the
// client's move type by session state (docs/spec/mp-spectate.md). Implemented from the spec.
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "game/mpbots.h"
#include "game/mpscoreboard.h"
#include "game/mpspectate.h"
#include "game/world.h"

namespace iw2::game {

using script::CallContext;
using script::Value;

namespace {
constexpr float kDeg = 3.14159265358979f / 180.0f;
}

int mpSessionStateNumber(const std::string& s) {
    if (s == "dead") return 1;
    if (s == "spectator") return 2;
    if (s == "intermission") return 3;
    return 0;
}

int mpClientPmType(int sessionState) {
    switch (sessionState) {
    case 1: return 6; // dead, not linked (0x8101934: 7 while linked, as pm_type 1 before the death)
    case 2: return 4; // spectator; while following, the target's state is copied over it with pm_flags 0x400000
    case 3: return 5; // intermission
    default: return 0;
    }
}

int World::mpSessionState(int client) const {
    if (!mp_ || client < 0 || size_t(client) >= mp_->clients.size()) return 0;
    const MpClient& cl = mp_->clients[size_t(client)];
    if (cl.state == MpClientState::Free || !cl.ent) return 0;
    return mpSessionStateNumber(cl.ent->obj->get("sessionstate").toString());
}

int World::mpFollowing(int client) const {
    if (!mp_ || client < 0 || size_t(client) >= mp_->clients.size()) return -1;
    return mp_->clients[size_t(client)].following ? mp_->clients[size_t(client)].followClient : -1;
}

bool World::mpSpectateAllowed(int client, int bit) const {
    if (!mp_ || client < 0 || size_t(client) >= mp_->clients.size()) return false;
    return ((mp_->clients[size_t(client)].spectateMask >> bit) & 1) == 0;
}

// The archived player state of a target (0x809a0b6) exists for an active client; here a client that is playing.
// TODO(spec): the snapshot archive's validity test (dead clients, archive time).
bool World::mpFollowTargetValid(int client, int target) const {
    if (target < 0 || size_t(target) >= mp_->clients.size() || target == client) return false;
    const MpClient& t = mp_->clients[size_t(target)];
    if (t.state != MpClientState::Active || !t.ent) return false;
    if (t.ent->obj->get("sessionstate").toString() != "playing") return false;
    return mpSpectateAllowed(client, mpTeamOf(t.ent));
}

// Follow cycle (0x80ff4ac): only for a spectator whose spectatorclient is below 0; from the current target (0 when none)
// step by dir with wrap-around over the client slots and take the first valid target; true when one was found.
bool World::mpFollowCycle(int client, int dir) {
    if (!mp_ || client < 0 || size_t(client) >= mp_->clients.size()) return false;
    if (dir != 1 && dir != -1) throw std::runtime_error("Cmd_FollowCycle_f: bad dir " + std::to_string(dir));
    MpClient& cl = mp_->clients[size_t(client)];
    if (!cl.ent || mpSessionState(client) != 2) return false;
    const Value sc = cl.ent->obj->get("spectatorclient");
    if (sc.isNumber() && sc.asInt() >= 0) return false;
    const int n = int(mp_->clients.size());
    int i = std::max(cl.followClient, 0);
    const int start = i;
    do {
        i += dir;
        if (i >= n) i = 0;
        if (i < 0) i = n - 1;
        if (mpFollowTargetValid(client, i)) {
            cl.followClient = i;
            cl.following = true;
            return true;
        }
    } while (i != start);
    return false;
}

// Stop following (0x80ff252): spectatorclient and the target become -1; when the client was following, it is placed
// behind the target's eye: 40 units back along the view, 10 up, an 8-unit box traced there with mask 0x810011, and
// it looks with the target's angles, pitch + 15.
void World::mpStopFollow(int client) {
    if (!mp_ || client < 0 || size_t(client) >= mp_->clients.size()) return;
    MpClient& cl = mp_->clients[size_t(client)];
    if (cl.ent) cl.ent->obj->set("spectatorclient", Value(int32_t(-1)));
    const int target = cl.followClient;
    const bool was = cl.following;
    cl.followClient = -1;
    cl.following = false;
    if (!was || !cl.ent) return;
    Vec3 eye, angles;
    if (!mpClientView(target, eye, angles)) return;
    const float p = angles[0] * kDeg, y = angles[1] * kDeg;
    const Vec fwd{std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p)};
    const Vec up{std::sin(p) * std::cos(y), std::sin(p) * std::sin(y), std::cos(p)};
    const Vec want{eye[0] - 40.0f * fwd[0] + 10.0f * up[0], eye[1] - 40.0f * fwd[1] + 10.0f * up[1],
                   eye[2] - 40.0f * fwd[2] + 10.0f * up[2]};
    Vec end = want;
    if (collision_) {
        const TraceResult tr = collision_->trace({eye[0], eye[1], eye[2]}, want, {-8, -8, -8}, {8, 8, 8}, 0x810011u, cl.ent->number);
        end = tr.endPos;
    }
    const Vec3 o{end[0], end[1], end[2]};
    const Vec3 a{angles[0] + 15.0f, angles[1], 0.0f};
    cl.ent->obj->set("origin", Value(o));
    cl.ent->velocity = {0, 0, 0};
    if (cl.ent == player_) {
        playerTeleport_ = o;
        playerViewAngles_ = a;
    }
}

bool World::mpClientView(int client, Vec3& eye, Vec3& angles) {
    if (!mp_ || client < 0 || size_t(client) >= mp_->clients.size()) return false;
    const MpClient& cl = mp_->clients[size_t(client)];
    if (!cl.ent) return false;
    const Vec3 o = cl.ent->obj->get("origin").asVector();
    if (cl.ent == player_) {
        eye = playerEye();
        angles = playerCmd_.viewAngles;
        return true;
    }
    // The followed client's view comes from its player state (docs/spec/mp-spectate.md): a bot's movement, a network
    // client's newest command and movement.
    float pitch = 0, yaw = 0, roll = 0, viewHeight = 60.0f;
    const MpBot* bot = nullptr;
    if (bots_)
        for (const MpBot& b : bots_->bots)
            if (b.client == client) bot = &b;
    if (bot) {
        pitch = bot->pitch;
        yaw = bot->yaw;
        if (bot->psValid) viewHeight = bot->ps.viewHeight;
    } else {
        pitch = cl.viewAngles[0], yaw = cl.viewAngles[1], roll = cl.viewAngles[2];
        viewHeight = cl.viewHeight;
    }
    eye = {o[0], o[1], o[2] + viewHeight};
    angles = {pitch, yaw, roll};
    return true;
}

uint32_t World::mpClientButtons(int client) const {
    const MpClient& cl = mp_->clients[size_t(client)];
    return cl.ent == player_ ? playerCmd_.buttons : cl.buttons;
}

// Per frame for every client: the spectator think (0x80f4eb2) on the button presses, then the end-of-frame follow
// update (0x80f6916). Not spectating clears the follow state.
void World::mpSpectateFrame() {
    for (size_t i = 0; i < mp_->clients.size(); ++i) {
        MpClient& cl = mp_->clients[i];
        const int client = int(i);
        if (cl.state == MpClientState::Free || !cl.ent) continue;
        const uint32_t buttons = mpClientButtons(client), old = cl.spectateOldButtons;
        cl.spectateOldButtons = buttons;
        if (mpSessionState(client) != 2) {
            cl.followClient = -1;
            cl.following = false;
            continue;
        }
        const Value scv = cl.ent->obj->get("spectatorclient");
        const int forced = scv.isNumber() ? scv.asInt() : -1;
        // Think: button 4 changing while following with free look allowed stops following; an attack press (1) cycles
        // forward, else a 0x1000 press backward.
        if (forced < 0 && mpSpectateAllowed(client, 4) && cl.followClient >= 0 && ((buttons ^ old) & 4u)) mpStopFollow(client);
        if ((buttons & 1u) && !(old & 1u)) mpFollowCycle(client, 1);
        else if ((buttons & 0x1000u) && !(old & 0x1000u)) mpFollowCycle(client, -1);
        // End of frame: a spectatorclient set by the script is the killcam's (archived view, src/game/mpkillcam.cpp);
        // the free follow state is left alone meanwhile. Without a target and without free look the client cycles to
        // the next one; a target that is no longer valid is dropped.
        if (forced >= 0) continue;
        if (cl.followClient < 0 && !mpSpectateAllowed(client, 4)) mpFollowCycle(client, 1);
        if (cl.followClient >= 0 && !mpFollowTargetValid(client, cl.followClient)) mpStopFollow(client);
    }
}

// Clients following `target` stop (disconnect 0x80f94ce, death 0x810175a).
void World::mpStopFollowersOf(int target) {
    if (!mp_) return;
    for (size_t i = 0; i < mp_->clients.size(); ++i)
        if (mp_->clients[i].followClient == target && mp_->clients[i].following) mpStopFollow(int(i));
}

// The scoreboard command of this server (0x80fdf08 with the order of 0x81097e6): every client slot in use, its score,
// deaths and status icon index (the statusicon field's position among the precached icons + 1, 0 for "" or unknown).
// The ping of a network client is the server's mean round trip; a local client and a bot report 0.
std::string World::mpScoresCommand() {
    if (!mp_) return "b 0 0 0";
    std::vector<MpScoreClient> list;
    for (size_t i = 0; i < mp_->clients.size(); ++i) {
        const MpClient& cl = mp_->clients[i];
        if (cl.state == MpClientState::Free || !cl.ent) continue;
        MpScoreClient c;
        c.num = int(i);
        const Value sc = cl.ent->obj->get("score"), de = cl.ent->obj->get("deaths");
        c.score = sc.isNumber() ? sc.asInt() : 0;
        c.ping = cl.ping;
        c.deaths = de.isNumber() ? de.asInt() : 0;
        c.connecting = cl.state == MpClientState::Connected;
        c.spectator = cl.ent->obj->get("sessionteam").toString() == "spectator";
        const std::string icon = cl.ent->obj->get("statusicon").toString();
        for (size_t k = 0; k < mp_->statusIcons.size() && !icon.empty(); ++k)
            if (mp_->statusIcons[k] == icon) c.statusIcon = int(k) + 1;
        list.push_back(c);
    }
    mpSortScoreClients(list);
    return game::mpScoresCommand(list, mp_->teamScore[1], mp_->teamScore[0]);
}

void World::registerSpectateBuiltins() {
    // precacheStatusIcon(name): the server keeps up to 8 status icon names (config strings); the scoreboard sends the
    // index. TODO(spec): the config string range and the error past 8.
    vm_.registerBuiltin("precachestatusicon", [this](CallContext& c) {
        const std::string n = c.arg(0).toString();
        auto& v = mp_->statusIcons;
        if (std::find(v.begin(), v.end(), n) == v.end() && v.size() < 8) v.push_back(n);
        return Value();
    });
    // allowSpectateTeam(team, allow) (0x80fdbe0): the bit of "none" 1, "axis" 2, "allies" 4, "freelook" 0x10 is cleared
    // when allowed and set when not; the mask starts at 0 (everything allowed).
    vm_.registerBuiltin("allowspectateteam", [this](CallContext& c) {
        Entity* e = entityOf(c.self);
        const int cl = e ? mpClientOf(*e) : -1;
        if (cl < 0) throw std::runtime_error("entity " + std::to_string(e ? e->number : -1) + " is not a player");
        const std::string t = c.arg(0).toString();
        const int bit = t == "axis" ? 2 : t == "allies" ? 4 : t == "none" ? 1 : t == "freelook" ? 0x10 : 0;
        if (!bit) throw std::runtime_error("team must be \"axis\", \"allies\", \"none\", or \"freelook\"");
        int& mask = mp_->clients[size_t(cl)].spectateMask;
        if (c.arg(1).truthy()) mask &= ~bit;
        else mask |= bit;
        return Value();
    });
}

} // namespace iw2::game
