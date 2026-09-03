/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "face_model.h"
#include "presence_protocol.h"
#include "capsule/capsule_codec.h"
#include "capsule/capsule_link.h"
#include "view/face_view.h"

#include <apps/common/key_manager/key_manager.h>
#include <atomic>
#include <array>
#include <memory>
#include <mooncake.h>
#include <vector>

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

    enum class CapsuleState : uint8_t {
        Idle,
        Recording,
        AwaitingSave,
        Saved,
        Failed,
    };

    static constexpr uint32_t AnimationIntervalMs = 16;  // 62.5 Hz target
    static constexpr uint32_t ImuSampleIntervalMs = 30;  // 33 Hz; spring interpolation stays at 62.5 Hz
    static constexpr uint32_t CapsuleMinimumMs = 800;
    static constexpr uint32_t CapsuleAckTimeoutMs = 8000;
    static constexpr float CapsuleMicGainDb = 12.0f;

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
    CapsuleState _capsule_state    = CapsuleState::Idle;
    uint16_t _capsule_session      = 0;
    uint16_t _capsule_sequence     = 0;
    uint32_t _capsule_started_ms   = 0;
    uint32_t _capsule_deadline_ms  = 0;
    uint32_t _capsule_feedback_until_ms = 0;
    uint32_t _last_capsule_ack_generation = 0;
    uint32_t _capsule_total_samples = 0;
    FridayCapsuleAdpcmState _capsule_adpcm{};
    FridayCapsuleResamplerState _capsule_resampler{};
    std::vector<int16_t> _capsule_input;
    std::array<int16_t, FRIDAY_CAPSULE_CAPTURE_SAMPLES> _capsule_resampled{};
    std::array<uint8_t, FRIDAY_CAPSULE_ADPCM_BYTES> _capsule_encoded{};
    std::array<uint8_t, FRIDAY_CAPSULE_MAX_PACKET_SIZE> _capsule_packet{};

    void updateImu(uint32_t nowMs);
    void updateCompanionContext(uint32_t nowMs);
    void updatePresenceTransfer(uint32_t nowMs);
    bool tryStartPresenceTransfer(float x, float y, uint32_t nowMs);
    void recallFromHost(uint32_t nowMs);
    void beginPresenceReturn(uint32_t nowMs);
    void handleInputs(uint32_t nowMs);
    void beginCapsule(uint32_t nowMs);
    void captureCapsuleAudio();
    void finishCapsule(FridayCapsuleEndReason reason, uint32_t nowMs);
    void failCapsule(uint32_t nowMs);
    void updateCapsule(uint32_t nowMs);
    void updateCapsuleVisual(uint32_t nowMs);
};
