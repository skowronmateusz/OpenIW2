// The client side of src/netgame/worldgame.cpp (docs/spec/net.md section 11): applies snapshots and server commands
// to a remote World (World::mpStartRemote) so the front end draws the HUD, kill feed, chat and the other entities
// exactly as in a local game.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "netgame/predictedturret.h"
#include "game/world.h"
#include "net/gameclient.h"
#include "netgame/netmove.h"

namespace iw2::netgame {

class RemoteView {
public:
    // The remote world keeps the local view in client slot 0; the server's client 0 takes our own number's slot.
    static int worldSlot(int client, int self) { return client == self ? 0 : client == 0 ? self : client; }
    // The world slot of the local client's own body in a killcam replay (the server sends it as an entity then; slot 0
    // stays the local view): one past the 64 wire client numbers.
    static constexpr int kOwnBodySlot = 64;

    // The view weapon is not drawn for pm_type 4 (spectator) and 5 (intermission): the MP view weapon function returns
    // first thing on them (VAs in the patch 1.3 MP executable 0x4d6390..0x4d63ad), before cg_drawGun and the turret test.
    static bool viewWeaponHidden(int pmType) { return pmType == 4 || pmType == 5; }
    // Name and team number (0 none, 1 axis, 2 allies, 3 spectator) of server client `client` from the snapshot's client
    // states, which list every connected client whether or not its entity is in the snapshot (scoreboard).
    // False when the snapshot has no state for it.
    static bool clientInfo(const net::ClientSnapshot& snap, int client, std::string& name, int& team);
    // The other players between two snapshots (frac 0..1), for World::mpSetRemotePlayers.
    // Past the newest snapshot (docs/spec/net.md section 10 "Past the newest snapshot"): the players move on along
    // their velocity for `ms`.
    static void extrapolate(std::vector<game::World::RemotePlayer>& players, float ms) {
        for (auto& p : players)
            if (!p.dead)
                for (size_t k = 0; k < 3; ++k) p.origin[k] += p.velocity[k] * ms * 0.001f;
    }
    static std::vector<game::World::RemotePlayer> players(const net::ClientSnapshot& a, const net::ClientSnapshot& b,
                                                         float frac, int self, const std::map<int, std::string>& cs = {});
    // Everything else of the newest snapshot: the local player's health, weapon, ammo and team, HUD elements,
    // objectives, new obituary events and script models. worldTimeMs: the time the HUD and shell shock times are
    // read on, at the moment of snap.serverTime (they are shifted from server time by the difference).
    // Models with a trajectory (docs/spec/net.md section 11 "Trajectories") placed at time t, per rendered frame
    // (after apply placed them at the snapshot time). snaps: the snapshot drawn at t first (the newest one at or
    // before t), then every later one the client holds, in time order (GameClient::snapshotsFrom). A model's path is
    // the one of the first snapshot; when a later snapshot carries a re-based trajectory (a bounce, a stop; the first
    // one that differs), the path ends where it reaches the new base and the model waits there until the new start
    // time, as it did on the server. A model the first snapshot lacks (thrown since) waits at the start of the path
    // of the first snapshot that holds it.
    static void placeTrajectories(game::World& w, const std::vector<const net::ClientSnapshot*>& snaps, int t);
    // The missiles' entity events (EV_GRENADE_BOUNCE: the surface's bounce sound and effect at the drawn missile), from
    // the same snapshots as placeTrajectories: those of the snapshot after the drawn one (the cgame checks a
    // snapshot's events when it becomes the next one of the drawn pair, docs/spec/net.md section 11), once each; a
    // missile's events from before its first sight are old.
    void missileEvents(game::World& w, const std::vector<const net::ClientSnapshot*>& snaps);
    // The map turrets' events and fire loop (docs/spec/turrets.md "Multiplayer"): from the newer snapshot of the drawn
    // pair, as the cgame checks a snapshot's events when it becomes the next one; each shot (EV_FIRE_WEAPON_MG42) and
    // stop sound once, the loop sound as the entity state has it. Paced by the client clock, not by the arrival of
    // the snapshots: a burst of late snapshots neither stops the loop nor fires its shots at once.
    // own: the predicted fire of the own turret (its shot events stand for predicted shots first; its sounds are the
    // prediction's).
    void turretEvents(game::World& w, const net::ClientSnapshot& next, const std::map<int, std::string>& cs,
                      PredictedTurret* own = nullptr);
    // One command of the client on its own turret (docs/spec/net.md section 10 "Predicted turret fire"): the predicted
    // shots (flash, shake, impact) and the fire loop and its end, at once. Off a turret the prediction is cleared, and
    // so it is while the newest snapshot's player state has a pm_type of 4 or more (spectator, intermission, dead:
    // PM_Weapon does nothing then) or follows another client (a killcam: nothing is predicted, 0x4deae0), which also
    // ends a running fire loop. latest: the newest snapshot (nullptr: none yet).
    static void predictTurret(game::World& w, PredictedTurret& pt, int commandTime, bool attack, const net::ClientSnapshot* latest);
    // How far behind the players' drawn time the models with a trajectory are drawn (docs/spec/net.md section 11
    // "Trajectories"): with snapshots between the game frames, the part of a game frame the snapshot interval does not
    // cover, so that the frame that re-bases a path has been received when the drawn time reaches the hit.
    static int trajectoryDelayMs(int snapshotIntervalMs);
    // The position of such a model at time t from the pair's two entity states (exposed for the tests). spanMs: the time
    // between the two snapshots, the span in which the server may have re-based the path.
    static void trajectoryAt(const std::vector<uint8_t>& ea, const std::vector<uint8_t>* eb, int t, float out[3], int spanMs);
    void apply(game::World& w, const net::ClientSnapshot& snap, const std::map<int, std::string>& configstrings,
               int self, int64_t worldTimeMs, PredictedAmmo* predicted = nullptr);
    // Other players' weapons and their entity events (docs/spec/net.md section 13): each new event of a player's event ring
    // is played once (World::mpRemotePlayerEvent). Call once per received snapshot.
    void playerEvents(game::World& w, const net::ClientSnapshot& snap, const std::map<int, std::string>& configstrings, int self);
    // The damage feedback of the player states of `snaps` (oldest first; those already seen are skipped): the server
    // clears damageCount in every frame without damage, so only the snapshots of the damage's own frame carry it, and
    // the cgame meets them because it walks through every snapshot in order. A front end that reads the newest snapshot
    // once per drawn frame passes all it received since (GameClient::snapshotsFrom); apply() does it for its own one.
    // A player state of another client or another life is no new damage (docs/spec/mp-hud.md "Hit direction and damage
    // feedback").
    void damageFeedback(game::World& w, const std::vector<const net::ClientSnapshot*>& snaps);
    // The followed client's view (killcam, docs/spec/mp-hud.md "Killcam"): when the snapshot's player state has the
    // follow bit (pm_flags 0x400000) the server sent another client's player state; the camera is its eye and view
    // angles as sent (no prediction), interpolated between two such snapshots; when only one has the bit the view stays
    // on that one's state (the first and last drawn frames of a replay). False without the bit in both.
    static bool followView(const net::ClientSnapshot& a, const net::ClientSnapshot& b, float frac, float eye[3],
                           float angles[3]);
    static bool following(const net::ClientSnapshot& snap);
    // Whether this client's own input drives its predicted weapon in this snapshot: not while it follows another client
    // (the killcam; the cgame predicts nothing then, MP 0x4deae0), nor without a snapshot.
    static bool ownWeaponRuns(const net::ClientSnapshot* snap) { return snap && !following(*snap); }
    // The followed client's shots in a network killcam: each new fire event of the player state's event ring (+164..)
    // is played once from the sent eye along the sent view (fire sound without a position, impacts, tracers;
    // World::mpRemoteFollowShot) and counted for the view model's fire animation. nowMs: the client clock of the drawn frame.
    // Call once per received snapshot.
    void followEvents(game::World& w, const net::ClientSnapshot& snap, const std::map<int, std::string>& configstrings,
                      int64_t nowMs);
    // The killcam view of a network client for the view model and the HUD (the local killcam's World::MpKillcamView
    // filled from the followed player state between a and b): eye, angles, weapon, hands, clip, stock, health, the
    // recorded view state and event ring (docs/spec/mp-hud.md "The recorded view state"), the followed client's server
    // client number in `client` (the copied player state's clientNum), and the shots counted by followEvents (since the
    // killcam started). nowMs: the client clock of the drawn frame. inactive without the follow bit in a and b.
    game::World::MpKillcamView killcamView(const net::ClientSnapshot& a, const net::ClientSnapshot& b, float frac,
                                           const std::map<int, std::string>& configstrings, int64_t nowMs) const;
    // The synced dvars of the gamestate (configstrings 142..237 names, values 96 later, docs/spec/net.md section 11):
    // name -> value ("" when the value has no configstring). The client sets them as its dvars (the serverinfo menu
    // reads ui_<gt>_scorelimit / ui_<gt>_timelimit from them).
    static std::map<std::string, std::string> syncedDvars(const std::map<int, std::string>& configstrings);
    // The local player's stance from the player state (pm_flags): 0 stand, 1 crouch, 2 prone.
    static int stance(const net::ClientSnapshot& snap);

private:
    std::set<std::pair<int, int>> seenEvents_;  // (entity number, server time) of the events already played
    int lastSnapshot_ = -1;
    std::array<int, 3> shock_{0, 0, 0}; // shockType, shockStartTime (server time), shockDuration of the last applied player state
    int64_t shockStartMs_ = 0;          // that shock's start on the client clock, taken when the shock was first seen
    int feedbackSnapshot_ = -1; // message number of the last snapshot damageFeedback saw
    int psIdentity_ = -1; // clientNum | spawn count << 8 of the last applied snapshot's player state
    int lastEventSnapshot_ = -1;
    bool lastEventsFollowing_ = false; // the follow bit of the last snapshot playerEvents saw
    std::map<int, int> eventSeq_;  // client -> last event sequence played
    std::map<int, int> turretSeq_; // turret wire number -> last event sequence played
    std::map<int, int> missileSeq_; // missile wire number -> last event sequence played
    int missileEventsSnapshot_ = -1;
    int turretEventsSnapshot_ = -1;
    // messageNum is newer than `last` (then it becomes `last`); a much older one (a new connection) clears `seqs`.
    static bool newerSnapshot(int messageNum, int& last, std::map<int, int>& seqs);  // messageNum of the snapshot whose turret events were checked last // messageNum of the snapshot whose missile events were checked last
    int lastFollowSnapshot_ = -1;
    int followSeq_ = -1;            // the followed player state's last event sequence played (-1: not following)
    int followFires_ = 0;           // shots played since the killcam started
    bool followLastShot_ = false;
    int64_t followFireMs_ = -1;     // world time of the newest shot played
};

// The client command of a line typed into the message field (0x40ade0): `say` / `say_team` followed by 0x15 and the
// text (docs/spec/net.md section 10).
std::string sayCommand(bool team, const std::string& text);

} // namespace iw2::netgame
