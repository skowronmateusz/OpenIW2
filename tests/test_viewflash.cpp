// The view weapon's muzzle flash flag is consumed in every posed frame and played only when the view model is drawn
// (not under the scope overlay), so a shot fired while scoped never flashes after leaving the scope.

#include "game/viewflash.h"

#include <iostream>

int main() {
    using iw2::game::consumeViewFlash;
    int failures = 0;
    auto check = [&](bool c, int line) { if (!c) { std::cerr << "test_viewflash.cpp:" << line << ": CHECK failed\n"; ++failures; } };
    bool pending = true;
    check(consumeViewFlash(pending, true) && !pending, __LINE__);   // normal frame: played, cleared
    check(!consumeViewFlash(pending, true), __LINE__);              // nothing queued: nothing played
    pending = true;
    check(!consumeViewFlash(pending, false) && !pending, __LINE__); // under the scope overlay: cleared, not played
    check(!consumeViewFlash(pending, true), __LINE__);              // leaving the scope: no late flash
    if (failures) return 1;
    std::cout << "viewflash ok\n";
    return 0;
}
