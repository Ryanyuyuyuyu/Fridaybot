/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "face_model.h"

#include <algorithm>
#include <cmath>

namespace friday {
namespace {

constexpr float Pi                   = 3.14159265358979323846f;
constexpr float FrameDtFallback      = 1.0f / 60.0f;
constexpr float MaxFrameDt           = 1.0f / 20.0f;
constexpr uint32_t BlinkDurationMs   = 280;
constexpr uint32_t LookAroundAfterMs = 10000;
constexpr uint32_t SleepAfterMs      = 45000;
constexpr uint32_t AwaySearchMs      = 6000;
constexpr uint32_t TouchHoldPoseMs   = 520;
constexpr uint32_t TapStreakWindowMs = 1800;
constexpr float EdgeTouchStart       = 0.72f;
constexpr float DialShiftRangeX      = 7.0f;
constexpr float DialShiftRangeY      = 5.0f;
constexpr float ActivityMotionFloor  = 0.045f;
constexpr float MotionWakeThreshold  = 0.085f;
constexpr float MotionSleepThreshold = 0.035f;
constexpr float GentleTiltEnter      = 0.24f;
constexpr float GentleTiltExit       = 0.17f;
constexpr float SteepTiltEnter       = 0.66f;
constexpr float SteepTiltExit        = 0.54f;
constexpr float UpsideDownEnterZ     = -0.48f;
constexpr float UpsideDownExitZ      = -0.28f;

float clamp(float value, float minimum, float maximum)
{
    return std::max(minimum, std::min(value, maximum));
}

float smoothstep(float value)
{
    value = clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

float magnitude3(float x, float y, float z)
{
    return std::sqrt(x * x + y * y + z * z);
}

bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs)
{
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

}  // namespace

void FaceModel::Spring::teleport(float next)
{
    value    = next;
    target   = next;
    velocity = 0.0f;
}

void FaceModel::Spring::step(float dt, float frequencyHz, float dampingRatio)
{
    // Small fixed substeps keep the semi-implicit spring stable after an
    // occasional slow frame without changing the perceived animation speed.
    const int substeps = std::max(1, static_cast<int>(std::ceil(dt * 120.0f)));
    const float stepDt = dt / static_cast<float>(substeps);
    const float omega  = 2.0f * Pi * frequencyHz;

    for (int i = 0; i < substeps; ++i) {
        const float acceleration = omega * omega * (target - value) - 2.0f * dampingRatio * omega * velocity;
        velocity += acceleration * stepDt;
        value += velocity * stepDt;
    }
}

void FaceModel::reset(uint32_t nowMs)
{
    _expression           = Expression::Idle;
    _companion_context    = {};
    _last_update_ms       = nowMs;
    _last_activity_ms     = nowMs;
    _reaction_until_ms    = nowMs;
    _reaction_start_ms    = nowMs;
    _afterglow_until_ms   = nowMs;
    _last_accel_magnitude = 1.0f;
    _motion_energy        = 0.0f;
    _autonomous_gaze_x    = 0.0f;
    _autonomous_gaze_y    = 0.0f;
    _blink_active         = false;
    _manual_reaction      = false;
    _autonomous_scan      = false;
    _touch_active         = false;
    _scan_direction       = 1;
    _blink_start_ms       = nowMs;
    _next_blink_ms        = nowMs;
    _next_saccade_ms      = nowMs + randomRange(800, 1800);
    _next_scan_ms         = nowMs + randomRange(10000, 13000);
    _next_emote_ms        = nowMs + randomRange(4800, 7200);
    _context_since_ms     = nowMs;
    _touch_start_ms       = nowMs;
    _last_tap_ms          = nowMs - TapStreakWindowMs;
    _tilt_since_ms        = 0;
    _steep_tilt_since_ms  = 0;
    _upside_down_since_ms = 0;
    _last_tilt_reversal_ms = nowMs;
    _last_gyro_reversal_ms = nowMs;
    _last_motion_emote_ms = nowMs - 2000;
    _next_pose_ms         = nowMs;
    _pose_started_ms      = nowMs;
    _touch_x              = 0.0f;
    _touch_y              = 0.0f;
    _touch_start_x        = 0.0f;
    _touch_start_y        = 0.0f;
    _touch_travel         = 0.0f;
    _reaction_direction_x = 0.0f;
    _reaction_direction_y = 0.0f;
    _filtered_accel_x     = 0.0f;
    _filtered_accel_y     = 0.0f;
    _filtered_accel_z     = 1.0f;
    _shake_energy         = 0.0f;
    _rock_energy          = 0.0f;
    _tap_streak           = 0;
    _pose_variant         = 0;
    _last_tilt_sign       = 0;
    _last_gyro_sign       = 0;
    _motion_active        = false;
    _pose_expression      = Expression::Idle;
    _afterglow_expression = Expression::Idle;

    scheduleNextBlink(nowMs);
    scheduleNextMoodPose(nowMs);

    _gaze_x.teleport(0.0f);
    _gaze_y.teleport(0.0f);
    _spacing.teleport(142.0f);
    _left_width.teleport(66.0f);
    _left_height.teleport(124.0f);
    _right_width.teleport(64.0f);
    _right_height.teleport(120.0f);
    _left_rotation.teleport(7.0f);
    _right_rotation.teleport(-1.5f);
    _left_y_offset.teleport(0.0f);
    _right_y_offset.teleport(0.0f);

    _pose = {};
    setExpressionTargets(Expression::Idle, nowMs);
    _pose.eyeSpacing    = _spacing.value;
    _pose.leftWidth     = _left_width.value;
    _pose.leftHeight    = _left_height.value;
    _pose.rightWidth    = _right_width.value;
    _pose.rightHeight   = _right_height.value;
    _pose.leftRotation  = _left_rotation.value;
    _pose.rightRotation = _right_rotation.value;
    _pose.leftOffsetY   = _left_y_offset.value;
    _pose.rightOffsetY  = _right_y_offset.value;
    _pose.blinkScale    = 1.0f;
}

void FaceModel::update(uint32_t nowMs, const ImuSample& imu)
{
    float dt = static_cast<float>(nowMs - _last_update_ms) / 1000.0f;
    if (dt <= 0.0f) {
        dt = FrameDtFallback;
    }
    dt              = std::min(dt, MaxFrameDt);
    _last_update_ms = nowMs;

    const float gyroMagnitude  = magnitude3(imu.gyroX, imu.gyroY, imu.gyroZ);
    const float accelMagnitude = magnitude3(imu.accelX, imu.accelY, imu.accelZ);
    const float accelJerk      = std::fabs(accelMagnitude - _last_accel_magnitude) / std::max(dt, 0.008f);
    _last_accel_magnitude      = accelMagnitude;

    const uint32_t inactiveBeforeMotion = nowMs - _last_activity_ms;
    const float rawMotion = clamp(gyroMagnitude / 220.0f + accelJerk / 8.0f, 0.0f, 1.0f);
    const float motionMix = 1.0f - std::exp(-dt * 9.0f);
    _motion_energy += (rawMotion - _motion_energy) * motionMix;

    updateMotionState(nowMs, dt, imu, gyroMagnitude, accelJerk);

    const bool wasMoving = _motion_active;
    if (!_motion_active && _motion_energy >= MotionWakeThreshold) {
        _motion_active = true;
    } else if (_motion_active && _motion_energy <= MotionSleepThreshold) {
        _motion_active = false;
    }

    if (!wasMoving && _motion_active && inactiveBeforeMotion >= 6500 && !_manual_reaction && !_touch_active) {
        react(Reaction::Welcome, nowMs);
    }

    const float filteredTilt = std::sqrt(_filtered_accel_x * _filtered_accel_x +
                                         _filtered_accel_y * _filtered_accel_y);
    if (_motion_energy > ActivityMotionFloor || filteredTilt > GentleTiltEnter) {
        _last_activity_ms = nowMs;
    }

    updateAutonomousGaze(nowMs);
    chooseExpression(nowMs, gyroMagnitude, accelJerk);
    updateMoodPose(nowMs);
    setExpressionTargets(_expression, nowMs);

    // The physical dial is Friday's head. Gravity therefore changes the
    // facial weight and eye geometry; it does not slide a pair of UI sprites
    // around the display. Only a few pixels of vestibular counter-motion are
    // allowed, with sub-pixel autonomous drift while the dial is still.
    const float tiltX = clamp(imu.accelX / 0.70f, -1.0f, 1.0f);
    const float tiltY = clamp(imu.accelY / 0.70f, -1.0f, 1.0f);
    if (_autonomous_scan) {
        // A deliberate glance is larger than idle micro-motion, but it is a
        // short whole-face gesture with a beginning and end, not continuous
        // pupil-like sliding.
        _gaze_x.target = static_cast<float>(_scan_direction) * 16.0f;
        _gaze_y.target = -3.0f;
    } else {
        _gaze_x.target = (tiltX * 0.88f + _autonomous_gaze_x * 0.12f) * DialShiftRangeX;
        _gaze_y.target = (tiltY * 0.88f + _autonomous_gaze_y * 0.12f) * DialShiftRangeY;
    }
    applyDialResponse(tiltX, tiltY, nowMs);
    applyPseudoTurn();

    const bool edgeBounce = _manual_reaction && _active_reaction == Reaction::EdgeBounce;
    const bool buttonBounce = _manual_reaction &&
                              (_active_reaction == Reaction::ButtonA || _active_reaction == Reaction::ButtonB);
    const float gazeDamping   = buttonBounce ? 0.58f : (edgeBounce ? 0.62f : 0.88f);
    const float gazeFrequency = buttonBounce ? 6.4f : 4.7f;
    const float shapeDamping  = buttonBounce ? 0.72f : 0.90f;
    _gaze_x.step(dt, gazeFrequency, gazeDamping);
    _gaze_y.step(dt, gazeFrequency, gazeDamping);
    _spacing.step(dt, buttonBounce ? 6.3f : 5.2f, shapeDamping);
    _left_width.step(dt, buttonBounce ? 7.2f : 6.2f, buttonBounce ? 0.76f : 0.92f);
    _left_height.step(dt, buttonBounce ? 7.2f : 6.2f, buttonBounce ? 0.76f : 0.92f);
    _right_width.step(dt, buttonBounce ? 7.2f : 6.2f, buttonBounce ? 0.76f : 0.92f);
    _right_height.step(dt, buttonBounce ? 7.2f : 6.2f, buttonBounce ? 0.76f : 0.92f);
    _left_rotation.step(dt, 5.4f, 0.88f);
    _right_rotation.step(dt, 5.4f, 0.88f);
    _left_y_offset.step(dt, 5.2f, 0.90f);
    _right_y_offset.step(dt, 5.2f, 0.90f);

    updateBlink(nowMs);

    _pose.gazeX         = _gaze_x.value;
    _pose.gazeY         = _gaze_y.value;
    _pose.eyeSpacing    = _spacing.value;
    _pose.leftWidth     = _left_width.value;
    _pose.leftHeight    = _left_height.value;
    _pose.rightWidth    = _right_width.value;
    _pose.rightHeight   = _right_height.value;
    _pose.leftRotation  = _left_rotation.value;
    _pose.rightRotation = _right_rotation.value;
    _pose.leftOffsetY   = _left_y_offset.value;
    _pose.rightOffsetY  = _right_y_offset.value;
}

void FaceModel::react(Reaction reaction, uint32_t nowMs, float directionX, float directionY)
{
    const bool wakingFromSleep = _expression == Expression::Sleepy;
    const bool alreadyAnnoyed  = _manual_reaction && _active_reaction == Reaction::Annoyed &&
                                !deadlineReached(nowMs, _reaction_until_ms);

    if (reaction == Reaction::Tap) {
        if (alreadyAnnoyed) {
            reaction = Reaction::Annoyed;
        } else {
            _tap_streak = nowMs - _last_tap_ms <= TapStreakWindowMs ? static_cast<uint8_t>(_tap_streak + 1U) : 1U;
            _last_tap_ms = nowMs;
            if (_tap_streak >= 3U) {
                reaction    = Reaction::Annoyed;
                _tap_streak = 0;
            }
        }
    } else if (reaction != Reaction::Annoyed) {
        _tap_streak = 0;
    }

    _last_activity_ms          = nowMs;
    _manual_reaction           = true;
    _active_reaction           = reaction;
    _reaction_start_ms         = nowMs;
    _reaction_direction_x      = clamp(directionX, -1.0f, 1.0f);
    _reaction_direction_y      = clamp(directionY, -1.0f, 1.0f);
    _next_emote_ms             = nowMs + randomRange(5200, 8800);
    _afterglow_until_ms        = nowMs;
    _afterglow_expression      = Expression::Idle;

    switch (reaction) {
        case Reaction::Tap:
            _expression        = Expression::Surprised;
            _reaction_until_ms = nowMs + 760;
            break;
        case Reaction::Pet:
            _expression        = wakingFromSleep ? Expression::Welcome : Expression::Shy;
            _reaction_until_ms = nowMs + (wakingFromSleep ? 3000 : 3400);
            if (wakingFromSleep) {
                _active_reaction = Reaction::Welcome;
                _afterglow_expression = Expression::Delighted;
                _afterglow_until_ms   = _reaction_until_ms + 3400;
            }
            break;
        case Reaction::DragRelease:
            _expression        = Expression::Playful;
            _reaction_until_ms = nowMs + 1450;
            _afterglow_expression = Expression::Proud;
            _afterglow_until_ms   = _reaction_until_ms + 3200;
            break;
        case Reaction::EdgeBounce:
            _expression        = Expression::Playful;
            _reaction_until_ms = nowMs + 1550;
            _afterglow_expression = Expression::Proud;
            _afterglow_until_ms   = _reaction_until_ms + 3400;
            break;
        case Reaction::Swipe:
            _expression        = Expression::Excited;
            _reaction_until_ms = nowMs + 1050;
            _afterglow_expression = Expression::Proud;
            _afterglow_until_ms   = _reaction_until_ms + 3000;
            break;
        case Reaction::Annoyed:
            _expression        = Expression::Annoyed;
            _reaction_until_ms = nowMs + 3400;
            _afterglow_expression = Expression::Sulky;
            _afterglow_until_ms   = _reaction_until_ms + 4600;
            break;
        case Reaction::Welcome:
            _expression        = Expression::Welcome;
            _reaction_until_ms = nowMs + 3000;
            _afterglow_expression = Expression::Delighted;
            _afterglow_until_ms   = _reaction_until_ms + 3400;
            break;
        case Reaction::Curious:
            _expression        = Expression::Curious;
            _reaction_until_ms = nowMs + 3600;
            break;
        case Reaction::Play:
            _expression        = Expression::Delighted;
            _reaction_until_ms = nowMs + 3800;
            break;
        case Reaction::PortalExit:
            _expression        = Expression::PortalExit;
            _reaction_until_ms = nowMs + 1050;
            break;
        case Reaction::PortalReturn:
            _expression        = Expression::PortalReturn;
            _reaction_until_ms = nowMs + 1400;
            _afterglow_expression = Expression::Delighted;
            _afterglow_until_ms   = _reaction_until_ms + 2600;
            break;
        case Reaction::ButtonA:
            _expression        = Expression::ButtonWatch;
            _reaction_until_ms = nowMs + 4200;
            _afterglow_expression = Expression::Suspicious;
            _afterglow_until_ms   = _reaction_until_ms + 4000;
            break;
        case Reaction::ButtonB:
            _expression        = Expression::ButtonMischief;
            _reaction_until_ms = nowMs + 4700;
            _afterglow_expression = Expression::Delighted;
            _afterglow_until_ms   = _reaction_until_ms + 3800;
            break;
    }

    // Keep the response readable before the next autonomous blink. The old
    // implementation forced the same squeeze after every gesture, which made
    // tap, drag and swipe feel identical.
    scheduleNextBlink(nowMs);
}

void FaceModel::setTouchContact(bool active, float x, float y, uint32_t nowMs)
{
    x = clamp(x, -1.0f, 1.0f);
    y = clamp(y, -1.0f, 1.0f);

    if (active && !_touch_active) {
        _touch_active   = true;
        _touch_start_ms = nowMs;
        _touch_start_x  = x;
        _touch_start_y  = y;
        _touch_travel   = 0.0f;
        const bool keepAnnoyed = _manual_reaction && _active_reaction == Reaction::Annoyed &&
                                 !deadlineReached(nowMs, _reaction_until_ms);
        if (!keepAnnoyed) {
            _manual_reaction   = false;
            _reaction_until_ms = nowMs;
        }
        _autonomous_scan = false;
        _next_emote_ms      = nowMs + randomRange(5200, 8800);
    }

    if (active) {
        const float dx = x - _touch_start_x;
        const float dy = y - _touch_start_y;
        _touch_travel  = std::max(_touch_travel, std::sqrt(dx * dx + dy * dy));
        _touch_x       = x;
        _touch_y       = y;
        _last_activity_ms = nowMs;
    } else if (_touch_active) {
        _touch_active = false;
        _touch_x      = x;
        _touch_y      = y;
        _last_activity_ms = nowMs;
    }
}

void FaceModel::updateMotionState(uint32_t nowMs, float dt, const ImuSample& imu, float gyroMagnitude,
                                  float accelJerk)
{
    const float orientationMix = 1.0f - std::exp(-dt * 8.0f);
    _filtered_accel_x += (imu.accelX - _filtered_accel_x) * orientationMix;
    _filtered_accel_y += (imu.accelY - _filtered_accel_y) * orientationMix;
    _filtered_accel_z += (imu.accelZ - _filtered_accel_z) * orientationMix;

    _rock_energy *= std::exp(-dt * 0.62f);
    _shake_energy *= std::exp(-dt * 1.45f);

    const float tiltAxis = std::fabs(_filtered_accel_x) >= std::fabs(_filtered_accel_y)
                               ? _filtered_accel_x
                               : _filtered_accel_y;
    if (std::fabs(tiltAxis) >= 0.20f) {
        const int8_t sign = tiltAxis >= 0.0f ? 1 : -1;
        const uint32_t reversalAge = nowMs - _last_tilt_reversal_ms;
        if (_last_tilt_sign != 0 && sign != _last_tilt_sign && reversalAge >= 130 && reversalAge <= 1250) {
            _rock_energy = std::min(1.0f, _rock_energy + 0.36f);
            _last_tilt_reversal_ms = nowMs;
        } else if (_last_tilt_sign == 0 || reversalAge > 1250) {
            _last_tilt_reversal_ms = nowMs;
        }
        _last_tilt_sign = sign;
    }

    float gyroAxis = imu.gyroX;
    if (std::fabs(imu.gyroY) > std::fabs(gyroAxis)) {
        gyroAxis = imu.gyroY;
    }
    if (std::fabs(imu.gyroZ) > std::fabs(gyroAxis)) {
        gyroAxis = imu.gyroZ;
    }
    if (std::fabs(gyroAxis) >= 48.0f) {
        const int8_t sign = gyroAxis >= 0.0f ? 1 : -1;
        const uint32_t reversalAge = nowMs - _last_gyro_reversal_ms;
        if (_last_gyro_sign != 0 && sign != _last_gyro_sign && reversalAge >= 70 && reversalAge <= 750) {
            _shake_energy = std::min(1.0f, _shake_energy + 0.42f);
            _last_gyro_reversal_ms = nowMs;
        } else if (_last_gyro_sign == 0 || reversalAge > 750) {
            _last_gyro_reversal_ms = nowMs;
        }
        _last_gyro_sign = sign;
    }

    if (accelJerk > 1.2f && gyroMagnitude > 35.0f) {
        _shake_energy = std::min(1.0f, _shake_energy + clamp((accelJerk - 1.2f) / 18.0f, 0.0f, 0.14f));
    }

    const float tiltMagnitude = std::sqrt(_filtered_accel_x * _filtered_accel_x +
                                          _filtered_accel_y * _filtered_accel_y);
    if (tiltMagnitude >= GentleTiltEnter) {
        if (_tilt_since_ms == 0) {
            _tilt_since_ms = nowMs;
        }
    } else if (tiltMagnitude <= GentleTiltExit) {
        _tilt_since_ms = 0;
    }

    if (tiltMagnitude >= SteepTiltEnter) {
        if (_steep_tilt_since_ms == 0) {
            _steep_tilt_since_ms = nowMs;
        }
    } else if (tiltMagnitude <= SteepTiltExit) {
        _steep_tilt_since_ms = 0;
    }

    if (_filtered_accel_z <= UpsideDownEnterZ) {
        if (_upside_down_since_ms == 0) {
            _upside_down_since_ms = nowMs;
        }
    } else if (_filtered_accel_z >= UpsideDownExitZ) {
        _upside_down_since_ms = 0;
    }
}

void FaceModel::setCompanionContext(const CompanionContext& context, uint32_t nowMs)
{
    CompanionContext next = context;
    next.workDirection     = static_cast<int8_t>(std::max(-1, std::min(1, static_cast<int>(next.workDirection))));

    const CompanionState previous = _companion_context.state;
    if (previous == next.state && _companion_context.workDirection == next.workDirection) {
        // A heartbeat is still meaningful activity, but it must not restart a
        // facial reaction or the context dwell timer.
        if (next.state == CompanionState::Working || next.state == CompanionState::Meeting ||
            next.state == CompanionState::Returned) {
            _last_activity_ms = nowMs;
        }
        return;
    }

    _companion_context = next;
    _context_since_ms  = nowMs;
    _next_scan_ms      = nowMs + randomRange(1200, 2600);
    if (next.state == CompanionState::Away || next.state == CompanionState::Offline) {
        // Presence changes outrank a decorative post-reaction mood. Friday
        // should begin searching/sleeping promptly when the desk becomes
        // empty instead of finishing an unrelated celebration first.
        _afterglow_until_ms = nowMs;
    }

    const bool returnedFromAway = previous == CompanionState::Away && next.state == CompanionState::Working;
    if (next.state == CompanionState::Returned || returnedFromAway) {
        _expression        = Expression::Welcome;
        _manual_reaction   = true;
        _active_reaction   = Reaction::Welcome;
        _reaction_start_ms = nowMs;
        _reaction_until_ms = nowMs + 3000;
        _afterglow_expression = Expression::Delighted;
        _afterglow_until_ms   = _reaction_until_ms + 3400;
        _last_activity_ms  = nowMs;
    } else if (next.state == CompanionState::Working || next.state == CompanionState::Meeting) {
        _last_activity_ms = nowMs;
    }
}

void FaceModel::chooseExpression(uint32_t nowMs, float gyroMagnitude, float accelJerk)
{
    if (_manual_reaction && _active_reaction == Reaction::Annoyed &&
        !deadlineReached(nowMs, _reaction_until_ms)) {
        _expression = Expression::Annoyed;
        return;
    }

    // While a finger is down, Friday follows the contact continuously. A
    // stationary hold softens into the petted pose; a moving contact remains
    // alert and curious until the release gesture is classified.
    if (_touch_active) {
        const uint32_t heldMs = nowMs - _touch_start_ms;
        const float edge = std::max(std::fabs(_touch_x), std::fabs(_touch_y));
        if (edge >= EdgeTouchStart) {
            _expression = Expression::EdgeSquished;
        } else {
            _expression = (heldMs >= TouchHoldPoseMs && _touch_travel < 0.12f) ? Expression::Petted
                                                                               : Expression::Curious;
        }
        return;
    }

    // A direct touch/button response gets a short exclusive window. Without
    // it, the acceleration caused by the user's tap could immediately replace
    // the petted expression with surprise.
    if (_manual_reaction) {
        if (!deadlineReached(nowMs, _reaction_until_ms)) {
            return;
        }
        _manual_reaction = false;
    }

    const bool motionCooldownReady = nowMs - _last_motion_emote_ms >= 850;
    if (_shake_energy >= 0.70f && motionCooldownReady) {
        _expression            = Expression::Annoyed;
        _reaction_start_ms     = nowMs;
        _reaction_until_ms     = nowMs + 3200;
        _last_motion_emote_ms  = nowMs;
        _shake_energy          = 0.24f;
        _last_activity_ms      = nowMs;
        return;
    }

    if (gyroMagnitude > 145.0f && motionCooldownReady) {
        _expression        = Expression::Dizzy;
        _reaction_start_ms = nowMs;
        _reaction_until_ms = nowMs + 2600;
        _last_motion_emote_ms = nowMs;
        _last_activity_ms  = nowMs;
        return;
    }

    if (_last_accel_magnitude < 0.64f && motionCooldownReady) {
        _expression        = Expression::Floating;
        _reaction_start_ms = nowMs;
        _reaction_until_ms = nowMs + 2200;
        _last_motion_emote_ms = nowMs;
        _last_activity_ms  = nowMs;
        return;
    }

    if ((accelJerk > 2.15f || _last_accel_magnitude > 1.34f) && motionCooldownReady) {
        _expression        = Expression::Startled;
        _reaction_start_ms = nowMs;
        _reaction_until_ms = nowMs + 2400;
        _last_motion_emote_ms = nowMs;
        _last_activity_ms  = nowMs;
        return;
    }

    if (_rock_energy >= 0.66f && gyroMagnitude < 145.0f && motionCooldownReady) {
        _expression        = Expression::Rocking;
        _reaction_start_ms = nowMs;
        _reaction_until_ms = nowMs + 3200;
        _last_motion_emote_ms = nowMs;
        _rock_energy       = 0.20f;
        _last_activity_ms  = nowMs;
        return;
    }

    if (!deadlineReached(nowMs, _reaction_until_ms)) {
        return;
    }

    if (_autonomous_scan) {
        _autonomous_scan = false;
    }

    if (_upside_down_since_ms != 0 && nowMs - _upside_down_since_ms >= 280) {
        _expression = Expression::UpsideDown;
        return;
    }

    if (_steep_tilt_since_ms != 0 && nowMs - _steep_tilt_since_ms >= 150) {
        _expression = Expression::Braced;
        return;
    }

    if (_tilt_since_ms != 0 && nowMs - _tilt_since_ms >= 170) {
        _expression = Expression::Peeking;
        return;
    }

    if (!deadlineReached(nowMs, _afterglow_until_ms)) {
        _expression = _afterglow_expression;
        return;
    }

    // A connected computer contributes coarse context, never animation
    // commands. Friday still chooses the timing and shape of each gesture.
    switch (_companion_context.state) {
        case CompanionState::Away:
            if (nowMs - _context_since_ms >= AwaySearchMs) {
                _expression = Expression::Sleepy;
            } else if (deadlineReached(nowMs, _next_scan_ms)) {
                _expression        = Expression::Curious;
                _reaction_until_ms = nowMs + 3800;
                _autonomous_scan   = true;
                _scan_direction    = -_scan_direction;
                _next_scan_ms      = _reaction_until_ms + 1700;
            } else {
                _expression = Expression::Idle;
            }
            return;

        case CompanionState::Working:
            if (deadlineReached(nowMs, _next_scan_ms)) {
                _expression        = Expression::Curious;
                _reaction_until_ms = nowMs + 4200;
                _autonomous_scan   = true;
                _scan_direction    = _companion_context.workDirection == 0 ? -_scan_direction
                                                                            : _companion_context.workDirection;
                _next_scan_ms      = nowMs + randomRange(8000, 14000);
            } else {
                _expression = Expression::Listening;
            }
            return;

        case CompanionState::Uncertain:
            if (deadlineReached(nowMs, _next_scan_ms)) {
                _expression        = Expression::Curious;
                _reaction_until_ms = nowMs + 4400;
                _autonomous_scan   = true;
                _scan_direction    = -_scan_direction;
                _next_scan_ms      = nowMs + randomRange(5000, 9000);
            } else {
                const uint32_t moodPhase = (nowMs - _context_since_ms) / 6000U;
                _expression = moodPhase % 2U == 0U ? Expression::Thinking : Expression::Suspicious;
            }
            return;

        case CompanionState::Meeting:
            // Meeting mode is intentionally quiet and does not solicit the
            // user, but direct touch and physical motion still work.
            _expression = Expression::Listening;
            return;

        case CompanionState::Returned:
            _expression = Expression::Idle;
            return;

        case CompanionState::Offline:
            break;
    }

    const uint32_t inactiveMs = nowMs - _last_activity_ms;
    if (inactiveMs >= SleepAfterMs) {
        _expression = Expression::Sleepy;
    } else if (deadlineReached(nowMs, _next_emote_ms)) {
        // Small, legible moments keep Friday alive even without the desktop
        // companion. They are sparse enough to read as personality rather
        // than a looping demo reel.
        const uint32_t choice = randomRange(0, 99);
        if (inactiveMs >= 26000 && choice < 13) {
            _expression        = Expression::Sad;
            _reaction_until_ms = nowMs + 6500;
        } else if (inactiveMs >= 18000 && choice < 30) {
            _expression        = Expression::Daydreaming;
            _reaction_until_ms = nowMs + 7200;
        } else if (inactiveMs >= 10000 && choice < 47) {
            _expression        = Expression::Playful;
            _reaction_until_ms = nowMs + 6200;
        } else if (choice < 68) {
            _expression        = Expression::Happy;
            _reaction_until_ms = nowMs + 5200;
        } else if (choice < 84) {
            _expression        = Expression::Expectant;
            _reaction_until_ms = nowMs + 5600;
        } else {
            _expression        = Expression::Curious;
            _reaction_until_ms = nowMs + 5000;
            _autonomous_scan   = true;
            _scan_direction    = -_scan_direction;
        }
        _next_emote_ms = _reaction_until_ms + randomRange(5000, 9000);
    } else if (inactiveMs >= LookAroundAfterMs && deadlineReached(nowMs, _next_scan_ms)) {
        _expression        = Expression::Expectant;
        _reaction_until_ms = nowMs + 5600;
        _autonomous_scan   = true;
        _scan_direction    = -_scan_direction;
        _next_scan_ms      = _reaction_until_ms + randomRange(3600, 6200);
    } else if (_motion_energy > 0.14f) {
        _expression = Expression::Curious;
    } else {
        _expression = Expression::Idle;
    }
}

uint8_t FaceModel::moodPoseCount(Expression expression) const
{
    switch (expression) {
        case Expression::Idle:
            return 2;
        case Expression::Listening:
            return 3;
        case Expression::Thinking:
            return 4;
        case Expression::Daydreaming:
            return 4;
        case Expression::Proud:
        case Expression::Expectant:
        case Expression::Sulky:
        case Expression::Suspicious:
        case Expression::Delighted:
            return 3;
        case Expression::Curious:
            return 4;
        case Expression::Happy:
            return 3;
        case Expression::Sleepy:
            return 3;
        case Expression::Petted:
        case Expression::Shy:
            return 3;
        case Expression::Sad:
        case Expression::Annoyed:
        case Expression::Peeking:
            return 3;
        case Expression::Playful:
            return 4;
        default:
            return 1;
    }
}

void FaceModel::scheduleNextMoodPose(uint32_t nowMs)
{
    uint32_t minimum = 4000;
    uint32_t maximum = 8000;
    switch (_pose_expression) {
        case Expression::Idle:
            minimum = 9000;
            maximum = 16000;
            break;
        case Expression::Listening:
            minimum = 2800;
            maximum = 5000;
            break;
        case Expression::Thinking:
            minimum = 2000;
            maximum = 3600;
            break;
        case Expression::Daydreaming:
            minimum = 3200;
            maximum = 5200;
            break;
        case Expression::Proud:
            minimum = 2200;
            maximum = 3800;
            break;
        case Expression::Expectant:
            minimum = 1800;
            maximum = 3200;
            break;
        case Expression::Sulky:
            minimum = 3000;
            maximum = 5200;
            break;
        case Expression::Suspicious:
            minimum = 2300;
            maximum = 4000;
            break;
        case Expression::Delighted:
            minimum = 1600;
            maximum = 2800;
            break;
        case Expression::Curious:
            minimum = 1800;
            maximum = 3200;
            break;
        case Expression::Happy:
            minimum = 2500;
            maximum = 4500;
            break;
        case Expression::Playful:
            minimum = 1500;
            maximum = 3000;
            break;
        case Expression::Sleepy:
            minimum = 4000;
            maximum = 8000;
            break;
        case Expression::Petted:
        case Expression::Shy:
            minimum = 3000;
            maximum = 5500;
            break;
        case Expression::Annoyed:
            minimum = 2600;
            maximum = 4500;
            break;
        default:
            break;
    }
    _next_pose_ms = nowMs + randomRange(minimum, maximum);
}

void FaceModel::updateMoodPose(uint32_t nowMs)
{
    if (_expression != _pose_expression) {
        _pose_expression = _expression;
        _pose_variant    = 0;
        _pose_started_ms = nowMs;
        scheduleNextMoodPose(nowMs);
        scheduleNextBlink(nowMs);
        return;
    }

    if (_manual_reaction || _touch_active || !deadlineReached(nowMs, _next_pose_ms)) {
        return;
    }

    const uint8_t count = moodPoseCount(_expression);
    if (count > 1U) {
        const uint8_t step = static_cast<uint8_t>(randomRange(1, count - 1U));
        _pose_variant      = static_cast<uint8_t>((_pose_variant + step) % count);
        _pose_started_ms   = nowMs;
    }
    scheduleNextMoodPose(nowMs);
}

void FaceModel::applyMoodPoseVariant(Expression expression, uint32_t nowMs)
{
    const float settle = smoothstep(static_cast<float>(nowMs - _pose_started_ms) / 520.0f);
    const float amount = 0.35f + settle * 0.65f;

    switch (expression) {
        case Expression::Idle:
            if (_pose_variant == 1U) {
                _spacing.target -= 5.0f * amount;
                _left_height.target += 5.0f * amount;
                _right_height.target -= 3.0f * amount;
                _left_y_offset.target -= 2.0f * amount;
                _right_y_offset.target += 3.0f * amount;
            }
            break;
        case Expression::Listening:
            if (_pose_variant == 1U) {
                _spacing.target -= 7.0f * amount;
                _left_height.target += 8.0f * amount;
                _right_height.target -= 5.0f * amount;
                _left_y_offset.target -= 4.0f * amount;
                _right_y_offset.target += 3.0f * amount;
            } else if (_pose_variant == 2U) {
                _spacing.target += 6.0f * amount;
                _left_height.target -= 6.0f * amount;
                _right_height.target += 9.0f * amount;
                _left_y_offset.target += 3.0f * amount;
                _right_y_offset.target -= 5.0f * amount;
            }
            break;
        case Expression::Thinking:
            if (_pose_variant == 1U) {
                _left_height.target -= 20.0f * amount;
                _right_height.target += 13.0f * amount;
                _left_y_offset.target += 8.0f * amount;
                _right_y_offset.target -= 6.0f * amount;
            } else if (_pose_variant == 2U) {
                _spacing.target -= 14.0f * amount;
                _left_height.target += 9.0f * amount;
                _right_height.target += 7.0f * amount;
                _left_y_offset.target -= 5.0f * amount;
                _right_y_offset.target -= 5.0f * amount;
            } else if (_pose_variant == 3U) {
                _spacing.target += 9.0f * amount;
                _left_height.target += 4.0f * amount;
                _right_height.target -= 15.0f * amount;
                _left_y_offset.target -= 3.0f * amount;
                _right_y_offset.target += 9.0f * amount;
            }
            break;
        case Expression::Daydreaming:
            if (_pose_variant == 1U) {
                _spacing.target += 8.0f * amount;
                _left_height.target -= 8.0f * amount;
                _right_height.target += 6.0f * amount;
                _left_y_offset.target += 5.0f * amount;
                _right_y_offset.target -= 4.0f * amount;
            } else if (_pose_variant == 2U) {
                _spacing.target -= 7.0f * amount;
                _left_height.target += 10.0f * amount;
                _right_height.target += 7.0f * amount;
                _left_y_offset.target -= 6.0f * amount;
                _right_y_offset.target -= 3.0f * amount;
            } else if (_pose_variant == 3U) {
                _left_height.target += 4.0f * amount;
                _right_height.target -= 11.0f * amount;
                _left_y_offset.target -= 3.0f * amount;
                _right_y_offset.target += 7.0f * amount;
            }
            break;
        case Expression::Proud:
            if (_pose_variant == 1U) {
                _spacing.target -= 8.0f * amount;
                _left_height.target += 10.0f * amount;
                _right_height.target += 13.0f * amount;
                _left_y_offset.target -= 5.0f * amount;
                _right_y_offset.target -= 6.0f * amount;
            } else if (_pose_variant == 2U) {
                _spacing.target += 6.0f * amount;
                _left_height.target += 12.0f * amount;
                _right_height.target -= 5.0f * amount;
                _left_y_offset.target -= 7.0f * amount;
                _right_y_offset.target += 3.0f * amount;
            }
            break;
        case Expression::Expectant:
            if (_pose_variant == 1U) {
                _spacing.target -= 10.0f * amount;
                _left_height.target += 12.0f * amount;
                _right_height.target += 12.0f * amount;
                _left_y_offset.target -= 7.0f * amount;
                _right_y_offset.target -= 7.0f * amount;
            } else if (_pose_variant == 2U) {
                _spacing.target += 7.0f * amount;
                _left_height.target -= 7.0f * amount;
                _right_height.target += 9.0f * amount;
                _left_y_offset.target += 5.0f * amount;
                _right_y_offset.target -= 5.0f * amount;
            }
            break;
        case Expression::Sulky:
            if (_pose_variant == 1U) {
                _spacing.target += 9.0f * amount;
                _left_height.target -= 8.0f * amount;
                _right_height.target += 5.0f * amount;
                _left_y_offset.target += 6.0f * amount;
            } else if (_pose_variant == 2U) {
                _spacing.target -= 6.0f * amount;
                _left_height.target += 7.0f * amount;
                _right_height.target -= 6.0f * amount;
                _right_y_offset.target += 7.0f * amount;
            }
            break;
        case Expression::Suspicious:
            if (_pose_variant == 1U) {
                _spacing.target -= 8.0f * amount;
                _left_height.target -= 12.0f * amount;
                _right_height.target += 14.0f * amount;
                _left_y_offset.target += 7.0f * amount;
                _right_y_offset.target -= 6.0f * amount;
            } else if (_pose_variant == 2U) {
                _spacing.target += 9.0f * amount;
                _left_height.target += 10.0f * amount;
                _right_height.target -= 9.0f * amount;
                _left_y_offset.target -= 5.0f * amount;
                _right_y_offset.target += 7.0f * amount;
            }
            break;
        case Expression::Delighted:
            if (_pose_variant == 1U) {
                _spacing.target -= 10.0f * amount;
                _left_height.target += 13.0f * amount;
                _right_height.target += 10.0f * amount;
                _left_y_offset.target -= 8.0f * amount;
                _right_y_offset.target -= 8.0f * amount;
            } else if (_pose_variant == 2U) {
                _spacing.target += 7.0f * amount;
                _left_height.target -= 8.0f * amount;
                _right_height.target += 12.0f * amount;
                _left_y_offset.target += 6.0f * amount;
                _right_y_offset.target -= 7.0f * amount;
            }
            break;
        case Expression::Curious:
            if (_pose_variant == 1U) {
                _left_height.target -= 18.0f * amount;
                _right_height.target += 21.0f * amount;
                _left_y_offset.target += 8.0f * amount;
                _right_y_offset.target -= 9.0f * amount;
            } else if (_pose_variant == 2U) {
                _spacing.target -= 13.0f * amount;
                _left_width.target += 5.0f * amount;
                _right_width.target += 7.0f * amount;
                _left_height.target += 10.0f * amount;
                _right_height.target += 16.0f * amount;
            } else if (_pose_variant == 3U) {
                _spacing.target += 8.0f * amount;
                _left_height.target += 3.0f * amount;
                _right_height.target -= 13.0f * amount;
                _left_y_offset.target -= 7.0f * amount;
                _right_y_offset.target += 10.0f * amount;
            }
            break;
        case Expression::Happy:
            if (_pose_variant == 1U) {
                _spacing.target -= 9.0f * amount;
                _left_height.target += 9.0f * amount;
                _right_height.target += 6.0f * amount;
                _left_y_offset.target -= 6.0f * amount;
                _right_y_offset.target -= 6.0f * amount;
            } else if (_pose_variant == 2U) {
                _spacing.target += 7.0f * amount;
                _left_height.target -= 10.0f * amount;
                _right_height.target += 8.0f * amount;
                _left_y_offset.target += 5.0f * amount;
                _right_y_offset.target -= 5.0f * amount;
            }
            break;
        case Expression::Sleepy:
            if (_pose_variant == 1U) {
                _left_height.target -= 9.0f * amount;
                _right_height.target += 10.0f * amount;
                _left_y_offset.target += 4.0f * amount;
                _right_y_offset.target -= 3.0f * amount;
            } else if (_pose_variant == 2U) {
                _spacing.target -= 8.0f * amount;
                _left_height.target += 12.0f * amount;
                _right_height.target += 14.0f * amount;
                _left_y_offset.target -= 4.0f * amount;
                _right_y_offset.target -= 5.0f * amount;
            }
            break;
        case Expression::Petted:
        case Expression::Shy:
            if (_pose_variant == 1U) {
                _spacing.target -= 8.0f * amount;
                _left_height.target += 11.0f * amount;
                _right_height.target -= 7.0f * amount;
                _left_y_offset.target -= 5.0f * amount;
                _right_y_offset.target += 5.0f * amount;
            } else if (_pose_variant == 2U) {
                _spacing.target += 6.0f * amount;
                _left_height.target -= 8.0f * amount;
                _right_height.target += 9.0f * amount;
                _left_y_offset.target += 6.0f * amount;
                _right_y_offset.target -= 4.0f * amount;
            }
            break;
        case Expression::Sad:
            if (_pose_variant == 1U) {
                _spacing.target -= 10.0f * amount;
                _left_height.target -= 7.0f * amount;
                _right_height.target += 5.0f * amount;
            } else if (_pose_variant == 2U) {
                _spacing.target += 8.0f * amount;
                _left_height.target += 8.0f * amount;
                _right_height.target -= 6.0f * amount;
                _left_y_offset.target += 4.0f * amount;
                _right_y_offset.target += 7.0f * amount;
            }
            break;
        case Expression::Annoyed:
            if (_pose_variant == 1U) {
                _spacing.target += 10.0f * amount;
                _left_height.target -= 8.0f * amount;
                _right_height.target += 11.0f * amount;
                _left_y_offset.target += 6.0f * amount;
            } else if (_pose_variant == 2U) {
                _spacing.target -= 8.0f * amount;
                _left_height.target += 8.0f * amount;
                _right_height.target += 5.0f * amount;
                _right_y_offset.target += 7.0f * amount;
            }
            break;
        case Expression::Playful:
            if (_pose_variant == 1U) {
                _spacing.target -= 10.0f * amount;
                _left_y_offset.target -= 8.0f * amount;
                _right_y_offset.target += 8.0f * amount;
            } else if (_pose_variant == 2U) {
                _spacing.target += 9.0f * amount;
                _left_height.target -= 11.0f * amount;
                _right_height.target += 12.0f * amount;
            } else if (_pose_variant == 3U) {
                _spacing.target -= 5.0f * amount;
                _left_height.target += 12.0f * amount;
                _right_height.target -= 10.0f * amount;
                _left_y_offset.target += 7.0f * amount;
                _right_y_offset.target -= 7.0f * amount;
            }
            break;
        case Expression::Peeking:
            if (_pose_variant == 1U) {
                _spacing.target -= 9.0f * amount;
                _left_height.target -= 12.0f * amount;
                _right_height.target += 19.0f * amount;
            } else if (_pose_variant == 2U) {
                _spacing.target += 7.0f * amount;
                _left_height.target += 8.0f * amount;
                _right_height.target -= 8.0f * amount;
            }
            break;
        default:
            break;
    }

    // Variants may shorten or widen an eye, but never abandon Friday's
    // vertical capsule identity.
    _left_width.target  = std::min(_left_width.target, _left_height.target * 0.78f);
    _right_width.target = std::min(_right_width.target, _right_height.target * 0.78f);
}

void FaceModel::setExpressionTargets(Expression expression, uint32_t nowMs)
{
    switch (expression) {
        case Expression::Idle:
            _spacing.target        = 142.0f;
            _left_width.target     = 66.0f;
            _left_height.target    = 124.0f;
            _right_width.target    = 64.0f;
            _right_height.target   = 120.0f;
            _left_rotation.target  = 7.0f;
            _right_rotation.target = -1.5f;
            _left_y_offset.target  = 0.0f;
            _right_y_offset.target = 0.0f;
            break;
        case Expression::Curious:
            _spacing.target        = 136.0f;
            _left_width.target     = 70.0f;
            _left_height.target    = 132.0f;
            _right_width.target    = 62.0f;
            _right_height.target   = 108.0f;
            _left_rotation.target  = 10.0f;
            _right_rotation.target = -5.0f;
            _left_y_offset.target  = -6.0f;
            _right_y_offset.target = 7.0f;
            break;
        case Expression::Happy:
            _spacing.target        = 126.0f;
            _left_width.target     = 74.0f;
            _left_height.target    = 132.0f;
            _right_width.target    = 72.0f;
            _right_height.target   = 128.0f;
            _left_rotation.target  = -5.0f;
            _right_rotation.target = 5.0f;
            _left_y_offset.target  = -5.0f;
            _right_y_offset.target = -5.0f;
            break;
        case Expression::Surprised:
            _spacing.target        = 150.0f;
            _left_width.target     = 86.0f;
            _left_height.target    = 148.0f;
            _right_width.target    = 86.0f;
            _right_height.target   = 148.0f;
            _left_rotation.target  = 2.0f;
            _right_rotation.target = -2.0f;
            _left_y_offset.target  = -8.0f;
            _right_y_offset.target = -8.0f;
            break;
        case Expression::Dizzy: {
            const float phase      = static_cast<float>(nowMs % 700U) / 700.0f * 2.0f * Pi;
            const float wobble     = std::sin(phase);
            _spacing.target        = 132.0f;
            _left_width.target     = 72.0f;
            _left_height.target    = 110.0f + wobble * 13.0f;
            _right_width.target    = 72.0f;
            _right_height.target   = 100.0f - wobble * 13.0f;
            _left_rotation.target  = 20.0f + wobble * 9.0f;
            _right_rotation.target = -20.0f + wobble * 9.0f;
            _left_y_offset.target  = wobble * 9.0f;
            _right_y_offset.target = -wobble * 9.0f;
            break;
        }
        case Expression::Sleepy: {
            const float breath     = std::sin(static_cast<float>(nowMs % 2600U) / 2600.0f * 2.0f * Pi);
            _spacing.target        = 154.0f;
            _left_width.target     = 70.0f;
            _left_height.target    = 84.0f + breath * 3.0f;
            _right_width.target    = 68.0f;
            _right_height.target   = 76.0f + breath * 3.0f;
            _left_rotation.target  = 0.0f;
            _right_rotation.target = 0.0f;
            _left_y_offset.target  = 9.0f;
            _right_y_offset.target = 15.0f;
            break;
        }
        case Expression::Petted:
            _spacing.target        = 118.0f;
            _left_width.target     = 74.0f;
            _left_height.target    = 108.0f;
            _right_width.target    = 70.0f;
            _right_height.target   = 120.0f;
            _left_rotation.target  = 2.0f;
            _right_rotation.target = -2.0f;
            _left_y_offset.target  = 5.0f;
            _right_y_offset.target = -1.0f;
            break;
        case Expression::Shy:
            _spacing.target        = 124.0f;
            _left_width.target     = 62.0f;
            _left_height.target    = 114.0f;
            _right_width.target    = 60.0f;
            _right_height.target   = 108.0f;
            _left_rotation.target  = 3.0f;
            _right_rotation.target = -3.0f;
            _left_y_offset.target  = 8.0f;
            _right_y_offset.target = 5.0f;
            break;
        case Expression::Listening:
            _spacing.target        = 132.0f;
            _left_width.target     = 66.0f;
            _left_height.target    = 134.0f;
            _right_width.target    = 66.0f;
            _right_height.target   = 132.0f;
            _left_rotation.target  = 3.0f;
            _right_rotation.target = -3.0f;
            _left_y_offset.target  = -3.0f;
            _right_y_offset.target = -3.0f;
            break;
        case Expression::Thinking:
            _spacing.target        = 138.0f;
            _left_width.target     = 62.0f;
            _left_height.target    = 130.0f;
            _right_width.target    = 62.0f;
            _right_height.target   = 108.0f;
            _left_rotation.target  = 8.0f;
            _right_rotation.target = -5.0f;
            _left_y_offset.target  = -6.0f;
            _right_y_offset.target = 8.0f;
            break;
        case Expression::Daydreaming: {
            const float phase      = static_cast<float>(nowMs % 3600U) / 3600.0f * 2.0f * Pi;
            const float drift      = std::sin(phase);
            _spacing.target        = 154.0f + drift * 3.0f;
            _left_width.target     = 60.0f;
            _left_height.target    = 118.0f + drift * 3.0f;
            _right_width.target    = 62.0f;
            _right_height.target   = 112.0f - drift * 3.0f;
            _left_rotation.target  = 2.0f;
            _right_rotation.target = -2.0f;
            _left_y_offset.target  = 3.0f + drift * 4.0f;
            _right_y_offset.target = -5.0f - drift * 4.0f;
            break;
        }
        case Expression::Proud:
            _spacing.target        = 124.0f;
            _left_width.target     = 70.0f;
            _left_height.target    = 138.0f;
            _right_width.target    = 74.0f;
            _right_height.target   = 148.0f;
            _left_rotation.target  = -3.0f;
            _right_rotation.target = 3.0f;
            _left_y_offset.target  = -6.0f;
            _right_y_offset.target = -9.0f;
            break;
        case Expression::Expectant:
            _spacing.target        = 130.0f;
            _left_width.target     = 72.0f;
            _left_height.target    = 144.0f;
            _right_width.target    = 72.0f;
            _right_height.target   = 144.0f;
            _left_rotation.target  = 2.0f;
            _right_rotation.target = -2.0f;
            _left_y_offset.target  = -9.0f;
            _right_y_offset.target = -9.0f;
            break;
        case Expression::Sulky:
            _spacing.target        = 150.0f;
            _left_width.target     = 58.0f;
            _left_height.target    = 98.0f;
            _right_width.target    = 60.0f;
            _right_height.target   = 106.0f;
            _left_rotation.target  = 6.0f;
            _right_rotation.target = -6.0f;
            _left_y_offset.target  = 12.0f;
            _right_y_offset.target = 8.0f;
            break;
        case Expression::Suspicious:
            _spacing.target        = 138.0f;
            _left_width.target     = 58.0f;
            _left_height.target    = 116.0f;
            _right_width.target    = 66.0f;
            _right_height.target   = 138.0f;
            _left_rotation.target  = 8.0f;
            _right_rotation.target = -5.0f;
            _left_y_offset.target  = 8.0f;
            _right_y_offset.target = -7.0f;
            break;
        case Expression::Delighted: {
            const float phase      = static_cast<float>(nowMs % 1180U) / 1180.0f * 2.0f * Pi;
            const float lift       = std::fabs(std::sin(phase));
            _spacing.target        = 120.0f - lift * 5.0f;
            _left_width.target     = 78.0f;
            _left_height.target    = 150.0f + lift * 7.0f;
            _right_width.target    = 78.0f;
            _right_height.target   = 150.0f + lift * 7.0f;
            _left_rotation.target  = -3.0f;
            _right_rotation.target = 3.0f;
            _left_y_offset.target  = -12.0f - lift * 7.0f;
            _right_y_offset.target = -12.0f - lift * 7.0f;
            break;
        }
        case Expression::Sad:
            _spacing.target        = 128.0f;
            _left_width.target     = 62.0f;
            _left_height.target    = 104.0f;
            _right_width.target    = 62.0f;
            _right_height.target   = 104.0f;
            _left_rotation.target  = 12.0f;
            _right_rotation.target = -12.0f;
            _left_y_offset.target  = 12.0f;
            _right_y_offset.target = 12.0f;
            break;
        case Expression::Excited:
            _spacing.target        = 134.0f;
            _left_width.target     = 82.0f;
            _left_height.target    = 150.0f;
            _right_width.target    = 82.0f;
            _right_height.target   = 150.0f;
            _left_rotation.target  = 3.0f;
            _right_rotation.target = -3.0f;
            _left_y_offset.target  = -10.0f;
            _right_y_offset.target = -10.0f;
            break;
        case Expression::EdgeSquished:
            _spacing.target        = 88.0f;
            _left_width.target     = 58.0f;
            _left_height.target    = 110.0f;
            _right_width.target    = 58.0f;
            _right_height.target   = 110.0f;
            _left_rotation.target  = 0.0f;
            _right_rotation.target = 0.0f;
            _left_y_offset.target  = 0.0f;
            _right_y_offset.target = 0.0f;
            break;
        case Expression::Annoyed:
            _spacing.target        = 126.0f;
            _left_width.target     = 58.0f;
            _left_height.target    = 102.0f;
            _right_width.target    = 58.0f;
            _right_height.target   = 86.0f;
            _left_rotation.target  = 14.0f;
            _right_rotation.target = -14.0f;
            _left_y_offset.target  = 9.0f;
            _right_y_offset.target = -7.0f;
            break;
        case Expression::Playful: {
            const float phase      = static_cast<float>(nowMs % 820U) / 820.0f * 2.0f * Pi;
            const float hop        = std::sin(phase);
            _spacing.target        = 122.0f + std::fabs(hop) * 6.0f;
            _left_width.target     = 70.0f + hop * 3.0f;
            _left_height.target    = 132.0f + hop * 7.0f;
            _right_width.target    = 70.0f - hop * 3.0f;
            _right_height.target   = 132.0f - hop * 7.0f;
            _left_rotation.target  = -4.0f;
            _right_rotation.target = 4.0f;
            _left_y_offset.target  = -7.0f - hop * 8.0f;
            _right_y_offset.target = -7.0f + hop * 8.0f;
            break;
        }
        case Expression::Welcome:
            _spacing.target        = 128.0f;
            _left_width.target     = 84.0f;
            _left_height.target    = 152.0f;
            _right_width.target    = 84.0f;
            _right_height.target   = 152.0f;
            _left_rotation.target  = 3.0f;
            _right_rotation.target = -3.0f;
            _left_y_offset.target  = -12.0f;
            _right_y_offset.target = -12.0f;
            break;
        case Expression::Peeking:
            _spacing.target        = 130.0f;
            _left_width.target     = 70.0f;
            _left_height.target    = 140.0f;
            _right_width.target    = 60.0f;
            _right_height.target   = 104.0f;
            _left_rotation.target  = 8.0f;
            _right_rotation.target = -4.0f;
            _left_y_offset.target  = -10.0f;
            _right_y_offset.target = 9.0f;
            break;
        case Expression::Braced:
            _spacing.target        = 108.0f;
            _left_width.target     = 78.0f;
            _left_height.target    = 104.0f;
            _right_width.target    = 76.0f;
            _right_height.target   = 100.0f;
            _left_rotation.target  = 4.0f;
            _right_rotation.target = -4.0f;
            _left_y_offset.target  = 4.0f;
            _right_y_offset.target = 4.0f;
            break;
        case Expression::UpsideDown: {
            const float phase      = static_cast<float>(nowMs % 1100U) / 1100.0f * 2.0f * Pi;
            const float wobble     = std::sin(phase) * 4.0f;
            _spacing.target        = 144.0f;
            _left_width.target     = 68.0f;
            _left_height.target    = 116.0f;
            _right_width.target    = 68.0f;
            _right_height.target   = 116.0f;
            _left_rotation.target  = -6.0f;
            _right_rotation.target = 6.0f;
            _left_y_offset.target  = 13.0f + wobble;
            _right_y_offset.target = -13.0f - wobble;
            break;
        }
        case Expression::Rocking: {
            const float phase      = static_cast<float>((nowMs - _reaction_start_ms) % 760U) / 760.0f * 2.0f * Pi;
            const float sway       = std::sin(phase);
            _spacing.target        = 126.0f + std::fabs(sway) * 5.0f;
            _left_width.target     = 72.0f;
            _left_height.target    = 130.0f + sway * 6.0f;
            _right_width.target    = 72.0f;
            _right_height.target   = 130.0f - sway * 6.0f;
            _left_rotation.target  = -3.0f;
            _right_rotation.target = 3.0f;
            _left_y_offset.target  = -6.0f - sway * 9.0f;
            _right_y_offset.target = -6.0f + sway * 9.0f;
            break;
        }
        case Expression::Startled: {
            const float elapsed = static_cast<float>(nowMs - _reaction_start_ms);
            const float impact  = smoothstep(elapsed / 260.0f);
            const float recoil  = std::sin(clamp(elapsed / 620.0f, 0.0f, 1.0f) * Pi);
            const float trembleEnvelope = smoothstep((elapsed - 420.0f) / 220.0f) *
                                           (1.0f - smoothstep((elapsed - 1600.0f) / 520.0f));
            const float tremble = std::sin((elapsed - 420.0f) / 170.0f * 2.0f * Pi) * trembleEnvelope;
            const float recover = smoothstep((elapsed - 1660.0f) / 620.0f);

            // A set-down or bump now has a readable performance: pop open,
            // hold a nervous asymmetric tremble, then exhale back toward the
            // resting capsule proportions instead of freezing after 650 ms.
            _spacing.target        = 146.0f + recoil * 13.0f - recover * 7.0f + tremble * 3.0f;
            _left_width.target     = 76.0f + impact * 8.0f - recover * 10.0f;
            _left_height.target    = 94.0f + impact * 48.0f - recover * 19.0f + tremble * 5.0f;
            _right_width.target    = 76.0f + impact * 8.0f - recover * 10.0f;
            _right_height.target   = 94.0f + impact * 48.0f - recover * 23.0f - tremble * 5.0f;
            _left_rotation.target  = 1.0f;
            _right_rotation.target = -1.0f;
            _left_y_offset.target  = -4.0f - recoil * 5.0f + tremble * 4.0f + recover * 4.0f;
            _right_y_offset.target = -4.0f - recoil * 5.0f - tremble * 4.0f + recover * 6.0f;
            break;
        }
        case Expression::Floating: {
            const float phase      = static_cast<float>((nowMs - _reaction_start_ms) % 900U) / 900.0f * 2.0f * Pi;
            const float bob        = std::sin(phase);
            _spacing.target        = 158.0f;
            _left_width.target     = 66.0f;
            _left_height.target    = 126.0f;
            _right_width.target    = 64.0f;
            _right_height.target   = 122.0f;
            _left_rotation.target  = 2.0f;
            _right_rotation.target = -2.0f;
            _left_y_offset.target  = -11.0f + bob * 5.0f;
            _right_y_offset.target = -7.0f + bob * 5.0f;
            break;
        }
        case Expression::PortalExit:
            _spacing.target        = 118.0f;
            _left_width.target     = 68.0f;
            _left_height.target    = 132.0f;
            _right_width.target    = 68.0f;
            _right_height.target   = 132.0f;
            _left_rotation.target  = 0.0f;
            _right_rotation.target = 0.0f;
            _left_y_offset.target  = 0.0f;
            _right_y_offset.target = 0.0f;
            break;
        case Expression::PortalReturn:
            _spacing.target        = 126.0f;
            _left_width.target     = 74.0f;
            _left_height.target    = 144.0f;
            _right_width.target    = 74.0f;
            _right_height.target   = 144.0f;
            _left_rotation.target  = 0.0f;
            _right_rotation.target = 0.0f;
            _left_y_offset.target  = -7.0f;
            _right_y_offset.target = -7.0f;
            break;
        case Expression::ButtonWatch:
            _spacing.target        = 132.0f;
            _left_width.target     = 75.0f;
            _left_height.target    = 139.0f;
            _right_width.target    = 61.0f;
            _right_height.target   = 108.0f;
            _left_rotation.target  = 12.0f;
            _right_rotation.target = -7.0f;
            _left_y_offset.target  = -10.0f;
            _right_y_offset.target = 8.0f;
            break;
        case Expression::ButtonMischief:
            _spacing.target        = 124.0f;
            _left_width.target     = 74.0f;
            _left_height.target    = 136.0f;
            _right_width.target    = 76.0f;
            _right_height.target   = 140.0f;
            _left_rotation.target  = -6.0f;
            _right_rotation.target = 7.0f;
            _left_y_offset.target  = -5.0f;
            _right_y_offset.target = -8.0f;
            break;
    }

    applyMoodPoseVariant(expression, nowMs);
}

void FaceModel::applyDialResponse(float tiltX, float tiltY, uint32_t nowMs)
{
    // Opposing deformation makes the eyes feel attached to a soft facial
    // structure inside the round bezel. The downhill eye bears more vertical
    // weight while the uphill eye opens, producing a readable "head tilt"
    // without introducing pupils or another illuminated feature.
    const float leftHeightWeight  = clamp(1.0f + tiltX * 0.10f - tiltY * 0.04f, 0.82f, 1.18f);
    const float rightHeightWeight = clamp(1.0f - tiltX * 0.10f - tiltY * 0.04f, 0.82f, 1.18f);
    const float leftWidthWeight   = clamp(1.0f - tiltX * 0.06f + tiltY * 0.05f, 0.86f, 1.14f);
    const float rightWidthWeight  = clamp(1.0f + tiltX * 0.06f + tiltY * 0.05f, 0.86f, 1.14f);

    _left_height.target *= leftHeightWeight;
    _right_height.target *= rightHeightWeight;
    _left_width.target *= leftWidthWeight;
    _right_width.target *= rightWidthWeight;
    _left_rotation.target -= tiltX * 7.0f;
    _right_rotation.target -= tiltX * 7.0f;
    _spacing.target += std::fabs(tiltX) * 3.0f;

    if (_expression == Expression::Sad) {
        _gaze_y.target += 11.0f;
    } else if (_expression == Expression::Excited) {
        const float phase = static_cast<float>(nowMs % 420U) / 420.0f * 2.0f * Pi;
        _gaze_y.target -= 9.0f + std::sin(phase) * 3.0f;
    } else if (_expression == Expression::Happy) {
        const float phase = static_cast<float>(nowMs % 900U) / 900.0f * 2.0f * Pi;
        _gaze_y.target -= 5.0f + std::sin(phase) * 2.0f;
        _spacing.target -= std::sin(phase) * 2.5f;
    } else if (_expression == Expression::Sleepy) {
        _gaze_y.target += 7.0f;
    } else if (_expression == Expression::Petted) {
        _gaze_y.target += 4.0f;
    } else if (_expression == Expression::Shy) {
        const float t       = static_cast<float>(nowMs - _reaction_start_ms);
        const float peek    = std::sin(clamp((t - 900.0f) / 1500.0f, 0.0f, 1.0f) * Pi);
        const float sourceX = std::fabs(_reaction_direction_x) < 0.10f ? -1.0f : _reaction_direction_x;
        _gaze_x.target += sourceX * (3.0f + peek * 7.0f);
        _gaze_y.target += 7.0f - peek * 5.0f;
        _spacing.target -= peek * 5.0f;
    } else if (_expression == Expression::Listening) {
        const float phase = static_cast<float>(nowMs % 2400U) / 2400.0f * 2.0f * Pi;
        const float direction = _companion_context.workDirection == 0
                                    ? static_cast<float>(_scan_direction) * 0.35f
                                    : static_cast<float>(_companion_context.workDirection);
        _gaze_x.target += direction * 12.0f;
        _gaze_y.target -= 4.0f + std::sin(phase) * 1.5f;
        _left_y_offset.target += std::sin(phase) * 1.5f;
        _right_y_offset.target += std::sin(phase) * 1.5f;
    } else if (_expression == Expression::Thinking) {
        static constexpr float ThinkGazeX[4] = {-9.0f, 8.0f, 0.0f, 6.0f};
        static constexpr float ThinkGazeY[4] = {-2.0f, -6.0f, 3.0f, -4.0f};
        const uint8_t variant = std::min<uint8_t>(_pose_variant, 3U);
        _gaze_x.target += ThinkGazeX[variant];
        _gaze_y.target += ThinkGazeY[variant];
    } else if (_expression == Expression::Daydreaming) {
        const float phase = static_cast<float>(nowMs % 4200U) / 4200.0f * 2.0f * Pi;
        _gaze_x.target += std::sin(phase) * 9.0f;
        _gaze_y.target += std::cos(phase) * 4.0f - 2.0f;
    } else if (_expression == Expression::Proud) {
        const float phase = static_cast<float>(nowMs % 1700U) / 1700.0f * 2.0f * Pi;
        const float lift  = std::fabs(std::sin(phase)) * 2.5f;
        _gaze_y.target -= 8.0f + lift;
        _spacing.target -= lift;
    } else if (_expression == Expression::Expectant) {
        const float phase = static_cast<float>(nowMs % 1250U) / 1250.0f * 2.0f * Pi;
        const float bob   = std::fabs(std::sin(phase));
        _gaze_x.target += static_cast<float>(_scan_direction) * (7.0f + bob * 3.0f);
        _gaze_y.target -= 7.0f + bob * 5.0f;
        _spacing.target -= bob * 4.0f;
    } else if (_expression == Expression::Sulky) {
        const float phase = static_cast<float>(nowMs % 2200U) / 2200.0f * 2.0f * Pi;
        const float huff  = std::max(0.0f, std::sin(phase));
        const float away  = std::fabs(_reaction_direction_x) < 0.12f ? -1.0f : -_reaction_direction_x;
        _gaze_x.target += away * (11.0f + huff * 4.0f);
        _gaze_y.target += 8.0f + huff * 2.0f;
        _spacing.target += huff * 5.0f;
    } else if (_expression == Expression::Suspicious) {
        const float phase = static_cast<float>(nowMs % 1900U) / 1900.0f * 2.0f * Pi;
        float source = _reaction_direction_x;
        if (std::fabs(source) < 0.12f) {
            source = _companion_context.workDirection == 0 ? static_cast<float>(_scan_direction)
                                                            : static_cast<float>(_companion_context.workDirection);
        }
        _gaze_x.target += source * (13.0f + std::sin(phase) * 2.0f);
        _gaze_y.target -= 2.0f + std::fabs(std::sin(phase)) * 2.0f;
    } else if (_expression == Expression::Delighted) {
        const float phase = static_cast<float>(nowMs % 1180U) / 1180.0f * 2.0f * Pi;
        const float hop   = std::fabs(std::sin(phase));
        _gaze_y.target -= 8.0f + hop * 7.0f;
        _spacing.target -= hop * 5.0f;
        _left_y_offset.target -= hop * 3.0f;
        _right_y_offset.target -= hop * 3.0f;
    } else if (_expression == Expression::Playful) {
        const float phase = static_cast<float>(nowMs % 820U) / 820.0f * 2.0f * Pi;
        _gaze_x.target += std::sin(phase) * 7.0f;
        _gaze_y.target -= 6.0f + std::fabs(std::sin(phase)) * 3.0f;
    } else if (_expression == Expression::Welcome) {
        _gaze_y.target -= 9.0f;
    } else if (_expression == Expression::Annoyed) {
        _gaze_y.target += 5.0f;
        if (!_manual_reaction) {
            const float phase = static_cast<float>(nowMs - _reaction_start_ms) / 150.0f * Pi;
            _gaze_x.target += std::sin(phase) * 10.0f;
            _spacing.target += std::fabs(std::sin(phase)) * 5.0f;
        }
    } else if (_expression == Expression::Peeking) {
        _gaze_x.target += tiltX * 13.0f;
        _gaze_y.target += tiltY * 9.0f - 3.0f;
        _spacing.target -= std::fabs(tiltX) * 5.0f;
    } else if (_expression == Expression::Braced) {
        _gaze_x.target -= tiltX * 9.0f;
        _gaze_y.target -= tiltY * 7.0f;
        _spacing.target -= 7.0f;
    } else if (_expression == Expression::UpsideDown) {
        const float phase = static_cast<float>(nowMs % 1100U) / 1100.0f * 2.0f * Pi;
        _gaze_x.target += std::sin(phase) * 8.0f;
        _gaze_y.target += 10.0f;
    } else if (_expression == Expression::Rocking) {
        const float phase = static_cast<float>((nowMs - _reaction_start_ms) % 760U) / 760.0f * 2.0f * Pi;
        _gaze_x.target += std::sin(phase) * 11.0f;
        _gaze_y.target -= 6.0f + std::fabs(std::sin(phase)) * 4.0f;
    } else if (_expression == Expression::Startled) {
        const float phase = clamp(static_cast<float>(nowMs - _reaction_start_ms) / 650.0f, 0.0f, 1.0f);
        _gaze_y.target -= std::sin(phase * Pi) * 12.0f;
    } else if (_expression == Expression::Floating) {
        const float phase = static_cast<float>((nowMs - _reaction_start_ms) % 900U) / 900.0f * 2.0f * Pi;
        _gaze_y.target -= 9.0f + std::sin(phase) * 5.0f;
    }

    if (_touch_active && _expression != Expression::Annoyed) {
        // The pair moves only a little; most of the response is opposing eye
        // deformation, so the round dial continues to read as Friday's whole
        // head instead of a cursor chasing a finger inside a screen.
        _gaze_x.target += _touch_x * 13.0f;
        _gaze_y.target += _touch_y * 9.0f;
        _spacing.target -= std::fabs(_touch_x) * 5.0f;
        const float side = _touch_x * 0.10f;
        _left_height.target *= 1.0f + side;
        _right_height.target *= 1.0f - side;
        _left_y_offset.target += _touch_y * 5.0f;
        _right_y_offset.target += _touch_y * 5.0f;

        const float edgeX = smoothstep((std::fabs(_touch_x) - EdgeTouchStart) / (1.0f - EdgeTouchStart));
        const float edgeY = smoothstep((std::fabs(_touch_y) - EdgeTouchStart) / (1.0f - EdgeTouchStart));
        const float edge  = std::max(edgeX, edgeY);
        if (edge > 0.0f) {
            // At the bezel the normal seven-pixel head response deliberately
            // gives way to a rubber-face gag: both capsules are pushed to the
            // touched edge, compacted, and left with spring energy for release.
            _gaze_x.target += _touch_x * edgeX * 142.0f;
            _gaze_y.target += _touch_y * edgeY * 160.0f;
            _spacing.target *= 1.0f - edge * 0.16f;
            if (edgeX >= edgeY) {
                _left_width.target *= 1.0f - edgeX * 0.18f;
                _right_width.target *= 1.0f - edgeX * 0.18f;
                _left_height.target *= 1.0f + edgeX * 0.08f;
                _right_height.target *= 1.0f + edgeX * 0.08f;
            } else {
                _left_height.target *= 1.0f - edgeY * 0.24f;
                _right_height.target *= 1.0f - edgeY * 0.24f;
                _left_width.target *= 1.0f + edgeY * 0.10f;
                _right_width.target *= 1.0f + edgeY * 0.10f;
            }
        }

        if (edge == 0.0f && nowMs - _touch_start_ms >= TouchHoldPoseMs && _touch_travel < 0.12f) {
            const float phase = static_cast<float>((nowMs - _touch_start_ms) % 1200U) / 1200.0f * 2.0f * Pi;
            const float nuzzle = std::sin(phase);
            _spacing.target -= 6.0f + nuzzle * 2.0f;
            _gaze_x.target += _touch_x * 4.0f;
            _left_y_offset.target += nuzzle * 2.0f;
            _right_y_offset.target -= nuzzle * 2.0f;
        }
    }

    if (!_manual_reaction) {
        return;
    }

    const uint32_t elapsed = nowMs - _reaction_start_ms;
    switch (_active_reaction) {
        case Reaction::Tap: {
            // A short boop opens the capsules and ripples the whole face away
            // from the contact point. It deliberately does not force a blink.
            const float phase = clamp(static_cast<float>(elapsed) / 520.0f, 0.0f, 1.0f);
            const float pop   = std::sin(phase * Pi);
            _spacing.target += pop * 13.0f;
            _left_width.target *= 1.0f + pop * 0.10f;
            _right_width.target *= 1.0f + pop * 0.10f;
            _left_height.target *= 1.0f + pop * 0.08f;
            _right_height.target *= 1.0f + pop * 0.08f;
            _gaze_x.target -= _reaction_direction_x * pop * 9.0f;
            _gaze_y.target -= _reaction_direction_y * pop * 7.0f;
            break;
        }
        case Reaction::Pet: {
            const float t         = static_cast<float>(elapsed);
            const float nuzzle    = std::sin(clamp(t / 1050.0f, 0.0f, 1.0f) * Pi);
            const float settleIn  = smoothstep((t - 700.0f) / 500.0f);
            const float settleOut = 1.0f - smoothstep((t - 2700.0f) / 550.0f);
            const float settle    = settleIn * settleOut;
            const float peek      = std::sin(clamp((t - 1150.0f) / 1250.0f, 0.0f, 1.0f) * Pi) * settle;
            const float breath    = std::sin((t - 850.0f) / 1050.0f * 2.0f * Pi) * settle;
            _spacing.target -= nuzzle * 7.0f + settle * 5.0f;
            _gaze_x.target += _reaction_direction_x * (nuzzle * 8.0f + peek * 6.0f);
            _gaze_y.target += _reaction_direction_y * nuzzle * 5.0f + settle * 4.0f - peek * 5.0f;
            _left_y_offset.target += nuzzle * 3.0f - breath * 2.5f;
            _right_y_offset.target -= nuzzle * 3.0f - breath * 2.5f;
            _left_height.target *= 1.0f + peek * 0.05f;
            _right_height.target *= 1.0f - peek * 0.04f;
            break;
        }
        case Reaction::DragRelease: {
            const float phase  = clamp(static_cast<float>(elapsed) / 720.0f, 0.0f, 1.0f);
            const float settle = std::sin(phase * Pi);
            _gaze_x.target += _reaction_direction_x * settle * 15.0f;
            _gaze_y.target += _reaction_direction_y * settle * 10.0f;
            _spacing.target -= settle * 5.0f;
            break;
        }
        case Reaction::EdgeBounce: {
            const float phase  = clamp(static_cast<float>(elapsed) / 950.0f, 0.0f, 1.0f);
            const float recoil = std::sin(clamp(phase / 0.58f, 0.0f, 1.0f) * Pi);
            _gaze_x.target -= _reaction_direction_x * recoil * 34.0f;
            _gaze_y.target -= _reaction_direction_y * recoil * 28.0f;
            _spacing.target += std::sin(phase * 2.0f * Pi) * 9.0f;
            _left_height.target *= 1.0f + recoil * 0.07f;
            _right_height.target *= 1.0f + recoil * 0.07f;
            break;
        }
        case Reaction::Swipe: {
            const float length = std::max(0.001f, std::sqrt(_reaction_direction_x * _reaction_direction_x +
                                                          _reaction_direction_y * _reaction_direction_y));
            const float directionX = _reaction_direction_x / length;
            const float directionY = _reaction_direction_y / length;
            const float phase      = clamp(static_cast<float>(elapsed) / 880.0f, 0.0f, 1.0f);
            const float chase      = std::sin(phase * Pi) + std::sin(phase * 2.0f * Pi) * 0.22f;
            _gaze_x.target += directionX * chase * 28.0f;
            _gaze_y.target += directionY * chase * 20.0f;
            _spacing.target -= std::fabs(chase) * 8.0f;
            _left_y_offset.target += directionY * chase * 5.0f;
            _right_y_offset.target += directionY * chase * 5.0f;
            break;
        }
        case Reaction::Annoyed: {
            const float phase    = clamp(static_cast<float>(elapsed) / 2100.0f, 0.0f, 1.0f);
            const float envelope = 1.0f - smoothstep(phase);
            const float twitch   = std::sin(phase * 8.0f * Pi) * envelope;
            const float awayX    = std::fabs(_reaction_direction_x) < 0.12f ? -1.0f : -_reaction_direction_x;
            _gaze_x.target += awayX * (13.0f + twitch * 6.0f);
            _spacing.target += 7.0f + std::fabs(twitch) * 4.0f;
            _left_y_offset.target += twitch * 4.0f;
            _right_y_offset.target -= twitch * 4.0f;
            break;
        }
        case Reaction::Welcome: {
            const float phase = clamp(static_cast<float>(elapsed) / 1600.0f, 0.0f, 1.0f);
            const float hop   = std::fabs(std::sin(phase * 2.0f * Pi)) * (1.0f - phase * 0.35f);
            _gaze_y.target -= hop * 13.0f;
            _spacing.target -= hop * 7.0f;
            _left_width.target *= 1.0f + hop * 0.06f;
            _right_width.target *= 1.0f + hop * 0.06f;
            break;
        }
        case Reaction::Curious:
            // The expression targets already create the asymmetric inquisitive
            // pose; keep its facial anchor firmly attached to the bezel.
            break;
        case Reaction::Play: {
            const float phase  = clamp(static_cast<float>(elapsed) / 520.0f, 0.0f, 1.0f);
            const float bounce = std::sin(phase * Pi);
            _spacing.target += bounce * 7.0f;
            _gaze_y.target -= bounce * 6.0f;
            break;
        }
        case Reaction::PortalExit: {
            const float phase = smoothstep(static_cast<float>(elapsed) / 900.0f);
            const float squeeze = std::sin(clamp(static_cast<float>(elapsed) / 620.0f, 0.0f, 1.0f) * Pi);
            _gaze_x.target += _reaction_direction_x * phase * 245.0f;
            _gaze_y.target += _reaction_direction_y * phase * 245.0f;
            _spacing.target *= 1.0f - squeeze * 0.35f;
            if (std::fabs(_reaction_direction_x) >= std::fabs(_reaction_direction_y)) {
                _left_width.target *= 1.0f - squeeze * 0.48f;
                _right_width.target *= 1.0f - squeeze * 0.48f;
                _left_height.target *= 1.0f + squeeze * 0.10f;
                _right_height.target *= 1.0f + squeeze * 0.10f;
            } else {
                _left_height.target *= 1.0f - squeeze * 0.42f;
                _right_height.target *= 1.0f - squeeze * 0.42f;
                _left_width.target *= 1.0f + squeeze * 0.08f;
                _right_width.target *= 1.0f + squeeze * 0.08f;
            }
            break;
        }
        case Reaction::PortalReturn: {
            const float phase = smoothstep(static_cast<float>(elapsed) / 760.0f);
            const float remaining = 1.0f - phase;
            const float overshoot = std::sin(clamp((static_cast<float>(elapsed) - 520.0f) / 700.0f, 0.0f, 1.0f) *
                                             2.0f * Pi) * (1.0f - smoothstep((static_cast<float>(elapsed) - 920.0f) / 430.0f));
            _gaze_x.target += _reaction_direction_x * (remaining * 245.0f - overshoot * 19.0f);
            _gaze_y.target += _reaction_direction_y * (remaining * 245.0f - overshoot * 19.0f);
            const float pop = std::sin(clamp(static_cast<float>(elapsed) / 900.0f, 0.0f, 1.0f) * Pi);
            _spacing.target -= pop * 12.0f;
            _left_height.target *= 1.0f + pop * 0.10f;
            _right_height.target *= 1.0f + pop * 0.10f;
            break;
        }
        case Reaction::ButtonA: {
            // The upper-left button first dents the near side of the face and
            // knocks the whole expression down-right. What follows is a small
            // performance rather than one tween: acquire the source, lean in,
            // breathe while watching, perform a wary double-take, flinch once
            // more, then reluctantly spring home.
            const float t           = static_cast<float>(elapsed);
            const float impactPhase = clamp(t / 260.0f, 0.0f, 1.0f);
            const float impact      = std::sin(impactPhase * Pi);
            const float notice      = smoothstep((t - 100.0f) / 280.0f);
            const float returnPhase = smoothstep((t - 3200.0f) / 850.0f);
            const float watch       = notice * (1.0f - returnPhase);
            const float leanIn      = smoothstep((t - 620.0f) / 520.0f) *
                                      (1.0f - smoothstep((t - 1900.0f) / 620.0f));
            const float breath      = std::sin((t - 420.0f) / 980.0f * 2.0f * Pi) * watch;
            const float doublePhase = clamp((t - 1700.0f) / 900.0f, 0.0f, 1.0f);
            const float doubleTake  = std::sin(doublePhase * 2.0f * Pi) *
                                      (1.0f - smoothstep((t - 2540.0f) / 260.0f));
            const float secondFlinch = std::sin(clamp((t - 2580.0f) / 560.0f, 0.0f, 1.0f) * Pi);
            const float settle       = std::sin(returnPhase * 2.0f * Pi) * (1.0f - returnPhase);

            _gaze_x.target += -_reaction_direction_x * impact * 25.0f +
                              _reaction_direction_x * watch * (19.0f + leanIn * 5.0f) +
                              doubleTake * 7.0f + settle * 5.0f;
            _gaze_y.target += -_reaction_direction_y * impact * 19.0f +
                              _reaction_direction_y * watch * (14.0f + leanIn * 4.0f) -
                              breath * 2.2f + secondFlinch * 5.0f;
            _spacing.target -= impact * 13.0f;
            _spacing.target += watch * 4.0f - leanIn * 7.0f + secondFlinch * 8.0f;
            _left_width.target *= 1.0f + impact * 0.16f;
            _left_height.target *= 1.0f - impact * 0.24f + breath * 0.025f + secondFlinch * 0.08f;
            _right_width.target *= 1.0f + impact * 0.05f;
            _right_height.target *= 1.0f + impact * 0.13f - breath * 0.025f + secondFlinch * 0.08f;
            _left_y_offset.target += impact * 13.0f - watch * 5.0f - breath * 2.5f + doubleTake * 2.0f;
            _right_y_offset.target += impact * 7.0f + watch * 3.0f + breath * 2.5f - doubleTake * 2.0f;
            break;
        }
        case Reaction::ButtonB: {
            // B begins as the mirrored corner bump, but Friday decides it is
            // a game. It chases the upper-right corner, plays around it for a
            // few beats, feints back toward centre, returns for a celebratory
            // pair of hops, and only then takes an elastic route home.
            const float t           = static_cast<float>(elapsed);
            const float impactPhase = clamp(t / 240.0f, 0.0f, 1.0f);
            const float impact      = std::sin(impactPhase * Pi);
            const float chaseIn     = smoothstep((t - 90.0f) / 300.0f);
            const float returnPhase = smoothstep((t - 3850.0f) / 720.0f);
            const float chase       = chaseIn * (1.0f - returnPhase);
            const float playWindow  = smoothstep((t - 430.0f) / 300.0f) *
                                      (1.0f - smoothstep((t - 2050.0f) / 500.0f));
            const float hops        = std::sin((t - 430.0f) / 610.0f * 2.0f * Pi) * playWindow;
            const float orbit       = std::sin((t - 520.0f) / 1420.0f * 2.0f * Pi) * playWindow;
            const float feint       = std::sin(clamp((t - 2150.0f) / 820.0f, 0.0f, 1.0f) * Pi);
            const float finalePhase = clamp((t - 2920.0f) / 900.0f, 0.0f, 1.0f);
            const float finaleEnvelope = std::sin(finalePhase * Pi);
            const float finaleHops     = std::sin(finalePhase * 4.0f * Pi) * finaleEnvelope;
            const float proudHop       = std::fabs(std::sin(finalePhase * 2.0f * Pi)) * finaleEnvelope;
            const float settle         = std::sin(returnPhase * 2.0f * Pi) * (1.0f - returnPhase);

            _gaze_x.target += -_reaction_direction_x * impact * 23.0f +
                              _reaction_direction_x * chase * (24.0f + orbit * 6.0f) -
                              _reaction_direction_x * feint * 13.0f + settle * 7.0f;
            _gaze_y.target += -_reaction_direction_y * impact * 18.0f +
                              _reaction_direction_y * chase * 17.0f -
                              std::fabs(hops) * 5.0f + feint * 7.0f - proudHop * 9.0f;
            _spacing.target -= impact * 11.0f + std::fabs(hops) * 7.0f + proudHop * 8.0f;
            _left_height.target *= 1.0f + hops * 0.12f + finaleHops * 0.10f;
            _right_height.target *= 1.0f - hops * 0.12f - finaleHops * 0.10f;
            _left_width.target *= 1.0f - hops * 0.05f + proudHop * 0.05f;
            _right_width.target *= 1.0f + hops * 0.05f + proudHop * 0.05f;
            _left_y_offset.target -= hops * 12.0f + finaleHops * 11.0f + proudHop * 5.0f;
            _right_y_offset.target += hops * 12.0f + finaleHops * 11.0f - proudHop * 5.0f;
            break;
        }
    }
}

void FaceModel::applyPseudoTurn()
{
    // A lateral glance deforms the pair as if it were painted on the curved
    // stopwatch face: the near capsule comes forward while the far capsule
    // narrows and sits slightly deeper. This small perspective cue makes the
    // entire dial appear to turn, rather than sliding two sprites in a panel.
    const float turn     = clamp(_gaze_x.target / 32.0f, -1.0f, 1.0f);
    const float strength = smoothstep(std::fabs(turn));
    if (strength <= 0.001f) {
        return;
    }

    const float nearWidthScale  = 1.0f + strength * 0.11f;
    const float nearHeightScale = 1.0f + strength * 0.035f;
    const float farWidthScale   = 1.0f - strength * 0.27f;
    const float farHeightScale  = 1.0f - strength * 0.075f;
    _spacing.target *= 1.0f - strength * 0.12f;
    _gaze_y.target -= strength * 1.8f;

    if (turn > 0.0f) {
        _left_width.target *= farWidthScale;
        _left_height.target *= farHeightScale;
        _right_width.target *= nearWidthScale;
        _right_height.target *= nearHeightScale;
        _left_y_offset.target += strength * 3.5f;
        _right_y_offset.target -= strength * 2.0f;
    } else {
        _left_width.target *= nearWidthScale;
        _left_height.target *= nearHeightScale;
        _right_width.target *= farWidthScale;
        _right_height.target *= farHeightScale;
        _left_y_offset.target -= strength * 2.0f;
        _right_y_offset.target += strength * 3.5f;
    }

    _left_width.target  = std::min(_left_width.target, _left_height.target * 0.78f);
    _right_width.target = std::min(_right_width.target, _right_height.target * 0.78f);
}

void FaceModel::scheduleNextBlink(uint32_t nowMs)
{
    uint32_t minimum = 3000;
    uint32_t maximum = 7200;
    switch (_expression) {
        case Expression::Idle:
            minimum = 6000;
            maximum = 14000;
            break;
        case Expression::Listening:
            minimum = 3000;
            maximum = 7000;
            break;
        case Expression::Thinking:
            minimum = 3500;
            maximum = 7000;
            break;
        case Expression::Daydreaming:
            minimum = 5000;
            maximum = 9000;
            break;
        case Expression::Proud:
            minimum = 3500;
            maximum = 6500;
            break;
        case Expression::Expectant:
            minimum = 2200;
            maximum = 4500;
            break;
        case Expression::Sulky:
            minimum = 5000;
            maximum = 9000;
            break;
        case Expression::Suspicious:
            minimum = 4000;
            maximum = 7200;
            break;
        case Expression::Delighted:
            minimum = 1800;
            maximum = 3800;
            break;
        case Expression::Startled:
            minimum = 1200;
            maximum = 3000;
            break;
        case Expression::Happy:
        case Expression::Playful:
        case Expression::Welcome:
        case Expression::ButtonMischief:
            minimum = 2500;
            maximum = 5000;
            break;
        case Expression::Sleepy:
            minimum = 6500;
            maximum = 11000;
            break;
        case Expression::Petted:
        case Expression::Shy:
            minimum = 3000;
            maximum = 6000;
            break;
        case Expression::Annoyed:
        case Expression::ButtonWatch:
            minimum = 4500;
            maximum = 8000;
            break;
        default:
            break;
    }
    _next_blink_ms = nowMs + randomRange(minimum, maximum);
}

void FaceModel::updateBlink(uint32_t nowMs)
{
    if (!_blink_active && deadlineReached(nowMs, _next_blink_ms)) {
        _blink_active   = true;
        _blink_start_ms = nowMs;
    }

    if (!_blink_active || !deadlineReached(nowMs, _blink_start_ms)) {
        _pose.blinkScale = 1.0f;
        return;
    }

    const uint32_t elapsed = nowMs - _blink_start_ms;
    if (elapsed >= BlinkDurationMs) {
        _blink_active    = false;
        _pose.blinkScale = 1.0f;
        scheduleNextBlink(nowMs);
        return;
    }

    const float phase            = static_cast<float>(elapsed) / static_cast<float>(BlinkDurationMs);
    constexpr float minimumScale = 0.055f;
    if (phase < 0.36f) {
        // Close quickly, but with zero velocity at the fully closed point.
        const float close = smoothstep(phase / 0.36f);
        _pose.blinkScale  = 1.0f - close * (1.0f - minimumScale);
    } else {
        // Opening is slower; this asymmetry reads as organic at small sizes.
        const float open = smoothstep((phase - 0.36f) / 0.64f);
        _pose.blinkScale = minimumScale + open * (1.0f - minimumScale);
    }
}

void FaceModel::updateAutonomousGaze(uint32_t nowMs)
{
    if (!deadlineReached(nowMs, _next_saccade_ms)) {
        return;
    }

    const int32_t randomX = static_cast<int32_t>(randomRange(0, 2000)) - 1000;
    const int32_t randomY = static_cast<int32_t>(randomRange(0, 2000)) - 1000;
    _autonomous_gaze_x    = static_cast<float>(randomX) / 1000.0f;
    _autonomous_gaze_y    = static_cast<float>(randomY) / 1000.0f;
    _next_saccade_ms      = nowMs + randomRange(1100, 2800);
}

uint32_t FaceModel::randomRange(uint32_t minimum, uint32_t maximum)
{
    _rng_state = _rng_state * 1664525U + 1013904223U;
    if (maximum <= minimum) {
        return minimum;
    }
    return minimum + (_rng_state % (maximum - minimum + 1U));
}

}  // namespace friday
