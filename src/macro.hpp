// Pure logic for Frame Macro. No Geode includes, so tests/test_macro.cpp can build it anywhere.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace fm {

constexpr int kTps = 240;  // GD's default physics rate; one "frame" here is one physics tick

struct Input {
    uint32_t frame = 0;     // tick the input is applied on (applied just before that tick runs)
    uint8_t button = 1;     // 1 jump, 2 left, 3 right (platformer)
    bool down = false;
    bool isPlayer1 = true;  // raw 3rd argument of GJBaseGameLayer::handleButton, replayed as-is
    bool operator==(Input const&) const = default;
};

constexpr int kBuckets = 9;
inline constexpr char const* kBucketNames[kBuckets] = {"1", "2", "3", "4", "5-6", "7-8", "9-12", "13-16", "17+"};

// Frames between two inputs -> histogram bucket. A gap of 0 (same tick) is not a gap: -1.
constexpr int bucketOf(uint32_t gap) {
    if (gap == 0) return -1;
    if (gap <= 4) return static_cast<int>(gap) - 1;
    if (gap <= 6) return 4;
    if (gap <= 8) return 5;
    if (gap <= 12) return 6;
    if (gap <= 16) return 7;
    return 8;
}

struct GapStats {
    std::array<uint32_t, kBuckets> n{};
    int64_t last = -1;

    void add(uint32_t frame) {
        if (last >= 0 && frame > last) ++n[bucketOf(static_cast<uint32_t>(frame - last))];
        last = std::max<int64_t>(last, frame);
    }

    // Stats for the first `count` inputs. pressesOnly = ignore releases.
    static GapStats build(std::vector<Input> const& in, size_t count, bool pressesOnly) {
        GapStats s;
        for (size_t i = 0; i < std::min(count, in.size()); ++i)
            if (!pressesOnly || in[i].down) s.add(in[i].frame);
        return s;
    }
};

struct Macro {
    std::vector<Input> inputs;  // always sorted by frame

    uint32_t length() const { return inputs.empty() ? 0 : inputs.back().frame; }

    // Index of the first input on or after `frame`.
    size_t lowerBound(uint32_t frame) const {
        auto it = std::lower_bound(inputs.begin(), inputs.end(), frame,
                                   [](Input const& i, uint32_t f) { return i.frame < f; });
        return static_cast<size_t>(it - inputs.begin());
    }

    // Drop every input on or after `frame` (a respawn rewinds the attempt to that frame).
    void truncateFrom(uint32_t frame) { inputs.erase(inputs.begin() + lowerBound(frame), inputs.end()); }

    // Text format: "framemacro 1", then one "frame button down isPlayer1" line per input.
    std::string serialize() const {
        std::ostringstream o;
        o << "framemacro 1\n";
        for (auto const& i : inputs)
            o << i.frame << ' ' << int(i.button) << ' ' << int(i.down) << ' ' << int(i.isPlayer1) << '\n';
        return o.str();
    }

    // False (and `out` untouched) on any malformed line.
    static bool parse(std::string const& text, Macro& out) {
        std::istringstream in(text);
        std::string line;
        auto next = [&] {
            if (!std::getline(in, line)) return false;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        };
        if (!next() || line != "framemacro 1") return false;

        Macro m;
        while (next()) {
            if (line.empty()) continue;
            std::istringstream ls(line);
            int64_t frame;
            int button, down, p1;
            char extra;
            if (!(ls >> frame >> button >> down >> p1) || (ls >> extra)) return false;
            if (frame < 0 || frame > UINT32_MAX || button < 1 || button > 3) return false;
            if (down < 0 || down > 1 || p1 < 0 || p1 > 1) return false;
            if (!m.inputs.empty() && frame < m.inputs.back().frame) return false;
            m.inputs.push_back({static_cast<uint32_t>(frame), static_cast<uint8_t>(button), down == 1, p1 == 1});
        }
        out = std::move(m);
        return true;
    }
};

}  // namespace fm
