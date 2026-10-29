// beginprediction / predictanim / predictoriginandangles / endprediction (needs IW2_MAIN): a prediction run on an actor
// steps its animation and moves its origin, and endprediction puts the tree, origin and angles back.
#include "fs/vfs.h"
#include "game/world.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace iw2::game;

static int failures = 0;
#define CHECK(c)                                                                        \
    do {                                                                                \
        if (!(c)) { std::cout << "FAIL line " << __LINE__ << ": " #c "\n"; ++failures; } \
    } while (0)

int main() {
    const char* main = std::getenv("IW2_MAIN");
    if (!main) {
        std::cout << "actor predict test skipped (IW2_MAIN not set)\n";
        return 0;
    }
    const auto dir = std::filesystem::temp_directory_path() / ("iw2_test_predict_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "maps");
    std::ofstream(dir / "maps/predtest.gsc") << R"(
main()
{
	self.o0 = self.origin;
	self.a0 = self.angles;
	self beginprediction();
	self.stops = 0;
	for (i = 0; i < 20; i++)
	{
		if (self predictanim(true))
			self.stops++;
		self predictanim(false);
		self predictoriginandangles();
	}
	self.o1 = self.origin;
	self endprediction();
	self.o2 = self.origin;
	self.a2 = self.angles;
	self.done = 1;
}
)";
    iw2::fs::Vfs vfs;
    vfs.mountDirectory(main);
    vfs.mountDirectory(dir.string());
    World w(vfs, "toujane");
    w.start();
    for (int i = 0; i < 200; ++i) w.frame();
    // The first actor the script gets through (riders are linked to their vehicle, which predictoriginandangles refuses).
    Entity* actor = nullptr;
    int refused = 0;
    for (const auto& e : w.entities()) {
        if (!e->isActor || !e->anim || e->obj->removed || !e->obj->get("origin").isVector()) continue;
        w.vm().start("maps/predtest", "main", {}, iw2::script::Value(e->obj));
        if (e->obj->get("done").isNumber()) {
            actor = e.get();
            break;
        }
        ++refused;
        w.debugResetPrediction();
    }
    std::cout << "actors refused before one ran: " << refused << "\n";
    CHECK(actor != nullptr);
    if (!actor) return 1;
    const auto o0 = actor->obj->get("o0").asVector(), o1 = actor->obj->get("o1").asVector(), o2 = actor->obj->get("o2").asVector();
    std::cout << "o0 " << o0[0] << " " << o0[1] << " " << o0[2] << " o1 " << o1[0] << " " << o1[1] << " " << o1[2] << " stops "
              << actor->obj->get("stops").toString() << "\n";
    CHECK(!(o1 == o0)); // the predicted steps moved the actor
    CHECK(o2 == o0);   // and endprediction put it back
    CHECK(actor->obj->get("a2").asVector() == actor->obj->get("a0").asVector());
    std::filesystem::remove_all(dir);
    if (failures) return 1;
    std::cout << "actor predict test passed\n";
    return 0;
}
