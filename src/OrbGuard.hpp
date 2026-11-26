#pragma once
#include "Types.hpp"

namespace sb {

// Orb assist (rescue layer): runs on physics ticks (`tick`), NOT render frames.
//  - all standard gameplay orbs of the level are scanned once into zones (Core::zones);
//  - only while the player keeps spamming, and the button is up, the ghost-player engine
//    checks if an unconsumed orb can be triggered NOW, whether doing nothing kills or leaves
//    the orb chain, and whether tapping NOW hits the orb and survives clearly longer;
//  - only then a crisp tap (press + release after Cfg::orbTap physics ticks) is injected.
//    Stop spamming and the assist stops at once.
struct OrbGuard {
    uint64_t releaseAt = 0;   // physics tick when our rescue tap must release
    uint64_t lastRescue = 0;  // physics tick of the last rescue tap
    uint64_t rescues = 0;
    bool modHeld = false;
    bool engaged = false;     // inside an orb zone and being watched this tick

    void reset() { *this = OrbGuard{}; }

    template <class Verdict>
    Action step(uint64_t tick, const View& v, bool gameDown, bool playerDown, bool enabled,
                bool spam, Verdict&& verdict) {
        if (releaseAt && tick >= releaseAt) {            // finish our own tap first
            releaseAt = 0;
            const bool was = modHeld;
            modHeld = false;
            if (was && gameDown && !playerDown) return {true, false, Source::Orb};
        }
        engaged = false;
        if (!enabled || !v.valid || v.orbZone < 0 || modHeld || !spam) return {};
        if (playerDown || gameDown) return {};
        if (lastRescue && tick < lastRescue + uint64_t(Cfg::orbCooldown)) return {};
        engaged = true;
        if (verdict()) {
            lastRescue = tick;
            ++rescues;
            releaseAt = tick + uint64_t(Cfg::orbTap);
            modHeld = true;
            return {true, true, Source::Orb};
        }
        return {};
    }
};

} // namespace sb
