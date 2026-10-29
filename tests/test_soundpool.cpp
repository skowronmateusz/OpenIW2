// Sound slot ownership through the World (docs/spec/sound.md, "Slot owners"), only with IW2_MAIN=<game main dir>.
// The generic play 0x4423d0 gets the entity number as the slot owner: the weapon channel then replaces the owner's previous sound
// (0x44250a -> 0x441b50), so an actor's burst holds ONE of the 8 3D sample slots. Without owners every shot kept its own slot for the
// whole sample, a firefight filled the pool with weapon voices (priority 3) and the grenade bounce / explosion sounds (channel "auto",
// priority 0, which may only steal priority-0 slots) were refused for as long as the shooting went on.
#include "audio/sound.h"
#include "fs/vfs.h"
#include "game/world.h"
#include "script/vm.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace iw2::game;

static int failures = 0;
#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #c "\n"; \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "sound pool test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    World w(vfs, "toujane");
    iw2::audio::SoundAliases aliases;
    aliases.load(vfs, "toujane");
    iw2::audio::Mixer mixer(vfs, aliases, 48000);
    // The front end's hook (apps/iw2view): the world's sounds go to the mixer together with their slot owner.
    struct Started { std::string alias; int handle; };
    std::vector<Started> started;
    w.playSound = [&](const std::string& alias, const Vec3* origin, float* duration, int owner) {
        const int h = mixer.play(alias, origin ? std::optional<iw2::audio::Vec3>(*origin) : std::nullopt, duration, {.owner = owner});
        started.push_back({alias, h});
        return h;
    };
    w.start();
    for (int i = 0; i < 5; ++i) w.frame();

    std::vector<Entity*> shooters;
    for (const auto& e : w.entities())
        if (e->isActor && e->obj->get("weapon").isString() && !e->obj->get("weapon").asString().empty()) shooters.push_back(e.get());
    CHECK(!shooters.empty());
    if (shooters.empty()) return 1;
    Entity& a = *shooters[0];
    const Vec3 ao = a.obj->get("origin").asVector();
    mixer.setListener({ao[0], ao[1], ao[2]}, {1, 0, 0}, {0, -1, 0});
    std::vector<float> buf(2 * 2400);

    // 24 shots of one actor, 50 ms apart (the mixer advances 50 ms per shot, like a frame): its fire sample (~0.5 s) would hold ~10
    // slots without ownership; with it the burst holds exactly one.
    int fired = 0, weaponVoices = 0;
    for (int i = 0; i < 24; ++i) {
        const size_t before = started.size();
        w.debugFireBulletFrom(a, {ao[0], ao[1], ao[2] + 56.0f}, {ao[0] + 400.0f, ao[1], ao[2] + 56.0f});
        for (size_t k = before; k < started.size(); ++k)
            if (started[k].alias.find("_fire") != std::string::npos && started[k].handle) ++fired;
        mixer.mix(buf.data(), 2400);
    }
    for (const std::string& d : mixer.describeVoices())
        if (d.find("_fire") != std::string::npos) ++weaponVoices;
    std::cout << "shots with a fire sound: " << fired << ", fire voices alive after the burst: " << weaponVoices << "\n";
    CHECK(fired >= 20);
    CHECK(weaponVoices == 1);

    // A second shooter with a burst of its own: one more slot, not ten.
    if (shooters.size() > 1) {
        Entity& b = *shooters[1];
        const Vec3 bo = b.obj->get("origin").asVector();
        for (int i = 0; i < 12; ++i) {
            w.debugFireBulletFrom(b, {bo[0], bo[1], bo[2] + 56.0f}, {bo[0] + 400.0f, bo[1], bo[2] + 56.0f});
            w.debugFireBulletFrom(a, {ao[0], ao[1], ao[2] + 56.0f}, {ao[0] + 400.0f, ao[1], ao[2] + 56.0f});
            mixer.mix(buf.data(), 2400);
        }
        weaponVoices = 0;
        for (const std::string& d : mixer.describeVoices())
            if (d.find("_fire") != std::string::npos) ++weaponVoices;
        std::cout << "fire voices of two shooters: " << weaponVoices << "\n";
        CHECK(weaponVoices <= 2);
    }

    // Grenades that bounce on the floor in the middle of the shooting (event sounds go through the world, owner 0x7fe). Eight dropped around
    // the shooter; those that land on world geometry bounce (a hit on an entity brush is not a bounce event, 0x5262bf).
    started.clear();
    for (int k = 0; k < 8; ++k) {
        const float ang = float(k) * 0.785398f;
        w.debugSpawnGrenade("fraggrenade", {ao[0] + 220.0f * std::cos(ang), ao[1] + 220.0f * std::sin(ang), ao[2] + 60.0f}, {0, 0, -700.0f}, 5000);
    }
    int bounces = 0, refused = 0;
    for (int i = 0; i < 12; ++i) {
        w.debugFireBulletFrom(a, {ao[0], ao[1], ao[2] + 56.0f}, {ao[0] + 400.0f, ao[1], ao[2] + 56.0f});
        w.frame();
        mixer.mix(buf.data(), 2400);
    }
    for (const Started& s : started)
        if (s.alias.rfind("grenade_bounce_", 0) == 0) (s.handle ? bounces : refused)++;
    std::cout << "grenade bounce sounds during the burst: started " << bounces << ", refused " << refused << "\n";
    CHECK(bounces > 0);
    CHECK(refused == 0);

    if (failures) return 1;
    std::cout << "sound pool tests passed\n";
    return 0;
}
