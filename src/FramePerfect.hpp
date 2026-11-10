#pragma once
#include "Types.hpp"

namespace sb {

// Frame Perfect has two phases:
// 1) Start only after real input reaches targetCps - fpSlowerCps.
// 2) Once started, produce exactly the configured target cadence. Do not
//    re-check CPS, level parts, structures, collisions, or path state.
//    Stop only after real input has gone quiet.
struct FramePerfect {
    bool active = false;
    bool hasReal = false;
    std::deque<uint64_t> t;
    uint64_t lastReal = 0;
    int half = 1;             // approximate half-period, for HUD only
    bool aligned = false;
    double phase = 0.0;       // accumulated output toggles; range [0, 1)

    void reset() { *this = FramePerfect{}; }

    void onReal(uint64_t s) {
        lastReal = s;
        hasReal = true;
        // Multiple physical events may share one game step. They refresh the
        // activity timer, but equal timestamps cannot be used for CPS measurement.
        if (!t.empty() && s <= t.back()) return;
        t.push_back(s);
        while (t.size() > 5) t.pop_front();
    }

    float targetCps(const Cfg& c) const {
        const float tps = float(std::max(1, c.tps));
        // At most one button-state toggle can be emitted per physics step.
        return std::clamp(c.fpCps, 0.1f, tps * 0.5f);
    }

    // Absolute CPS subtraction, not a percentage/ratio.
    // Example: target 15 CPS and allowance 3 CPS => minimum input 12 CPS.
    float minimumCps(const Cfg& c) const {
        const float shortfall = std::max(0.f, c.fpSlowerCps);
        return std::max(0.1f, targetCps(c) - shortfall);
    }

    void configure(const Cfg& c) {
        const float tps = float(std::max(1, c.tps));
        const float exactHalfPeriod = tps / (2.f * targetCps(c));
        half = std::max(1, int(std::lround(exactHalfPeriod)));
        aligned = std::fabs(exactHalfPeriod - float(half)) < 0.0001f;
    }

    float measuredCps(const Cfg& c) const {
        if (t.size() < 2 || t.back() <= t.front()) return 0.f;
        const float toggleCount = float(t.size() - 1);
        const float elapsedSteps = float(t.back() - t.front());
        return toggleCount * float(std::max(1, c.tps)) / (2.f * elapsedSteps);
    }

    float measuredToggleInterval() const {
        if (t.size() < 2) return 0.f;
        std::deque<float> intervals;
        for (size_t i = 1; i < t.size(); ++i) {
            if (t[i] > t[i - 1])
                intervals.push_back(float(t[i] - t[i - 1]));
        }
        return medianOf(intervals);
    }

    static Action sync(bool gameDown, bool playerDown) {
        if (gameDown != playerDown) return {true, playerDown, Source::Sync};
        return {};
    }

    Action off(bool gameDown, bool playerDown) {
        const bool wasActive = active;
        active = false;
        hasReal = false;
        lastReal = 0;
        t.clear();
        phase = 0.0;
        return wasActive ? sync(gameDown, playerDown) : Action{};
    }

    Action step(uint64_t s, bool gameDown, bool playerDown, const Cfg& c) {
        configure(c);
        const float requiredInputCps = minimumCps(c);

        if (!active) {
            // Two complete input intervals are required before enabling.
            // This CPS threshold is checked only at activation, never while active.
            if (t.size() < 3 || measuredCps(c) < requiredInputCps) return {};
            active = true;
            phase = 0.0;
        } else {
            // Activity watchdog only. A lower measured CPS never disables FP once
            // active; only the absence of physical input for the timeout does.
            // Use the slower of the recent cadence and activation minimum as the
            // baseline so ordinary jitter does not look like a stopped spam stream.
            const float minExpectedInterval = float(std::max(1, c.tps)) /
                                              (2.f * requiredInputCps);
            const float observedInterval = measuredToggleInterval();
            const float expectedInterval = std::max(minExpectedInterval,
                                                    observedInterval);
            const uint64_t stopAfter = uint64_t(std::max(2.f,
                std::ceil(expectedInterval * 1.6f)));
            if (!hasReal || s < lastReal || s - lastReal > stopAfter)
                return off(gameDown, playerDown);
        }

        // Fixed target output: emit 2 * targetCps state changes per second.
        // Fractional rates are distributed across frames by the phase accumulator.
        // This deliberately does not inspect level parts, objects, collisions,
        // noclip state, player path, or current CPS after activation.
        const double togglesPerStep = (2.0 * double(targetCps(c))) /
                                      double(std::max(1, c.tps));
        phase += togglesPerStep;
        if (phase >= 1.0) {
            phase -= 1.0;
            return {true, !gameDown, Source::FramePerfect};
        }
        return {};
    }
};

} // namespace sb
