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

    handlePhysicalButtons(now, key_event);
    if (currentState() != StateRunning) {
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

void AppCodexMicro::handlePhysicalButtons(uint32_t now, input::KeyEvent keyEvent)
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
            return;
        }

        finishButtonChordIfReleased();
        return;
    }

    if (left_pressed_edge && !_left_mic_pressed) {
        codex_micro::GetService().sendKey(kLeftMicKey, 1);
        _left_mic_pressed = true;
    }
    if (left_released_edge && _left_mic_pressed) {
        codex_micro::GetService().sendKey(kLeftMicKey, 0);
        _left_mic_pressed = false;
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

void AppCodexMicro::processTouchIntents(uint32_t now)
{
    if (!_view) {
        return;
    }

    codex_micro_app::view::TouchIntent intent;
    while (_view->popIntent(intent)) {
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
                    beginAgentPulse(intent.agent, now);
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
                    beginSendPulse(now);
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
                beginAnalogPulse(intent.angle, now);
                GetHAL().vibrate(28, 50);
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
    const bool host_live = state.connected && state.hostRpcObserved && now - state.lastHostRpcAtMs <= kHostRpcLiveForMs;
    if (!state.connected) {
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

    // Missing schema-v3 windows remain visually unknown. The legacy quota
    // snapshot is retained for protocol compatibility but is never relabelled.
    model.fiveHourLimitAvailable = state.rateLimits.fiveHourAvailable;
    model.weeklyLimitAvailable   = state.rateLimits.weeklyAvailable;
    model.fiveHourUsedPercent    = state.rateLimits.fiveHourUsedPercent;
    model.weeklyUsedPercent      = state.rateLimits.weeklyUsedPercent;

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
    GetHAL().stopVibrate();
}
