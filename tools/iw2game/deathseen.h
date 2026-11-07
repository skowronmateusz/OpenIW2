// iw2game --autoplay: when each script "death" wait on an object first showed up (the kill gate takes the oldest wave first).
// Kept by object address, with the object held weakly: a freed object's address comes back for a later object (a corpse's
// released actor object), and which address the allocator hands out differs between runs, so an entry is valid only while it
// still names the same object. A stale entry is replaced, never inherited, which keeps two runs with the same seed identical.
#pragma once

#include "script/value.h"

#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace iw2::tools {

class DeathSeen {
public:
    // Records `t` for `o` unless `o` itself is already recorded.
    void note(const script::ObjectPtr& o, float t) {
        auto [it, fresh] = seen_.try_emplace(o.get(), o, t);
        if (!fresh && it->second.first.lock() != o) it->second = {o, t};
    }
    // The time `o` was first noted, none when it was not (or only an earlier object at the same address was).
    std::optional<float> first(const script::ObjectPtr& o) const {
        const auto it = seen_.find(o.get());
        if (it == seen_.end() || it->second.first.lock() != o) return std::nullopt;
        return it->second.second;
    }

private:
    std::map<const void*, std::pair<std::weak_ptr<script::Object>, float>> seen_;
};

} // namespace iw2::tools
