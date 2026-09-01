/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "face_model.h"
#include "presence_protocol.h"
#include "view/face_view.h"

#include <apps/common/key_manager/key_manager.h>
#include <atomic>
#include <memory>
#include <mooncake.h>

class AppFriday : public mooncake::AppAbility {
public:
    AppFriday();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum class PresenceState : uint8_t {
        Device,
        Departing,
        Host,
        AwaitingReturn,
        Returning,
    };

    static constexpr uint32_t AnimationIntervalMs = 16;  // 62.5 Hz target
    static constexpr uint32_t ImuSampleIntervalMs = 30;  // 33 Hz; spring interpolation stays at 62.5 Hz

    std::unique_ptr<input::KeyManager> _key_manager;
    std::unique_ptr<friday::view::FaceView> _view;
    friday::FaceModel _face_model;
    friday::ImuSample _imu_sample;
    std::atomic<bool> _touch_active{false};
    std::atomic<int32_t> _touch_x_milli{0};
    std::atomic<int32_t> _touch_y_milli{0};
    std::atomic<int32_t> _gesture_x_milli{0};
    std::atomic<int32_t> _gesture_y_milli{0};
    std::atomic<int32_t> _gesture_dx_milli{0};
    std::atomic<int32_t> _gesture_dy_milli{0};
    std::atomic<uint8_t> _gesture_pending{0};
    uint32_t _next_animation_ms = 0;
    uint32_t _last_imu_ms       = 0;
    uint32_t _stats_start_ms    = 0;
    uint32_t _rendered_frames   = 0;
    uint32_t _late_frames       = 0;
    uint8_t _last_context_sequence = 0;
    bool _context_was_fresh        = false;
    PresenceState _presence_state  = PresenceState::Device;
    friday::presence::Direction _transfer_direction = friday::presence::Direction::Right;
    uint32_t _transfer_started_ms  = 0;
    uint32_t _transfer_deadline_ms = 0;
    uint32_t _last_transfer_generation = 0;
    uint8_t _transfer_sequence     = 0;
    uint8_t _active_transfer_sequence = 0;
    uint16_t _transfer_seed        = 0;
    bool _host_accepted            = false;
    bool _departure_announced      = false;
    bool _face_visible             = true;

    void updateImu(uint32_t nowMs);
    void updateCompanionContext(uint32_t nowMs);
    void updatePresenceTransfer(uint32_t nowMs);
    bool tryStartPresenceTransfer(float x, float y, uint32_t nowMs);
    void recallFromHost(uint32_t nowMs);
    void beginPresenceReturn(uint32_t nowMs);
    void handleInputs(uint32_t nowMs);
};
