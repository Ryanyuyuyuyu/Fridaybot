// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace codex_micro {

// An endpoint number may be reused after reconnect. Both epochs must match
// before an input retained across worker iterations can reach that endpoint.
struct ControlRouteStamp {
    uint32_t controlEpoch = 0;
    int32_t endpoint = -1;
    uint32_t linkEpoch = 0;

    bool operator==(const ControlRouteStamp& other) const
    {
        return controlEpoch == other.controlEpoch && endpoint == other.endpoint && linkEpoch == other.linkEpoch;
    }
    bool operator!=(const ControlRouteStamp& other) const { return !(*this == other); }
};

enum class ControlAdmission { Dispatch, Defer, Discard };
enum class ControlAttempt { Consumed, DeferredBeforeAttempt };

// Keep payloads in the existing bounded normal/release queues. The owner peeks
// their oldest sequence, inspects it here, and removes it only after Discard or
// a Consumed attempt. Defer ends the batch, so a later release cannot overtake
// its press and neither queue needs a third allocation or changed capacity.
//
// This class performs no waits. waiting() asks the worker to yield, then give
// the retained head first use of the NEXT iteration's normal USB budget. Never
// renew the budget inside a batch. Reconcile link events/epochs before dispatch.
class DeferredControlGate {
public:
    ControlAdmission inspect(uint32_t sequence, uint32_t queuedControlEpoch, const ControlRouteStamp& route,
                             bool routeReady, bool usb, bool budgetAvailable)
    {
        if (queuedControlEpoch != route.controlEpoch || !routeReady || route.endpoint < 0 ||
            (waiting_ && sequence_ == sequence && route_ != route)) {
            consumed(sequence);
            return ControlAdmission::Discard;
        }
        if (usb && !budgetAvailable) {
            retain(sequence, route);
            return ControlAdmission::Defer;
        }
        return ControlAdmission::Dispatch;
    }

    // Call after a dispatch. Only a write that never attempted to submit any
    // report may be deferred. A timeout/partial/uncertain write is Consumed and
    // must keep the transport's existing disconnect recovery policy.
    void complete(uint32_t sequence, ControlAttempt result, const ControlRouteStamp& attemptedRoute)
    {
        if (result == ControlAttempt::DeferredBeforeAttempt) {
            retain(sequence, attemptedRoute);
        } else {
            consumed(sequence);
        }
    }

    bool waiting() const { return waiting_; }
    void reset() { waiting_ = false; }

private:
    void retain(uint32_t sequence, const ControlRouteStamp& route)
    {
        if (!waiting_ || sequence_ != sequence) {
            sequence_ = sequence;
            route_ = route;
        }
        waiting_ = true;
    }
    void consumed(uint32_t sequence)
    {
        if (waiting_ && sequence_ == sequence) waiting_ = false;
    }

    bool waiting_ = false;
    uint32_t sequence_ = 0;
    ControlRouteStamp route_{};
};

// Merge two FIFO heads in enqueue order, including uint32 sequence wrap. The
// queues together are bounded far below half the uint32 range. Releases keep
// their independently reserved queue; only older presses precede a release.
constexpr bool releaseHeadFirst(bool hasNormal, uint32_t normalSequence, bool hasRelease, uint32_t releaseSequence)
{
    return hasRelease && (!hasNormal || static_cast<int32_t>(releaseSequence - normalSequence) < 0);
}

}  // namespace codex_micro
