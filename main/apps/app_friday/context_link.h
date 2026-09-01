/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "context_protocol.h"
#include "presence_protocol.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace friday::context {

struct Snapshot {
    CompanionContext context;
    uint32_t packetAgeMs = UINT32_MAX;
    uint8_t sequence = 0;
    bool connected   = false;
    bool fresh       = false;
    bool travelReady = false;
};

struct TransferSnapshot {
    presence::Transfer transfer;
    uint32_t generation = 0;
    uint32_t receivedAtMs = 0;
    bool valid = false;
};

class ContextLink {
public:
    static ContextLink& instance();

    void start();
    Snapshot snapshot(uint32_t nowMs) const;
    TransferSnapshot transferSnapshot() const;
    bool sendTransfer(const presence::Transfer& transfer);
    void setTravelActive(bool active);

    // Called only by the shared Bluedroid worker. These functions deliberately
    // do no rendering, allocation or emotion inference.
    bool acceptPacket(const uint8_t* bytes, size_t length, uint32_t nowMs);
    bool acceptTransferPacket(const uint8_t* bytes, size_t length, uint32_t nowMs);
    void setConnected(bool connected, uint16_t connectionHandle = UINT16_MAX);
    void setTransferSubscribed(bool subscribed);

private:
    std::atomic<uint32_t> _packed_packet{0};
    std::atomic<uint32_t> _last_packet_ms{0};
    std::atomic<uint32_t> _transfer_word{0};
    std::atomic<uint32_t> _transfer_generation{0};
    std::atomic<uint32_t> _transfer_received_ms{0};
    std::atomic<uint16_t> _connection_handle{UINT16_MAX};
    std::atomic<bool> _connected{false};
    std::atomic<bool> _transfer_subscribed{false};
    std::atomic<bool> _relaxed_connection_requested{false};
    std::atomic<bool> _travel_active{false};
    std::atomic<bool> _started{false};
};

}  // namespace friday::context
