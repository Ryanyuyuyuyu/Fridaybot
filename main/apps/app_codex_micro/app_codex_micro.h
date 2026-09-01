/*
 * SPDX-FileCopyrightText: 2026 Codex Micro for StopWatch contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "model/chat_mode_model.h"
#include "model/mode_button_gesture.h"
#include "view/view.h"

#include <apps/common/key_manager/key_manager.h>
#include <array>
#include <cstdint>
#include <memory>
#include <mooncake.h>

class AppCodexMicro : public mooncake::AppAbility {
public:
    AppCodexMicro();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onSleeping() override;
    void onClose() override;
    void onDestroy() override;

private:
    static constexpr uint32_t KeyPulseDurationMs      = 70;
    static constexpr uint32_t RightMinimumPressMs     = 40;
    static constexpr uint32_t BatteryUpdateIntervalMs = 30000;
    static constexpr uint32_t ViewUpdateIntervalMs    = 1000;

    bool handlePhysicalButtons(uint32_t now, input::KeyEvent keyEvent);
    void beginButtonChord();
    void finishButtonChordIfReleased();
    void cancelModeButtonGesture();
    void toggleDashboardMode(uint32_t now);
    void selectChatSlot(int index, uint32_t now);
    void processTouchIntents(uint32_t now);
    void updatePendingReleases(uint32_t now);
    void updateBattery(uint32_t now, bool force = false);
    void updateView(uint32_t now, bool force = false);
    void sendAgentKey(int index, bool pressed);
    void sendSendKey(bool pressed);
    void beginAgentPulse(int index, uint32_t now);
    void beginSendPulse(uint32_t now);
    void beginRightCommandPulse(uint32_t now);
    void beginAnalogPulse(float angle, uint32_t now);
    void releaseAllControls();

    std::unique_ptr<input::KeyManager> _key_manager;
    std::unique_ptr<codex_micro_app::view::DashboardView> _view;

    codex_micro_app::model::ModeButtonGesture _mode_button_gesture;
    codex_micro_app::model::DashboardMode _dashboard_mode = codex_micro_app::model::DashboardMode::Codex;
    codex_micro_app::model::ChatSlots _chat_slots         = {};
    size_t _selected_chat_slot                            = 0;

    std::array<bool, 6> _agent_pressed        = {};
    std::array<uint32_t, 6> _agent_release_at = {};
    bool _send_pressed                        = false;
    bool _left_mic_pressed                    = false;
    bool _right_press_observed                = false;
    bool _right_command_pressed               = false;
    bool _analog_pressed                      = false;
    float _analog_angle                       = 0.0f;
    uint32_t _right_release_at                = 0;
    uint32_t _right_press_at                  = 0;
    uint32_t _send_release_at                 = 0;
    uint32_t _analog_release_at               = 0;

    int8_t _touch_agent_candidate = -1;
    bool _touch_send_candidate    = false;
    bool _touch_gesture_consumed  = false;

    bool _button_chord_active   = false;
    bool _button_chord_canceled = false;

    uint8_t _battery_level          = 0;
    bool _battery_charging          = false;
    uint32_t _last_battery_at       = 0;
    uint32_t _last_view_update_at   = 0;
    uint32_t _last_service_revision = UINT32_MAX;
};
