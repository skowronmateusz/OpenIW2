// iw2game --autoplay bookkeeping of when each "death" wait first showed up (tools/iw2game/deathseen.h): an entry is tied to its
// object, so a later object at a reused address starts fresh. Before, it inherited the freed object's time, and as the allocator's
// address reuse differs between runs, the autoplay kill waves (and so hill400_defend's fire rates) differed between runs of one seed.
#include "deathseen.h"
#include "script/vm.h"

#include <iostream>

namespace {
int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                                \
        }                                                                              \
    } while (0)
} // namespace

int main() {
    using iw2::script::Object;
    using iw2::script::ObjectPtr;
    iw2::tools::DeathSeen seen;

    // The first time an object is noted is kept.
    const ObjectPtr a = std::make_shared<Object>();
    CHECK(!seen.first(a));
    seen.note(a, 10.0f);
    seen.note(a, 12.0f);
    CHECK(seen.first(a) == 10.0f);

    // A freed object's address handed out again: modelled with two aliasing pointers to one address, each with its own owner.
    static Object slot;
    auto owner1 = std::make_shared<int>(1);
    ObjectPtr first(owner1, &slot);
    seen.note(first, 20.0f);
    CHECK(seen.first(first) == 20.0f);
    first.reset();
    owner1.reset(); // the first object is gone
    auto owner2 = std::make_shared<int>(2);
    const ObjectPtr second(owner2, &slot);
    CHECK(second.get() == &slot);
    CHECK(!seen.first(second)); // not the old time
    seen.note(second, 30.0f);
    CHECK(seen.first(second) == 30.0f);
    seen.note(second, 31.0f);
    CHECK(seen.first(second) == 30.0f);

    // Another object's entry is untouched.
    CHECK(seen.first(a) == 10.0f);

    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
