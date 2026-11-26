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
// Wave / Ship / Orb buffering uses the ghost-player engine (Engine.hpp) through
// the `decide` callback of onReal(). The lane stores held-back events in physics-tick
// order and injects them at the start of each physics tick (fireDueWave).
struct InputLane {
    EventStream stream;
    OrbGuard orb;

    std::deque<Pend> pend;          // held-back wave/ship/orb events, in physics ticks
    uint64_t lastArr = 0;           // physics tick the last held-back event arrived
    uint64_t lastDelayStep = 0;     // step the last delay was applied (HUD)
    int lastDelay = 0;              // its length, in physics ticks (HUD)
    uint64_t delayed = 0;           // events delayed since the last reset (HUD)

    uint64_t lastPress = 0;         // step of the last physical press on this side
    uint64_t lastPressTick = 0;     // physics tick of the last physical press on this side
    bool everPressed = false;

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
        orb.reset();
        lastPress = 0;
        lastPressTick = 0;
        everPressed = false;
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
    // `decide(down)` returns the number of physics ticks to hold the event back (0 = pass).
    template <class Decide>
    bool onReal(uint64_t step, uint64_t tick, bool down, const Cfg& cfg, bool fpOwns, Decide&& decide) {
        bool forward = !fpOwns && down != gameDown;
        if (!fpOwns && (cfg.wave || cfg.ship || cfg.orb)) {
            if (!pend.empty()) {
                if (cfg.orb) {
                    // Orb buffer only buffers presses; keep a clean release right after a pending press.
                    if (!down && pend.back().down) {
                        uint64_t relAt = pend.back().at + std::max<uint64_t>(1, uint64_t(Cfg::orbTap));
                        pend.push_back({relAt, false});
                        lastArr = tick;
                        forward = false;
                    } else if (down) {
                        // A brand-new press while an old orb tap is pending: drop stale tail release and evaluate fresh.
                        pend.clear();
                        if (down != gameDown) {
                            const int delay = decide(true);
                            if (delay > 0) {
                                pend.push_back({tick + uint64_t(delay), true});
                                lastArr = tick;
                                lastDelayStep = step;
                                lastDelay = delay;
                                ++delayed;
                                forward = false;
                            } else {
                                forward = true;
                            }
                        } else {
                            forward = false;
                        }
                    } else {
                        forward = false;
                    }
                } else {
                    // Wave / Ship: avoid runaway queue pile-up during rapid spam so `decide()` stays active!
                    if (pend.back().down == down) {
                        forward = false;
                    } else if (pend.size() >= 2) {
                        // Queue already has a delayed pair pending: collapse the tail so latency never snowballs,
                        // and preserve the player's latest toggle right after the front event.
                        uint64_t nextAt = std::max(tick, pend.front().at + 1);
                        pend.resize(1);
                        pend.push_back({nextAt, down});
                        lastArr = tick;
                        forward = false;
                    } else {
                        const uint64_t rawGap = tick > lastArr ? (tick - lastArr) : 1;
                        const uint64_t gap = std::clamp<uint64_t>(rawGap, 1, 8);
                        pend.push_back({pend.back().at + gap, down});
                        lastArr = tick;
                        forward = false;
                    }
                }
            } else if (forward && (!cfg.orb || down)) {
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
        stream.push(step, tick, double(step) / double(std::max(1, cfg.tps)), down,
                    Origin::Player, forward);
        playerDown = down;
        modRun = 0;
        if (down) {
            lastPress = step;
            lastPressTick = tick;
            everPressed = true;
        }
        if (forward) {
            gameDown = down;
            lastGameToggle = step;
        }
        return forward;
    }

    // Held-back events are fired by main.cpp at the start of each physics tick (fireDueWave).
    // Only drop the queue when buffering is no longer in charge (left vehicle / switched off).
    Action flushPending(const View& view) {
        if (pend.empty() || (view.valid && view.buffered)) return {};
        pend.clear();
        return gameDown != playerDown ? Action{true, playerDown, Source::Sync} : Action{};
    }

    // Keep the per-side bookkeeping current (every step, whoever owns the button).
    void track(uint64_t step, const View& view) {
        if (view.valid) {
            upside = view.upsideDown;
            stream.trim(step, 4096);
        }
        mode = Mode::None;
    }

    // Measure the player's actual spam rhythm in PHYSICS TICKS:
    // separate median hold duration (`outHold`) and release duration (`outRel`),
    // so ship prediction reflects asymmetric real ship duty cycles (e.g. 42% hold / 58% release).
    void spamDurations(int& outHold, int& outRel) const {
        std::deque<float> holds, rels;
        bool haveNext = false;
        uint64_t nextTick = 0;
        bool nextDown = false;
        for (auto it = stream.ev.rbegin(); it != stream.ev.rend() && (holds.size() < 6 || rels.size() < 6); ++it) {
            if (it->origin != Origin::Player) continue;
            if (haveNext && nextTick > it->tick && nextDown != it->down) {
                uint64_t d = nextTick - it->tick;
                if (d >= 1 && d <= 40) {
                    if (it->down) {
                        if (holds.size() < 6) holds.push_back(float(d));
                    } else {
                        if (rels.size() < 6) rels.push_back(float(d));
                    }
                }
            }
            nextTick = it->tick;
            nextDown = it->down;
            haveNext = true;
        }
        outHold = holds.empty() ? 4 : std::clamp(int(std::lround(medianOf(holds))), 2, 16);
        outRel = rels.empty() ? 5 : std::clamp(int(std::lround(medianOf(rels))), 2, 16);
    }

    // This side recently got physical input: it takes part in Frame Perfect.
    bool participates(uint64_t step, const Cfg& cfg) const {
        return everPressed && step - lastPress <= uint64_t(std::max(1, cfg.tps));
    }

    // Step bookkeeping + flush when leaving buffered mode (orb rescue runs on physics ticks via stepOrbTick).
    Action onStep(uint64_t step, const View& view, const Cfg& cfg) {
        Action action = flushPending(view);
        if (!pend.empty() || (lastDelayStep && step - lastDelayStep < 30)) {
            mode = cfg.orb ? Mode::Orb : Mode::Wave;
        }
        if (orb.engaged || orb.modHeld) mode = Mode::Orb;
        if (action.want && action.down == gameDown) action = {};
        return action;
    }

    // Physics-tick orb rescue (called at start of physics tick, aligned with the ghost engine).
    template <class Verdict>
    Action onOrbTick(uint64_t tick, float tps, const View& view, const Cfg& cfg, Verdict&& verdict) {
        if (!pend.empty()) return {};
        Action action = orb.step(tick, view, gameDown, playerDown, cfg.orb, spammingTicks(tick, tps), verdict);
        if (orb.engaged || orb.modHeld) mode = Mode::Orb;
        if (action.want && action.down == gameDown) action = {};
        return action;
    }

    void committed(uint64_t step, uint64_t tick, const Action& action, const Cfg& cfg) {
        stream.push(step, tick, double(step) / double(std::max(1, cfg.tps)),
                    action.down, Origin::Mod, true);
        gameDown = action.down;
        lastGameToggle = step;
        lastAssistStep = step;
        ++modRun;
        ++assists;
    }

    // The player is actively spamming (measured in physics ticks so it works at any FPS/TPS):
    // at least two physical presses in the last Cfg::orbSpamWindow seconds and latest <= Cfg::orbSpamIdle.
    bool spammingTicks(uint64_t tick, float tps) const {
        const double rate = double(std::max(30.f, tps));
        const uint64_t win = uint64_t(Cfg::orbSpamWindow * rate);
        const uint64_t idle = uint64_t(Cfg::orbSpamIdle * rate);
        int presses = 0;
        bool recent = false;
        for (auto it = stream.ev.rbegin(); it != stream.ev.rend(); ++it) {
            if (it->tick + win < tick) break;
            if (it->origin == Origin::Player && it->down) {
                ++presses;
                if (it->tick + idle >= tick) recent = true;
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
// over orb/wave/ship assistance separately on each side.
struct Core {
    Cfg cfg;
    InputLane p1;
    InputLane p2;
    FramePerfect fp;                // ONE Frame Perfect for everybody: CPS counted over all input

    std::vector<OrbPt> orbs;        // every gameplay spam orb of the level, sorted by x0
    std::vector<OrbZone> zones;     // groups of orbs, sorted by x0
    View lastViews[2];              // latest views cached for physics-tick orb rescue
    bool dual = false;              // two separate lanes (real 2-player mode): show P2

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
        fp.reset();
        lastViews[0] = View{};
        lastViews[1] = View{};
        dbg = Dbg{};
    }
    void onPause() { reset(); }

    template <class Decide>
    bool onReal(bool isPlayer1, bool down, uint64_t tick, Decide&& decide) {
        if (!active) return true;
        const bool fpOwns = fp.active;
        fp.onReal(step);          // every press, any side, feeds the same CPS count
        return lane(isPlayer1).onReal(step, tick, down, cfg, fpOwns, std::forward<Decide>(decide));
    }

    // Index of the orb zone x is inside (or within `margin` of), else -1.
    int zoneIndex(float x, float margin) const {
        auto it = std::upper_bound(zones.begin(), zones.end(), x + margin,
            [](float v, const OrbZone& z) { return v < z.x0; });
        if (it == zones.begin()) return -1;
        --it;
        return x <= it->x1 + margin ? int(it - zones.begin()) : -1;
    }

    std::array<Action, 2> onStep(const View& view1, const View& view2) {
        ++step;
        lastViews[0] = view1;
        lastViews[1] = view2;
        std::array<Action, 2> actions{};
        if (!active) return actions;

        InputLane* lanes[2] = {&p1, &p2};
        const View* views[2] = {&view1, &view2};
        for (int i = 0; i < 2; ++i) lanes[i]->track(step, *views[i]);

        // Frame Perfect: no player-1 / player-2 distinction. All input is one count;
        // once enough CPS is clicked it drives every side that is being pressed.
        const bool wasActive = fp.active;
        const bool anyPlayer = p1.playerDown || p2.playerDown;
        const bool anyGame = p1.gameDown || p2.gameDown;
        const Action fa = cfg.fp ? fp.step(step, anyGame, anyPlayer, cfg)
                                 : fp.off(anyGame, anyPlayer);

        bool part[2] = {p1.participates(step, cfg), p2.participates(step, cfg)};
        if (!part[0] && !part[1]) part[0] = true;

        for (int i = 0; i < 2; ++i) {
            InputLane& lane = *lanes[i];
            Action a;
            if (fp.active) {
                lane.pend.clear();
                lane.mode = Mode::FramePerfect;
                if (fa.want && part[i]) a = {true, fa.down, Source::FramePerfect};
            } else if (wasActive) {
                // FP just ended: every side goes back to its own finger
                if (lane.gameDown != lane.playerDown) a = {true, lane.playerDown, Source::Sync};
            } else {
                a = lane.onStep(step, *views[i], cfg);
            }
            if (a.want && a.down == lane.gameDown) a = {};
            actions[i] = a;
        }
        return actions;
    }

    void committed(bool isPlayer1, uint64_t tick, const Action& action) {
        lane(isPlayer1).committed(step, tick, action, cfg);
    }

    std::string hud() const {
        auto laneText = [this](const InputLane& input) {
            char bufSt[48];
            if (!(cfg.wave || cfg.ship || cfg.orb)) std::snprintf(bufSt, sizeof bufSt, "off");
            else if (!input.pend.empty()) std::snprintf(bufSt, sizeof bufSt, "HOLD %zu", input.pend.size());
            else if (cfg.orb && (input.orb.rescues || input.delayed))
                std::snprintf(bufSt, sizeof bufSt, "orb d%d r%llu", input.lastDelay,
                              (unsigned long long)input.orb.rescues);
            else if (input.delayed) std::snprintf(bufSt, sizeof bufSt, "on d%d x%llu", input.lastDelay,
                                                  (unsigned long long)input.delayed);
            else std::snprintf(bufSt, sizeof bufSt, "on");
            char b[224];
            std::snprintf(b, sizeof b,
                "%s:%s  cps:%.1f  fp:%s %.1f/%.1f  buf:%s",
                input.modeName(), input.assists && step - input.lastAssistStep < 30 ? "ON" : "off",
                input.inputCps(step, cfg), fp.active ? "ACTIVE" : (cfg.fp ? "armed" : "off"),
                fp.targetCps(cfg), fp.minimumCps(cfg), bufSt);
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
