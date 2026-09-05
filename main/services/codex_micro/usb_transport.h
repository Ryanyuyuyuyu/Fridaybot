// SPDX-License-Identifier: MIT
#pragma once

#include "usb_descriptor.h"

namespace codex_micro::usb {

enum class EventType : uint8_t { Mounted, Unmounted, OutputReport, HostIdentity, Quota, Overflow };

struct Event {
    EventType type = EventType::Unmounted;
    // A bus wake is a fresh RPC session, but must preserve a manual host choice.
    bool resumed = false;
    uint32_t epoch = 0;
    size_t length = 0;
    uint8_t data[kQuotaBodySize] = {};
};

// Call from the Codex service task. With USB disabled begin() returns false
// and the remaining APIs are inert. USB enumeration alone is not a host claim.
bool begin();
bool poll(Event& event);
bool mounted();
uint32_t sessionEpoch();

// Waits at most 100 ms for an endpoint slot in the service task. Returns false
// on disconnect, epoch change or timeout. Never call from a USB callback.
// expectedEpoch belongs to the service's accepted Mounted event, not the
// transport's current epoch: old-route actions must never follow a new cable.
bool sendReport(const uint8_t* body, size_t size, uint32_t expectedEpoch);

// Fail-closed recovery for a lost button release. disconnect() immediately
// invalidates the session; the caller may connect() after at least 100 ms.
void disconnect();
void connect();

}  // namespace codex_micro::usb
