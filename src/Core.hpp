#pragma once
#include <cstdio>
#include <utility>
#include "Types.hpp"
#include "FramePerfect.hpp"
#include "OrbGuard.hpp"

namespace sb {

// Each side has its own physical/game button state and its own assist engines.
// This is important in dual mode: P1 and P2 must never share a CPS history or
// a logical held state, otherwise one side can suppress or overwrite the other.
//
// Wave spam is a delay buffer: the decision (how many ticks to hold an event
// back) comes from the ghost-player engine (Engine.hpp) through the `decide`
// callback of onReal(). The lane only stores the held-back events in order and
// injects them itself, one per step, so lastGameToggle / gameDown stay exact.
struct InputLane {
    EventStream stream;
    FramePerfect fp;
    OrbGuard orb;

    std::deque<Pend> pend;          // held-back wave events, in order
    uint64_t lastArr = 0;           // step the last held-back event arrived
    uint64_t lastDelayStep = 0;     // last time a delay was applied (HUD)
    int lastDelay = 0;              // its length, in ticks (HUD)
    uint64_t delayed = 0;           // events delayed since the last reset (HUD)

    uint64_t lastGameToggle = 0;
    uint64_t lastAssistStep = 0;
    uint64_t assists = 0;
    bool gameDown = false;
    bool playerDown = false;
    bool upside = false;
    int modRun = 0;
    Mode mode = Mode::None;

    void reset(uint64_t now = 0) {
        stream.clear();
        fp.reset();
        orb.reset();
        pend.clear();
        lastArr = 0;
        lastDelayStep = 0;
        lastDelay = 0;
        delayed = 0;
        gameDown = false;
        playerDown = false;
        lastGameToggle = now;
        lastAssistStep = 0;
        assists = 0;
        modRun = 0;
        mode = Mode::None;
        upside = false;
    }

    // Record one physical input for this side only. While FP owns the lane,
    // physical events update its activity timer but are not forwarded twice.
    // `decide(down)` is asked ONLY for an event that would change the game's
    // button state right now; it returns the number of ticks to hold it back.
    // `tick` = index of the next physics tick (the engine's clock): held-back events
    // are counted and fired in those ticks, not in input-queue steps.
    template <class Decide>
    bool onReal(uint64_t step, uint64_t tick, bool down, const Cfg& cfg, Decide&& decide) {
        bool forward = !fp.active && down != gameDown;
        if (!fp.active && cfg.wave) {
            if (!pend.empty()) {
                // an earlier event is still held back: stay behind it and keep
                // the spacing the player had (no new search)
                const uint64_t gap = std::max<uint64_t>(1, tick - lastArr);
                pend.push_back({pend.back().at + gap, down});
                lastArr = tick;
                forward = false;
            } else if (forward) {
                const int delay = decide(down);
                if (delay > 0) {
                    pend.push_back({tick + uint64_t(delay), down});
                    lastArr = tick;
                    lastDelayStep = step;
                    lastDelay = delay;
                    ++delayed;
                    forward = false;
                }
            }
        }
        stream.push(step, double(step) / double(std::max(1, cfg.tps)), down,
                    Origin::Player, forward);
        playerDown = down;
        modRun = 0;
        fp.onReal(step);
        if (forward) {
            gameDown = down;
            lastGameToggle = step;
        }
        return forward;
    }

    // Held-back events are fired by main.cpp at the start of each physics tick
    // (fireDueWave). Here we only drop the queue when wave control is no longer in
    // charge (left wave / switched off): the game goes back to the finger's state.
    Action flushPending(const View& view, bool waveOn) {
        if (pend.empty() || (waveOn && view.valid && view.isWave)) return {};
        pend.clear();
        return gameDown != playerDown ? Action{true, playerDown, Source::Sync} : Action{};
    }

    template <class Verdict>
    Action onStep(uint64_t step, const View& view, const Cfg& cfg, Verdict&& verdict) {
        if (view.valid) {
            upside = view.upsideDown;
            stream.trim(step, 4096);
        }
        mode = Mode::None;

        Action action;
        if (cfg.fp) action = fp.step(step, gameDown, playerDown, cfg);
        else action = fp.off(gameDown, playerDown);

        if (fp.active) {
            pend.clear();                       // FP owns the lane
            mode = Mode::FramePerfect;
        } else {
            if (!action.want) action = flushPending(view, cfg.wave);
            if (!pend.empty() || (lastDelayStep && step - lastDelayStep < 30))
                mode = Mode::Wave;
            if (!action.want && pend.empty() && view.valid) {
                action = orb.step(step, view, gameDown, playerDown, cfg.orb, spamming(step, cfg), verdict);
                if (orb.engaged || orb.modHeld) mode = Mode::Orb;
            }
        }
        if (action.want && action.down == gameDown) action = {};
        return action;
    }

    void committed(uint64_t step, const Action& action, const Cfg& cfg) {
        stream.push(step, double(step) / double(std::max(1, cfg.tps)),
                    action.down, Origin::Mod, true);
        gameDown = action.down;
        lastGameToggle = step;
        lastAssistStep = step;
        ++modRun;
        ++assists;
    }

    // The player is spamming: at least two presses in the last Cfg::orbSpamWindow
    // seconds and the latest one no older than Cfg::orbSpamIdle.
    bool spamming(uint64_t step, const Cfg& cfg) const {
        const double tps = double(std::max(1, cfg.tps));
        const uint64_t win = uint64_t(Cfg::orbSpamWindow * tps);
        const uint64_t idle = uint64_t(Cfg::orbSpamIdle * tps);
        int presses = 0;
        bool recent = false;
        for (auto it = stream.ev.rbegin(); it != stream.ev.rend(); ++it) {
            if (it->step + win < step) break;
            if (it->origin == Origin::Player && it->down) {
                ++presses;
                if (it->step + idle >= step) recent = true;
            }
        }
        return presses >= 2 && recent;
    }

    float inputCps(uint64_t step, const Cfg& cfg) const {
        const uint64_t window = uint64_t(std::max(1, cfg.tps));
        int presses = 0;
        for (auto it = stream.ev.rbegin(); it != stream.ev.rend(); ++it) {
            if (it->step + window < step) break;
            if (it->origin == Origin::Player && it->down) ++presses;
        }
        return float(presses);
    }

    const char* modeName() const {
        switch (mode) {
            case Mode::Wave: return "Wave";
            case Mode::FramePerfect: return "FP";
            case Mode::Orb: return "Orb";
            default: return "idle";
        }
    }
};

// Arbiter shared by the two independent input lanes. Frame Perfect has priority
// over orb/wave assistance separately on each side.
struct Core {
    Cfg cfg;
    InputLane p1;
    InputLane p2;

    std::vector<OrbPt> orbs;        // every blue orb of the level, sorted by x0
    std::vector<OrbZone> zones;     // groups of orbs, sorted by x0
    bool dual = false;                   // two separate lanes (real 2-player mode): show P2

    // raw input counters, shown in the CPS window while TRAJECTORY is on
    struct Dbg {
        int in[2][2] = {};   // [flag: 0 = player 1, 1 = player 2][0 = press, 1 = release]
        int fwd = 0, inj = 0;
    } dbg;

    uint64_t step = 0;
    bool active = false;
    bool clockBad = false;
    float clockRate = 0.f;

    static Core& get() { static Core instance; return instance; }

    InputLane& lane(bool isPlayer1) { return isPlayer1 ? p1 : p2; }
    const InputLane& lane(bool isPlayer1) const { return isPlayer1 ? p1 : p2; }

    void reset() {
        p1.reset(step);
        p2.reset(step);
        dbg = Dbg{};
    }
    void onPause() { reset(); }

    template <class Decide>
    bool onReal(bool isPlayer1, bool down, uint64_t tick, Decide&& decide) {
        if (!active) return true;
        return lane(isPlayer1).onReal(step, tick, down, cfg, std::forward<Decide>(decide));
    }

    // Index of the blue-orb zone x is inside (or within `margin` of), else -1.
    int zoneIndex(float x, float margin) const {
        auto it = std::upper_bound(zones.begin(), zones.end(), x + margin,
            [](float v, const OrbZone& z) { return v < z.x0; });
        if (it == zones.begin()) return -1;
        --it;
        return x <= it->x1 + margin ? int(it - zones.begin()) : -1;
    }

    // verdict(isPlayer1) -> bool: "a rescue tap is needed now" (ghost engine)
    template <class Verdict>
    std::array<Action, 2> onStep(const View& view1, const View& view2, Verdict&& verdict) {
        ++step;
        std::array<Action, 2> actions{};
        if (!active) return actions;
        actions[0] = p1.onStep(step, view1, cfg, [&] { return verdict(true); });
        actions[1] = p2.onStep(step, view2, cfg, [&] { return verdict(false); });
        return actions;
    }

    void committed(bool isPlayer1, const Action& action) {
        lane(isPlayer1).committed(step, action, cfg);
    }

    std::string hud() const {
        auto laneText = [this](const InputLane& input) {
            char wave[40];
            if (!cfg.wave) std::snprintf(wave, sizeof wave, "off");
            else if (!input.pend.empty()) std::snprintf(wave, sizeof wave, "HOLD %zu", input.pend.size());
            else if (input.delayed) std::snprintf(wave, sizeof wave, "on d%d x%llu", input.lastDelay,
                                                  (unsigned long long)input.delayed);
            else std::snprintf(wave, sizeof wave, "on");
            char b[224];
            std::snprintf(b, sizeof b,
                "%s:%s  cps:%.1f  fp:%s %.1f/%.1f  wave:%s",
                input.modeName(), input.assists && step - input.lastAssistStep < 30 ? "ON" : "off",
                input.inputCps(step, cfg), input.fp.active ? "ACTIVE" : (cfg.fp ? "armed" : "off"),
                input.fp.targetCps(cfg), input.fp.minimumCps(cfg), wave);
            return std::string(b);
        };

        const bool showP2 = dual || dbg.in[1][0] || dbg.in[1][1];
        std::string result = (showP2 ? "P1 " : "") + laneText(p1);
        if (showP2) result += "\nP2 " + laneText(p2);
        if (clockBad) {
            char warning[96];
            std::snprintf(warning, sizeof warning,
                "\nclock %.0f/s != TPS %d (timing warning)", clockRate, cfg.tps);
            result += warning;
        }
        return result;
    }
};

} // namespace sb
