#pragma once
#include "Types.hpp"

namespace sb {

struct WaveSpam {
    std::deque<float> holds, rels;
    uint64_t lastStep = 0;
    bool lastDown = false, have = false;
    float conf = 0.f, ratio = 0.f;
    int dir = 0;
    int pendCnt = 0;

    void reset() { *this = WaveSpam{}; }

    void onReal(uint64_t step, bool down, const Cfg& c) {
        if (have && step > lastStep) {
            auto& q = lastDown ? holds : rels;
            q.push_back(float(step - lastStep));
            while (int(q.size()) > c.waveWindow) q.pop_front();
        }
        lastStep = step;
        lastDown = down;
        have = true;
        evaluate(c);
    }

    void evaluate(const Cfg& c) {
        if (holds.size() < 3 || rels.size() < 3) { conf = ratio = 0.f; dir = 0; pendCnt = 0; return; }
        float mh = meanOf(holds), mr = meanOf(rels);
        ratio = (mh - mr) / (mh + mr);
        float cons = std::clamp(1.f - 0.5f * (cvOf(holds) + cvOf(rels)), 0.f, 1.f);
        conf = std::fabs(ratio) * cons;
        int cand = ratio > 0.f ? 1 : -1;
        if (conf < c.waveExit) { dir = 0; pendCnt = 0; return; }
        if (conf < c.waveEnter) return;
        if (dir == cand) { pendCnt = 0; return; }
        float margin = dir == 0 ? 0.f : 0.15f;
        int need = dir == 0 ? 2 : 3;
        if (conf < c.waveEnter + margin) { pendCnt = 0; return; }
        if (++pendCnt >= need) { dir = cand; pendCnt = 0; }
    }

    Action onStep(uint64_t step, bool gameDown, uint64_t lastToggle, int modRun, const Cfg& c) const {
        Action a;
        if (dir == 0 || !have || modRun >= c.waveMaxRun) return a;
        float mH = medianOf(holds), mR = medianOf(rels);
        if (float(step - lastStep) > 3.f * (mH + mR) + 2.f) return a;
        float el = float(step - lastToggle);
        if (dir > 0 && !gameDown) {
            float lo = std::ceil(mR * (1.f + c.waveSlack));
            if (el >= lo && el <= mR * 2.f + 1.f) a = {true, true, Source::Wave};
        } else if (dir < 0 && gameDown) {
            float lo = std::ceil(mH * (1.f + c.waveSlack));
            if (el >= lo && el <= mH * 2.f + 1.f) a = {true, false, Source::Wave};
        }
        return a;
    }
};

} // namespace sb