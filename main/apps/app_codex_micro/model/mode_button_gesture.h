/*
 * SPDX-FileCopyrightText: 2026 Codex Micro for StopWatch contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>

namespace codex_micro_app::model {

enum class ModeButtonEvent : uint8_t {
    None,
    PttPress,
    PttRelease,
    ToggleMode,
};

struct ModeButtonGestureConfig {
    uint32_t minimumTapMs    = 40;
    uint32_t holdThresholdMs = 180;
    uint32_t maximumTapGapMs = 300;
};

class ModeButtonGesture {
public:
    explicit constexpr ModeButtonGesture(ModeButtonGestureConfig config = {}) : _config(config)
    {
    }

    ModeButtonEvent update(uint32_t now, bool pressed, bool pressedEdge, bool releasedEdge)
    {
        if (_state == State::AwaitSecond && now - _firstReleaseAt > _config.maximumTapGapMs) {
            _state = State::Idle;
        }

        if (pressedEdge) {
            if (_state == State::AwaitSecond) {
                _state = State::SecondDown;
            } else if (_state == State::Idle) {
                _state = State::FirstDown;
            }
            _pressAt = now;
        }

        if (pressed && (_state == State::FirstDown || _state == State::SecondDown || _state == State::PttOnlyDown) &&
            now - _pressAt >= _config.holdThresholdMs) {
            _state = State::PttActive;
            return ModeButtonEvent::PttPress;
        }

        if (!releasedEdge) {
            return ModeButtonEvent::None;
        }

        if (_state == State::PttActive) {
            reset();
            return ModeButtonEvent::PttRelease;
        }

        const uint32_t heldFor = now - _pressAt;
        if (_state == State::FirstDown) {
            if (heldFor < _config.minimumTapMs || heldFor >= _config.holdThresholdMs) {
                reset();
                return ModeButtonEvent::None;
            }
            _state          = State::AwaitSecond;
            _firstReleaseAt = now;
            return ModeButtonEvent::None;
        }

        if (_state == State::SecondDown) {
            if (heldFor < _config.minimumTapMs || heldFor >= _config.holdThresholdMs) {
                reset();
                return ModeButtonEvent::None;
            }
            reset();
            return ModeButtonEvent::ToggleMode;
        }

        if (_state == State::PttOnlyDown) {
            reset();
        }

        return ModeButtonEvent::None;
    }

    // A screen interaction invalidates a possible mode toggle, but it must not
    // release an active PTT hold. If A is still physically down, keep it as a
    // PTT-only candidate so a continued hold can cross the normal threshold.
    void cancelPendingToggle()
    {
        if (_state == State::AwaitSecond) {
            reset();
        } else if (_state == State::FirstDown || _state == State::SecondDown) {
            _state = State::PttOnlyDown;
        }
    }

    ModeButtonEvent cancel()
    {
        const bool releasePtt = _state == State::PttActive;
        reset();
        return releasePtt ? ModeButtonEvent::PttRelease : ModeButtonEvent::None;
    }

    void reset()
    {
        _state          = State::Idle;
        _pressAt        = 0;
        _firstReleaseAt = 0;
    }

private:
    enum class State : uint8_t {
        Idle,
        FirstDown,
        AwaitSecond,
        SecondDown,
        PttOnlyDown,
        PttActive,
    };

    ModeButtonGestureConfig _config;
    State _state             = State::Idle;
    uint32_t _pressAt        = 0;
    uint32_t _firstReleaseAt = 0;
};

}  // namespace codex_micro_app::model
