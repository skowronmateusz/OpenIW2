// Debug recording of player moves: the state before each playerMove call, its input and frame time, and the entity
// clips the trace saw. iw2view keeps the last seconds in memory and writes them out (`P`, or when the player ends up
// under the world); `iw2game <main> <map> --replay file` runs them again. The file is a raw dump for the same build.
#pragma once

#include "game/collision.h"
#include "game/pmove.h"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace iw2::game {

struct RecordedMove {
    PlayerState before, after;
    PlayerInput input;
    float dt = 0;
    MoveParams params;
    std::vector<EntityClip> clips;
};

class MoveRecorder {
public:
    size_t capacity = 1200; // moves kept (20 s at 60 Hz)

    void add(RecordedMove m) {
        if (moves_.size() >= capacity) moves_.pop_front();
        moves_.push_back(std::move(m));
    }

    bool write(const std::string& path) const {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        const uint32_t n = uint32_t(moves_.size());
        std::fwrite(&n, sizeof n, 1, f);
        for (const RecordedMove& m : moves_) {
            std::fwrite(&m.before, sizeof m.before, 1, f);
            std::fwrite(&m.after, sizeof m.after, 1, f);
            std::fwrite(&m.input, sizeof m.input, 1, f);
            std::fwrite(&m.dt, sizeof m.dt, 1, f);
            std::fwrite(&m.params, sizeof m.params, 1, f);
            const uint32_t c = uint32_t(m.clips.size());
            std::fwrite(&c, sizeof c, 1, f);
            if (c) std::fwrite(m.clips.data(), sizeof(EntityClip), c, f);
        }
        return std::fclose(f) == 0;
    }

    static std::vector<RecordedMove> read(const std::string& path) {
        std::vector<RecordedMove> out;
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return out;
        uint32_t n = 0;
        if (std::fread(&n, sizeof n, 1, f) != 1) n = 0;
        for (uint32_t i = 0; i < n; ++i) {
            RecordedMove m;
            uint32_t c = 0;
            if (std::fread(&m.before, sizeof m.before, 1, f) != 1 || std::fread(&m.after, sizeof m.after, 1, f) != 1 ||
                std::fread(&m.input, sizeof m.input, 1, f) != 1 || std::fread(&m.dt, sizeof m.dt, 1, f) != 1 ||
                std::fread(&m.params, sizeof m.params, 1, f) != 1 || std::fread(&c, sizeof c, 1, f) != 1)
                break;
            m.clips.resize(c);
            if (c && std::fread(m.clips.data(), sizeof(EntityClip), c, f) != c) break;
            out.push_back(std::move(m));
        }
        std::fclose(f);
        return out;
    }

private:
    std::deque<RecordedMove> moves_;
};

} // namespace iw2::game
