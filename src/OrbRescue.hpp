#pragma once
#include "Types.hpp"

namespace sb {

// Feature C: adaptive rescue, not an auto-player. Acts only when ALL hold:
//  - the player has a recent real click rhythm (>=3 presses, not stopped),
//  - a blue orb's region is about to be left (progressive danger 0..1),
//  - the player is "overdue" relative to their own median click interval,
//  - the player has not clicked inside this orb and is not holding the button.
// At most one rescue tap per orb. The tap is a press + release pair (consistent).
struct OrbRescue {
    const void* cur = nullptr;
    bool pressedSince = false;
    std::unordered_set<const void*> rescued; // cleared on reset/death/level change
    std::deque<uint64_t> presses;            // analysis window of real presses
    uint64_t releaseAt = 0;
    bool modHeld = false;
    float danger = 0.f;
    bool engaged = false;

    void reset() {
        cur = nullptr; pressedSince = false; rescued.clear(); presses.clear();
        releaseAt = 0; modHeld = false; danger = 0.f; engaged = false;
    }

    void onReal(uint64_t s, bool down) {
        if (!down) return;
        presses.push_back(s);
        while (presses.size() > 8) presses.pop_front();
        if (cur) pressedSince = true;
    }

    Action step(uint64_t s, const OrbView& o, bool gameDown, bool playerDown, bool enabled, const Cfg& c) {
        if (releaseAt && s >= releaseAt) {              // finish our own tap first
            releaseAt = 0;
            bool was = modHeld;
            modHeld = false;
            if (was && gameDown && !playerDown) return {true, false, Source::Orb};
        }
        engaged = false;
        danger = 0.f;
        if (!o.valid) { cur = nullptr; pressedSince = false; return {}; }
        if (!enabled) return {};
        if (o.id != cur) { cur = o.id; pressedSince = false; }
        if (rescued.count(o.id) || pressedSince || playerDown || gameDown) return {};
        if (presses.size() < 3) return {};

        std::deque<float> iv;
        for (size_t i = 1; i < presses.size(); ++i) iv.push_back(float(presses[i] - presses[i - 1]));
        float med = std::max(1.f, medianOf(iv));
        float since = float(s - presses.back());
        if (since > 3.f * med + 2.f) return {};          // player is not spamming

        danger = std::clamp(1.f - o.stepsLeft / float(std::max(1, c.orbLookahead)), 0.f, 1.f);
        engaged = danger > 0.f;
        float late = std::min(1.5f, since / med);
        if (danger * late >= c.orbSens) {
            rescued.insert(o.id);
            releaseAt = s + 1;
            modHeld = true;
            return {true, true, Source::Orb};
        }
        return {};
    }
};

} // namespace sb
