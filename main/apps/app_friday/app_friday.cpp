/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_friday.h"
#include "context_link.h"

#include <cmath>
#include <hal/hal.h>
#include <mooncake_log.h>

namespace {

uint32_t FridayLauncherColor = 0xDDE2E6;

}  // namespace

AppFriday::AppFriday()
{
    setAppInfo().name = "Friday";
    // The launcher draws Friday's two-eye glyph in code when no bitmap icon is
    // supplied, matching the vector-rendered face without another flash asset.
    setAppInfo().icon     = nullptr;
    setAppInfo().userData = &FridayLauncherColor;
}

void AppFriday::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
    friday::context::ContextLink::instance().start();
    friday::capsule::CapsuleLink::instance().start();
}

void AppFriday::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    const uint32_t now = GetHAL().millis();
    _key_manager       = std::make_unique<input::KeyManager>();
    _face_model.reset(now);
    _imu_sample        = {};
    _touch_active      = false;
    _touch_x_milli     = 0;
    _touch_y_milli     = 0;
    _gesture_pending   = 0;
    _last_imu_ms       = now - ImuSampleIntervalMs;
    _next_animation_ms = now;
    _stats_start_ms    = now;
    _rendered_frames   = 0;
    _late_frames       = 0;
    _last_context_sequence = 0;
    _context_was_fresh     = false;
    _presence_state          = PresenceState::Device;
    _transfer_started_ms     = now;
    _transfer_deadline_ms    = now;
    _transfer_sequence       = 0;
    _active_transfer_sequence = 0;
    _transfer_seed           = 0;
    _host_accepted           = false;
    _departure_announced     = false;
    _face_visible            = true;
    _capsule_state           = CapsuleState::Idle;
    _capsule_session         = 0;
    _capsule_sequence        = 0;
    _capsule_started_ms      = 0;
    _capsule_deadline_ms     = 0;
    _capsule_feedback_until_ms = 0;
    _capsule_total_samples   = 0;
    friday_capsule_adpcm_reset(&_capsule_adpcm);
    _capsule_input.clear();
    _capsule_input.reserve(882);
    _last_capsule_ack_generation = friday::capsule::CapsuleLink::instance().ackSnapshot().generation;
    _last_transfer_generation = friday::context::ContextLink::instance().transferSnapshot().generation;

    LvglLockGuard lock;
    _view = std::make_unique<friday::view::FaceView>();
    _view->onContactChanged = [this](bool active, float x, float y) {
        _touch_x_milli.store(static_cast<int32_t>(std::lround(x * 1000.0f)), std::memory_order_relaxed);
        _touch_y_milli.store(static_cast<int32_t>(std::lround(y * 1000.0f)), std::memory_order_relaxed);
        _touch_active.store(active, std::memory_order_release);
    };
    _view->onGesture = [this](const friday::view::TouchGestureEvent& gesture) {
        _gesture_x_milli.store(static_cast<int32_t>(std::lround(gesture.x * 1000.0f)),
                               std::memory_order_relaxed);
        _gesture_y_milli.store(static_cast<int32_t>(std::lround(gesture.y * 1000.0f)),
                               std::memory_order_relaxed);
        _gesture_dx_milli.store(static_cast<int32_t>(std::lround(gesture.deltaX * 1000.0f)),
                                std::memory_order_relaxed);
        _gesture_dy_milli.store(static_cast<int32_t>(std::lround(gesture.deltaY * 1000.0f)),
                                std::memory_order_relaxed);
        _gesture_pending.store(static_cast<uint8_t>(gesture.gesture) + 1U, std::memory_order_release);
    };
    _view->init(lv_screen_active());
    _view->setPose(_face_model.pose());
}

void AppFriday::onRunning()
{
    const uint32_t now = GetHAL().millis();
    updatePresenceTransfer(now);
    handleInputs(now);
    updateCapsule(GetHAL().millis());
    if (currentState() != mooncake::AppAbility::StateRunning) {
        return;
    }

    updateImu(now);
    updateCompanionContext(now);

    if (static_cast<int32_t>(now - _next_animation_ms) < 0) {
        return;
    }

    const uint32_t frameLateness = now - _next_animation_ms;
    if (frameLateness >= AnimationIntervalMs) {
        ++_late_frames;
    }

    // Advance against a fixed clock instead of `now`. Reset only after a
    // severe stall so normal render time never accumulates into frame drift.
    _next_animation_ms += AnimationIntervalMs;
    if (frameLateness >= AnimationIntervalMs * 4) {
        _next_animation_ms = now + AnimationIntervalMs;
    }

    _face_model.update(now, _imu_sample);

    {
        LvglLockGuard lock;
        if (_view) {
            _view->setPose(_face_model.pose());
            _view->setFaceVisible(_face_visible);
            updateCapsuleVisual(now);
        }
    }

    ++_rendered_frames;
    const uint32_t statsElapsed = now - _stats_start_ms;
    if (statsElapsed >= 5000) {
        const float fps = static_cast<float>(_rendered_frames) * 1000.0f / static_cast<float>(statsElapsed);
        mclog::tagInfo(getAppInfo().name, "animation {:.1f} fps, late frames: {}", fps, _late_frames);
        _stats_start_ms  = now;
        _rendered_frames = 0;
        _late_frames     = 0;
    }
}

void AppFriday::updatePresenceTransfer(uint32_t nowMs)
{
    auto& link = friday::context::ContextLink::instance();
    const auto transfer = link.transferSnapshot();
    if (transfer.valid && transfer.generation != _last_transfer_generation) {
        _last_transfer_generation = transfer.generation;
        const bool matchingSequence = transfer.transfer.sequence == _active_transfer_sequence;
        if (_presence_state != PresenceState::Device && !matchingSequence) {
            mclog::tagInfo(getAppInfo().name, "ignoring stale travel sequence {} (active {})",
                           transfer.transfer.sequence, _active_transfer_sequence);
        } else if (transfer.transfer.command == friday::presence::Command::HostAccept &&
                   _presence_state == PresenceState::Departing) {
            // Readiness is not ownership. The Mac must remain invisible until
            // the StopWatch publishes DeviceHidden below.
            _host_accepted = true;
            mclog::tagInfo(getAppInfo().name, "Mac ready for Friday, sequence {}",
                           transfer.transfer.sequence);
        } else if (transfer.transfer.command == friday::presence::Command::HostReturnBegin &&
                   (_presence_state == PresenceState::Host ||
                    _presence_state == PresenceState::AwaitingReturn)) {
            link.setTravelActive(true);
            _presence_state       = PresenceState::AwaitingReturn;
            // The low-duty BLE link can take roughly two seconds per
            // direction. ReturnBegin renews the lease so a long cross-display
            // exit cannot race the HostHidden visibility barrier.
            _transfer_deadline_ms = nowMs + 6500U;
            mclog::tagInfo(getAppInfo().name, "Mac return animation started, sequence {}",
                           transfer.transfer.sequence);
        } else if (transfer.transfer.command == friday::presence::Command::HostHidden &&
                   (_presence_state == PresenceState::Host ||
                    _presence_state == PresenceState::AwaitingReturn)) {
            // HostHidden is the return visibility barrier. Only now may the
            // StopWatch render the first frame of Friday's arrival.
            beginPresenceReturn(nowMs);
        } else if (transfer.transfer.command == friday::presence::Command::Cancel &&
                   _presence_state != PresenceState::Device) {
            beginPresenceReturn(nowMs);
        }
    }

    const auto context = link.snapshot(nowMs);
    if (_presence_state != PresenceState::Device && _presence_state != PresenceState::Returning &&
        !context.travelReady) {
        mclog::tagInfo(getAppInfo().name, "travel link lost; returning Friday to StopWatch");
        beginPresenceReturn(nowMs);
    }

    if (_presence_state == PresenceState::Departing) {
        const bool exitComplete = static_cast<int32_t>(nowMs - (_transfer_started_ms + 1040U)) >= 0;
        if (exitComplete && _host_accepted && !_departure_announced) {
            // Hide first, then publish the barrier. BLE latency can delay the
            // Mac, but can never create two simultaneously visible faces.
            _face_visible = false;
            {
                LvglLockGuard lock;
                if (_view) {
                    _view->setFaceVisible(false);
                }
            }
            friday::presence::Transfer departed;
            departed.command      = friday::presence::Command::DeviceHidden;
            departed.direction    = _transfer_direction;
            departed.sequence     = _active_transfer_sequence;
            departed.seed         = _transfer_seed;
            departed.expression   = static_cast<uint8_t>(_face_model.expression());
            departed.startDelayMs = 0;
            if (link.sendTransfer(departed)) {
                _departure_announced = true;
                _presence_state      = PresenceState::Host;
                link.setTravelActive(false);
                mclog::tagInfo(getAppInfo().name, "StopWatch hidden; Friday ownership moved to Mac, sequence {}",
                               departed.sequence);
            } else {
                beginPresenceReturn(nowMs);
            }
        }
        if (static_cast<int32_t>(nowMs - _transfer_deadline_ms) >= 0) {
            mclog::tagInfo(getAppInfo().name, "Mac did not accept travel offer; cancelling handoff");
            beginPresenceReturn(nowMs);
        }
    } else if (_presence_state == PresenceState::AwaitingReturn) {
        if (static_cast<int32_t>(nowMs - _transfer_deadline_ms) >= 0) {
            mclog::tagInfo(getAppInfo().name, "Mac return barrier timed out; restoring Friday locally");
            beginPresenceReturn(nowMs);
        }
    } else if (_presence_state == PresenceState::Returning &&
               static_cast<int32_t>(nowMs - _transfer_deadline_ms) >= 0) {
        _presence_state = PresenceState::Device;
        link.setTravelActive(false);
        mclog::tagInfo(getAppInfo().name, "Friday ownership returned to StopWatch");
    }
}

bool AppFriday::tryStartPresenceTransfer(float x, float y, uint32_t nowMs)
{
    if (_presence_state != PresenceState::Device || std::fabs(x) < 0.90f ||
        std::fabs(x) < std::fabs(y)) {
        return false;
    }

    auto& link = friday::context::ContextLink::instance();
    const auto snapshot = link.snapshot(nowMs);
    if (!snapshot.travelReady) {
        return false;
    }

    const int8_t edgeDirection = x < 0.0f ? -1 : 1;
    if (snapshot.fresh && snapshot.context.workDirection != 0 &&
        edgeDirection != snapshot.context.workDirection) {
        // The other bezel still behaves like the normal rubber edge. The
        // configured monitor side is the physical portal.
        return false;
    }

    _transfer_direction = x < 0.0f ? friday::presence::Direction::Left
                                   : friday::presence::Direction::Right;
    friday::presence::Transfer offer;
    offer.command       = friday::presence::Command::DeviceOffer;
    offer.direction     = _transfer_direction;
    offer.expression    = static_cast<uint8_t>(_face_model.expression());
    offer.poseVariant   = 0;
    offer.sequence      = ++_transfer_sequence;
    offer.seed          = static_cast<uint16_t>((nowMs ^ (nowMs >> 16U)) & 0xFFFFU);
    offer.startDelayMs  = 620;
    link.setTravelActive(true);
    if (!link.sendTransfer(offer)) {
        link.setTravelActive(false);
        return false;
    }

    _presence_state           = PresenceState::Departing;
    _transfer_started_ms      = nowMs;
    _transfer_deadline_ms     = nowMs + 4000U;
    _active_transfer_sequence = offer.sequence;
    _transfer_seed            = offer.seed;
    _host_accepted            = false;
    _departure_announced      = false;
    _face_visible             = true;
    _face_model.react(friday::Reaction::PortalExit, nowMs, static_cast<float>(edgeDirection), 0.0f);
    mclog::tagInfo(getAppInfo().name, "travel offer sent toward {}, sequence {}",
                   edgeDirection < 0 ? "left" : "right", offer.sequence);
    return true;
}

void AppFriday::recallFromHost(uint32_t nowMs)
{
    if (_presence_state == PresenceState::Departing) {
        friday::presence::Transfer cancel;
        cancel.command   = friday::presence::Command::Cancel;
        cancel.direction = _transfer_direction;
        cancel.sequence  = _active_transfer_sequence;
        friday::context::ContextLink::instance().sendTransfer(cancel);
        beginPresenceReturn(nowMs);
        return;
    }
    if (_presence_state != PresenceState::Host) {
        return;
    }

    friday::presence::Transfer recall;
    recall.command       = friday::presence::Command::DeviceRecall;
    recall.direction     = _transfer_direction;
    recall.sequence      = _active_transfer_sequence;
    recall.seed          = static_cast<uint16_t>(nowMs & 0xFFFFU);
    recall.startDelayMs  = 0;
    auto& link = friday::context::ContextLink::instance();
    link.setTravelActive(true);
    if (link.sendTransfer(recall)) {
        // Remain black until the Mac confirms its last visible frame has
        // disappeared. A timeout still guarantees local recovery.
        _presence_state       = PresenceState::AwaitingReturn;
        _transfer_deadline_ms = nowMs + 8000U;
    } else {
        link.setTravelActive(false);
    }
}

void AppFriday::beginPresenceReturn(uint32_t nowMs)
{
    const float direction = _transfer_direction == friday::presence::Direction::Left ? -1.0f : 1.0f;
    _presence_state       = PresenceState::Returning;
    _face_visible         = true;
    _transfer_started_ms  = nowMs;
    _transfer_deadline_ms = nowMs + 1500U;
    _face_model.react(friday::Reaction::PortalReturn, nowMs, direction, 0.0f);
}

void AppFriday::updateCompanionContext(uint32_t nowMs)
{
    const auto snapshot = friday::context::ContextLink::instance().snapshot(nowMs);
    if (snapshot.fresh) {
        if (!_context_was_fresh || snapshot.sequence != _last_context_sequence) {
            _face_model.setCompanionContext(snapshot.context, nowMs);
            _last_context_sequence = snapshot.sequence;
        }
    } else if (_context_was_fresh) {
        _face_model.setCompanionContext({}, nowMs);
        mclog::tagInfo(getAppInfo().name,
                       "companion context expired at age {} ms (connected: {}); local personality active",
                       snapshot.packetAgeMs, snapshot.connected);
    }
    _context_was_fresh = snapshot.fresh;
}

void AppFriday::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    _touch_active    = false;
    _gesture_pending = 0;
    if (_capsule_state == CapsuleState::Recording) {
        finishCapsule(FridayCapsuleEndCanceled, GetHAL().millis());
    }
    friday::capsule::CapsuleLink::instance().setStreamingActive(false);
    _capsule_state = CapsuleState::Idle;
    _key_manager.reset();
    GetHAL().stopVibrate();
    friday::context::ContextLink::instance().setTravelActive(false);

    LvglLockGuard lock;
    _view.reset();
}

void AppFriday::updateImu(uint32_t nowMs)
{
    if (nowMs - _last_imu_ms < ImuSampleIntervalMs) {
        return;
    }
    _last_imu_ms = nowMs;

    GetHAL().updateImuData();
    const auto& sample = GetHAL().getImuData();
    _imu_sample.accelX = sample.accelX;
    _imu_sample.accelY = sample.accelY;
    _imu_sample.accelZ = sample.accelZ;
    _imu_sample.gyroX  = sample.gyroX;
    _imu_sample.gyroY  = sample.gyroY;
    _imu_sample.gyroZ  = sample.gyroZ;
}

void AppFriday::handleInputs(uint32_t nowMs)
{
    const bool touchActive = _touch_active.load(std::memory_order_acquire);
    const float touchX     = static_cast<float>(_touch_x_milli.load(std::memory_order_relaxed)) / 1000.0f;
    const float touchY     = static_cast<float>(_touch_y_milli.load(std::memory_order_relaxed)) / 1000.0f;
    if (_presence_state == PresenceState::Device || _presence_state == PresenceState::Returning) {
        _face_model.setTouchContact(touchActive, touchX, touchY, nowMs);
    } else {
        _face_model.setTouchContact(false, touchX, touchY, nowMs);
    }

    const uint8_t pendingGesture = _gesture_pending.exchange(0, std::memory_order_acquire);
    if (pendingGesture != 0) {
        const auto gesture = static_cast<friday::view::TouchGesture>(pendingGesture - 1U);
        const float x = static_cast<float>(_gesture_x_milli.load(std::memory_order_relaxed)) / 1000.0f;
        const float y = static_cast<float>(_gesture_y_milli.load(std::memory_order_relaxed)) / 1000.0f;
        const float dx = static_cast<float>(_gesture_dx_milli.load(std::memory_order_relaxed)) / 1000.0f;
        const float dy = static_cast<float>(_gesture_dy_milli.load(std::memory_order_relaxed)) / 1000.0f;

        if (_presence_state == PresenceState::Host || _presence_state == PresenceState::Departing) {
            recallFromHost(nowMs);
        } else if (_presence_state == PresenceState::Device) {
        switch (gesture) {
            case friday::view::TouchGesture::Tap:
                _face_model.react(friday::Reaction::Tap, nowMs, x, y);
                break;
            case friday::view::TouchGesture::Hold:
                _face_model.react(friday::Reaction::Pet, nowMs, x, y);
                break;
            case friday::view::TouchGesture::Drag:
                if (tryStartPresenceTransfer(x, y, nowMs)) {
                    break;
                }
                if (std::fabs(x) >= 0.72f || std::fabs(y) >= 0.72f) {
                    _face_model.react(friday::Reaction::EdgeBounce, nowMs, x, y);
                } else {
                    _face_model.react(friday::Reaction::DragRelease, nowMs, x, y);
                }
                break;
            case friday::view::TouchGesture::Swipe:
                _face_model.react(friday::Reaction::Swipe, nowMs, dx, dy);
                break;
        }
        }
    }

    if (!_key_manager) {
        return;
    }

    const auto keyEvent = _key_manager->update();

    if (keyEvent == input::KeyEvent::GoHome) {
        if (_capsule_state == CapsuleState::Recording) {
            finishCapsule(FridayCapsuleEndCanceled, nowMs);
        }
        close();
        return;
    }

    // A belongs exclusively to Flash Capsule in Friday. If B joins it, cancel
    // the partial stream immediately and let the existing A+B hold gesture
    // return to the launcher once KeyManager reaches its Home threshold.
    if (GetHAL().btnB.wasPressed() && GetHAL().btnA.isPressed()) {
        if (_capsule_state == CapsuleState::Recording) {
            finishCapsule(FridayCapsuleEndCanceled, nowMs);
        }
        return;
    }

    if (GetHAL().btnA.wasPressed() && GetHAL().btnB.isReleased()) {
        beginCapsule(nowMs);
    }
    if (GetHAL().btnA.wasReleased() && _capsule_state == CapsuleState::Recording) {
        const FridayCapsuleEndReason reason = nowMs - _capsule_started_ms < CapsuleMinimumMs
            ? FridayCapsuleEndTooShort
            : FridayCapsuleEndReleased;
        finishCapsule(reason, nowMs);
    }

    if ((_presence_state == PresenceState::Host || _presence_state == PresenceState::Departing ||
         _presence_state == PresenceState::AwaitingReturn) && GetHAL().btnB.wasPressed()) {
        if (_presence_state != PresenceState::AwaitingReturn) {
            recallFromHost(nowMs);
        }
        return;
    }

    if (GetHAL().btnB.wasPressed() && GetHAL().btnA.isReleased()) {
        _face_model.react(friday::Reaction::ButtonB, nowMs, 0.84f, -0.78f);
    }

    switch (keyEvent) {
        case input::KeyEvent::GoPrevious:
        case input::KeyEvent::GoNext:
            // Physical button actions are handled on their debounced edges.
            break;
        case input::KeyEvent::GoHome:
            break;
        default:
            break;
    }
}

void AppFriday::beginCapsule(uint32_t nowMs)
{
    if (_capsule_state == CapsuleState::Recording || _capsule_state == CapsuleState::AwaitingSave) {
        return;
    }
    auto& link = friday::capsule::CapsuleLink::instance();
    if (!link.ready() || GetHAL().getAudioSampleRate() != 44100) {
        failCapsule(nowMs);
        return;
    }

    uint16_t session = static_cast<uint16_t>((nowMs ^ (nowMs >> 16U)) & 0xFFFFU);
    if (session == 0 || session == _capsule_session) {
        ++session;
        if (session == 0) {
            session = 1;
        }
    }
    _capsule_session = session;
    _capsule_sequence = 0;
    _capsule_total_samples = 0;
    _capsule_started_ms = nowMs;
    _capsule_deadline_ms = nowMs + FRIDAY_CAPSULE_MAX_DURATION_MS;
    _capsule_feedback_until_ms = 0;
    friday_capsule_adpcm_reset(&_capsule_adpcm);
    friday_capsule_resampler_reset(&_capsule_resampler);

    const size_t length = friday_capsule_encode_start(_capsule_packet.data(), _capsule_session);
    if (length == 0 || !link.sendPacket(_capsule_packet.data(), length)) {
        failCapsule(nowMs);
        return;
    }
    link.setStreamingActive(true);
    _capsule_state = CapsuleState::Recording;
    mclog::tagInfo(getAppInfo().name, "Flash Capsule recording started, session {}", _capsule_session);
}

void AppFriday::captureCapsuleAudio()
{
    auto& link = friday::capsule::CapsuleLink::instance();
    if (!link.ready()) {
        failCapsule(GetHAL().millis());
        return;
    }

    GetHAL().audioRecord(_capsule_input, FRIDAY_CAPSULE_CAPTURE_CHUNK_MS, CapsuleMicGainDb);
    if (_capsule_input.size() != 882) {
        failCapsule(GetHAL().millis());
        return;
    }
    friday_capsule_resample_44100_to_16000(_capsule_input.data(), _capsule_resampled.data(),
                                           &_capsule_resampler);

    for (size_t offset = 0; offset < _capsule_resampled.size(); offset += FRIDAY_CAPSULE_FRAME_SAMPLES) {
        int16_t predictor = 0;
        uint8_t stepIndex = 0;
        friday_capsule_adpcm_encode(_capsule_resampled.data() + offset, _capsule_encoded.data(),
                                    &_capsule_adpcm, &predictor, &stepIndex);
        const size_t length = friday_capsule_encode_audio(
            _capsule_packet.data(), _capsule_session, _capsule_sequence++, predictor, stepIndex,
            _capsule_encoded.data());
        if (length == 0 || !link.sendPacket(_capsule_packet.data(), length)) {
            failCapsule(GetHAL().millis());
            return;
        }
        _capsule_total_samples += FRIDAY_CAPSULE_FRAME_SAMPLES;
    }
}

void AppFriday::finishCapsule(FridayCapsuleEndReason reason, uint32_t nowMs)
{
    if (_capsule_state != CapsuleState::Recording) {
        return;
    }
    auto& link = friday::capsule::CapsuleLink::instance();
    const size_t length = friday_capsule_encode_end(_capsule_packet.data(), _capsule_session,
                                                    reason, _capsule_total_samples);
    const bool queued = length != 0 && link.sendPacket(_capsule_packet.data(), length);
    link.setStreamingActive(false);

    if (!queued || reason == FridayCapsuleEndTransportError) {
        failCapsule(nowMs);
        return;
    }
    if (reason == FridayCapsuleEndTooShort || reason == FridayCapsuleEndCanceled) {
        _capsule_state = CapsuleState::Idle;
        mclog::tagInfo(getAppInfo().name, "Flash Capsule discarded, reason {}", static_cast<unsigned>(reason));
        return;
    }
    _capsule_state = CapsuleState::AwaitingSave;
    _capsule_deadline_ms = nowMs + CapsuleAckTimeoutMs;
    mclog::tagInfo(getAppInfo().name, "Flash Capsule awaiting Mac save, session {}", _capsule_session);
}

void AppFriday::failCapsule(uint32_t nowMs)
{
    friday::capsule::CapsuleLink::instance().setStreamingActive(false);
    _capsule_state = CapsuleState::Failed;
    _capsule_feedback_until_ms = nowMs + 1600U;
    mclog::tagWarn(getAppInfo().name, "Flash Capsule failed, session {}", _capsule_session);
}

void AppFriday::updateCapsule(uint32_t nowMs)
{
    auto& link = friday::capsule::CapsuleLink::instance();
    const auto ack = link.ackSnapshot();
    if (ack.generation != _last_capsule_ack_generation) {
        _last_capsule_ack_generation = ack.generation;
        if (ack.valid && ack.session == _capsule_session) {
            if (ack.status == FridayCapsuleAckSaved && _capsule_state == CapsuleState::AwaitingSave) {
                _capsule_state = CapsuleState::Saved;
                _capsule_feedback_until_ms = nowMs + 1500U;
                _face_model.react(friday::Reaction::Play, nowMs);
            } else if (ack.status == FridayCapsuleAckDiscarded &&
                       ack.detail == FridayCapsuleAckDetailTooShort) {
                _capsule_state = CapsuleState::Idle;
            } else if (_capsule_state == CapsuleState::Recording ||
                       _capsule_state == CapsuleState::AwaitingSave) {
                failCapsule(nowMs);
            }
        }
    }

    if (_capsule_state == CapsuleState::Recording) {
        if (!link.ready()) {
            failCapsule(nowMs);
            return;
        }
        if (static_cast<int32_t>(nowMs - _capsule_deadline_ms) >= 0) {
            finishCapsule(FridayCapsuleEndTimeLimit, nowMs);
            return;
        }
        captureCapsuleAudio();
    } else if (_capsule_state == CapsuleState::AwaitingSave &&
               static_cast<int32_t>(nowMs - _capsule_deadline_ms) >= 0) {
        failCapsule(nowMs);
    } else if ((_capsule_state == CapsuleState::Saved || _capsule_state == CapsuleState::Failed) &&
               static_cast<int32_t>(nowMs - _capsule_feedback_until_ms) >= 0) {
        _capsule_state = CapsuleState::Idle;
    }
}

void AppFriday::updateCapsuleVisual(uint32_t nowMs)
{
    if (!_view) {
        return;
    }
    friday::view::CapsuleVisualState visual = friday::view::CapsuleVisualState::Hidden;
    float progress = 0.0f;
    switch (_capsule_state) {
        case CapsuleState::Idle:
            break;
        case CapsuleState::Recording:
            visual = friday::view::CapsuleVisualState::Recording;
            progress = std::min(1.0f, static_cast<float>(nowMs - _capsule_started_ms) /
                                           static_cast<float>(FRIDAY_CAPSULE_MAX_DURATION_MS));
            break;
        case CapsuleState::AwaitingSave:
            visual = friday::view::CapsuleVisualState::AwaitingSave;
            progress = static_cast<float>(nowMs % 800U) / 800.0f;
            break;
        case CapsuleState::Saved:
            visual = friday::view::CapsuleVisualState::Saved;
            progress = 1.0f;
            break;
        case CapsuleState::Failed:
            visual = friday::view::CapsuleVisualState::Failed;
            progress = 1.0f;
            break;
    }
    _view->setCapsuleVisual(visual, progress);
}
