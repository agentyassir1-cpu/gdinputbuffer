#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <algorithm>
#include <deque>

using namespace geode::prelude;

namespace gdinput {
    struct Stage {
        int bufferTicks;
        int waveTicks;
    };

    // At 240 physics ticks per second, 32 ticks is about 133 ms.
    // These are input-buffer windows, not artificial input delays.
    constexpr Stage STAGES[] = {
        {4, 0},
        {8, 0},
        {16, 0},
        {32, 0}
    };
    constexpr int OFF = 4;

    // The stage lives in a Geode setting ("buffer-stage", 1..5) so it can be
    // changed from the mod settings on devices without an F6 key (Android).
    // Setting value 1..4 = stages 0..3, 5 = OFF.
    inline int currentStage() {
        auto value = Mod::get()->getSettingValue<int64_t>("buffer-stage");
        return static_cast<int>(std::clamp<int64_t>(value - 1, 0, OFF));
    }

    inline bool verbose() {
        return Mod::get()->getSettingValue<bool>("verbose-log");
    }

    inline void showStage(int stage) {
        if (stage == OFF) {
            log::info("[GDInput] Jump buffer OFF");
            Notification::create("GDInput  |  OFF", NotificationIcon::Info)->show();
            return;
        }

        const auto s = STAGES[stage];
        const char* names[] = { "NOOB", "PRO", "BEST MODE", "BEST MODE +" };
        const char* descriptions[] = {
            "4-tick jump buffer",
            "8-tick jump buffer",
            "16-tick jump buffer",
            "32-tick jump buffer"
        };

        log::info("[GDInput] {}: {}-tick jump buffer", names[stage], s.bufferTicks);
        Notification::create(
            fmt::format("GDInput  |  {}\n{}", names[stage], descriptions[stage]),
            NotificationIcon::Success
        )->show();
    }
}

// Geometry Dash's jump button ID is 1.
constexpr int GDINPUT_JUMP_BUTTON = 1;

struct GDInputPendingJump {
    bool player1;
    int remaining;
    bool released;
};

class $modify(GDInputBaseLayer, GJBaseGameLayer) {
    struct Fields {
        std::deque<GDInputPendingJump> pendingJumps;
        bool applying = false;
    };

    void handleButton(bool down, int button, bool isPlayer1) {
        auto* fields = m_fields.self();
        const int stage = gdinput::currentStage();

        if (fields->applying || stage == gdinput::OFF ||
            button != GDINPUT_JUMP_BUTTON) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        auto* player = isPlayer1 ? m_player1 : m_player2;
        if (!player) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // If a jump press is already waiting for landing, remember the release
        // but don't send a release for a press the game has not received yet.
        auto pending = fields->pendingJumps.end();
        for (auto it = fields->pendingJumps.begin(); it != fields->pendingJumps.end(); ++it) {
            if (it->player1 == isPlayer1) {
                pending = it;
                break;
            }
        }

        if (!down && pending != fields->pendingJumps.end()) {
            pending->released = true;
            if (gdinput::verbose()) {
                log::info("[GDInput] Remembered release for buffered jump");
            }
            return;
        }

        // Only buffer an early jump press. Other buttons, including orbs and
        // directional inputs, pass through immediately and keep their timing.
        if (down && !player->m_isDart && !player->m_isOnGround) {
            const int window = gdinput::STAGES[stage].bufferTicks;
            if (pending != fields->pendingJumps.end()) {
                pending->remaining = window;
                pending->released = false;
            } else {
                fields->pendingJumps.push_back({isPlayer1, window, false});
            }

            if (gdinput::verbose()) {
                log::info("[GDInput] Buffered early jump for {} ticks", window);
            }
            return;
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto* fields = m_fields.self();

        for (auto it = fields->pendingJumps.begin(); it != fields->pendingJumps.end();) {
            auto* player = it->player1 ? m_player1 : m_player2;

            if (!player) {
                it = fields->pendingJumps.erase(it);
                continue;
            }

            // Execute the saved press as soon as the player reaches the ground.
            if (player->m_isOnGround && !player->m_isDart) {
                const bool wasReleased = it->released;
                const bool player1 = it->player1;

                fields->applying = true;
                GJBaseGameLayer::handleButton(true, GDINPUT_JUMP_BUTTON, player1);
                if (wasReleased) {
                    GJBaseGameLayer::handleButton(false, GDINPUT_JUMP_BUTTON, player1);
                }
                fields->applying = false;

                if (gdinput::verbose()) {
                    log::info("[GDInput] Applied buffered jump on landing");
                }
                it = fields->pendingJumps.erase(it);
                continue;
            }

            --it->remaining;
            if (it->remaining <= 0) {
                if (gdinput::verbose()) {
                    log::info("[GDInput] Buffered jump expired");
                }
                it = fields->pendingJumps.erase(it);
            } else {
                ++it;
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(GDInputPlayLayer, PlayLayer) {
    void resetLevel() {
        auto* base = static_cast<GDInputBaseLayer*>(
            static_cast<GJBaseGameLayer*>(this)
        );
        base->m_fields->pendingJumps.clear();
        PlayLayer::resetLevel();
    }
};

class $modify(GDInputKeyboard, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        if (down && !repeat && key == KEY_F6) {
            const int next = (gdinput::currentStage() + 1) % 5;
            Mod::get()->setSettingValue<int64_t>("buffer-stage", next + 1);
            gdinput::showStage(next);
            return true;
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};

$on_mod(Loaded) {
    log::info("[GDInput] Loaded. F6 or the 'buffer-stage' setting cycles jump buffer windows: 4 -> 8 -> 16 -> 32 ticks -> OFF.");
}
