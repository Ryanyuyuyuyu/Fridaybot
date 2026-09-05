// SPDX-License-Identifier: MIT
#pragma once

#include "usb_descriptor.h"

namespace codex_micro::usb {

enum class EventType : uint8_t { Mounted, Unmounted, OutputReport, HostIdentity, Quota, Overflow };

struct Event {
    EventType type = EventType::Unmounted;
    // A bus wake/software recovery may preserve manual choice after the service
    // confirms that the new session belongs to the same stable host identity.
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

// Start once per service worker iteration. All USB sends in that iteration
// share an 8 ms budget, protecting Friday's independent BLE audio queue.
void beginServiceCycle();
bool serviceBudgetAvailable();

// Waits only within the current service-cycle budget. A final RTOS delay can
// overshoot by at most one tick (1 ms in the USB build). Never call from a USB
// callback. Disconnect, epoch change or budget exhaustion returns false.
// expectedEpoch belongs to the service's accepted Mounted event, not the
// transport's current epoch: old-route actions must never follow a new cable.
bool sendReport(const uint8_t* body, size_t size, uint32_t expectedEpoch);

// Fail-closed recovery for a lost button release. disconnect() immediately
// invalidates the session; the caller may connect() after at least 100 ms.
// The next mount is marked resumed, subject to the service's host-ID check.
void disconnect();
void connect();

}  // namespace codex_micro::usb
