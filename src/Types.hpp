#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

namespace sb {

enum class Origin : uint8_t { Player, Mod };
enum class Source : uint8_t { None, Wave, FramePerfect, Orb, Sync };
enum class Mode : uint8_t { None, Wave, FramePerfect, Orb };

struct Cfg {
    bool wave = true, fp = false, orb = true, hud = true, traj = false;
    // Wave spam = ghost-player buffer (Engine.hpp). Ranges are enforced in refreshCfg().
    int waveDelay = 6;                    // max ticks a press/release may be held back (0..8)
    int waveLook = 16;                    // ticks the ghost must survive (13..18)
    static constexpr int waveImminent = 8; // fixed
    float fpCps = 30.f;
    float fpSlowerCps = 3.f; // minimum input is target CPS minus this absolute CPS allowance
    int tps = 240;
    int orbLookahead = 40;                 // ticks the ghost looks ahead (10..120)
    static constexpr int orbTap = 4;       // steps a rescue tap is held (matches the ghost)
    static constexpr int orbCooldown = 6;  // steps between two rescues
    static constexpr float orbPad = 60.f;  // how far the painted path extends around the orbs
    static constexpr float orbSpamWindow = 0.8f;  // s: the orb assist runs only while you keep spamming
    static constexpr float orbSpamIdle = 0.35f;   // s: no press for this long = spam stopped, assist off
};

struct InputEvent {
    uint64_t seq;
    uint64_t step;
    double time;
    bool down;
    Origin origin;
    bool forwarded;
};

// A press / release held back by the wave buffer. Injected by the lane at step `at`.
struct Pend {
    uint64_t at;
    bool down;
};

struct EventStream {
    std::deque<InputEvent> ev;
    uint64_t nextSeq = 1;
    void push(uint64_t step, double t, bool down, Origin o, bool fwd) {
        ev.push_back({nextSeq++, step, t, down, o, fwd});
    }
    void trim(uint64_t now, uint64_t horizon) {
        while (!ev.empty() && ev.front().step + horizon < now) ev.pop_front();
    }
    void clear() { ev.clear(); }
};

struct View {
    bool valid = false, isWave = false, upsideDown = false;
    int orbZone = -1;         // index of the blue-orb zone the player is in / near (-1 = none)
};

// One blue orb's hitbox (level units), and a group of them that sit close together.
struct OrbPt {
    float x0, x1, y0, y1;
};
struct OrbZone {
    float x0, x1;       // x range of the zone
    int first, count;   // its orbs: Core::orbs[first .. first + count)
};

struct Action {
    bool want = false;
    bool down = false;
    Source src = Source::None;
};

inline float meanOf(const std::deque<float>& q) {
    if (q.empty()) return 0.f;
    float s = 0.f;
    for (float x : q) s += x;
    return s / float(q.size());
}
inline float medianOf(const std::deque<float>& q) {
    if (q.empty()) return 0.f;
    std::vector<float> v(q.begin(), q.end());
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}
inline float cvOf(const std::deque<float>& q) {
    float m = meanOf(q);
    if (q.size() < 2 || m <= 0.f) return 0.f;
    float s = 0.f;
    for (float x : q) s += (x - m) * (x - m);
    return std::sqrt(s / float(q.size())) / m;
}

} // namespace sb