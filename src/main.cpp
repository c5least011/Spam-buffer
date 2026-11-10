#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/RingObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/binding/RingObject.hpp>
#include <Geode/utils/cocos.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <cstdlib>
#include <functional>
#include <Geode/binding/ButtonSprite.hpp>
#include <chrono>
#include "Core.hpp"
#include "Engine.hpp"

using namespace geode::prelude;
using buf::g_engine;

namespace sb {
inline bool g_injecting = false;

// Wave, Frame Perfect and Orb are mutually exclusive: turning one on turns the
// other two off, so two assist engines can never fight over the same input.
// "wave" / "fp" / "orb" selects the mode; on=false just switches it off.
inline void setExclusiveMode(const char* mode, bool on) {
    auto* mod = Mod::get();
    auto& c = Core::get().cfg;
    const std::string m = mode;
    c.wave = on && m == "wave";
    c.fp = on && m == "fp";
    c.orb = false;   // Orb assist: coming soon
    mod->setSavedValue("cfg.wave-enabled", c.wave);
    mod->setSavedValue("cfg.fp-enabled", c.fp);
}

inline void setFramePerfectEnabled(bool enabled) {
    setExclusiveMode("fp", enabled);
}

inline void toggleFramePerfect() {
    setFramePerfectEnabled(!Core::get().cfg.fp);
}

static void refreshCfg() {
    auto* mod = Mod::get();
    auto& c = Core::get().cfg;

    // All user-facing configuration lives in this mod's saved data. The
    // Geode Settings page is intentionally unused.
    c.wave = mod->getSavedValue<bool>("cfg.wave-enabled", true);
    c.fp = mod->getSavedValue<bool>("cfg.fp-enabled", false);
    c.orb = false;   // Orb assist: coming soon
    c.hud = mod->getSavedValue<bool>("cfg.hud-enabled", true);
    c.traj = mod->getSavedValue<bool>("cfg.traj-enabled", false);
    c.waveDelay = mod->getSavedValue<int>("cfg.wave-delay", 6);
    c.waveLook = mod->getSavedValue<int>("cfg.wave-look", 16);
    c.fpCps = mod->getSavedValue<float>("cfg.fp-cps", 30.f);
    c.tps = mod->getSavedValue<int>("cfg.tps", 240);
    c.fpSlowerCps = mod->getSavedValue<float>("cfg.fp-slower-cps", 3.f);
    c.orbLookahead = mod->getSavedValue<int>("cfg.orb-lookahead", 40);

    c.tps = std::clamp(c.tps, 30, 1000);
    c.fpCps = std::clamp(c.fpCps, 1.f, float(c.tps) * 0.5f);
    c.fpSlowerCps = std::clamp(c.fpSlowerCps, 0.f, std::max(0.f, c.fpCps - 0.1f));
    if (c.fp) { c.wave = false; c.orb = false; }
    else if (c.wave) { c.orb = false; }

    c.waveDelay = std::clamp(c.waveDelay, 0, 8);
    c.waveLook = std::clamp(c.waveLook, 13, 18);
    c.orbLookahead = std::clamp(c.orbLookahead, 10, 120);

    // Ghost-player engine (wave buffer). Imminent is fixed at 8; delay 0 = off.
    auto& b = buf::cfg();
    b.enabled = c.wave || c.orb;
    b.orbDetect = true;
    b.tapTicks = Cfg::orbTap;
    b.maxDelay[buf::M_WAVE] = c.waveDelay;
    b.waveHorizon = c.waveLook;
    b.waveImminent = Cfg::waveImminent;
    b.showTrajectory = c.traj;
    b.showPath = false;
    b.autoplay = false;
    b.logEnabled = false;
    b.tps = 0;   // the engine detects the game's TPS / FPS itself
    b.fps = 0;
}

// The CPS window (HUD) is the only thing the hotkey toggles.
inline void toggleHud() {
    Mod::get()->setSavedValue("cfg.hud-enabled", !Core::get().cfg.hud);
    refreshCfg();
}

static bool parseInt(std::string const& s, int& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return false;
    out = int(v);
    return true;
}

static bool parseFloat(std::string const& s, float& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') return false;
    out = v;
    return true;
}
}

$on_mod(Loaded) {
    sb::refreshCfg();
}

// ---------------------------------------------------------------------------
// Mod menu. Opened from a small icon in the pause menu / main menu only (there
// is nothing on screen while playing). Values are typed into text boxes.
// ---------------------------------------------------------------------------
class SBConfigPopup : public Popup {
protected:
    static constexpr float kW = 270.f;
    static constexpr float kH = 186.f;
    static inline int s_tab = 0;          // settings shown: 0 wave, 1 frame perfect, 2 orb

    CCNode* m_content = nullptr;
    bool m_dragging = false;

    bool init() override {
        if (!Popup::init(kW, kH, "square02b_001.png"))
            return false;

        this->setTitle("Spam Buffer", "goldFont.fnt", 0.6f);
        this->setKeypadEnabled(true);
        if (m_bgSprite) {                  // small, dark, rounded
            m_bgSprite->setColor(ccc3(24, 24, 32));
            m_bgSprite->setOpacity(238);
        }
        m_content = CCNode::create();
        m_mainLayer->addChild(m_content, 5);

        auto& c = sb::Core::get().cfg;
        s_tab = c.fp ? 1 : (c.orb ? 2 : 0);
        build();
        return true;
    }

    // ---- drag the window by its title bar
    bool ccTouchBegan(CCTouch* touch, CCEvent* event) override {
        auto local = m_mainLayer->convertToNodeSpace(touch->getLocation());
        m_dragging = local.x >= 0.f && local.x <= kW && local.y >= kH - 30.f && local.y <= kH + 6.f;
        if (m_dragging) return true;
        return Popup::ccTouchBegan(touch, event);
    }

    void ccTouchMoved(CCTouch* touch, CCEvent* event) override {
        if (!m_dragging) {
            Popup::ccTouchMoved(touch, event);
            return;
        }
        auto delta = touch->getLocation() - touch->getPreviousLocation();
        m_mainLayer->setPosition(m_mainLayer->getPosition() + delta);
    }

    void ccTouchEnded(CCTouch* touch, CCEvent* event) override {
        if (m_dragging) {
            m_dragging = false;
            return;
        }
        Popup::ccTouchEnded(touch, event);
    }

    void ccTouchCancelled(CCTouch* touch, CCEvent* event) override {
        m_dragging = false;
        Popup::ccTouchCancelled(touch, event);
    }

    // rounded pill button
    CCMenuItemSpriteExtra* makePill(
        const std::string& text, SEL_MenuHandler callback, bool on, float width, float height = 22.f
    ) {
        auto* node = CCNode::create();
        node->setContentSize({width, height});
        auto* bg = CCScale9Sprite::create("square02b_001.png");
        bg->setContentSize({width, height});
        bg->setColor(on ? ccc3(70, 200, 120) : ccc3(80, 80, 92));
        bg->setPosition({width * 0.5f, height * 0.5f});
        node->addChild(bg);
        auto* label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
        label->setScale(0.3f);
        label->setPosition({width * 0.5f, height * 0.5f});
        node->addChild(label);
        return CCMenuItemSpriteExtra::create(node, this, callback);
    }

    void addLabel(const std::string& text, CCPoint pos, float scale, ccColor3B color, GLubyte opacity = 255) {
        auto* label = CCLabelBMFont::create(text.c_str(), "chatFont.fnt");
        label->setScale(scale);
        label->setColor(color);
        label->setOpacity(opacity);
        label->setPosition(pos);
        m_content->addChild(label);
    }

    // label above, text box below
    void addInput(
        CCPoint pos, const char* label, const std::string& value, bool isFloat,
        std::function<void(std::string const&)> callback
    ) {
        addLabel(label, {pos.x, pos.y + 18.f}, 0.38f, ccc3(255, 255, 255));
        auto* input = TextInput::create(84.f, "0", "chatFont.fnt");
        input->setCommonFilter(isFloat ? CommonFilter::Float : CommonFilter::Uint);
        input->setMaxCharCount(7);
        input->setString(value, false);
        input->setCallback(std::move(callback));
        input->setScale(0.78f);
        input->setPosition(pos);
        m_content->addChild(input);
    }

    void build() {
        m_content->removeAllChildrenWithCleanup(true);
        auto& c = sb::Core::get().cfg;
        const ccColor3B gold = ccc3(255, 215, 0);

        auto* menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        m_content->addChild(menu, 2);

        // --- the three assist modes (only one can be on); also picks the settings below
        const char* names[3] = {"WAVE", "FRAME PERFECT", "ORB SOON"};
        const bool states[3] = {c.wave, c.fp, false};
        const float px[3] = {52.f, 135.f, 218.f};
        for (int i = 0; i < 3; ++i) {
            auto* item = makePill(names[i], menu_selector(SBConfigPopup::onToggle), states[i], 76.f);
            item->setTag(i);
            item->setPosition({px[i], 134.f});
            menu->addChild(item);
        }
        // little marker under the tab whose settings are shown
        auto* mark = CCLabelBMFont::create(".", "bigFont.fnt");
        mark->setScale(0.5f);
        mark->setColor(gold);
        mark->setPosition({px[s_tab], 116.f});
        m_content->addChild(mark);

        // --- settings of the selected tab
        if (s_tab == 0) {
            addInput({70.f, 80.f}, "Delay (0-8)", std::to_string(c.waveDelay), false,
                     [](std::string const& s) {
                         int v;
                         if (!sb::parseInt(s, v)) return;
                         Mod::get()->setSavedValue("cfg.wave-delay", std::clamp(v, 0, 8));
                         sb::refreshCfg();
                     });
            addInput({200.f, 80.f}, "Look (13-18)", std::to_string(c.waveLook), false,
                     [](std::string const& s) {
                         int v;
                         if (!sb::parseInt(s, v)) return;
                         Mod::get()->setSavedValue("cfg.wave-look", std::clamp(v, 13, 18));
                         sb::refreshCfg();
                     });
            addLabel(fmt::format("Imminent: {} (fixed)", sb::Cfg::waveImminent), {135.f, 50.f}, 0.36f,
                     ccc3(180, 180, 180));
        } else if (s_tab == 1) {
            addInput({50.f, 80.f}, "Target CPS", fmt::format("{:.1f}", c.fpCps), true,
                     [](std::string const& s) {
                         float v;
                         if (!sb::parseFloat(s, v)) return;
                         auto& cc = sb::Core::get().cfg;
                         Mod::get()->setSavedValue("cfg.fp-cps", std::clamp(v, 1.f, float(cc.tps) * 0.5f));
                         sb::refreshCfg();
                     });
            addInput({135.f, 80.f}, "CPS below", fmt::format("{:.1f}", c.fpSlowerCps), true,
                     [](std::string const& s) {
                         float v;
                         if (!sb::parseFloat(s, v)) return;
                         Mod::get()->setSavedValue("cfg.fp-slower-cps", std::max(0.f, v));
                         sb::refreshCfg();
                     });
            addInput({220.f, 80.f}, "Game TPS", std::to_string(c.tps), false,
                     [](std::string const& s) {
                         int v;
                         if (!sb::parseInt(s, v)) return;
                         Mod::get()->setSavedValue("cfg.tps", std::clamp(v, 30, 1000));
                         sb::refreshCfg();
                     });
        } else {
            addLabel("Orb assist: coming soon", {135.f, 84.f}, 0.5f, gold);
            addLabel("Planned: keeps you on the orb path while you spam", {135.f, 58.f}, 0.34f,
                     ccc3(180, 180, 180));
        }

        // --- general
        auto* hud = makePill("CPS WINDOW", menu_selector(SBConfigPopup::onToggle), c.hud, 104.f, 20.f);
        hud->setTag(3);
        hud->setPosition({75.f, 24.f});
        menu->addChild(hud);

        auto* traj = makePill("TRAJECTORY", menu_selector(SBConfigPopup::onToggle), c.traj, 92.f, 20.f);
        traj->setTag(4);
        traj->setPosition({189.f, 24.f});
        menu->addChild(traj);
    }

    void onToggle(CCObject* sender) {
        auto* mod = Mod::get();
        const int tag = sender->getTag();
        switch (tag) {
            case 0: s_tab = 0; sb::setExclusiveMode("wave", !sb::Core::get().cfg.wave); break;
            case 1: s_tab = 1; sb::setExclusiveMode("fp", !sb::Core::get().cfg.fp); break;
            case 2: s_tab = 2; break;   // Orb assist: coming soon (just shows its tab)
            case 3: mod->setSavedValue("cfg.hud-enabled", !sb::Core::get().cfg.hud); break;
            case 4: mod->setSavedValue("cfg.traj-enabled", !sb::Core::get().cfg.traj); break;
            default: return;
        }
        sb::refreshCfg();
        build();
    }

public:
    // show it with a light backdrop: the game stays visible behind the small window
    void showWindow() {
        this->show();
        this->stopAllActions();
        this->setOpacity(70);
    }

    static SBConfigPopup* create() {
        auto* ret = new SBConfigPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};


// Scan the level once: every blue (gravity) orb, grouped into zones of orbs that
// sit close together. The orb rescue only runs the ghost inside a zone.
static void buildOrbZones(PlayLayer* pl) {
    auto& core = sb::Core::get();
    core.orbs.clear();
    core.zones.clear();
    if (!pl || !pl->m_objects) return;
    for (auto* obj : CCArrayExt<GameObject*>(pl->m_objects)) {
        if (!obj || obj->m_objectID != 84) continue;
        auto r = obj->getObjectRect();
        core.orbs.push_back({r.getMinX(), r.getMaxX(), r.getMinY(), r.getMaxY()});
    }
    std::sort(core.orbs.begin(), core.orbs.end(),
              [](sb::OrbPt const& a, sb::OrbPt const& b) { return a.x0 < b.x0; });
    constexpr float kZoneGap = 150.f;   // orbs closer than this (level units) share a zone
    for (int i = 0; i < int(core.orbs.size()); ++i) {
        auto const& o = core.orbs[i];
        if (!core.zones.empty() && o.x0 - core.zones.back().x1 <= kZoneGap) {
            auto& z = core.zones.back();
            z.x1 = std::max(z.x1, o.x1);
            ++z.count;
        } else {
            core.zones.push_back({o.x0, o.x1, i, 1});
        }
    }
}

namespace sb {
// Wave control: fire the held-back press / release whose tick has come. Same moment
// as the input buffer mod (start of the physics tick, before the player moves), one
// event per tick and lane.
static void fireDueWave(Core& core, uint64_t now) {
    auto* pl = PlayLayer::get();
    if (!pl) return;
    for (int i = 0; i < 2; ++i) {
        auto& lane = core.lane(i == 0);
        if (lane.pend.empty() || lane.pend.front().at > now) continue;
        const Pend e = lane.pend.front();
        lane.pend.pop_front();
        if (e.down == lane.gameDown) continue;
        g_injecting = true;
        pl->handleButton(e.down, 1, i == 0);
        g_injecting = false;
        ++core.dbg.inj;
        core.committed(i == 0, Action{true, e.down, Source::Wave});
    }
}
}

class $modify(SBBase, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", -1000000);
    }

    // ---- ghost-player support (the wave buffer simulates on hidden players)
    bool canBeActivatedByPlayer(PlayerObject* p0, EffectGameObject* p1) {
        if (g_engine.isSimulating()) return g_engine.canSimRing(p0, p1);
        return GJBaseGameLayer::canBeActivatedByPlayer(p0, p1);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* vec, int objectsCount, float dt) {
        if (g_engine.isSimulating()) {
            g_engine.simTouchObjects(player, vec, objectsCount);   // gravity / speed portals, unknown objects
            thread_local gd::vector<GameObject*> extra;
            thread_local std::vector<std::pair<GameObject*, CCPoint>> moved;
            extra.clear();
            moved.clear();
            if (extra.capacity() < static_cast<size_t>(objectsCount)) {
                extra.reserve(objectsCount);
            }
            for (int i = 0; i < objectsCount; i++) {
                GameObject* obj = vec->at(i);
                if (!obj) continue;
                if (obj->m_groups && obj->m_groups->at(0) != 0) {
                    CCPoint off;
                    auto st = g_engine.simMove(obj, off);
                    if (st == buf::Engine::MoveState::Predicted) {
                        moved.emplace_back(obj, obj->getPosition());   // restored right after the check
                        obj->setPosition(obj->getPosition() + off);
                    } else if (st == buf::Engine::MoveState::Unknown) {
                        g_engine.flagMovingObject();       // moving, speed unknown: do not trust it
                    }
                }
                auto type = obj->m_objectType;
                if (type == GameObjectType::Solid ||
                    type == GameObjectType::Hazard ||
                    type == GameObjectType::AnimatedHazard ||
                    type == GameObjectType::CollisionObject ||
                    type == GameObjectType::Slope ||
                    type == GameObjectType::NormalGravityPortal ||
                    type == GameObjectType::InverseGravityPortal ||
                    type == GameObjectType::GravityTogglePortal ||
                    geode::cast::typeinfo_cast<RingObject*>(obj)) {
                    extra.push_back(obj);
                }
            }
            GJBaseGameLayer::collisionCheckObjects(player, &extra, extra.size(), dt);
            for (auto& mv : moved) mv.first->setPosition(mv.second);
            moved.clear();
            return;
        }
        if (auto pl = PlayLayer::get()) {
            if (player == pl->m_player1) g_engine.observeObjects(vec, objectsCount);
        }
        GJBaseGameLayer::collisionCheckObjects(player, vec, objectsCount, dt);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (g_engine.isSimulating()) {
            if (g_engine.canSimRing(player, ring)) player->ringJump(ring, true);
            return;
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& core = sb::Core::get();
        if (g_engine.isSimulating() || sb::g_injecting || !core.active || button != 1 ||
            static_cast<GJBaseGameLayer*>(PlayLayer::get()) != this) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        auto* pl = PlayLayer::get();

        ++core.dbg.in[isPlayer1 ? 0 : 1][down ? 0 : 1];

        // Every flag keeps its own lane and its own flag: an event flagged player 2
        // is a player-2 event (no dual or dual alike), it is never re-tagged as player 1.

        // Two-player / dual: every side has its own input lane.
        // The wave buffer asks the ghost engine how long this event may be held
        // back; a held-back event is injected later by the lane.
        const bool forward = core.onReal(isPlayer1, down, g_engine.sbNext(), [&](bool d) {
            return g_engine.sbWaveDelay(pl, isPlayer1, d, core.p1.gameDown,
                                        core.p2.gameDown, core.lane(isPlayer1).gameDown);
        });
        if (forward) {
            ++core.dbg.fwd;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
    }

    void processQueuedButtons(float dt, bool clearInputQueue) {
        auto& core = sb::Core::get();
        if (core.active && static_cast<GJBaseGameLayer*>(PlayLayer::get()) == this) {
            PlayerObject* players[2] = {m_player1, m_player2};
            sb::View views[2];
            static float lastX[2] = {0.f, 0.f};
            static bool haveX[2] = {false, false};

            for (int i = 0; i < 2; ++i) {
                auto* player = players[i];
                auto& view = views[i];
                if (player && !player->m_isDead) {
                    view.valid = true;
                    view.isWave = player->m_isDart;
                    view.upsideDown = player->m_isUpsideDown;
                    float x = player->getPositionX();
                    float dx = haveX[i] ? x - lastX[i] : 0.f;
                    if (std::fabs(dx) > 200.f) dx = 0.f; // teleport / restart
                    lastX[i] = x;
                    haveX[i] = true;
                    // orb rescue: only cube / ball / robot / spider / ufo, only near a zone
                    const bool orbMode = !player->m_isDart && !player->m_isShip && !player->m_isSwing;
                    if (core.cfg.orb && orbMode && !core.zones.empty()) {
                        const float margin = std::max(40.f, std::fabs(dx) * 6.f);
                        view.orbZone = core.zoneIndex(x, margin);
                        if (view.orbZone >= 0) {
                            // only when an orb is really close: keeps the ghost off the CPU otherwise
                            const auto& z = core.zones[view.orbZone];
                            const float py = player->getPositionY();
                            const float hx = 30.f + std::fabs(dx) * 3.f;
                            bool closeToOrb = false;
                            for (int k = 0; k < z.count && !closeToOrb; ++k) {
                                const auto& o = core.orbs[z.first + k];
                                closeToOrb = x >= o.x0 - hx && x <= o.x1 + hx &&
                                       py >= o.y0 - 45.f && py <= o.y1 + 45.f;
                            }
                            if (!closeToOrb) view.orbZone = -1;
                        }
                    }
                } else {
                    haveX[i] = false;
                }
            }

            // Solo: the right-side lane drives the same (only) player. It follows the
            // player 1 view, and never runs the orb rescue a second time.
            // No dual: only player 1 exists, so the player-2-flagged lane (right side)
            // looks at the same player as the player-1 lane.
            core.dual = m_gameState.m_isDualMode && m_player2;
            if (!core.dual) {
                views[1].valid = views[0].valid;
                views[1].isWave = views[0].isWave;
                views[1].upsideDown = views[0].upsideDown;
                views[1].orbZone = views[0].orbZone;
            }

            if ((core.step & 63) == 0) sb::refreshCfg();

            using clock = std::chrono::steady_clock;
            static auto t0 = clock::now();
            static uint64_t n0 = 0;
            double elapsed = std::chrono::duration<double>(clock::now() - t0).count();
            if (elapsed >= 1.0) {
                core.clockRate = float(double(core.step - n0) / elapsed);
                core.clockBad = std::fabs(core.clockRate / float(std::max(1, core.cfg.tps)) - 1.f) > 0.15f;
                t0 = clock::now();
                n0 = core.step;
            }

            auto* pl = PlayLayer::get();
            auto actions = core.onStep(views[0], views[1], [&](bool isP1) {
                const auto& v = views[isP1 ? 0 : 1];
                if (v.orbZone < 0 || v.orbZone >= int(core.zones.size())) return false;
                const auto& z = core.zones[v.orbZone];
                return g_engine.sbOrbRescue(pl, isP1, core.p1.gameDown, core.p2.gameDown,
                                            core.lane(isP1).gameDown, core.cfg.orbLookahead,
                                            core.orbs.data() + z.first, z.count);
            });
            for (int i = 0; i < 2; ++i) {
                auto const& action = actions[i];
                if (!action.want) continue;
                sb::g_injecting = true;
                this->handleButton(action.down, 1, i == 0);
                sb::g_injecting = false;
                ++core.dbg.inj;
                core.committed(i == 0, action);
            }
        }
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
    }
};

class $modify(SBPlay, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", -2147483647);  // run before noclip mods
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", -1000000);
        (void)self.setHookPriority("PlayLayer::flipGravity", -1000000);
    }

    struct Fields {
        CCLabelBMFont* hud = nullptr;
        CCLabelBMFont* watermark = nullptr;
        int tick = 0;
        bool shiftWas = false;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        g_engine.init();                       // hidden ghost players for the wave buffer
        return true;
    }

    void playEndAnimationToPos(cocos2d::CCPoint p0) {
        if (g_engine.isSimulating()) return;
        PlayLayer::playEndAnimationToPos(p0);
    }

    void flipGravity(PlayerObject* p0, bool p1, bool p2) {
        if (g_engine.isSimulating()) {
            if (!g_engine.isSimPlayer(p0)) return;
            PlayLayer::flipGravity(p0, p1, true);
            return;
        }
        PlayLayer::flipGravity(p0, p1, p2);
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        sb::refreshCfg();
        // Orb assist is coming soon: no zone scan
        auto& core = sb::Core::get();
        core.reset();
        core.active = true;

        auto* hud = CCLabelBMFont::create("", "chatFont.fnt");
        hud->setAnchorPoint({0.f, 1.f});
        auto winSize = CCDirector::get()->getWinSize();
        hud->setPosition({6.f, winSize.height - 6.f});
        hud->setScale(0.45f);
        hud->setOpacity(150);
        this->addChild(hud, 900);
        m_fields->hud = hud;

        // Watermark: small, gold, 35% opacity, bottom centre, only while a mode is on.
        auto* mark = CCLabelBMFont::create("Spam Buffer by C5least", "bigFont.fnt");
        mark->setScale(0.2f);
        mark->setColor(ccc3(255, 215, 0));
        mark->setOpacity(89);                       // 0.35 * 255
        mark->setAnchorPoint({0.5f, 0.f});
        mark->setPosition({winSize.width * 0.5f, 3.f});
        this->addChild(mark, 900);
        m_fields->watermark = mark;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto& core = sb::Core::get();

#if defined(GEODE_IS_WINDOWS)
        {   // Left Shift: show / hide the CPS window (nothing else)
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0;
            if (down && !m_fields->shiftWas) sb::toggleHud();
            m_fields->shiftWas = down;
        }
#endif

        if (++m_fields->tick % 6 != 0) return;

        if (auto* hud = m_fields->hud) {
            hud->setVisible(core.cfg.hud);
            if (core.cfg.hud) {
                std::string text = core.hud();
                if (core.cfg.traj)
                    text += fmt::format("\n{} L {}/{} R {}/{} fwd {} inj {}",
                                        g_engine.sbLayoutName(PlayLayer::get()),
                                        core.dbg.in[0][0], core.dbg.in[0][1],
                                        core.dbg.in[1][0], core.dbg.in[1][1],
                                        core.dbg.fwd, core.dbg.inj);
                if (core.cfg.traj && core.cfg.wave) text += "\n" + g_engine.sbStats();
                if (core.cfg.orb)
                    text += fmt::format("\norb zones {} / orbs {} | {}", core.zones.size(),
                                        core.orbs.size(), g_engine.sbLastText());
                hud->setString(text.c_str());
            }
        }
        if (auto* mark = m_fields->watermark)
            mark->setVisible(core.cfg.wave || core.cfg.fp || core.cfg.orb);
    }

    void resetLevel() {
        g_engine.onReset();
        PlayLayer::resetLevel();
        sb::Core::get().reset();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_engine.onSimDeath(player)) return;       // ghost player: swallow
        // Capture the lane before the original method mutates player pointers.
        const bool wasPlayer1 = player == m_player1;
        const bool wasPlayer2 = player == m_player2;
        PlayLayer::destroyPlayer(player, object);
        g_engine.onRealDestroyed(player);              // real death, or one a noclip mod blocked
        auto& core = sb::Core::get();
        // Collision/death must not cancel FP. Clear only helpers that depend on
        // the old player's current movement/object context, for that side.
        if (wasPlayer1) {
            core.p1.pend.clear();
            core.p1.orb.reset();
        } else if (wasPlayer2) {
            core.p2.pend.clear();
            core.p2.orb.reset();
        }
    }

    void pauseGame(bool paused) {
        sb::Core::get().onPause();
        PlayLayer::pauseGame(paused);
    }

    void onQuit() {
        auto& core = sb::Core::get();
        core.active = false;
        core.reset();
        core.zones.clear();
        core.orbs.clear();
        g_engine.quit();
        PlayLayer::onQuit();
    }
};

// ---------------------------------------------------------------------------
// Ghost-player plumbing (everything a hidden simulation must not show / trigger).
// ---------------------------------------------------------------------------
class $modify(SBHardStreak, HardStreak) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("HardStreak::addPoint", -1000000);
    }

    void addPoint(cocos2d::CCPoint p0) {
        if (g_engine.isSimulating()) return;
        HardStreak::addPoint(p0);
    }
};

class $modify(SBEffectObject, EffectGameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("EffectGameObject::triggerObject", -1000000);
    }

    void triggerObject(GJBaseGameLayer* p0, int p1, const gd::vector<int>* p2) {
        if (g_engine.isSimulating()) {
            g_engine.simGravityPortal(this);
            return;
        }
        if (m_objectID == 901) g_engine.onRealMoveTrigger(m_duration);   // move trigger
        EffectGameObject::triggerObject(p0, p1, p2);
    }
};

class $modify(SBGameObject, GameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GameObject::playShineEffect", -1000000);
    }

    void playShineEffect() {
        if (g_engine.isSimulating()) return;
        GameObject::playShineEffect();
    }
};

class $modify(SBPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", -1000000);
        (void)self.setHookPriority("PlayerObject::incrementJumps", -1000000);
        (void)self.setHookPriority("PlayerObject::ringJump", -1000000);
    }

    void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
        if (g_engine.isSimulating()) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void incrementJumps() {
        if (g_engine.isSimulating() && !g_engine.isSimPlayer(this)) return;
        PlayerObject::incrementJumps();
    }

    void update(float dt) {
        if (PlayLayer::get() && !g_engine.isSimulating()) {
            auto& core = sb::Core::get();
            g_engine.setFrameDelta(dt);
            g_engine.addClock(this, dt);
            if (g_engine.sbTick(this, core.p1.gameDown, core.p2.gameDown) && core.active)
                sb::fireDueWave(core, g_engine.sbNow());     // start of a tick, like the input buffer
        }
        PlayerObject::update(dt);
    }

    void ringJump(RingObject* p0, bool p1) {
        if (g_engine.isSimulating()) {
            if (!g_engine.isSimPlayer(this)) return;
            // the ghost bounces on the real orb: leave the real orb exactly as it was
            const bool a0 = p0->m_activated;
            const bool a1 = p0->m_activatedByPlayer1;
            const bool a2 = p0->m_activatedByPlayer2;
            const bool a3 = p0->m_isActivated;
            const bool a4 = p0->m_isRingPoweredOn;
            PlayerObject::ringJump(p0, p1);
            p0->m_activated = a0;
            p0->m_activatedByPlayer1 = a1;
            p0->m_activatedByPlayer2 = a2;
            p0->m_isActivated = a3;
            p0->m_isRingPoweredOn = a4;
            return;
        }
        PlayerObject::ringJump(p0, p1);
    }

    void flipGravity(bool flip, bool noEffects) {
        if (g_engine.isSimulating() && g_engine.isSimPlayer(this)) {
            PlayerObject::flipGravity(flip, true);          // ghost: never show the effect
            return;
        }
        PlayerObject::flipGravity(flip, noEffects);
    }

    void spawnPortalCircle(cocos2d::ccColor3B color, float startRadius) {
        if (g_engine.isSimulating()) return;
        PlayerObject::spawnPortalCircle(color, startRadius);
    }

    void playBumpEffect(int objectType, GameObject* object) {
        if (g_engine.isSimulating()) return;
        PlayerObject::playBumpEffect(objectType, object);
    }
};

// Orbs touched by a ghost player must not flash / pulse on screen.
class $modify(SBRingObject, RingObject) {
    void powerOnObject(int state) {
        if (g_engine.isSimulating()) return;
        RingObject::powerOnObject(state);
    }

    void spawnCircle() {
        if (g_engine.isSimulating()) return;
        RingObject::spawnCircle();
    }
};

// ---------------------------------------------------------------------------
// Small menu icon: pause menu and main menu only, never during play.
// ---------------------------------------------------------------------------
static CCMenuItemSpriteExtra* makeMenuIcon(CCObject* target, SEL_MenuHandler callback) {
    // small round icon
    auto* label = CCLabelBMFont::create("SB", "bigFont.fnt");
    label->setScale(0.55f);
    auto* sprite = CircleButtonSprite::create(label, CircleBaseColor::Gray, CircleBaseSize::Small);
    auto* item = CCMenuItemSpriteExtra::create(sprite, target, callback);
    item->setScale(0.6f);
    return item;
}

class $modify(SBPauseLayer, PauseLayer) {
    void onSpamBuffer(CCObject*) {
        if (auto* popup = SBConfigPopup::create()) popup->showWindow();
    }

    void customSetup() {
        PauseLayer::customSetup();
        auto* item = makeMenuIcon(this, menu_selector(SBPauseLayer::onSpamBuffer));
        if (auto* side = this->getChildByID("right-button-menu")) {
            side->addChild(item);
            side->updateLayout();
        } else {
            auto* menu = CCMenu::create();
            menu->setPosition({0.f, 0.f});
            item->setPosition({24.f, 24.f});
            menu->addChild(item);
            this->addChild(menu, 100);
        }
    }
};

class $modify(SBMenuLayer, MenuLayer) {
    void onSpamBuffer(CCObject*) {
        if (auto* popup = SBConfigPopup::create()) popup->showWindow();
    }

    bool init() {
        if (!MenuLayer::init()) return false;
        auto* item = makeMenuIcon(this, menu_selector(SBMenuLayer::onSpamBuffer));
        if (auto* bottom = this->getChildByID("bottom-menu")) {
            bottom->addChild(item);
            bottom->updateLayout();
        } else {
            auto* menu = CCMenu::create();
            menu->setPosition({0.f, 0.f});
            item->setPosition({24.f, 24.f});
            menu->addChild(item);
            this->addChild(menu, 100);
        }
        return true;
    }
};
