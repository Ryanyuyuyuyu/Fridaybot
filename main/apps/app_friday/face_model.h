/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>

namespace friday {

enum class Expression {
    Idle,
    Curious,
    Happy,
    Surprised,
    Dizzy,
    Sleepy,
    Petted,
    Shy,
    Listening,
    Thinking,
    Daydreaming,
    Proud,
    Expectant,
    Sulky,
    Suspicious,
    Delighted,
    Sad,
    Excited,
    EdgeSquished,
    Annoyed,
    Playful,
    Welcome,
    Peeking,
    Braced,
    UpsideDown,
    Rocking,
    Startled,
    Floating,
    PortalExit,
    PortalReturn,
    ButtonWatch,
    ButtonMischief,
};

enum class Reaction {
    Tap,
    Pet,
    DragRelease,
    EdgeBounce,
    Swipe,
    Annoyed,
    Welcome,
    Curious,
    Play,
    PortalExit,
    PortalReturn,
    ButtonA,
    ButtonB,
};

enum class CompanionState : uint8_t {
    Offline = 0,
    Working,
    Uncertain,
    Away,
    Returned,
    Meeting,
};

struct CompanionContext {
    CompanionState state = CompanionState::Offline;
    // Direction of the monitor relative to Friday: -1 left, 0 centre, 1 right.
    int8_t workDirection = 0;
};

struct ImuSample {
    float accelX = 0.0f;
    float accelY = 0.0f;
    float accelZ = 1.0f;
    float gyroX  = 0.0f;
    float gyroY  = 0.0f;
    float gyroZ  = 0.0f;
};

/**
 * @brief Geometry consumed by the LVGL view.
 *
 * All positions and sizes are in display pixels. Rotation is in degrees.
 */
struct FacePose {
    // A small shift of the facial anchor, not pupil-like travel. Friday's
    // "head" is the physical dial, so the eye pair remains bezel-anchored.
    float gazeX         = 0.0f;
    float gazeY         = 0.0f;
    float eyeSpacing    = 142.0f;
    float leftWidth     = 66.0f;
    float leftHeight    = 124.0f;
    float rightWidth    = 64.0f;
    float rightHeight   = 120.0f;
    float leftRotation  = 7.0f;
    float rightRotation = -1.5f;
    float leftOffsetY   = 0.0f;
    float rightOffsetY  = 0.0f;
    float blinkScale    = 1.0f;
};

/**
 * @brief Frame-rate independent animation and behaviour model for Friday.
 *
 * The model deliberately has no LVGL or ESP-IDF dependencies, which keeps the
 * motion logic deterministic and host-testable. The view only renders the
 * resulting pose.
 */
class FaceModel {
public:
    void reset(uint32_t nowMs);
    void update(uint32_t nowMs, const ImuSample& imu);
    void react(Reaction reaction, uint32_t nowMs, float directionX = 0.0f, float directionY = 0.0f);
    void setTouchContact(bool active, float x, float y, uint32_t nowMs);
    void setCompanionContext(const CompanionContext& context, uint32_t nowMs);

    const FacePose& pose() const
    {
        return _pose;
    }

    Expression expression() const
    {
        return _expression;
    }

private:
    struct Spring {
        float value    = 0.0f;
        float velocity = 0.0f;
        float target   = 0.0f;

        void teleport(float next);
        void step(float dt, float frequencyHz, float dampingRatio = 1.0f);
    };

    FacePose _pose;
    Expression _expression = Expression::Idle;
    CompanionContext _companion_context;

    Spring _gaze_x;
    Spring _gaze_y;
    Spring _spacing;
    Spring _left_width;
    Spring _left_height;
    Spring _right_width;
    Spring _right_height;
    Spring _left_rotation;
    Spring _right_rotation;
    Spring _left_y_offset;
    Spring _right_y_offset;

    uint32_t _last_update_ms    = 0;
    uint32_t _last_activity_ms  = 0;
    uint32_t _reaction_until_ms = 0;
    uint32_t _reaction_start_ms = 0;
    uint32_t _afterglow_until_ms = 0;
    uint32_t _next_blink_ms     = 0;
    uint32_t _blink_start_ms    = 0;
    uint32_t _next_saccade_ms   = 0;
    uint32_t _next_scan_ms      = 0;
    uint32_t _next_emote_ms     = 0;
    uint32_t _context_since_ms  = 0;
    uint32_t _touch_start_ms    = 0;
    uint32_t _last_tap_ms       = 0;
    uint32_t _tilt_since_ms     = 0;
    uint32_t _steep_tilt_since_ms = 0;
    uint32_t _upside_down_since_ms = 0;
    uint32_t _last_tilt_reversal_ms = 0;
    uint32_t _last_gyro_reversal_ms = 0;
    uint32_t _last_motion_emote_ms = 0;
    uint32_t _next_pose_ms       = 0;
    uint32_t _pose_started_ms    = 0;
    uint32_t _rng_state         = 0x46524944;  // "FRID"
    float _last_accel_magnitude = 1.0f;
    float _motion_energy        = 0.0f;
    float _autonomous_gaze_x    = 0.0f;
    float _autonomous_gaze_y    = 0.0f;
    float _touch_x              = 0.0f;
    float _touch_y              = 0.0f;
    float _touch_start_x        = 0.0f;
    float _touch_start_y        = 0.0f;
    float _touch_travel         = 0.0f;
    float _reaction_direction_x = 0.0f;
    float _reaction_direction_y = 0.0f;
    float _filtered_accel_x     = 0.0f;
    float _filtered_accel_y     = 0.0f;
    float _filtered_accel_z     = 1.0f;
    float _shake_energy         = 0.0f;
    float _rock_energy          = 0.0f;
    bool _blink_active          = false;
    bool _manual_reaction       = false;
    bool _autonomous_scan       = false;
    bool _touch_active          = false;
    bool _motion_active         = false;
    uint8_t _tap_streak         = 0;
    uint8_t _pose_variant       = 0;
    int8_t _last_tilt_sign      = 0;
    int8_t _last_gyro_sign      = 0;
    int8_t _scan_direction      = 1;
    Reaction _active_reaction   = Reaction::Tap;
    Expression _pose_expression = Expression::Idle;
    Expression _afterglow_expression = Expression::Idle;

    void chooseExpression(uint32_t nowMs, float gyroMagnitude, float accelJerk);
    void updateMotionState(uint32_t nowMs, float dt, const ImuSample& imu, float gyroMagnitude, float accelJerk);
    void updateMoodPose(uint32_t nowMs);
    uint8_t moodPoseCount(Expression expression) const;
    void scheduleNextMoodPose(uint32_t nowMs);
    void applyMoodPoseVariant(Expression expression, uint32_t nowMs);
    void setExpressionTargets(Expression expression, uint32_t nowMs);
    void applyDialResponse(float tiltX, float tiltY, uint32_t nowMs);
    void applyPseudoTurn();
    void updateBlink(uint32_t nowMs);
    void scheduleNextBlink(uint32_t nowMs);
    void updateAutonomousGaze(uint32_t nowMs);
    uint32_t randomRange(uint32_t minimum, uint32_t maximum);
};

}  // namespace friday
