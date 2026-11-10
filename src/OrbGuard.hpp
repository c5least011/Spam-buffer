#pragma once
#include "Types.hpp"

namespace sb {

// Orb assist: a rescue, not an auto-player.
//  - the level's blue orbs are scanned once into zones (Core::zones);
//  - the orbs of a zone are "painted" into a path (the orbs plus the boxes joining
//    neighbours, padded by Cfg::orbPad);
//  - only while the player keeps spamming, and the button is up, the ghost-player
//    engine is asked whether the player is about to leave that path (fall off the
//    side, hit something) and whether a tap now, on a blue orb, keeps it on the
//    path (Engine::sbOrbRescue);
//  - only then a tap (press + release after Cfg::orbTap steps) is injected. The
//    player's CPS is never touched: stop spamming and the assist stops at once.
struct OrbGuard {
    uint64_t releaseAt = 0;
    uint64_t lastRescue = 0;
    uint64_t rescues = 0;
    bool modHeld = false;
    bool engaged = false;   // inside a zone and being watched this step

    void reset() { *this = OrbGuard{}; }

    template <class Verdict>
    Action step(uint64_t s, const View& v, bool gameDown, bool playerDown, bool enabled,
                bool spam, Verdict&& verdict) {
        if (releaseAt && s >= releaseAt) {              // finish our own tap first
            releaseAt = 0;
            const bool was = modHeld;
            modHeld = false;
            if (was && gameDown && !playerDown) return {true, false, Source::Orb};
        }
        engaged = false;
        if (!enabled || !v.valid || v.orbZone < 0 || modHeld || !spam) return {};
        if (playerDown || gameDown) return {};
        if (lastRescue && s < lastRescue + uint64_t(Cfg::orbCooldown)) return {};
        engaged = true;
        if (verdict()) {
            lastRescue = s;
            ++rescues;
            releaseAt = s + uint64_t(Cfg::orbTap);
            modHeld = true;
            return {true, true, Source::Orb};
        }
        return {};
    }
};

} // namespace sb
