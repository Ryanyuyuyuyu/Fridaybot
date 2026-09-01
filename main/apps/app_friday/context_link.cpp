/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "context_link.h"

#include "services/codex_micro/codex_micro_service.h"

#include <array>

#include <esp_log.h>

namespace friday::context {
namespace {

constexpr char Tag[] = "Friday-BLE";

}  // namespace

ContextLink& ContextLink::instance()
{
    static ContextLink link;
    return link;
}

void ContextLink::start()
{
    bool expected = false;
    if (!_started.compare_exchange_strong(expected, true)) {
        return;
    }

    // Codex Micro owns the single process-lifetime Bluedroid host. begin() is
    // idempotent, so Friday can safely ensure that the shared transport is
    // alive without initializing a second BLE stack.
    if (!codex_micro::GetService().begin()) {
        ESP_LOGE(Tag, "shared Bluedroid service failed to start");
        _started.store(false);
        return;
    }
    ESP_LOGI(Tag, "context bridge ready");
}

Snapshot ContextLink::snapshot(uint32_t nowMs) const
{
    Snapshot result;
    result.connected   = _connected.load(std::memory_order_relaxed);
    result.travelReady = result.connected && _transfer_subscribed.load(std::memory_order_acquire);
    const uint32_t lastPacket = _last_packet_ms.load(std::memory_order_acquire);
    if (lastPacket != 0) {
        // `nowMs` belongs to the beginning of the face frame. A BLE callback
        // can publish a packet a few milliseconds later, before this snapshot
        // runs. Treat that tiny negative age as zero instead of letting an
        // unsigned subtraction wrap to roughly 49 days.
        result.packetAgeMs = clampedPacketAgeMs(nowMs, lastPacket);
    }
    result.fresh = result.connected && result.packetAgeMs <= PacketTimeoutMs;
    if (!result.fresh) {
        result.context.state = CompanionState::Offline;
        return result;
    }

    const uint32_t packed = _packed_packet.load(std::memory_order_relaxed);
    std::array<uint8_t, PacketSize> bytes{
        static_cast<uint8_t>(packed),
        static_cast<uint8_t>(packed >> 8U),
        static_cast<uint8_t>(packed >> 16U),
        static_cast<uint8_t>(packed >> 24U),
    };
    DecodedPacket decoded;
    if (decode(bytes.data(), bytes.size(), decoded)) {
        result.context  = decoded.context;
        result.sequence = decoded.sequence;
    } else {
        result.fresh = false;
    }
    return result;
}

TransferSnapshot ContextLink::transferSnapshot() const
{
    TransferSnapshot result;
    result.generation = _transfer_generation.load(std::memory_order_acquire);
    if (result.generation == 0) {
        return result;
    }

    const uint32_t word = _transfer_word.load(std::memory_order_relaxed);
    result.transfer.command = static_cast<presence::Command>(word & 0xFFU);
    result.transfer.direction = static_cast<presence::Direction>((word >> 8U) & 0xFFU);
    result.transfer.sequence = static_cast<uint8_t>((word >> 16U) & 0xFFU);
    result.receivedAtMs = _transfer_received_ms.load(std::memory_order_relaxed);
    result.valid = presence::validCommand(static_cast<uint8_t>(result.transfer.command)) &&
                   presence::validDirection(static_cast<uint8_t>(result.transfer.direction));
    return result;
}

bool ContextLink::sendTransfer(const presence::Transfer& transfer)
{
    const uint16_t handle = _connection_handle.load(std::memory_order_relaxed);
    if (!_connected.load(std::memory_order_acquire) ||
        !_transfer_subscribed.load(std::memory_order_acquire) || handle == UINT16_MAX) {
        return false;
    }

    const presence::Packet packet = presence::encode(transfer);
    if (!codex_micro::GetService().sendFridayTransfer(packet.data(), packet.size())) {
        ESP_LOGW(Tag, "travel notification could not be queued");
        return false;
    }
    ESP_LOGI(Tag, "travel command %u queued, sequence %u", static_cast<unsigned>(transfer.command),
             transfer.sequence);
    return true;
}

void ContextLink::setTravelActive(bool active)
{
    const bool previous = _travel_active.exchange(active, std::memory_order_acq_rel);
    if (previous == active || !_connected.load(std::memory_order_acquire)) {
        return;
    }

    if (!codex_micro::GetService().setFridayTravelActive(active)) {
        ESP_LOGW(Tag, "%s connection interval request could not be queued", active ? "travel" : "relaxed");
        return;
    }
    _relaxed_connection_requested.store(!active, std::memory_order_relaxed);
}

bool ContextLink::acceptPacket(const uint8_t* bytes, size_t length, uint32_t nowMs)
{
    DecodedPacket decoded;
    if (!decode(bytes, length, decoded)) {
        return false;
    }

    const uint32_t packed = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8U) |
                            (static_cast<uint32_t>(bytes[2]) << 16U) | (static_cast<uint32_t>(bytes[3]) << 24U);
    _packed_packet.store(packed, std::memory_order_relaxed);
    _last_packet_ms.store(nowMs, std::memory_order_release);

    // Keep the default fast interval for discovery and the first write. Once
    // Friday context is flowing, ask the shared host for the low-duty office
    // interval used by the original firmware.
    bool expected = false;
    if (!_travel_active.load(std::memory_order_acquire) &&
        _relaxed_connection_requested.compare_exchange_strong(expected, true) &&
        !codex_micro::GetService().setFridayTravelActive(false)) {
        _relaxed_connection_requested.store(false, std::memory_order_release);
        ESP_LOGW(Tag, "relaxed connection interval request could not be queued");
    }
    return true;
}

bool ContextLink::acceptTransferPacket(const uint8_t* bytes, size_t length, uint32_t nowMs)
{
    presence::Transfer transfer;
    if (!presence::decode(bytes, length, transfer)) {
        return false;
    }

    const uint32_t word = static_cast<uint32_t>(transfer.command) |
                          (static_cast<uint32_t>(transfer.direction) << 8U) |
                          (static_cast<uint32_t>(transfer.sequence) << 16U);
    _transfer_word.store(word, std::memory_order_relaxed);
    _transfer_received_ms.store(nowMs, std::memory_order_relaxed);
    _transfer_generation.fetch_add(1, std::memory_order_release);
    ESP_LOGI(Tag, "travel command %u received, sequence %u", static_cast<unsigned>(transfer.command),
             transfer.sequence);
    return true;
}

void ContextLink::setConnected(bool connected, uint16_t connectionHandle)
{
    const bool wasConnected = _connected.exchange(connected, std::memory_order_acq_rel);
    const uint16_t previousHandle =
        _connection_handle.exchange(connected ? connectionHandle : UINT16_MAX, std::memory_order_acq_rel);

    // Re-publishing the same selected Friday peer is deliberately idempotent.
    // A Codex-only connection can appear or disappear while this conn_id is
    // still active; that must not cancel Friday's travel state.
    const bool peerChanged = !connected || !wasConnected || previousHandle != connectionHandle;
    if (peerChanged) {
        _relaxed_connection_requested.store(false, std::memory_order_relaxed);
        _travel_active.store(false, std::memory_order_relaxed);
    }
    if (!connected || (wasConnected && previousHandle != connectionHandle)) {
        _last_packet_ms.store(0, std::memory_order_release);
    }
    if (!connected) {
        _transfer_subscribed.store(false, std::memory_order_release);
    }
}

void ContextLink::setTransferSubscribed(bool subscribed)
{
    _transfer_subscribed.store(subscribed, std::memory_order_release);
}

}  // namespace friday::context
