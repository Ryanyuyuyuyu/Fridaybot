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
        close();
        return;
    }

    // A has no standalone action. Preserve the A+B launcher gesture without
    // triggering B's reaction or recalling Friday during the chord.
    if (GetHAL().btnB.wasPressed() && GetHAL().btnA.isPressed()) {
        return;
    }

    if ((_presence_state == PresenceState::Host || _presence_state == PresenceState::Departing ||
         _presence_state == PresenceState::AwaitingReturn) && GetHAL().btnB.wasPressed()) {
        if (_presence_state != PresenceState::AwaitingReturn) {
            recallFromHost(nowMs);
        }
        return;
    }

    // React on the debounced B press; ignore KeyManager's release-click events
    // so one push cannot replay the animation a second time.
    if (GetHAL().btnB.wasPressed() && GetHAL().btnA.isReleased()) {
        _face_model.react(friday::Reaction::ButtonB, nowMs, 0.84f, -0.78f);
    }
}
