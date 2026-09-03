/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "capsule_protocol.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace friday::capsule {

struct AckSnapshot {
    uint16_t session = 0;
    FridayCapsuleAckStatus status = FridayCapsuleAckError;
    FridayCapsuleAckDetail detail = FridayCapsuleAckDetailNone;
    uint32_t receivedAtMs = 0;
    uint32_t generation = 0;
    bool valid = false;
};

class CapsuleLink {
public:
    static CapsuleLink& instance();

    void start();
    bool ready() const;
    bool sendPacket(const uint8_t* bytes, size_t length);
    void setStreamingActive(bool active);
    AckSnapshot ackSnapshot() const;

    // Called only by the shared Bluedroid worker. The capsule protocol remains
    // Friday-owned even though the hardware has one process-wide BLE host.
    bool acceptAck(const uint8_t* bytes, size_t length, uint32_t nowMs);
    void noteTransportFailure(uint16_t session, uint32_t nowMs);
    void setConnected(bool connected);
    void setSubscribed(bool subscribed);

private:
    std::atomic<uint32_t> _packed_ack{0};
    std::atomic<uint32_t> _ack_generation{0};
    std::atomic<uint32_t> _ack_received_at_ms{0};
    std::atomic<bool> _connected{false};
    std::atomic<bool> _subscribed{false};
    std::atomic<bool> _streaming{false};
    std::atomic<bool> _started{false};
};

}  // namespace friday::capsule
