// Frame Macro (GD 2.2081, Geode v5)
// Records each input with the physics tick it was applied on, replays it tick-for-tick,
// and draws a live frame counter plus a click-gap histogram.
#include <Geode/Geode.hpp>
#include <Geode/loader/GameEvent.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "macro.hpp"

using namespace geode::prelude;

namespace {

enum class FMMode { Idle, Record, Replay };

bool fmPressesOnly() { return Mod::get()->getSettingValue<bool>("presses-only"); }

struct FMState {
    FMMode mode = FMMode::Idle;
    fm::Macro macro;
    fm::GapStats stats;
    uint32_t tick = 0;          // full physics ticks completed this attempt = the "frame" counter
    size_t next = 0;            // replay cursor into macro.inputs
    bool injecting = false;     // our own handleButton call is in flight
    bool inTick = false;        // inside the game's processCommands
    bool resetInTick = false;   // a reset happened inside that call, so don't count it
    bool forcedTest = false;    // safe mode flipped m_isTestMode, restored on the next reset
    bool prevTest = false;

    // Called whenever an attempt (re)starts. startTick is 0, or the checkpoint's tick in practice mode.
    void restart(uint32_t startTick) {
        tick = startTick;
        if (mode == FMMode::Record) macro.truncateFrom(startTick);
        next = mode == FMMode::Idle ? 0 : macro.lowerBound(startTick);
        stats = mode == FMMode::Idle ? fm::GapStats{} : fm::GapStats::build(macro.inputs, next, fmPressesOnly());
    }
} fmState;

// ---- save / load: one macro per level in <mod save dir>/macros ----

std::filesystem::path fmMacroPath(GJGameLevel* level) {
    std::string name = level->m_levelName;
    for (auto& c : name)
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    return Mod::get()->getSaveDir() / "macros" / fmt::format("{}_{}.fmac", level->m_levelID.value(), name);
}

void fmSaveMacro(GJGameLevel* level) {
    if (!level || fmState.macro.inputs.empty()) return;
    auto path = fmMacroPath(level);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out << fmState.macro.serialize();
    out.close();
    bool ok = !out.fail();
    Notification::create(
        ok ? fmt::format("Macro saved: {} inputs", fmState.macro.inputs.size()) : std::string("Macro save failed"),
        ok ? NotificationIcon::Success : NotificationIcon::Error
    )->show();
}

bool fmLoadMacro(GJGameLevel* level) {
    std::ifstream in(fmMacroPath(level), std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    fm::Macro m;
    if (!fm::Macro::parse(ss.str(), m)) return false;
    fmState.macro = std::move(m);
    return true;
}

// ---- controls (shared by the pause menu and the keybinds) ----

bool fmBeginRecord() {
    if (!PlayLayer::get()) return false;
    fmState.macro = {};
    fmState.mode = FMMode::Record;
    return true;
}

bool fmBeginReplay() {
    auto pl = PlayLayer::get();
    if (!pl) return false;
    if (fmState.macro.inputs.empty() && !fmLoadMacro(pl->m_level)) {
        FLAlertLayer::create("Frame Macro", "No saved macro for this level. Record one first.", "OK")->show();
        return false;
    }
    fmState.mode = FMMode::Replay;
    return true;
}

void fmEndMacro() {
    if (auto pl = PlayLayer::get(); pl && fmState.mode == FMMode::Record) fmSaveMacro(pl->m_level);
    fmState.mode = FMMode::Idle;
}

// ---- overlay: frame counter + click-gap histogram (top row = longest gaps, like the screenshot) ----

class FMOverlay : public CCNode {
    CCLabelBMFont* m_head = nullptr;
    CCNode* m_rows = nullptr;
    CCLabelBMFont* m_vals[fm::kBuckets] = {};
    uint32_t m_shown[fm::kBuckets] = {};
    std::string m_headText;

    bool init() override {
        if (!CCNode::init()) return false;
        this->setPosition({6.f, CCDirector::get()->getWinSize().height / 2});

        auto make = [](CCNode* parent, char const* text, float x, float y, ccColor3B color) {
            auto label = CCLabelBMFont::create(text, "bigFont.fnt");
            label->setScale(.4f);
            label->setAnchorPoint({0.f, .5f});
            label->setPosition({x, y});
            label->setColor(color);
            parent->addChild(label);
            return label;
        };

        // index 0 = 1-frame gaps ... index 8 = 17+ frames
        ccColor3B const colors[fm::kBuckets] = {
            {255, 80, 80}, {255, 150, 50}, {255, 230, 60}, {255, 255, 255}, {90, 255, 90},
            {80, 230, 255}, {90, 130, 255}, {170, 110, 255}, {255, 90, 220},
        };

        m_head = make(this, "Frame: 0", 0.f, 105.f, {255, 255, 255});
        m_rows = CCNode::create();
        this->addChild(m_rows);
        for (int i = 0; i < fm::kBuckets; ++i) {
            float y = 85.f - (fm::kBuckets - 1 - i) * 17.f;
            make(m_rows, fmt::format("{}:", fm::kBucketNames[i]).c_str(), 0.f, y, {255, 255, 255});
            m_vals[i] = make(m_rows, "0", 62.f, y, colors[i]);
        }

        this->scheduleUpdate();
        return true;
    }

    void update(float) override {
        bool on = Mod::get()->getSettingValue<bool>("show-overlay");
        this->setVisible(on);
        if (!on) return;

        std::string head;
        switch (fmState.mode) {
            case FMMode::Record: head = fmt::format("REC  Frame: {}", fmState.tick); break;
            case FMMode::Replay: head = fmt::format("PLAY  Frame: {} / {}", fmState.tick, fmState.macro.length()); break;
            default: head = fmt::format("Frame: {}", fmState.tick); break;
        }
        if (head != m_headText) {
            m_headText = head;
            m_head->setString(head.c_str());
        }

        bool gaps = fmState.mode != FMMode::Idle && Mod::get()->getSettingValue<bool>("show-gap-stats");
        m_rows->setVisible(gaps);
        if (!gaps) return;
        for (int i = 0; i < fm::kBuckets; ++i) {
            if (fmState.stats.n[i] == m_shown[i]) continue;
            m_shown[i] = fmState.stats.n[i];
            m_vals[i]->setString(std::to_string(m_shown[i]).c_str());
        }
    }

public:
    static FMOverlay* create() {
        auto ret = new FMOverlay();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

bool fmIsPlay(GJBaseGameLayer* layer) { return layer && layer == PlayLayer::get(); }

}  // namespace

// ---- hooks ----

class $modify(FMGameLayer, GJBaseGameLayer) {
    // Apply every recorded input that is due on the tick about to run.
    void fmInject() {
        bool po = fmPressesOnly();
        fmState.injecting = true;
        while (fmState.next < fmState.macro.inputs.size() && fmState.macro.inputs[fmState.next].frame <= fmState.tick) {
            auto in = fmState.macro.inputs[fmState.next++];
            if (!po || in.down) fmState.stats.add(in.frame);
            this->handleButton(in.down, in.button, in.isPlayer1);
        }
        fmState.injecting = false;
    }

    // Runs once per physics tick; half-tick calls are the game splitting a tick, so they don't count.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!fmIsPlay(this) || !m_player1 || m_player1->m_isDead)
            return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (!isHalfTick && fmState.mode == FMMode::Replay) this->fmInject();

        fmState.inTick = true;
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        fmState.inTick = false;

        if (isHalfTick) return;
        if (fmState.resetInTick) fmState.resetInTick = false;
        else ++fmState.tick;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (fmState.mode == FMMode::Idle || !fmIsPlay(this))
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (fmState.mode == FMMode::Replay) {
            if (fmState.injecting) GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;  // real input is ignored while a macro plays
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (m_player1 && !m_player1->m_isDead) {
            fmState.macro.inputs.push_back({fmState.tick, static_cast<uint8_t>(button), down, isPlayer1});
            if (!fmPressesOnly() || down) fmState.stats.add(fmState.tick);
        }
    }
};

class $modify(FMPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        fmState = FMState{};
        if (auto overlay = FMOverlay::create()) this->addChild(overlay, 1000);
    }

    void resetLevel() {
        if (fmState.forcedTest) {
            m_isTestMode = fmState.prevTest;
            fmState.forcedTest = false;
        }
        PlayLayer::resetLevel();
        // Level time is 0 after a normal restart and the checkpoint's time after a practice respawn.
        fmState.restart(static_cast<uint32_t>(std::max(0.0, std::round(m_gameState.m_levelTime * fm::kTps))));
        fmState.resetInTick = fmState.inTick;
    }

    void levelComplete() {
        // Same trick other bots use: a test-mode run is never saved or submitted. Held until the next reset
        // because parts of the completion flow run after levelComplete() returns.
        if (fmState.mode == FMMode::Replay && !fmState.forcedTest && Mod::get()->getSettingValue<bool>("safe-mode")) {
            fmState.prevTest = m_isTestMode;
            fmState.forcedTest = true;
            m_isTestMode = true;
        }
        PlayLayer::levelComplete();
        if (fmState.mode == FMMode::Record) fmSaveMacro(m_level);
    }
};

class $modify(FMPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        float midY = CCDirector::get()->getWinSize().height / 2;

        auto add = [&](char const* text, SEL_MenuHandler callback, float y) {
            auto sprite = ButtonSprite::create(text, "goldFont.fnt", "GJ_button_01.png", .8f);
            auto button = CCMenuItemSpriteExtra::create(sprite, this, callback);
            button->setPosition({52.f, y});
            menu->addChild(button);
        };
        add("Rec", menu_selector(FMPauseLayer::onFMRecord), midY + 36.f);
        add("Play", menu_selector(FMPauseLayer::onFMReplay), midY);
        add("Stop", menu_selector(FMPauseLayer::onFMStop), midY - 36.f);
        this->addChild(menu);
    }

    // Restarting from the start (the stock "restart full" button) also closes the pause menu.
    void onFMRecord(CCObject* sender) { if (fmBeginRecord()) this->onRestartFull(sender); }
    void onFMReplay(CCObject* sender) { if (fmBeginReplay()) this->onRestartFull(sender); }
    void onFMStop(CCObject*) { fmEndMacro(); }
};

$on_game(Loaded) {
    // Keybinds are in the mod settings (defaults F6 / F7 / F8) and only act while a level is running.
    listenForKeybindSettingPresses("key-record", [](Keybind const&, bool down, bool repeat, double) {
        auto pl = PlayLayer::get();
        if (down && !repeat && pl && !pl->m_isPaused && fmBeginRecord()) pl->resetLevelFromStart();
    });
    listenForKeybindSettingPresses("key-replay", [](Keybind const&, bool down, bool repeat, double) {
        auto pl = PlayLayer::get();
        if (down && !repeat && pl && !pl->m_isPaused && fmBeginReplay()) pl->resetLevelFromStart();
    });
    listenForKeybindSettingPresses("key-stop", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) fmEndMacro();
    });
}
