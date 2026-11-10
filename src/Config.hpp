#pragma once
#include <Geode/Geode.hpp>
#include <algorithm>

namespace buf {

enum Mode { M_CUBE = 0, M_SHIP, M_BALL, M_WAVE, M_ROBOT, M_SPIDER, M_UFO, M_SWING, M_COUNT };

inline constexpr char const* kModeNames[M_COUNT] = {
    "Cube", "Ship", "Ball", "Wave", "Robot", "Spider", "UFO", "Swing"
};

struct Config {
    // general
    bool enabled = true;
    bool showTrajectory = false;        // draw the predicted path, always (not only after a click)
    bool logEnabled = false;            // rescue log (first page of the settings window)
    bool pauseButton = true;            // BUF button in the pause menu (Geode setting "pause-button")
    bool dualSupport = true;            // 1-player dual: one input drives both players
    bool bothSides = true;              // solo: both screen sides are player 1
    bool orbDetect = true;
    bool dropFatal = false;
    bool twoPlayerMode = true;          // handle real 2-player mode (off = untouched)
    bool moveDetect = true;             // a running move trigger makes the prediction back off
    // display
    int previewLen = 60;                // ticks of the live preview
    int previewEvery = 6;               // ticks between preview refreshes
    int keepTicks = 240;                // how long the last click path stays on screen
    int lineWidth = 8;                  // 1/10 px
    bool previewBoth = true;            // draw BOTH paths: "if you hold" and "if you release"
    // show path: scans a feasible hold/release route and only DRAWS it (no effect on the game)
    bool showPath = false;
    int pathHorizon = 90;               // ticks looked ahead
    int pathEvery = 6;                  // ticks between rescans
    int pathBudget = 2500;              // simulated ticks per scan
    int trajOpacity = 100;              // %

    // advanced search
    int settle = 6;                     // ticks on the ground that count as a stable landing
    int airHorizon = 45;                // UFO / Swing look-ahead cap
    int robotHold = 18;                 // robot long-hold length
    int gainClick = 6;                  // extra ticks a delay must survive to be preferred
    int minBudget = 300;                // smallest search budget under spam
    int noclipTicks = 480;              // how long NOCLIP stays flagged after a blocked death
    int failOpen = 4;                   // frames without a physics tick before input is never held

    // ship / wave: only act when holding or releasing NOW is about to kill you
    int shipImminent = 16;              // ticks: death sooner than this counts as imminent
    int waveImminent = 8;               // fixed by SpamBuffer
    int holdGain = 4;                   // a delay must push the death back by this many ticks

    // autoplay (learns from every death, independent of the click assist)
    bool autoplay = false;
    bool autoLearn = true;
    bool autoKeep = false;              // keep what it learned when another level loads
    int autoHorizon = 70;               // ticks looked ahead
    int autoEvery = 4;                  // replan every N ticks
    int autoStride = 3;                 // decision step near the player
    int autoBudget = 1500;              // simulated ticks per plan
    int autoCell = 8;                   // memory cell width (px)
    int autoWindow = 48;                // ticks before a death that get blamed
    int autoPrune = 4;                  // blame score after which an action is avoided
    int autoForget = 120;               // ticks after which a survived action is forgiven

    int trajectory = 120;               // lookahead (ticks) for click-style modes

    // per mode: max ticks a press / release may be held back. 0 = mode off.
    int maxDelay[M_COUNT] = {0, 0, 0, 6, 0, 0, 0, 0};   // SpamBuffer only uses WAVE

    // physics (0 = auto detect)
    int tps = 0;
    int fps = 0;

    // search
    // wave: fixed slope the ghost wave follows (angles in 1/100 degree)
    bool waveAngles = false;            // off: the ghost uses the game's own physics
    int waveUp = 4510;                  // 45.10 deg going up
    int waveDown = 4490;                // 44.90 deg going down
    int miniUp = 6343;                  // mini wave >= 63.43 deg
    int miniDown = 6343;

    int tapTicks = 4;                   // assumed length of a tap
    int budget = 3000;                  // max simulated ticks per search
    int shipHorizon = 36;               // ticks a held / released state must survive
    int waveHorizon = 16;               // SpamBuffer: 13..18
};

inline Config& cfg() {
    static Config c;
    return c;
}

inline int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

} // namespace buf