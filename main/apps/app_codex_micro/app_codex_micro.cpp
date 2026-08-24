/*
 * SPDX-FileCopyrightText: 2026 Codex Micro for StopWatch contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_codex_micro.h"

#include <algorithm>
#include <array>
#include <assets/assets.h>
#include <cmath>
#include <cstdio>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <services/codex_micro/codex_micro_service.h>

#if __has_include("model/local_chat_slots.h")
#include "model/local_chat_slots.h"
#else
#include "model/chat_slot_config.h"
#endif

namespace {

constexpr char kLeftMicKey[]                    = "ACT10";
constexpr char kRightCommandKey[]               = "ACT09";
constexpr char kSendKey[]                       = "ACT12";
constexpr std::array<const char*, 6> kAgentKeys = {
    "AG00", "AG01", "AG02", "AG03", "AG04", "AG05",
};
constexpr uint32_t kHostRpcLiveForMs = 300000;

bool deadlineReached(uint32_t now, uint32_t deadline)
{
    return static_cast<int32_t>(now - deadline) >= 0;
}

std::string formatResetTime(uint32_t seconds)
{
    char text[24] = {};
    if (seconds < 60) {
        std::snprintf(text, sizeof(text), "RESET <1M");
    } else if (seconds < 3600) {
        std::snprintf(text, sizeof(text), "RESET %luM", static_cast<unsigned long>(seconds / 60));
    } else if (seconds < 86400) {
        std::snprintf(text, sizeof(text), "RESET %luH %luM", static_cast<unsigned long>(seconds / 3600),
                      static_cast<unsigned long>((seconds % 3600) / 60));
    } else {
        std::snprintf(text, sizeof(text), "RESET %luD %luH", static_cast<unsigned long>(seconds / 86400),
                      static_cast<unsigned long>((seconds % 86400) / 3600));
    }
    return text;
}

std::string formatTokenCount(uint64_t tokens)
{
    if (tokens < 1000) {
        return std::to_string(tokens);
    }

    double divisor     = 1000.0;
    const char* suffix = "K";
    if (tokens >= 1000000000000ULL) {
        divisor = 1000000000000.0;
        suffix  = "T";
    } else if (tokens >= 1000000000ULL) {
        divisor = 1000000000.0;
        suffix  = "B";
    } else if (tokens >= 1000000ULL) {
        divisor = 1000000.0;
        suffix  = "M";
    }

    const double scaled = static_cast<double>(tokens) / divisor;
    char text[16]       = {};
    if (scaled < 100.0) {
        std::snprintf(text, sizeof(text), "%.1f%s", scaled, suffix);
    } else {
        std::snprintf(text, sizeof(text), "%.0f%s", scaled, suffix);
    }
    return text;
}

}  // namespace

AppCodexMicro::AppCodexMicro()
{
    setAppInfo().name = "Codex Micro";
    setAppInfo().icon = (void*)&icon_codex_micro;
}

void AppCodexMicro::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
    updateBattery(GetHAL().millis(), true);
}

void AppCodexMicro::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    releaseAllControls();
    _button_chord_active   = false;
    _button_chord_canceled = false;
    _key_manager           = std::make_unique<input::KeyManager>();
    _last_service_revision = UINT32_MAX;
    _last_view_update_at   = 0;
    _chat_slots            = codex_micro_app::model::composeChatSlots(codex_micro_app::model::config::kPinnedSlots,
                                                                      codex_micro_app::model::config::kRecentChats);
    if (_selected_chat_slot >= _chat_slots.size() || !_chat_slots[_selected_chat_slot].available) {
        _selected_chat_slot = codex_micro_app::model::firstAvailableChat(_chat_slots);
    }

    {
        LvglLockGuard lock;
        _view = std::make_unique<codex_micro_app::view::DashboardView>();
        if (!_view->init(lv_screen_active())) {
            mclog::tagError(getAppInfo().name, "dashboard init failed");
            _view.reset();
            close();
            return;
        }
    }
    updateView(GetHAL().millis(), true);
}

void AppCodexMicro::onRunning()
{
    const uint32_t now = GetHAL().millis();

    // The App owns both single-button actions and the shared factory Home
    // gesture, so update the hardware exactly once per Mooncake frame.
    GetHAL().updateButtonStates();
    const input::KeyEvent key_event = _key_manager ? _key_manager->update(false) : input::KeyEvent::None;

    const bool mode_changed = handlePhysicalButtons(now, key_event);
    if (currentState() != StateRunning) {
        return;
    }

    // Do not consume a release queued by the old visual mode in the same
    // frame. The new dashboard is already refreshed by toggleDashboardMode().
    if (mode_changed) {
        return;
    }

    processTouchIntents(now);
    updatePendingReleases(now);
    updateBattery(now);
    updateView(now);
}

void AppCodexMicro::onSleeping()
{
    // The service is intentionally process-scoped. Keeping its battery state
    // fresh while this UI sleeps avoids a disconnect/re-pair cycle every time
    // the user returns to the factory launcher.
    updateBattery(GetHAL().millis());
}

void AppCodexMicro::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    releaseAllControls();
    _button_chord_active   = false;
    _button_chord_canceled = false;
    _key_manager.reset();

    LvglLockGuard lock;
    _view.reset();
}

void AppCodexMicro::onDestroy()
{
    mclog::tagInfo(getAppInfo().name, "on destroy");
    releaseAllControls();
    _button_chord_active   = false;
    _button_chord_canceled = false;
    _key_manager.reset();

    if (_view) {
        LvglLockGuard lock;
        _view.reset();
    }
}

bool AppCodexMicro::handlePhysicalButtons(uint32_t now, input::KeyEvent keyEvent)
{
    auto& hal                      = GetHAL();
    const bool left_pressed_edge   = hal.btnA.wasPressed();
    const bool right_pressed_edge  = hal.btnB.wasPressed();
    const bool left_released_edge  = hal.btnA.wasReleased();
    const bool right_released_edge = hal.btnB.wasReleased();
    const bool both_pressed        = hal.btnA.isPressed() && hal.btnB.isPressed();

    // Batch both press edges before emitting a single-key action. Simultaneous
    // presses therefore never leak a short PTT or command pulse.
    if (!_button_chord_active && both_pressed &&
        (left_pressed_edge || right_pressed_edge || keyEvent == input::KeyEvent::GoHome)) {
        beginButtonChord();
    }

    if (_button_chord_active) {
        if ((left_released_edge || right_released_edge) && keyEvent != input::KeyEvent::GoHome) {
            _button_chord_canceled = true;
        }

        if (!_button_chord_canceled && keyEvent == input::KeyEvent::GoHome) {
            mclog::tagInfo(getAppInfo().name, "A+B hold: close to launcher");
            releaseAllControls();
            close();
            return false;
        }

        finishButtonChordIfReleased();
        return false;
    }

    if (right_pressed_edge) {
        cancelModeButtonGesture();
    }

    const auto mode_button_event =
        _mode_button_gesture.update(now, hal.btnA.isPressed(), left_pressed_edge, left_released_edge);
    switch (mode_button_event) {
        case codex_micro_app::model::ModeButtonEvent::PttPress:
            if (!_left_mic_pressed) {
                codex_micro::GetService().sendKey(kLeftMicKey, 1);
                _left_mic_pressed = true;
            }
            break;
        case codex_micro_app::model::ModeButtonEvent::PttRelease:
            if (_left_mic_pressed) {
                codex_micro::GetService().sendKey(kLeftMicKey, 0);
                _left_mic_pressed = false;
            }
            break;
        case codex_micro_app::model::ModeButtonEvent::ToggleMode:
            toggleDashboardMode(now);
            return true;
        case codex_micro_app::model::ModeButtonEvent::None:
            break;
    }

    if (right_pressed_edge) {
        _right_press_observed = true;
        _right_press_at       = now;
    }

    // ACT09 is a deterministic click pulse on release. A release inherited
    // from the launcher or a previous foreground session is ignored. Requiring
    // a short physical hold also rejects single-frame GPIO/IO-expander noise.
    if (right_released_edge && _right_press_observed) {
        const bool valid_press = now - _right_press_at >= RightMinimumPressMs;
        _right_press_observed  = false;
        _right_press_at        = 0;
        if (valid_press) {
            beginRightCommandPulse(now);
        }
    }
    return false;
}

void AppCodexMicro::beginButtonChord()
{
    _button_chord_active   = true;
    _button_chord_canceled = false;
    releaseAllControls();
}

void AppCodexMicro::finishButtonChordIfReleased()
{
    if (!GetHAL().btnA.isReleased() || !GetHAL().btnB.isReleased()) {
        return;
    }
    _button_chord_active   = false;
    _button_chord_canceled = false;
}

void AppCodexMicro::cancelModeButtonGesture()
{
    const auto event = _mode_button_gesture.cancel();
    if (event == codex_micro_app::model::ModeButtonEvent::PttRelease && _left_mic_pressed) {
        codex_micro::GetService().sendKey(kLeftMicKey, 0);
        _left_mic_pressed = false;
    }
}

void AppCodexMicro::toggleDashboardMode(uint32_t now)
{
    releaseAllControls();
    {
        LvglLockGuard lock;
        if (_view) {
            // Clear all completed old-mode intents while the LVGL task is
            // paused. If a finger is currently down, discard the remainder of
            // that touch sequence before accepting input for the new mode.
            auto* touchpad          = GetHAL().lvTouchpad;
            const bool touch_active = touchpad != nullptr && lv_indev_get_state(touchpad) == LV_INDEV_STATE_PRESSED;
            _view->beginModeTransition(touch_active);
        }
    }
    _dashboard_mode        = codex_micro_app::model::toggledMode(_dashboard_mode);
    _last_service_revision = UINT32_MAX;
    _last_view_update_at   = 0;
    mclog::tagInfo(getAppInfo().name, "physical A double tap: {} mode",
                   _dashboard_mode == codex_micro_app::model::DashboardMode::Chat ? "Chat" : "Codex");
    GetHAL().vibrate(42, 80);
    updateView(now, true);
}

void AppCodexMicro::selectChatSlot(int index, uint32_t now)
{
    if (index < 0 || index >= static_cast<int>(_chat_slots.size()) || !_chat_slots[index].available) {
        return;
    }
    _selected_chat_slot  = static_cast<size_t>(index);
    _last_view_update_at = 0;
    mclog::tagInfo(getAppInfo().name, "Chat slot {} selected locally", index + 1);
    updateView(now, true);
}

void AppCodexMicro::processTouchIntents(uint32_t now)
{
    if (!_view) {
        return;
    }

    codex_micro_app::view::TouchIntent intent;
    while (_view->popIntent(intent)) {
        // A touch invalidates only a pending mode toggle. It must not release
        // an active PTT session while the user is still holding physical A.
        _mode_button_gesture.cancelPendingToggle();
        switch (intent.type) {
            case codex_micro_app::view::TouchIntentType::AgentPress:
                if (intent.agent < 0) {
                    releaseAllControls();
                } else {
                    _touch_gesture_consumed = false;
                    _touch_send_candidate   = false;
                    _touch_agent_candidate  = intent.agent;
                }
                break;
            case codex_micro_app::view::TouchIntentType::AgentRelease:
                if (intent.agent < 0) {
                    releaseAllControls();
                } else if (!_touch_gesture_consumed && _touch_agent_candidate == intent.agent) {
                    if (_dashboard_mode == codex_micro_app::model::DashboardMode::Chat) {
                        selectChatSlot(intent.agent, now);
                    } else {
                        beginAgentPulse(intent.agent, now);
                    }
                    GetHAL().vibrate(28, 55);
                }
                _touch_agent_candidate = -1;
                break;
            case codex_micro_app::view::TouchIntentType::SendPress:
                _touch_gesture_consumed = false;
                _touch_agent_candidate  = -1;
                _touch_send_candidate   = true;
                break;
            case codex_micro_app::view::TouchIntentType::SendRelease:
                if (!_touch_gesture_consumed && _touch_send_candidate) {
                    // Until the host acknowledges a concrete Chat target,
                    // ACT12 could send into an unrelated composer. Keep the
                    // center control local in Chat mode and preserve the
                    // original Codex behavior byte-for-byte.
                    if (_dashboard_mode == codex_micro_app::model::DashboardMode::Codex) {
                        beginSendPulse(now);
                    }
                }
                _touch_send_candidate = false;
                break;
            case codex_micro_app::view::TouchIntentType::TouchCancel:
                _touch_agent_candidate = -1;
                _touch_send_candidate  = false;
                break;
            case codex_micro_app::view::TouchIntentType::AnalogPulse:
                _touch_gesture_consumed = true;
                _touch_agent_candidate  = -1;
                _touch_send_candidate   = false;
                if (_dashboard_mode == codex_micro_app::model::DashboardMode::Codex) {
                    beginAnalogPulse(intent.angle, now);
                    GetHAL().vibrate(28, 50);
                }
                break;
        }
    }
}

void AppCodexMicro::updatePendingReleases(uint32_t now)
{
    for (size_t i = 0; i < _agent_pressed.size(); ++i) {
        if (_agent_pressed[i] && deadlineReached(now, _agent_release_at[i])) {
            sendAgentKey(static_cast<int>(i), false);
            _agent_release_at[i] = 0;
        }
    }
    if (_send_pressed && deadlineReached(now, _send_release_at)) {
        sendSendKey(false);
        _send_release_at = 0;
    }
    if (_right_command_pressed && deadlineReached(now, _right_release_at)) {
        codex_micro::GetService().sendKey(kRightCommandKey, 0);
        _right_command_pressed = false;
    }
    if (_analog_pressed && deadlineReached(now, _analog_release_at)) {
        codex_micro::GetService().sendJoystick(_analog_angle, 0.0f);
        _analog_pressed = false;
    }
}

void AppCodexMicro::updateBattery(uint32_t now, bool force)
{
    if (!force && now - _last_battery_at < BatteryUpdateIntervalMs) {
        return;
    }

    const uint8_t level = GetHAL().getBatteryLevel();
    const bool charging = GetHAL().isBatteryCharging();

    _battery_level    = level;
    _battery_charging = charging;
    _last_battery_at  = now;
    codex_micro::GetService().setBattery(level, charging);
}

void AppCodexMicro::updateView(uint32_t now, bool force)
{
    if (!_view) {
        return;
    }

    const auto state = codex_micro::GetService().snapshot();
    if (!force && state.revision == _last_service_revision && now - _last_view_update_at < ViewUpdateIntervalMs) {
        return;
    }

    codex_micro_app::view::DashboardModel model;
    model.mode           = _dashboard_mode;
    const bool host_live = state.connected && state.hostRpcObserved && now - state.lastHostRpcAtMs <= kHostRpcLiveForMs;
    if (_dashboard_mode == codex_micro_app::model::DashboardMode::Chat) {
        model.connectionText  = "CHAT";
        model.connectionColor = 0xB9A9E8;
    } else if (!state.connected) {
        model.connectionText  = "OFFLINE";
        model.connectionColor = 0xFF7685;
    } else if (host_live) {
        model.connectionText  = "CODEX LIVE";
        model.connectionColor = 0x77E6A5;
    } else {
        model.connectionText  = "BLE LINK";
        model.connectionColor = 0xF0C978;
    }

    char battery_text[20] = {};
    std::snprintf(battery_text, sizeof(battery_text), "BAT %u%%%s", static_cast<unsigned>(_battery_level),
                  _battery_charging ? " +" : "");
    model.batteryText = battery_text;

    for (size_t i = 0; i < model.agents.size(); ++i) {
        model.agents[i].color      = state.threads[i].color;
        model.agents[i].brightness = state.threads[i].brightness;
        model.agents[i].focused    = state.threads[i].effect == "breath" || state.threads[i].speed > 0.01f;
    }

    for (size_t i = 0; i < model.chats.size(); ++i) {
        const auto& slot         = _chat_slots[i];
        model.chats[i].available = slot.available;
        model.chats[i].pinned    = slot.pinned;
        model.chats[i].selected  = slot.available && i == _selected_chat_slot;
        if (slot.available) {
            model.chats[i].alias   = std::string(slot.chat.alias);
            model.chats[i].project = std::string(slot.chat.project);
            model.chats[i].title   = std::string(slot.chat.title);
        }
    }

    model.quotaAvailable = state.quota.available;
    if (state.quota.available) {
        const int percent      = static_cast<int>(std::lround(std::clamp(state.quota.remainingPercent, 0.0f, 100.0f)));
        model.quotaText        = std::to_string(percent) + "%";
        const uint32_t elapsed = (now - state.quota.receivedAtMs) / 1000;
        const uint32_t remaining = elapsed >= state.quota.resetInSeconds ? 0 : state.quota.resetInSeconds - elapsed;
        model.resetText          = formatResetTime(remaining);
    } else {
        model.quotaText = "--";
        model.resetText = "QUOTA WAITING";
    }

    model.weeklyUsageAvailable   = state.usage.weeklyAvailable;
    model.lifetimeUsageAvailable = state.usage.lifetimeAvailable;
    model.weeklyUsageText        = state.usage.weeklyAvailable ? formatTokenCount(state.usage.weeklyTokens) : "--";
    model.lifetimeUsageText      = state.usage.lifetimeAvailable ? formatTokenCount(state.usage.lifetimeTokens) : "--";

    {
        LvglLockGuard lock;
        if (_view) {
            _view->update(model);
        }
    }

    _last_service_revision = state.revision;
    _last_view_update_at   = now;
}

void AppCodexMicro::sendAgentKey(int index, bool pressed)
{
    if (index < 0 || index >= static_cast<int>(_agent_pressed.size())) {
        return;
    }
    if (_agent_pressed[index] == pressed) {
        return;
    }

    codex_micro::GetService().sendKey(kAgentKeys[index], pressed ? 1 : 0, static_cast<int8_t>(index));
    _agent_pressed[index] = pressed;
}

void AppCodexMicro::sendSendKey(bool pressed)
{
    if (_send_pressed == pressed) {
        return;
    }
    codex_micro::GetService().sendKey(kSendKey, pressed ? 1 : 0);
    _send_pressed = pressed;
}

void AppCodexMicro::beginAgentPulse(int index, uint32_t now)
{
    if (index < 0 || index >= static_cast<int>(_agent_pressed.size())) {
        return;
    }
    if (_agent_pressed[index]) {
        sendAgentKey(index, false);
    }
    sendAgentKey(index, true);
    _agent_release_at[index] = now + KeyPulseDurationMs;
}

void AppCodexMicro::beginSendPulse(uint32_t now)
{
    if (_send_pressed) {
        sendSendKey(false);
    }
    mclog::tagInfo(getAppInfo().name, "center tap: ACT12 send");
    sendSendKey(true);
    _send_release_at = now + KeyPulseDurationMs;
}

void AppCodexMicro::beginRightCommandPulse(uint32_t now)
{
    if (_right_command_pressed) {
        codex_micro::GetService().sendKey(kRightCommandKey, 0);
    }
    mclog::tagInfo(getAppInfo().name, "physical B: ACT09 command");
    codex_micro::GetService().sendKey(kRightCommandKey, 1);
    _right_command_pressed = true;
    _right_release_at      = now + KeyPulseDurationMs;
}

void AppCodexMicro::beginAnalogPulse(float angle, uint32_t now)
{
    if (_analog_pressed) {
        codex_micro::GetService().sendJoystick(_analog_angle, 0.0f);
    }
    _analog_angle = angle;
    codex_micro::GetService().sendJoystick(_analog_angle, 1.0f);
    _analog_pressed    = true;
    _analog_release_at = now + KeyPulseDurationMs;
}

void AppCodexMicro::releaseAllControls()
{
    auto& service = codex_micro::GetService();
    for (size_t i = 0; i < _agent_pressed.size(); ++i) {
        if (_agent_pressed[i]) {
            service.sendKey(kAgentKeys[i], 0, static_cast<int8_t>(i));
            _agent_pressed[i] = false;
        }
        _agent_release_at[i] = 0;
    }
    if (_send_pressed) {
        service.sendKey(kSendKey, 0);
        _send_pressed = false;
    }
    if (_left_mic_pressed) {
        service.sendKey(kLeftMicKey, 0);
        _left_mic_pressed = false;
    }
    if (_right_command_pressed) {
        service.sendKey(kRightCommandKey, 0);
        _right_command_pressed = false;
    }
    _right_press_observed = false;
    _right_press_at       = 0;
    if (_analog_pressed) {
        service.sendJoystick(_analog_angle, 0.0f);
        _analog_pressed = false;
    }
    _right_release_at       = 0;
    _send_release_at        = 0;
    _analog_release_at      = 0;
    _touch_agent_candidate  = -1;
    _touch_send_candidate   = false;
    _touch_gesture_consumed = false;
    _mode_button_gesture.reset();
    GetHAL().stopVibrate();
}
