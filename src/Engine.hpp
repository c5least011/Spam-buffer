#pragma once
#include <Geode/Geode.hpp>
#include <Geode/binding/RingObject.hpp>
#include <algorithm>
#include <cmath>
#include <deque>
#include <string>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Config.hpp"
#include "Types.hpp"

using namespace geode::prelude;

namespace buf {

// ---------------------------------------------------------------------------
// Engine: input interception + ghost-player prediction.
//
//  * Every jump/hold event (press AND release) is swallowed while the mod is
//    active. onTick() then decides when the game gets to see it.
//  * For a pending event the engine tries delays 0..maxDelay (per game mode).
//    For each delay a hidden ghost player predicts the next ticks and checks
//    whether it survives. Only physics is predicted: the ghost never invents
//    inputs, and an orb reacts only to the click being predicted, on the tick
//    that click is pressed.
//      - click modes: press at the delay, hold a short tap, then release;
//        survive until a stable landing (or the whole trajectory);
//      - ship / wave: the new hold / release state is assumed to stay for the
//        whole look-ahead. The event is only delayed when doing it NOW kills
//        you but doing it a few ticks later does not. If nothing helps, the
//        event passes untouched.
//  * Events are queued in order, so a tap that starts and ends inside one
//    frame (spam) is never lost. Deaths blocked by a noclip mod do not
//    disturb the engine.
//  * Control layouts: solo (also "2-player mode" while dual is off) = both
//    screen sides are player 1; 1-player dual = one input drives both players
//    (the ghost mirrors it, fire() mirrors it to the game); 2-player mode with
//    dual = each player has its own input and delay.
//  * The ghost handles solids, hazards, slopes, orbs (only on the predicted
//    click), gravity portals and speed portals. Anything else it cannot
//    predict (pads, mode / size / mirror / dual / teleport portals) ends the
//    prediction there: it counts as survived instead of guessing.
//  * No autopilot: the engine only ever delays what you pressed or released.
// ---------------------------------------------------------------------------
class Engine {
public:
    struct TrailPt {
        CCPoint p;
        int tag; // 0 path, 1 click, 2 orb click, 3 death
    };
    struct Decision {
        bool known = false;   // false = pass the event straight through
        int delay = 0;        // ticks to hold the event back
        bool drop = false;    // swallow the click entirely
    };

private:
    static constexpr int kSlots = 14;

    struct SlotInfo {
        bool holding[2] = {false, false};
        bool grounded = false;
        int groundTicks = 0;
        bool unknown = false;          // touched something we cannot predict
        int ticks = 0;                 // ticks simulated in this slot
        std::vector<int> touched;      // portals already applied (uniqueID*2 + player)
    };

    enum class Layout { Solo, DualSolo, TwoPlayer };

    // ghost players: [player][slot]
    PlayerObject* m_p[2][kSlots] = {};
    SlotInfo m_info[kSlots];
    bool m_two = false;            // ghost simulates both objects (2-player mode)
    int m_tp = 0;                  // target player of the running search
    bool m_mirror = false;         // 1-player dual: player 2 copies the input
    Layout m_layout = Layout::Solo;
    int m_curSlot = 0;
    bool m_hitUnknown = false;
    int m_evGrav = 0;
    int m_evSpeed = 0;
    int m_evUnknown = 0;
    int m_lastTickFrame = -1000;   // frame of the last real physics tick
    // trajectory drawing
    std::vector<TrailPt> m_trailHold;      // live preview: path if the button is HELD
    std::vector<TrailPt> m_trailRelease;   // live preview: path if the button is RELEASED
    bool m_liveValid = false;
    int m_liveSkip = 0;                    // ticks since the last refresh (the path is trimmed)
    bool m_prevHold = false;               // input the live preview was computed for
    // show path (scan of a feasible route, display only)
    std::vector<TrailPt> m_pathPts;
    bool m_pathOk = false;
    bool m_pathValid = false;
    int m_pathCounter = 0;
    int m_pathSkip = 0;
    bool m_pathPrevHold = false;
    std::vector<TrailPt> m_keepOk;         // last click decision (kept on screen)
    std::vector<TrailPt> m_keepFail;
    int m_keepTicks = 0;
    int m_previewCounter = 0;
    bool m_justDecided = false;
    bool m_hadDraw = false;

    int m_lastP1Frame = -1000;     // frame of the last tick driven by player 1's update
    int m_dbgIn[2] = {0, 0};

    // hook-visible state
    bool m_simulating = false;
    bool m_simDead = false;
    PlayerObject* m_active = nullptr;
    PlayerObject* m_stepObj[2] = {nullptr, nullptr};
    bool m_stepEdge[2] = {false, false};   // this tick presses the button (false -> true)

    // search bookkeeping
    int m_used = 0;
    int m_budget = 3000;
    bool m_aborted = false;
    bool m_wantTrail = false;
    int m_reach = 0;
    std::vector<TrailPt> m_trail;
    std::vector<TrailPt> m_trailOk;
    std::vector<TrailPt> m_trailFail;

    // real-game input state, per player
    bool m_ready = false;
    bool m_phys[2] = {false, false};   // what your finger does
    bool m_game[2] = {false, false};   // what the game has been told
    bool m_supp[2] = {false, false};
    struct Pend { long tick; bool push; };
    std::deque<Pend> m_pend[2];        // events held back (only ever filled by a rescue)
    long m_arr[2] = {0, 0};            // arrival tick of the last held-back event
    long m_now = 0;                    // physics ticks started so far
    struct MoveTrack {
        CCPoint last;
        CCPoint vel;                   // units per unit of game time
        double changeClock = 0;        // game time of the last position change
        bool seen = false;
        bool moving = false;
        bool hasVel = false;
    };
    std::unordered_map<int, MoveTrack> m_moveTrk;
    double m_clock = 0;                // game time = sum of the dt of every real physics step
    float m_stepDtNow = 0.f;
    long m_dbgRingSeen = 0;            // orbs the ghost touched
    long m_dbgRingHit = 0;             // ...that reacted to the clicked tick
    long m_dbgBlueHit = 0;             // ...of which blue (gravity) orbs
    float m_blueX = 0.f;               // x of the last blue orb the ghost activated
    long m_dbgOrbDelays = 0;           // delays that were chosen because of an orb
    long m_moveUntil = 0;              // a move trigger is running until this tick
    int m_otherOverride = -1;          // dual: assumed hold of the other player (-1 = as the game has it)
    bool m_firing = false;
    float m_load = 0.f;                // recent search load (spam) -> smaller budgets

    // autoplay planner + memory
    struct AiCell { float bad[2] = {0.f, 0.f}; };      // [0] release, [1] hold
    struct AiStep { int cell; bool hold; int tick; };
    std::unordered_map<int, AiCell> m_ai;
    std::deque<AiStep> m_aiTrail;
    std::pair<int, bool> m_aiLastForgiven{std::numeric_limits<int>::min(), false};
    int m_aiTick = 0;
    bool m_aiHold = false;
    int m_aiHoldLeft = 0;
    int m_aiDeaths = 0;
    int m_aiHorizon = 70;
    bool m_autoWas = false;
    std::unordered_set<uint64_t> m_failMemo;
    std::vector<int> m_levels;
    bool m_planHold[kSlots] = {};
    int m_planSeg[kSlots] = {};
    int m_planLen = 0;
    bool m_bestHold[kSlots] = {};
    int m_bestSeg[kSlots] = {};
    int m_bestLen = 0;
    int m_bestReach = -1;
    int m_noclipTicks = 0;             // >0: a death was recently blocked (noclip)

    float m_measuredDt = 0.25f;
    CCDrawNode* m_draw = nullptr;

    // wave slope tracking (HUD)
    float m_prevX = 0.f;
    float m_prevY = 0.f;
    float m_realAngle = 0.f;
    float m_ghostAngle = 0.f;

    // rescue log (shown on the first page of the settings window)
    std::deque<std::string> m_log;

    int m_dbgInputs = 0;
    int m_dbgSearches = 0;
    int m_dbgDelayed = 0;
    std::string m_lastText = "-";

public:
    // ---------------------------------------------------------------- lifecycle
    void init() {
        quit();
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return;
        for (int i = 0; i < kSlots; ++i) {
            m_p[0][i] = createSimPlayer(pl);
        }
        m_draw = CCDrawNode::create();
        m_draw->setZOrder(5000);
        pl->m_objectLayer->addChild(m_draw);

        m_dbgInputs = m_dbgSearches = m_dbgDelayed = 0;
        m_lastText = "-";

        m_ready = true;
        for (int i = 0; i < kSlots; ++i) {
            if (!m_p[0][i]) m_ready = false;
        }
        m_lastTickFrame = -1000;
        m_lastP1Frame = -1000;
        m_dbgIn[0] = m_dbgIn[1] = 0;
        if (!cfg().autoKeep) {              // a new level: forget what was learned
            m_ai.clear();
            m_aiDeaths = 0;
        }
        m_aiTrail.clear();
        m_aiTick = 0;
        m_aiHoldLeft = 0;
        m_aiHold = false;
        m_autoWas = false;
        clearOverlay();
        resetInput();
    }

    void quit() {
        for (int pl = 0; pl < 2; ++pl) {
            for (int i = 0; i < kSlots; ++i) {
                if (m_p[pl][i]) {
                    m_p[pl][i]->removeFromParent();
                    m_p[pl][i]->release();
                    m_p[pl][i] = nullptr;
                }
            }
        }
        m_draw = nullptr; // owned by the layer
        m_ready = false;
        m_simulating = false;
        m_active = nullptr;
        resetInput();
    }

    void resetInput() {
        for (int i = 0; i < 2; ++i) {
            m_phys[i] = false;
            m_game[i] = false;
            m_supp[i] = false;
            m_pend[i].clear();
        }
    }

    void onReset() { // level restart / respawn
        for (int i = 0; i < 2; ++i) {
            m_pend[i].clear();
            m_supp[i] = false;
            m_game[i] = false;
        }
        m_aiTrail.clear();
        m_aiTick = 0;
        m_aiHoldLeft = 0;
        m_aiHold = false;
        m_aiLastForgiven = {std::numeric_limits<int>::min(), false};
        clearOverlay();
        if (m_draw) m_draw->clear();
    }

    void clearOverlay() {
        m_moveUntil = 0;
        m_moveTrk.clear();
        m_trailHold.clear();
        m_trailRelease.clear();
        m_pathPts.clear();
        m_liveValid = false;
        m_pathValid = false;
        m_liveSkip = 0;
        m_pathSkip = 0;
        m_previewCounter = 0;
        m_pathCounter = 0;
    }

    // ---------------------------------------------------------------- log
    static constexpr int kLogMax = 30;
    std::deque<std::string> const& logLines() const { return m_log; }
    void clearLog() { m_log.clear(); }

    // ---------------------------------------------------------------- hook API
    bool isSimulating() const { return m_simulating; }
    bool isFiring() const { return m_firing; }

    // ---------------------------------------------------- SpamBuffer integration
    // Decision-only API. SpamBuffer owns the input lanes and injects the delayed
    // events itself: the engine never swallows, queues or fires input here.
    //
    // Returns how many physics ticks the press / release should be held back
    // (0 = let it through now). Wave only.
    int sbWaveDelay(PlayLayer* pl, bool isP1, bool push, bool game0, bool game1, bool laneGame) {
        auto& c = cfg();
        if (!pl || !m_ready || !c.enabled) return 0;
        int frames = static_cast<int>(CCDirector::sharedDirector()->getTotalFrames());
        if (frames - m_lastTickFrame > c.failOpen) return 0;      // fail-open
        Layout lay = layoutOf(pl);
        m_layout = lay;
        if (lay == Layout::TwoPlayer && !c.twoPlayerMode) return 0;
        if (lay == Layout::DualSolo && !c.dualSupport) return 0;
        int pi = (lay != Layout::Solo && !isP1) ? 1 : 0;
        PlayerObject* po = pi == 0 ? pl->m_player1 : pl->m_player2;
        if (!po || po->m_isDead || detectMode(po) != M_WAVE) return 0;
        if (c.maxDelay[M_WAVE] <= 0) return 0;
        m_game[0] = game0;
        m_game[1] = game1;
        if (lay == Layout::Solo) m_game[0] = laneGame;
        m_otherOverride = (lay == Layout::DualSolo) ? (push ? 1 : 0) : -1;
        Decision d = decideHold(pl, M_WAVE, pi, push);
        m_otherOverride = -1;
        if (!d.known || d.drop || d.delay <= 0) return 0;
        ++m_dbgDelayed;
        return d.delay;
    }

    // Per physics tick (replaces onTick: no autoplay, nothing is ever fired).
    // Returns true when this call started a new physics tick (player 1 drives it).
    bool sbTick(PlayerObject* player, bool game0, bool game1) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl || !player) return false;
        int frames = static_cast<int>(CCDirector::sharedDirector()->getTotalFrames());
        if (player == pl->m_player1) {
            m_lastP1Frame = frames;
        } else if (player == pl->m_player2) {
            if (frames - m_lastP1Frame <= 2) return false;    // player 1 drives the tick
        } else {
            return false;
        }
        m_lastTickFrame = frames;
        ++m_now;
        m_game[0] = game0;
        m_game[1] = game1;
        m_load *= 0.97f;
        if (m_noclipTicks > 0) --m_noclipTicks;
        m_layout = layoutOf(pl);
        if (m_keepTicks > 0 && --m_keepTicks == 0) {
            m_keepOk.clear();
            m_keepFail.clear();
            drawResult();
        }
        m_justDecided = false;
        overlayTick(pl);
        return true;
    }

    // The engine's physics-tick clock. Wave delays are counted in these ticks and
    // fired at the start of a tick, exactly like the input buffer mod did.
    uint64_t sbNow() const { return uint64_t(m_now); }
    uint64_t sbNext() const { return uint64_t(m_now) + 1; }

    std::string sbStats() const {
        return fmt::format("buf {} searches {} delayed {} | {}", m_ready ? "ready" : "NOT-READY",
                           m_dbgSearches, m_dbgDelayed, m_lastText);
    }

    std::string const& sbLastText() const { return m_lastText; }

    // Everything except a real 2-player level is ONE logical input: with no dual
    // only player 1 exists, and in a one-player dual a single input drives both
    // players. The right side / a "player 2" copy of the same press are folded into
    // the player-1 lane. Only real 2-player mode keeps two lanes.
    bool sbSoloRemap(PlayLayer* pl) {
        return pl && cfg().bothSides && layoutOf(pl) != Layout::TwoPlayer;
    }
    // One-player dual: one logical input, but BOTH players must get it (the game
    // normally sends it twice, once per player; the lane keeps just one copy).
    bool sbDualSolo(PlayLayer* pl) {
        return pl && layoutOf(pl) == Layout::DualSolo;
    }
    bool sbTwoLanes(PlayLayer* pl) {
        return pl && layoutOf(pl) == Layout::TwoPlayer;
    }
    const char* sbLayoutName(PlayLayer* pl) {
        if (!pl) return "?";
        switch (layoutOf(pl)) {
            case Layout::Solo: return "solo";
            case Layout::DualSolo: return "dual-1p";
            default: return "2p";
        }
    }

    // ------------------------------------------------------------ orb rescue
    // Blue (gravity) orbs only, and only while the player keeps spamming.
    // The orbs of the zone are painted into a path (see inPath). Each step the
    // ghost checks:
    //   * would a click right now activate a blue orb? (otherwise nothing to do)
    //   * with no click, does the player stay on the path for the whole look-ahead?
    //     If yes: leave it alone.
    //   * if it would fall off the path or die, would a tap now keep it on the
    //     path clearly longer (and alive)? Then inject that tap.
    // Returns true = inject a tap now.
    bool sbOrbRescue(PlayLayer* pl, bool isP1, bool game0, bool game1, bool laneGame,
                     int look, const sb::OrbPt* orbs, int orbCount) {
        auto& c = cfg();
        if (!pl || !m_ready || !c.enabled || !c.orbDetect || !orbs || orbCount <= 0) return false;
        int frames = static_cast<int>(CCDirector::sharedDirector()->getTotalFrames());
        if (frames - m_lastTickFrame > c.failOpen) return false;  // fail-open
        if (m_load > 4.f) return false;                           // busy
        Layout lay = layoutOf(pl);
        m_layout = lay;
        if (lay == Layout::TwoPlayer && !c.twoPlayerMode) return false;
        if (lay == Layout::DualSolo && !c.dualSupport) return false;
        int pi = (lay != Layout::Solo && !isP1) ? 1 : 0;
        PlayerObject* po = pi == 0 ? pl->m_player1 : pl->m_player2;
        if (!po || po->m_isDead) return false;
        Mode mode = detectMode(po);
        if (mode == M_WAVE || mode == M_SHIP || mode == M_SWING) return false;

        m_game[0] = game0;
        m_game[1] = game1;
        if (lay == Layout::Solo) m_game[0] = laneGame;
        m_otherOverride = -1;
        const int H = std::max(10, look);
        const int tap = std::max(1, c.tapTicks);
        const float pad = sb::Cfg::orbPad;

        beginSearch(pl, pi, false);
        bool rescue = false;
        const char* why = "no-orb";

        // 1) cheap gate: would a click this very tick activate a blue orb?
        loadReal(1);
        m_info[1].groundTicks = 0;
        long blueBefore = m_dbgBlueHit;
        bool alive = step(1, true);
        if (alive && m_dbgBlueHit != blueBefore && !m_aborted) {
            // 2) no click: does it stay on the painted path?
            OrbRun idle = orbRun(0, -1, tap, H, orbs, orbCount, pad);
            if (idle.end == 3) {
                why = "unknown";
            } else if (idle.end == 0) {
                why = "on-path";
            } else {
                // 3) falling off / dying: does a tap now keep it on the path clearly longer?
                OrbRun tapNow = orbRun(1, 0, tap, H, orbs, orbCount, pad);
                const bool better = tapNow.blue && tapNow.end != 2 && tapNow.end != 3 &&
                                    (tapNow.end == 0 || tapNow.ticks >= idle.ticks + 4);
                rescue = better && !m_aborted;
                why = rescue ? "RESCUE" : (m_aborted ? "abort" : "tap-no-better");
            }
        }
        ++m_dbgSearches;
        m_lastText = fmt::format("P{} orb {}", pi + 1, why);
        endSearch(false, false);
        if (rescue) ++m_dbgDelayed;
        return rescue;
    }

    bool isSimPlayer(PlayerObject* p) const {
        if (!p) return false;
        for (int pl = 0; pl < 2; ++pl) {
            for (int i = 0; i < kSlots; ++i) {
                if (p == m_p[pl][i]) return true;
            }
        }
        return false;
    }

    // Ghost deaths are swallowed here. Real deaths are left to the game.
    bool onSimDeath(PlayerObject* player) {
        if (m_simulating && isSimPlayer(player)) {
            m_simDead = true;
            return true;
        }
        return false;
    }

    // Call after the game handled a REAL destroyPlayer. Only a death that
    // really happened resets the engine; one blocked by a noclip mod is
    // ignored (it used to desync the input state every frame inside a hazard).
    void onRealDestroyed(PlayerObject* player) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl || !player || (player != pl->m_player1 && player != pl->m_player2)) return;
        if (player->m_isDead) {
            if (player == pl->m_player1 && cfg().autoplay) aiOnDeath();   // learn first
            onReset();
        } else {
            m_noclipTicks = cfg().noclipTicks;
        }
    }

    // Gravity portal reached through the game's own dispatch (triggerObject).
    // Skipped when simTouchObjects already applied it.
    void simGravityPortal(EffectGameObject* object) {
        PlayLayer* pl = PlayLayer::get();
        PlayerObject* player = m_active;
        if (!m_simulating || !pl || !isSimPlayer(player) || !object) return;
        int who = player == m_stepObj[1] ? 1 : 0;
        if (markTouched(object, who)) return;
        applyGravityPortal(pl, player, object->m_objectType);
    }

    // The collision hook hands us the objects near a ghost player. Portals the
    // game would apply with side effects (or not at all) are applied here, by
    // geometry, with no visuals and without touching any real object state.
    void simTouchObjects(PlayerObject* player, gd::vector<GameObject*>* vec, int count) {
        PlayLayer* pl = PlayLayer::get();
        if (!m_simulating || !pl || !isSimPlayer(player) || !vec) return;
        int who = player == m_stepObj[1] ? 1 : 0;
        CCRect pr = player->getObjectRect();
        for (int i = 0; i < count; ++i) {
            GameObject* obj = vec->at(i);
            if (!obj) continue;
            TouchKind kind = classify(obj);
            if (kind == TouchKind::None) continue;
            CCRect orect = obj->getObjectRect();
            if (!orect.intersectsRect(pr)) continue;
            if (markTouched(obj, who)) continue;
            if (kind == TouchKind::Gravity) {
                ++m_evGrav;
                applyGravityPortal(pl, player, obj->m_objectType);
            } else if (kind == TouchKind::Speed) {
                ++m_evSpeed;
                player->updateTimeMod(speedOf(obj->m_objectID), true);
            } else {
                ++m_evUnknown;
                m_hitUnknown = true;
            }
        }
    }

    // An orb only reacts to the click being predicted, on the very tick that
    // click is pressed. Holding through orbs never activates them.
    bool canSimRing(PlayerObject* player, EffectGameObject* object) {
        auto ring = geode::cast::typeinfo_cast<RingObject*>(object);
        if (!m_simulating || !isSimPlayer(player) || !ring) return false;
        ++m_dbgRingSeen;
        if (player->m_touchedRings.find(ring->m_uniqueID) != player->m_touchedRings.end()) {
            return false;
        }
        bool ok = cfg().orbDetect && m_stepEdge[player == m_stepObj[0] ? 0 : 1];
        if (ok) {
            ++m_dbgRingHit;
            if (object->m_objectID == 84) {
                ++m_dbgBlueHit;
                m_blueX = object->getPositionX();
            }
        }
        return ok;
    }

    // shown on the Physics page: tells whether the ghost sees orbs at all
    std::string orbInfo() const {
        return fmt::format("Orb sim: touched {} | hit on click tick {} | delays for orbs {}",
                           m_dbgRingSeen, m_dbgRingHit, m_dbgOrbDelays);
    }

    // ---------------------------------------------------------- CBF / move objects
    // Click Between Frames: inputs arrive between frames. Nothing breaks, but it is shown.
    static bool cbfLoaded() {
        return Loader::get()->isModLoaded("syzzi.click_between_frames");
    }

    // Real game: a move trigger just fired. For its duration the objects it moves are
    // not where the ghost sees them, so the prediction must not be trusted near them.
    void onRealMoveTrigger(float duration) {
        if (!cfg().moveDetect) return;
        float tps = effectiveTps();
        if (tps <= 0.f) tps = 240.f;
        long ticks = static_cast<long>(std::ceil(std::max(0.f, duration) * tps)) + 4;
        m_moveUntil = std::max(m_moveUntil, m_now + ticks);
    }

    bool moveActive() const { return cfg().moveDetect && m_now < m_moveUntil; }

    // Ghost: an object that may be moving and cannot be predicted: stop trusting the sim
    void flagMovingObject() { m_hitUnknown = true; }

    // Moving objects. Their speed is MEASURED in the real game against game time (the sum
    // of the dt of the physics steps), not against frames or ticks, so Click Between
    // Frames (extra sub-steps with odd dt, objects only move once per frame) cannot skew it.
    // The ghost then shifts those objects along their measured speed for each simulated tick.
    void addClock(PlayerObject* p, float dt) {
        PlayLayer* pl = PlayLayer::get();
        if (pl && p == pl->m_player1 && dt > 0.f && dt < 5.f) m_clock += dt;
    }

    float moveFresh() const { return 40.f * std::max(1e-4f, stepDt()); }

    // real game, every collision pass of player 1
    void observeObjects(gd::vector<GameObject*>* vec, int count) {
        if (!cfg().moveDetect || !vec) return;
        if (m_moveTrk.size() > 4000) m_moveTrk.clear();
        double fresh = moveFresh();
        for (int i = 0; i < count; ++i) {
            GameObject* o = vec->at(i);
            if (!o || !o->m_groups || o->m_groups->at(0) == 0) continue;     // not movable
            auto& t = m_moveTrk[o->m_uniqueID];
            CCPoint p = o->getPosition();
            if (!t.seen) {
                t.seen = true;
                t.last = p;
                t.changeClock = m_clock;
                continue;
            }
            if (p.x != t.last.x || p.y != t.last.y) {
                double dtc = m_clock - t.changeClock;
                if (t.moving && dtc > 1e-6 && dtc <= fresh) {
                    t.vel = (p - t.last) * static_cast<float>(1.0 / dtc);
                    t.hasVel = true;
                } else {
                    t.hasVel = false;
                }
                t.moving = true;
                t.last = p;
                t.changeClock = m_clock;
            } else if (t.moving && m_clock - t.changeClock > fresh) {
                t.moving = false;
                t.hasVel = false;
            }
        }
    }

    enum class MoveState { Static, Predicted, Unknown };

    // ghost: where is this object during the tick that is being simulated?
    MoveState simMove(GameObject* o, CCPoint& offset) {
        auto it = m_moveTrk.find(o->m_uniqueID);
        if (it == m_moveTrk.end()) return moveActive() ? MoveState::Unknown : MoveState::Static;
        auto& t = it->second;
        if (!t.moving || m_clock - t.changeClock > moveFresh()) return MoveState::Static;
        if (!t.hasVel) return MoveState::Unknown;
        float elapsed = static_cast<float>(m_info[m_curSlot].ticks + 1) * m_stepDtNow;
        offset = t.vel * elapsed;
        return MoveState::Predicted;
    }

    void setFrameDelta(float dt) {
        if (dt > 0.001f && dt < 2.f) m_measuredDt = dt;
    }

    // ------------------------------------------------------------ timing info
    float timeWarp() const {
        PlayLayer* pl = PlayLayer::get();
        float w = pl ? pl->m_gameState.m_timeWarp : 1.f;
        return w > 0.f ? w : 1.f;
    }

    float stepDt() const {
        auto& c = cfg();
        if (c.tps > 0) return 60.f / static_cast<float>(c.tps) * timeWarp();
        return m_measuredDt;
    }

    float effectiveTps() const {
        auto& c = cfg();
        if (c.tps > 0) return static_cast<float>(c.tps);
        return 60.f / std::max(0.01f, m_measuredDt / timeWarp());
    }

    float effectiveFps() const {
        auto& c = cfg();
        if (c.fps > 0) return static_cast<float>(c.fps);
        double iv = CCDirector::sharedDirector()->getAnimationInterval();
        return iv > 0.0 ? static_cast<float>(1.0 / iv) : 60.f;
    }

    float ticksPerFrame() const {
        return std::max(1.f, effectiveTps() / std::max(1.f, effectiveFps()));
    }

    std::string timingInfo() const {
        return fmt::format(
            "Detected: {:.0f} TPS | {:.0f} FPS | {:.2f} ticks/frame | dt {:.4f}",
            effectiveTps(), effectiveFps(), ticksPerFrame(), stepDt()
        );
    }

    // ------------------------------------------------------------ mode helpers
    static Mode detectMode(PlayerObject* p) {
        if (!p) return M_CUBE;
        if (p->m_isShip) return M_SHIP;
        if (p->m_isDart) return M_WAVE;
        if (p->m_isBall) return M_BALL;
        if (p->m_isRobot) return M_ROBOT;
        if (p->m_isSpider) return M_SPIDER;
        if (p->m_isBird) return M_UFO;
        if (p->m_isSwing) return M_SWING;
        return M_CUBE;
    }
    static bool isContinuous(Mode m) { return m == M_SHIP || m == M_WAVE; }

    // Which control scheme is active. Player 2 only exists while dual mode is
    // on, so "2-player mode" without dual is just solo.
    static bool twoFlagOf(PlayLayer* pl) {
        return (pl->m_level && pl->m_level->m_twoPlayerMode) ||
               (pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode);
    }

    static Layout layoutOf(PlayLayer* pl) {
        bool dual = pl->m_gameState.m_isDualMode && pl->m_player2;
        if (!dual) return Layout::Solo;
        return twoFlagOf(pl) ? Layout::TwoPlayer : Layout::DualSolo;
    }

    // ================================================================ input
    // isP1 may be rewritten: in solo both screen sides are player 1.
    // Returns false when the event was swallowed (it may still have been
    // delivered through fire(), see below).
    bool onInput(GJBaseGameLayer* layer, bool push, bool& isP1) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return true;
        auto& c = cfg();

        Layout lay = layoutOf(pl);
        m_layout = lay;
        bool origP1 = isP1;

        {   // autoplay: the bot drives the input, your taps are ignored
            int fr = static_cast<int>(CCDirector::sharedDirector()->getTotalFrames());
            PlayerObject* pa = layer->m_player1;
            if (c.autoplay && m_ready && fr - m_lastTickFrame <= c.failOpen &&
                lay != Layout::TwoPlayer && pa && !pa->m_isDead) {
                m_phys[0] = push;
                return false;
            }
        }

        // "both sides = player 1" only in solo outside 2-player levels: there the
        // game already routes left / right to its own players
        bool remap = lay == Layout::Solo && c.bothSides && !twoFlagOf(pl);
        if (lay == Layout::Solo && !remap && !origP1) return true;
        // Solo: one player. Dual / 2-player: every event belongs to the player it
        // came from, and every player is its own buffer (own mode, own delay).
        int pi = (lay != Layout::Solo && !origP1) ? 1 : 0;

        ++m_dbgInputs;
        ++m_dbgIn[pi];
        m_phys[pi] = push;
        PlayerObject* po = pi == 0 ? layer->m_player1 : layer->m_player2;

        // fail-open: if the physics tick hook is not running, do not touch anything
        int frames = static_cast<int>(CCDirector::sharedDirector()->getTotalFrames());
        bool tickAlive = frames - m_lastTickFrame <= c.failOpen;

        bool active = c.enabled && m_ready && tickAlive && po && !po->m_isDead &&
                      c.maxDelay[detectMode(po)] > 0 &&
                      !(lay == Layout::TwoPlayer && !c.twoPlayerMode) &&
                      !(lay == Layout::DualSolo && !c.dualSupport);
        if (!active) {
            m_pend[pi].clear();
            m_supp[pi] = false;
            if (remap) {
                isP1 = true;                         // both screen sides are player 1
                // some builds send the same press as a "player 2" copy: drop duplicates
                if (!origP1 && push == m_game[pi]) return false;
            }
            m_game[pi] = push;
            return true;
        }
        return rescue(pl, pi, push, po);
    }

private:
    // The whole job: an EARLY press / release is held back a few ticks. Anything
    // else is left completely alone: the original event goes through untouched,
    // nothing is swallowed, queued or re-fired.
    // Returns true = let the original event through, false = swallowed.
    bool rescue(PlayLayer* pl, int pi, bool push, PlayerObject* po) {
        auto& pend = m_pend[pi];
        if (!push && m_supp[pi]) {                  // release of a click that was dropped
            m_supp[pi] = false;
            return false;
        }

        long a = m_now + 1;                         // index of the next tick
        if (!pend.empty()) {
            // an earlier event is still being held back: stay behind it and keep
            // the spacing you had (no new search, nothing is "re-decided")
            long fireAt = pend.back().tick + std::max<long>(1, a - m_arr[pi]);
            m_arr[pi] = a;
            pend.push_back({fireAt, push});
            return false;
        }
        if (push == m_game[pi]) return true;        // nothing changes for the game

        Mode mode = detectMode(po);
        m_otherOverride = (m_layout == Layout::DualSolo) ? (push ? 1 : 0) : -1;
        Decision d;
        long ringBefore = m_dbgRingHit;
        if (isContinuous(mode)) d = decideHold(pl, mode, pi, push);
        else if (push) d = decideClick(pl, mode, pi);
        m_otherOverride = -1;
        if (d.known && d.delay > 0 && m_dbgRingHit != ringBefore) ++m_dbgOrbDelays;

        if (!d.known || (!d.drop && d.delay == 0)) {    // not early: let it through
            m_game[pi] = push;
            return true;
        }
        if (d.drop) {
            m_supp[pi] = true;
            return false;
        }
        ++m_dbgDelayed;
        if (cfg().logEnabled) logRescue(pi, mode, po, d.delay);
        pend.push_back({a + d.delay, push});
        m_arr[pi] = a;
        return false;
    }

public:

    // Called at the start of every physics tick of a player (real game only).
    // Player 1 drives it; player 2's update is only a backup heartbeat for the
    // case where player 1's hook is not running.
    void onTick(PlayerObject* player) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl || !player) return;
        int frames = static_cast<int>(CCDirector::sharedDirector()->getTotalFrames());
        if (player == pl->m_player1) {
            m_lastP1Frame = frames;
        } else if (player == pl->m_player2) {
            if (frames - m_lastP1Frame <= 2) return;     // player 1 drives the tick
        } else {
            return;
        }
        m_lastTickFrame = frames;
        ++m_now;
        PlayerObject* p1 = pl->m_player1 ? pl->m_player1 : player;

        {   // measure the real wave slope (degrees) for the HUD
            float px = p1->getPositionX();
            float py = p1->getPositionY();
            float dx = px - m_prevX;
            float dy = py - m_prevY;
            if (p1->m_isDart && dx > 0.0001f && dx < 50.f && dy != 0.f && std::fabs(dy) < 80.f) {
                m_realAngle = std::atan2(std::fabs(dy), dx) * 57.29578f;
            }
            m_prevX = px;
            m_prevY = py;
        }
        m_load *= 0.97f;
        if (m_noclipTicks > 0) --m_noclipTicks;

        m_layout = layoutOf(pl);
        auto& cf = cfg();
        if (m_keepTicks > 0 && --m_keepTicks == 0) {
            m_keepOk.clear();
            m_keepFail.clear();
            drawResult();
        }

        bool autoOn = cf.autoplay && m_ready && m_layout != Layout::TwoPlayer &&
                      pl->m_player1 && !pl->m_player1->m_isDead;
        if (autoOn) {
            m_autoWas = true;
            autoTick(pl);
            return;
        }
        if (m_autoWas) {                          // autoplay just switched off: hand control back
            m_autoWas = false;
            m_pend[0].clear();
            m_supp[0] = false;
            matchPhysical(pl, 0);
            if (m_layout != Layout::Solo) {
                m_pend[1].clear();
                m_supp[1] = false;
                matchPhysical(pl, 1);
            }
        }

        int n = m_layout == Layout::Solo ? 1 : 2;
        m_justDecided = false;
        for (int pi = 0; pi < n; ++pi) tickPlayer(pl, pi);
        if (!m_justDecided) overlayTick(pl);
    }

private:
    // Fires the held-back events once their tick has come (one per tick).
    void tickPlayer(PlayLayer* pl, int pi) {
        auto& c = cfg();
        PlayerObject* po = pi == 0 ? pl->m_player1 : pl->m_player2;
        if (!po) return;
        Mode mode = detectMode(po);
        bool active = c.enabled && m_ready && !po->m_isDead && c.maxDelay[mode] > 0 &&
                      !(m_layout == Layout::TwoPlayer && !c.twoPlayerMode) &&
                      !(m_layout == Layout::DualSolo && !c.dualSupport);
        auto& pend = m_pend[pi];
        if (!active) {
            pend.clear();
            m_supp[pi] = false;
            matchPhysical(pl, pi);
            return;
        }
        if (!pend.empty() && pend.front().tick <= m_now) {
            bool p = pend.front().push;
            pend.pop_front();
            fire(pl, pi, p);
        }
    }

    enum class TouchKind { None, Gravity, Speed, Unknown };

    static float speedOf(int id) {
        switch (id) {
            case 200: return 0.7f;     // slow
            case 201: return 0.9f;     // normal
            case 202: return 1.1f;     // fast
            case 203: return 1.3f;     // faster
            case 1334: return 1.6f;    // fastest
            default: return 0.9f;
        }
    }

    static TouchKind classify(GameObject* o) {
        switch (o->m_objectType) {
            case GameObjectType::InverseGravityPortal:
            case GameObjectType::NormalGravityPortal:
            case GameObjectType::GravityTogglePortal:
                return TouchKind::Gravity;
            case GameObjectType::Modifier: {
                int id = o->m_objectID;
                if (id == 200 || id == 201 || id == 202 || id == 203 || id == 1334) {
                    return TouchKind::Speed;
                }
                return TouchKind::None;
            }
            // not predicted: the prediction stops here
            case GameObjectType::YellowJumpPad:
            case GameObjectType::PinkJumpPad:
            case GameObjectType::GravityPad:
            case GameObjectType::RedJumpPad:
            case GameObjectType::SpiderPad:
            case GameObjectType::TeleportOrb:
            case GameObjectType::ShipPortal:
            case GameObjectType::CubePortal:
            case GameObjectType::BallPortal:
            case GameObjectType::UfoPortal:
            case GameObjectType::WavePortal:
            case GameObjectType::RobotPortal:
            case GameObjectType::SpiderPortal:
            case GameObjectType::SwingPortal:
            case GameObjectType::RegularSizePortal:
            case GameObjectType::MiniSizePortal:
            case GameObjectType::InverseMirrorPortal:
            case GameObjectType::NormalMirrorPortal:
            case GameObjectType::DualPortal:
            case GameObjectType::SoloPortal:
            case GameObjectType::TeleportPortal:
                return TouchKind::Unknown;
            default:
                return TouchKind::None;
        }
    }

    // true = this ghost player already used this object
    bool markTouched(GameObject* obj, int who) {
        auto& t = m_info[m_curSlot].touched;
        int key = obj->m_uniqueID * 2 + who;
        if (std::find(t.begin(), t.end(), key) != t.end()) return true;
        t.push_back(key);
        return false;
    }

    void applyGravityPortal(PlayLayer* pl, PlayerObject* player, GameObjectType type) {
        bool upsideDown;
        switch (type) {
            case GameObjectType::InverseGravityPortal: upsideDown = true; break;
            case GameObjectType::NormalGravityPortal: upsideDown = false; break;
            case GameObjectType::GravityTogglePortal: upsideDown = !player->m_isUpsideDown; break;
            default: return;
        }
        pl->flipGravity(player, upsideDown, true);
    }

    // One line per rescued event: what the game was running at, how much the event was
    // held back, in which mode, and which way gravity points.
    void logRescue(int pi, Mode mode, PlayerObject* po, int ticks) {
        float tps = effectiveTps();
        float ms = tps > 0.f ? static_cast<float>(ticks) * 1000.f / tps : 0.f;
        m_log.push_back(fmt::format(
            "TPS {:.0f} | FPS {:.0f} | saved {}t | {:.1f}ms | {} P{} | {} gravity{}",
            tps, effectiveFps(), ticks, ms, kModeNames[mode], pi + 1,
            po->m_isUpsideDown ? "flipped" : "normal",
            cbfLoaded() ? " | CBF" : ""
        ));
        while (static_cast<int>(m_log.size()) > kLogMax) m_log.pop_front();   // oldest goes first
    }

    void fire(PlayLayer* pl, int pi, bool push) {
        m_firing = true;
        m_game[pi] = push;
        pl->handleButton(push, 1, pi == 0);
        m_firing = false;
    }

    void matchPhysical(PlayLayer* pl, int pi) {
        if (m_game[pi] != m_phys[pi]) fire(pl, pi, m_phys[pi]);
    }

    // ----------------------------------------------------------- ghost players
    PlayerObject* createSimPlayer(PlayLayer* pl) {
        PlayerObject* p = PlayerObject::create(1, 1, pl, pl, true);
        if (!p) return nullptr;
        p->retain();
        p->setPosition({0, 105});
        p->setVisible(false);
        pl->m_objectLayer->addChild(p);
        return p;
    }

    void ensureP2(PlayLayer* pl) {
        if (m_p[1][0]) return;
        for (int i = 0; i < kSlots; ++i) m_p[1][i] = createSimPlayer(pl);
    }

    void resetCollisionLog(PlayerObject* self) {
        self->m_collisionLogTop->removeAllObjects();
        self->m_collisionLogBottom->removeAllObjects();
        self->m_collisionLogLeft->removeAllObjects();
        self->m_collisionLogRight->removeAllObjects();
    }

    void syncPlayer(PlayerObject* sim, PlayerObject* src) {
        sim->setVisible(false);
        sim->copyAttributes(src);
        sim->setPosition(src->getPosition());
        sim->setScale(src->getScale());
        sim->setContentSize(src->getContentSize());
        sim->m_width = src->m_width;
        sim->m_height = src->m_height;
        sim->m_gravityMod = src->m_gravityMod;
        sim->m_isOnGround = src->m_isOnGround;
        sim->m_isUpsideDown = src->m_isUpsideDown;
        sim->m_vehicleSize = src->m_vehicleSize;
        sim->m_playerSpeed = src->m_playerSpeed;
        sim->m_yVelocity = src->m_yVelocity;
        sim->m_gravity = src->m_gravity;
        sim->m_speedMultiplier = src->m_speedMultiplier;
        sim->m_touchedRings = src->m_touchedRings;
        if (sim->m_touchingRings) sim->m_touchingRings->removeAllObjects();
    }

    void forceHoldObj(int s, int who, bool hold) {
        m_info[s].holding[who] = hold;
        PlayerObject* p = m_p[who][s];
        if (!p) return;
        if (hold) p->pushButton(PlayerButton::Jump);
        else p->releaseButton(PlayerButton::Jump);
    }

    // sets the hold state of the TARGET player
    void setHold(int s, bool hold) {
        if (m_info[s].holding[m_tp] != hold) forceHoldObj(s, m_tp, hold);
        if (m_mirror && m_info[s].holding[1] != hold) forceHoldObj(s, 1, hold);
    }

    // hold state the ghost starts with for a player (the target's own state is the
    // one the game has; the other player may be assumed to mirror the target)
    bool holdFor(int who) const {
        if (m_mirror) return m_game[0];
        if (who != m_tp && m_otherOverride >= 0) return m_otherOverride == 1;
        return m_game[who];
    }

    void loadReal(int s) {
        PlayLayer* pl = PlayLayer::get();
        syncPlayer(m_p[0][s], pl->m_player1);
        if (m_two) syncPlayer(m_p[1][s], pl->m_player2);
        m_info[s] = SlotInfo();
        m_info[s].grounded = m_p[m_tp][s]->m_isOnGround;
        m_info[s].groundTicks = m_info[s].grounded ? 1 : 0;
        forceHoldObj(s, 0, holdFor(0));
        if (m_two) forceHoldObj(s, 1, holdFor(1));
    }

    void copySlot(int dst, int src) {
        syncPlayer(m_p[0][dst], m_p[0][src]);
        if (m_two) syncPlayer(m_p[1][dst], m_p[1][src]);
        SlotInfo keep = m_info[src];
        m_info[dst] = keep;
        forceHoldObj(dst, 0, keep.holding[0]);
        if (m_two) forceHoldObj(dst, 1, keep.holding[1]);
    }

    CCPoint pos(int s) const { return m_p[m_tp][s]->getPosition(); }

    // Wave: optional fixed slope (up / down differ slightly, mini is steeper).
    // The ghost's own slope is recorded for the HUD before any override.
    void applyWaveAngle(PlayerObject* w, float x0, float y0, bool record) {
        if (!w->m_isDart) return;
        float dx = w->getPositionX() - x0;
        float dy = w->getPositionY() - y0;
        if (dx <= 0.0001f || dy == 0.f) return;
        if (record) m_ghostAngle = std::atan2(std::fabs(dy), dx) * 57.29578f;
        auto& c = cfg();
        if (!c.waveAngles) return;
        bool mini = w->m_vehicleSize < 0.9f;
        bool up = dy > 0.f;
        float deg = static_cast<float>(up ? (mini ? c.miniUp : c.waveUp)
                                          : (mini ? c.miniDown : c.waveDown)) / 100.f;
        float ndy = dx * std::tan(deg * 0.017453292f);
        w->setPosition(CCPoint(w->getPositionX(), y0 + (up ? ndy : -ndy)));
    }

    // One physics tick for a slot. `hold` is the target player's input; the
    // other player (2-player mode) keeps whatever the game currently has.
    // false = a player died or the budget ran out.
    bool step(int s, bool hold) {
        if (m_used >= m_budget) {
            m_aborted = true;
            return false;
        }
        ++m_used;
        PlayLayer* pl = PlayLayer::get();
        PlayerObject* a = m_p[0][s];
        PlayerObject* b = m_two ? m_p[1][s] : nullptr;
        // 2-player mode: the two players are independent. Simulating the other one with a
        // frozen input made it die in the ghost, which failed EVERY delay of the target
        // (so nothing was buffered, logged or orb-detected). Only the target runs now.
        if (m_two && m_layout == Layout::TwoPlayer) {
            if (m_tp == 0) b = nullptr;
            else a = nullptr;
        }
        float dt = stepDt();
        m_stepDtNow = dt;

        bool wasHold = m_info[s].holding[m_tp];
        setHold(s, hold);
        m_simDead = false;
        m_stepObj[0] = a;
        m_stepObj[1] = b;
        m_stepEdge[0] = m_stepEdge[1] = false;
        m_stepEdge[m_tp] = hold && !wasHold;
        if (m_mirror) m_stepEdge[1] = m_stepEdge[0];
        m_curSlot = s;
        m_hitUnknown = false;

        if (a) {
            float ax = a->getPositionX();
            float ay = a->getPositionY();
            m_active = a;
            a->update(dt);
            a->updateRotation(dt);
            applyWaveAngle(a, ax, ay, m_tp == 0);
        }
        if (b) {
            float bx = b->getPositionX();
            float by = b->getPositionY();
            m_active = b;
            b->update(dt);
            b->updateRotation(dt);
            applyWaveAngle(b, bx, by, m_tp == 1);
        }
        if (m_simDead) {
            m_active = nullptr;
            return false;
        }

        if (a) {
            m_active = a;
            resetCollisionLog(a);
            pl->checkCollisions(a, dt, false);
        }
        if (!m_simDead && b) {
            m_active = b;
            resetCollisionLog(b);
            pl->checkCollisions(b, dt, false);
        }
        m_active = nullptr;
        if (m_simDead) return false;

        SlotInfo& inf = m_info[s];
        if (m_hitUnknown) inf.unknown = true;
        inf.grounded = m_p[m_tp][s]->m_isOnGround;
        inf.groundTicks = inf.grounded ? inf.groundTicks + 1 : 0;
        ++inf.ticks;
        return true;
    }

    void beginSearch(PlayLayer* pl, int target, bool decision = true, bool mirror = false) {
        auto& c = cfg();
        m_two = m_layout != Layout::Solo;
        m_mirror = mirror;
        m_tp = target;
        bool two = m_two;
        m_evGrav = m_evSpeed = m_evUnknown = 0;
        if (two) ensureP2(pl);
        m_simulating = true;
        m_simDead = false;
        m_active = nullptr;
        m_used = 0;
        // spam: the more searches ran recently, the smaller each one may be
        float scale = std::max(0.15f, std::min(1.f, 1.f - (m_load - 2.f) / 6.f));
        m_budget = std::max(c.minBudget, static_cast<int>(c.budget * scale));
        if (decision) {
            m_load += 1.f;
            m_justDecided = true;
        }
        m_aborted = false;
        m_reach = 0;
        m_wantTrail = c.showTrajectory;
        m_trail.clear();
        m_trailOk.clear();
        m_trailFail.clear();
    }

    void endSearch(bool decision = true, bool redraw = true) {
        m_simulating = false;
        m_active = nullptr;
        if (decision && m_wantTrail) {      // keep the click path on screen for a while
            m_keepOk = m_trailOk;
            m_keepFail = m_trailFail;
            m_keepTicks = cfg().keepTicks;
        }
        if (redraw) drawResult();
    }

    void trailPush(int s, int tag) {
        if (m_wantTrail) m_trail.push_back({pos(s), tag});
    }

    void noteDeath(int s) {
        if (!m_wantTrail || m_simDead == false) return;
        if (m_trail.size() >= m_trailFail.size()) {
            m_trailFail = m_trail;
            m_trailFail.push_back({pos(s), 3});
        }
    }

    struct OrbRun {
        int ticks = 0;          // ticks simulated before it ended
        int end = 0;            // 0 = stayed on the path the whole look-ahead, 1 = left the path,
                                // 2 = died, 3 = unknown / out of budget
        bool blue = false;      // a click really activated a blue orb
    };

    // The painted path: every orb (hitbox padded), plus the box joining each pair of
    // neighbouring orbs (padded), so a zigzag between orbs stays "on the path".
    static bool inPath(CCPoint p, const sb::OrbPt* orbs, int n, float pad) {
        for (int i = 0; i < n; ++i) {
            const auto& o = orbs[i];
            if (p.x >= o.x0 - pad && p.x <= o.x1 + pad && p.y >= o.y0 - pad && p.y <= o.y1 + pad)
                return true;
            if (i + 1 < n) {
                const auto& q = orbs[i + 1];
                float x0 = std::min(o.x0, q.x0) - pad, x1 = std::max(o.x1, q.x1) + pad;
                float y0 = std::min(o.y0, q.y0) - pad, y1 = std::max(o.y1, q.y1) + pad;
                if (p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1) return true;
            }
        }
        return false;
    }

    // Ghost run for the orb rescue. clickAt < 0 = never click; otherwise a tap of
    // `tap` ticks starting at tick `clickAt`.
    OrbRun orbRun(int slot, int clickAt, int tap, int H, const sb::OrbPt* orbs, int n, float pad) {
        OrbRun r;
        loadReal(slot);
        m_info[slot].groundTicks = 0;
        for (int t = 0; t < H; ++t) {
            const bool hold = clickAt >= 0 && t >= clickAt && t < clickAt + tap;
            long before = m_dbgBlueHit;
            if (!step(slot, hold)) {
                r.ticks = t;
                r.end = m_aborted ? 3 : 2;
                return r;
            }
            if (m_dbgBlueHit != before) r.blue = true;
            if (m_info[slot].unknown) {
                r.ticks = t;
                r.end = 3;
                return r;
            }
            CCRect pr = m_p[m_tp][slot]->getObjectRect();
            if (!inPath(CCPoint(pr.getMidX(), pr.getMidY()), orbs, n, pad)) {
                r.ticks = t;
                r.end = 1;
                return r;
            }
        }
        r.ticks = H;
        r.end = 0;
        return r;
    }

    // ============================================================ click modes
    // Press at the delay, hold a short tap, release, and let physics run.
    // true  = survived until a stable landing (ground modes) or the whole
    //         trajectory (UFO / Swing, which have no ground to settle on).
    // false = died / budget; `reach` is the tick reached.
    bool rollClick(int s, int d, int holdTicks, int horizon, bool airMode, int& reach) {
        int tick = d;
        int holdLeft = holdTicks;
        while (true) {
            if (tick >= horizon) return true;
            if (!airMode && holdLeft == 0 && m_info[s].groundTicks >= cfg().settle) return true;
            if (m_aborted) return false;
            if (!step(s, holdLeft > 0)) {
                noteDeath(s);
                return false;
            }
            trailPush(s, 0);
            if (m_info[s].unknown) return true;      // reached something we cannot predict
            ++tick;
            reach = std::max(reach, tick);
            if (holdLeft > 0) --holdLeft;
        }
    }

    Decision decideClick(PlayLayer* pl, Mode mode, int pi) {
        auto& c = cfg();
        Decision r;
        int maxDelay = c.maxDelay[mode];
        if (maxDelay <= 0) return r;

        bool airMode = mode == M_UFO || mode == M_SWING;
        int horizon = airMode ? std::min(c.trajectory, c.airHorizon) : c.trajectory;
        std::vector<int> holds = {c.tapTicks};
        if (mode == M_ROBOT) holds.push_back(std::max(c.tapTicks, c.robotHold));

        beginSearch(pl, pi);
        loadReal(0);

        // A delay is "safe" if the click survives. If no delay is fully safe we
        // still pick the one whose death position is clearly further than the
        // delays before it.
        int gain = std::max(1, c.gainClick);
        int foundDelay = -1;
        int bestDelay = 0;
        int bestTicks = -1;

        for (int d = 0; d <= maxDelay; ++d) {
            if (d > 0) {
                if (!step(0, m_game[pi])) {
                    noteDeath(0);
                    break;                       // waiting longer is deadly too
                }
                trailPush(0, 0);
                if (m_info[0].unknown) break;    // cannot predict any further
            }
            int reach = d;
            bool ok = false;
            for (int h : holds) {
                copySlot(1, 0);
                m_info[1].groundTicks = 0;
                size_t mark = m_trail.size();
                trailPush(0, 1);
                ok = rollClick(1, d, h, horizon, airMode, reach);
                if (ok) {
                    foundDelay = d;
                    if (m_wantTrail) m_trailOk = m_trail;
                    break;
                }
                m_trail.resize(mark);
                if (m_aborted) break;
            }
            if (ok || m_aborted) break;
            if (bestTicks < 0 || reach >= bestTicks + gain) {   // death position
                bestTicks = reach;
                bestDelay = d;
            }
        }

        if (foundDelay >= 0) {
            r.known = true;
            r.delay = foundDelay;
        } else {
            r.known = !m_aborted || bestDelay > 0;
            r.delay = bestDelay;
            if (r.known && c.dropFatal && bestDelay == 0 && !m_aborted) {
                loadReal(0);
                bool alive = true;
                for (int t = 0; t < horizon && alive; ++t) alive = step(0, m_game[pi]);
                r.drop = alive && !m_aborted;
            }
        }

        ++m_dbgSearches;
        m_lastText = fmt::format(
            "P{} click d={} {} best={}t used={}{}",
            pi + 1, r.delay, foundDelay >= 0 ? "SAFE" : (r.drop ? "DROP" : "no-safe"),
            bestTicks, m_used, m_aborted ? " ABORT" : ""
        );
        endSearch();
        return r;
    }

    // ========================================================= ship / wave
    // The new hold / release state is assumed to stay for the look-ahead.
    //  - a delay (0..max) survives the whole look-ahead -> the smallest one wins;
    //  - otherwise, ONLY if doing it now kills you within `imminent` ticks, the
    //    delay that pushes the death position back the most wins (it must beat
    //    the earlier delays by `holdGain` ticks);
    //  - anything else passes untouched, so the controls stay free.
    Decision decideHold(PlayLayer* pl, Mode mode, int pi, bool target) {
        auto& c = cfg();
        Decision r;
        int maxDelay = c.maxDelay[mode];
        if (maxDelay <= 0) return r;
        int horizon = std::max(1, mode == M_WAVE ? c.waveHorizon : c.shipHorizon);
        int imminent = mode == M_WAVE ? c.waveImminent : c.shipImminent;
        int gain = std::max(1, c.holdGain);

        beginSearch(pl, pi);
        loadReal(0);

        int foundDelay = -1;
        int firstReach = -1;
        int bestReach = -1;
        int bestDelay = 0;

        for (int d = 0; d <= maxDelay; ++d) {
            if (d > 0) {
                if (!step(0, m_game[pi])) {
                    noteDeath(0);
                    break;                       // waiting longer is deadly too
                }
                trailPush(0, 0);
                if (m_info[0].unknown) break;    // cannot predict any further
            }
            copySlot(1, 0);
            size_t mark = m_trail.size();
            bool alive = true;
            int reach = d;
            for (int t = d; t < horizon; ++t) {
                if (!step(1, target)) {
                    noteDeath(1);
                    alive = false;
                    break;
                }
                trailPush(1, 0);
                if (m_info[1].unknown) break;    // prediction ends here: counts as survived
                ++reach;
            }
            if (alive) {
                foundDelay = d;
                if (m_wantTrail) m_trailOk = m_trail;
                break;
            }
            if (m_aborted) break;
            if (d == 0) {
                firstReach = reach;
                bestReach = reach;
                bestDelay = 0;
            } else if (reach >= bestReach + gain) {
                bestReach = reach;
                bestDelay = d;
            }
            m_trail.resize(mark);
        }

        int delay = 0;
        if (foundDelay > 0) delay = foundDelay;
        else if (foundDelay < 0 && firstReach >= 0 && firstReach < imminent) delay = bestDelay;

        if (delay > 0 && !m_aborted) {
            r.known = true;
            r.delay = delay;
        }   // else: fine right now, not imminent, nothing helps, or out of budget -> pass through

        ++m_dbgSearches;
        m_lastText = fmt::format(
            "P{} {} d={} {} dies@{} used={}{}",
            pi + 1, target ? "hold" : "release", r.delay,
            foundDelay == 0 ? "SAFE" : (r.delay > 0 ? "DELAY" : "pass"),
            firstReach, m_used, m_aborted ? " ABORT" : ""
        );
        endSearch();
        return r;
    }

    // ============================================================ autoplay
    // The bot drives the input itself. It looks ahead with the ghost (hold /
    // release segments, fine near the player and coarse far away) and replans
    // every few ticks from the real state. It learns from every real death:
    // the actions it took just before dying are blamed for the places they
    // happened, so the next attempt tries something else there; actions that
    // survived for a while are forgiven again.
    int aiCell(float x) const {
        return static_cast<int>(std::floor(x / static_cast<float>(std::max(1, cfg().autoCell))));
    }

    float aiBad(int cell, bool hold) const {
        auto it = m_ai.find(cell);
        return it == m_ai.end() ? 0.f : it->second.bad[hold ? 1 : 0];
    }

    uint64_t aiKey(int s, int level, bool hold) const {
        PlayerObject* a = m_p[0][s];
        int64_t x = static_cast<int64_t>(aiCell(a->getPositionX()));
        int64_t y = static_cast<int64_t>(std::lround(a->getPositionY() * 0.7f));
        int64_t v = static_cast<int64_t>(std::lround(a->m_yVelocity * 6.f));
        uint64_t k = static_cast<uint64_t>(level & 0xF);
        k = k * 1000003ULL + static_cast<uint64_t>(x + 100000);
        k = k * 1000003ULL + static_cast<uint64_t>(y + 100000);
        k = k * 1000003ULL + static_cast<uint64_t>(v + 100000);
        k = k * 2ULL + (hold ? 1ULL : 0ULL);
        k = k * 2ULL + (a->m_isUpsideDown ? 1ULL : 0ULL);
        return k;
    }

    bool aiDfs(int s, int level, int tick, bool prevHold) {
        if (tick >= m_aiHorizon || level >= static_cast<int>(m_levels.size()) ||
            s + 1 >= kSlots) {
            m_planLen = level;
            return true;
        }
        uint64_t key = aiKey(s, level, prevHold);
        if (m_failMemo.count(key)) return false;

        int seg = m_levels[level];
        int cell = aiCell(m_p[0][s]->getPositionX());
        bool order[2] = {prevHold, !prevHold};            // keeping the input is the default
        if (aiBad(cell, order[1]) + 0.01f < aiBad(cell, order[0])) std::swap(order[0], order[1]);
        float prune = static_cast<float>(cfg().autoPrune);

        for (int k = 0; k < 2; ++k) {
            bool h = order[k];
            float bh = aiBad(cell, h);
            float bo = aiBad(cell, !h);
            if (bh >= prune && bo < bh) continue;         // learned: this action killed us here
            m_planHold[level] = h;
            m_planSeg[level] = seg;
            copySlot(s + 1, s);
            size_t mark = m_trail.size();
            bool ok = true;
            int stepped = 0;
            for (int t = 0; t < seg && tick + t < m_aiHorizon; ++t) {
                if (!step(s + 1, h)) {
                    noteDeath(s + 1);
                    ok = false;
                    break;
                }
                trailPush(s + 1, 0);
                ++stepped;
                if (m_info[s + 1].unknown) break;
            }
            int reached = tick + stepped;
            if (reached > m_bestReach) {
                m_bestReach = reached;
                m_bestLen = level + 1;
                for (int i = 0; i <= level; ++i) {
                    m_bestHold[i] = m_planHold[i];
                    m_bestSeg[i] = m_planSeg[i];
                }
            }
            if (ok && m_info[s + 1].unknown) {
                m_planLen = level + 1;                    // cannot predict past this: good enough
                return true;
            }
            if (ok && aiDfs(s + 1, level + 1, tick + seg, h)) return true;
            m_trail.resize(mark);
            if (m_aborted) return false;
        }
        m_failMemo.insert(key);
        return false;
    }

    // true = a plan survives the whole look-ahead; otherwise the plan that
    // lived longest is used. outHold / outTicks = what to do next.
    bool planAuto(PlayLayer* pl, bool& outHold, int& outTicks) {
        auto& c = cfg();
        int s0 = std::max(1, c.autoStride);
        m_aiHorizon = c.autoHorizon;
        m_levels.clear();
        int total = 0;
        for (int i = 0; total < m_aiHorizon && i < kSlots - 2; ++i) {
            int seg = i < 6 ? s0 : (s0 << std::min(i - 5, 5));
            m_levels.push_back(seg);
            total += seg;
        }

        beginSearch(pl, 0, false, m_layout == Layout::DualSolo);   // autoplay: one input, both players
        m_budget = c.autoBudget;
        loadReal(0);
        m_failMemo.clear();
        m_bestReach = -1;
        m_bestLen = 0;

        bool prev = m_game[0];
        bool ok = aiDfs(0, 0, 0, prev);
        if (m_wantTrail && ok) m_trailOk = m_trail;

        outHold = prev;
        outTicks = 1;
        if (ok && m_planLen > 0) {
            outHold = m_planHold[0];
            outTicks = m_planSeg[0];
        } else if (m_bestLen > 0) {
            outHold = m_bestHold[0];
            outTicks = m_bestSeg[0];
        }
        endSearch();
        return ok;
    }

    void autoTick(PlayLayer* pl) {
        auto& c = cfg();
        PlayerObject* p1 = pl->m_player1;
        if (!p1) return;
        ++m_aiTick;

        // forgive what survived long enough (once per spot)
        while (!m_aiTrail.empty() && m_aiTick - m_aiTrail.front().tick > c.autoForget) {
            AiStep st = m_aiTrail.front();
            m_aiTrail.pop_front();
            if (st.cell == m_aiLastForgiven.first && st.hold == m_aiLastForgiven.second) continue;
            m_aiLastForgiven = {st.cell, st.hold};
            auto it = m_ai.find(st.cell);
            if (it != m_ai.end()) it->second.bad[st.hold ? 1 : 0] *= 0.6f;
        }

        if (m_aiHoldLeft <= 0) {
            bool h = m_aiHold;
            int ticks = 1;
            bool ok = planAuto(pl, h, ticks);
            m_aiHold = h;
            m_aiHoldLeft = std::max(1, std::min(c.autoEvery, ticks));
            ++m_dbgSearches;
            m_lastText = fmt::format("AI {} hold={} for {}t used={}", ok ? "plan" : "best", h ? 1 : 0,
                                     m_aiHoldLeft, m_used);
        }
        --m_aiHoldLeft;
        if (m_game[0] != m_aiHold) fire(pl, 0, m_aiHold);
        if (m_layout == Layout::DualSolo && m_game[1] != m_aiHold) fire(pl, 1, m_aiHold);

        m_aiTrail.push_back({aiCell(p1->getPositionX()), m_aiHold, m_aiTick});
        if (m_aiTrail.size() > 1200) m_aiTrail.pop_front();
    }

    void aiOnDeath() {
        auto& c = cfg();
        ++m_aiDeaths;
        if (c.autoLearn) {
            // blame the actions taken shortly before dying, once per spot,
            // the closer to the death the more
            std::unordered_map<int, float> blame[2];
            for (auto const& st : m_aiTrail) {
                int age = m_aiTick - st.tick;
                if (age > c.autoWindow) continue;
                float w = 1.f + 2.f * (1.f - static_cast<float>(age) /
                                                  static_cast<float>(std::max(1, c.autoWindow)));
                float& slot = blame[st.hold ? 1 : 0][st.cell];
                slot = std::max(slot, w);
            }
            for (int a = 0; a < 2; ++a) {
                for (auto const& kv : blame[a]) m_ai[kv.first].bad[a] += kv.second;
            }
        }
        m_aiTrail.clear();
        m_aiTick = 0;
        m_aiHoldLeft = 0;
        m_aiLastForgiven = {std::numeric_limits<int>::min(), false};
    }

    // ================================================================ trajectory
    // Live preview: where player 1 ends up if the button is HELD from now on and
    // where it ends up if it is RELEASED. The path that matches the current input is
    // drawn bright, the other one dim. Between two refreshes the paths are trimmed
    // (one point per tick) so the line never lags behind the player; a change of the
    // input refreshes at once.
    std::vector<TrailPt> simPreview(PlayLayer* pl, bool assumeHold, bool cur, Mode mode) {
        auto& c = cfg();
        beginSearch(pl, 0, false, m_layout == Layout::DualSolo);
        m_budget = c.previewLen + 8;
        loadReal(0);
        // hold: ship / wave (and anything already held) keep holding; click modes
        // that are not held yet get a tap, the way you would really press them
        int holdTicks = (isContinuous(mode) || cur) ? std::numeric_limits<int>::max() : c.tapTicks;
        std::vector<TrailPt> path;
        path.push_back({pos(0), 0});
        bool died = false;
        for (int t = 0; t < c.previewLen; ++t) {
            bool h = assumeHold && t < holdTicks;
            if (!step(0, h)) {
                died = !m_aborted;
                break;
            }
            path.push_back({pos(0), 0});
            if (m_info[0].unknown) break;                    // cannot predict past this
        }
        if (died) path.push_back({pos(0), 3});
        endSearch(false, false);
        return path;
    }

    // returns true when the overlay has to be redrawn
    bool previewPath(PlayLayer* pl) {
        auto& c = cfg();
        if (!c.showTrajectory) {
            if (m_trailHold.empty() && m_trailRelease.empty()) return false;
            m_trailHold.clear();
            m_trailRelease.clear();
            m_liveValid = false;
            return true;
        }
        if (!m_ready || m_load > 4.f) return false;         // not ready / busy (spam)
        PlayerObject* p1 = pl->m_player1;
        if (!p1 || p1->m_isDead) return false;

        bool cur = m_game[0];
        bool refresh = ++m_previewCounter >= std::max(1, c.previewEvery) ||
                       cur != m_prevHold || !m_liveValid;
        if (!refresh) {
            ++m_liveSkip;
            return true;
        }
        m_previewCounter = 0;
        m_liveSkip = 0;
        m_prevHold = cur;
        m_liveValid = true;

        Mode mode = detectMode(p1);
        bool both = c.previewBoth;
        if (both || cur) m_trailHold = simPreview(pl, true, cur, mode);
        else m_trailHold.clear();
        if (both || !cur) m_trailRelease = simPreview(pl, false, cur, mode);
        else m_trailRelease.clear();
        return true;
    }

    // Show path: looks for a hold / release route that survives the look-ahead
    // (same planner as autoplay, but it only draws the result: nothing is pressed,
    // delayed or learned). Pink = a route that survives, light red = best effort.
    bool pathScan(PlayLayer* pl) {
        auto& c = cfg();
        if (!c.showPath) {
            if (m_pathPts.empty()) return false;
            m_pathPts.clear();
            m_pathValid = false;
            return true;
        }
        if (!m_ready || m_load > 4.f) return false;
        PlayerObject* p1 = pl->m_player1;
        if (!p1 || p1->m_isDead) return false;

        bool cur = m_game[0];
        bool refresh = ++m_pathCounter >= std::max(1, c.pathEvery) ||
                       cur != m_pathPrevHold || !m_pathValid;
        if (!refresh) {
            ++m_pathSkip;
            return !m_pathPts.empty();
        }
        m_pathCounter = 0;
        m_pathSkip = 0;
        m_pathPrevHold = cur;
        m_pathValid = true;

        int s0 = std::max(1, c.autoStride);
        m_aiHorizon = c.pathHorizon;
        m_levels.clear();
        int total = 0;
        for (int i = 0; total < m_aiHorizon && i < kSlots - 2; ++i) {
            int seg = i < 6 ? s0 : (s0 << std::min(i - 5, 5));
            m_levels.push_back(seg);
            total += seg;
        }

        beginSearch(pl, 0, false, m_layout == Layout::DualSolo);
        m_budget = c.pathBudget;
        m_wantTrail = true;                                  // aiDfs records the route in m_trail
        loadReal(0);
        m_failMemo.clear();
        m_bestReach = -1;
        m_bestLen = 0;

        std::vector<TrailPt> path;
        path.push_back({pos(0), 0});
        bool ok = aiDfs(0, 0, 0, cur);
        if (ok) {
            path.insert(path.end(), m_trail.begin(), m_trail.end());
        } else if (m_bestLen > 0) {
            // nothing survives the whole look-ahead: replay the route that got furthest
            m_aborted = false;
            m_budget = m_used + m_aiHorizon + 16;
            loadReal(0);
            bool stop = false;
            bool died = false;
            for (int i = 0; i < m_bestLen && !stop; ++i) {
                for (int t = 0; t < m_bestSeg[i]; ++t) {
                    if (!step(0, m_bestHold[i])) {
                        died = !m_aborted;
                        stop = true;
                        break;
                    }
                    path.push_back({pos(0), 0});
                    if (m_info[0].unknown) {
                        stop = true;
                        break;
                    }
                }
            }
            if (died) path.push_back({pos(0), 3});
        }
        m_pathOk = ok;
        m_pathPts = std::move(path);
        endSearch(false, false);
        return true;
    }

    void overlayTick(PlayLayer* pl) {
        if (!m_draw) return;
        bool a = previewPath(pl);
        bool b = pathScan(pl);
        if (a || b) drawResult();
    }

    void drawResult() {
        auto& c = cfg();
        if (!m_draw) return;
        m_draw->clear();
        float alpha = static_cast<float>(c.trajOpacity) / 100.f;
        float width = static_cast<float>(c.lineWidth) / 10.f;
        auto line = [&](std::vector<TrailPt> const& t, ccColor4F col, float w, size_t skip) {
            col.a *= alpha;
            for (size_t i = std::max<size_t>(1, skip + 1); i < t.size(); ++i) {
                m_draw->drawSegment(t[i - 1].p, t[i].p, w, col);
            }
            for (size_t i = skip; i < t.size(); ++i) {
                auto const& pt = t[i];
                if (pt.tag == 1) m_draw->drawDot(pt.p, 3.f, {1.f, .85f, .1f, alpha});
                else if (pt.tag == 2) m_draw->drawDot(pt.p, 3.5f, {.85f, .3f, 1.f, alpha});
                else if (pt.tag == 3) m_draw->drawDot(pt.p, 4.f, {1.f, .1f, .1f, alpha});
            }
        };

        // scanned feasible route (display only), underneath everything else
        if (c.showPath && !m_pathPts.empty()) {
            ccColor4F col = m_pathOk ? ccColor4F{.95f, .4f, 1.f, .95f} : ccColor4F{1.f, .5f, .5f, .8f};
            line(m_pathPts, col, width * 1.25f, static_cast<size_t>(m_pathSkip));
        }
        if (!c.showTrajectory) return;

        if (m_keepTicks > 0) {
            line(m_keepFail, {1.f, .3f, .3f, .7f}, width, 0);
            line(m_keepOk, {.2f, 1.f, .4f, 1.f}, width, 0);
        }
        // hold = cyan, release = orange; the one matching the current input is brighter
        ccColor4F holdCol = {.35f, .85f, 1.f, .9f};
        ccColor4F relCol = {1.f, .65f, .2f, .9f};
        size_t skip = static_cast<size_t>(m_liveSkip);
        ccColor4F holdDim = holdCol;
        ccColor4F relDim = relCol;
        holdDim.a *= .5f;
        relDim.a *= .5f;
        if (m_prevHold) {
            line(m_trailRelease, relDim, width * .7f, skip);
            line(m_trailHold, holdCol, width, skip);
        } else {
            line(m_trailHold, holdDim, width * .7f, skip);
            line(m_trailRelease, relCol, width, skip);
        }
    }
};

inline Engine g_engine;

} // namespace buf
