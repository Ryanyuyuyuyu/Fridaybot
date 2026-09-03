/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "capsule_link.h"

#include "services/codex_micro/codex_micro_service.h"

#include <esp_log.h>

namespace friday::capsule {
namespace {

constexpr char Tag[] = "Friday-Capsule";

uint32_t packAck(uint16_t session, FridayCapsuleAckStatus status, FridayCapsuleAckDetail detail)
{
    return static_cast<uint32_t>(session) | (static_cast<uint32_t>(status) << 16U) |
           (static_cast<uint32_t>(detail) << 24U);
}

}  // namespace

CapsuleLink& CapsuleLink::instance()
{
    static CapsuleLink link;
    return link;
}

void CapsuleLink::start()
{
    bool expected = false;
    if (!_started.compare_exchange_strong(expected, true)) {
        return;
    }
    if (!codex_micro::GetService().begin()) {
        ESP_LOGE(Tag, "shared Bluedroid service failed to start");
        _started.store(false);
        return;
    }
    ESP_LOGI(Tag, "stream bridge ready");
}

bool CapsuleLink::ready() const
{
    return _connected.load(std::memory_order_acquire) && _subscribed.load(std::memory_order_acquire);
}

bool CapsuleLink::sendPacket(const uint8_t* bytes, size_t length)
{
    if (!ready() || bytes == nullptr || length < 4) {
        return false;
    }
    if (!codex_micro::GetService().sendFridayCapsule(bytes, length)) {
        noteTransportFailure(friday_capsule_read_u16(bytes + 2), 0);
        return false;
    }
    return true;
}

void CapsuleLink::setStreamingActive(bool active)
{
    const bool previous = _streaming.exchange(active, std::memory_order_acq_rel);
    if (previous == active || !_connected.load(std::memory_order_acquire)) {
        return;
    }
    if (!codex_micro::GetService().setFridayCapsuleActive(active)) {
        ESP_LOGW(Tag, "%s interval request could not be queued", active ? "streaming" : "relaxed");
    }
}

AckSnapshot CapsuleLink::ackSnapshot() const
{
    AckSnapshot result;
    result.generation = _ack_generation.load(std::memory_order_acquire);
    if (result.generation == 0) {
        return result;
    }
    const uint32_t packed = _packed_ack.load(std::memory_order_relaxed);
    result.session = static_cast<uint16_t>(packed & 0xFFFFU);
    result.status = static_cast<FridayCapsuleAckStatus>((packed >> 16U) & 0xFFU);
    result.detail = static_cast<FridayCapsuleAckDetail>((packed >> 24U) & 0xFFU);
    result.receivedAtMs = _ack_received_at_ms.load(std::memory_order_relaxed);
    result.valid = result.session != 0 && result.status >= FridayCapsuleAckSaved &&
                   result.status <= FridayCapsuleAckError;
    return result;
}

bool CapsuleLink::acceptAck(const uint8_t* bytes, size_t length, uint32_t nowMs)
{
    if (!friday_capsule_valid_ack(bytes, length)) {
        return false;
    }
    const uint16_t session = friday_capsule_read_u16(bytes + 2);
    const auto status = static_cast<FridayCapsuleAckStatus>(bytes[1]);
    const auto detail = static_cast<FridayCapsuleAckDetail>(bytes[4]);
    _packed_ack.store(packAck(session, status, detail), std::memory_order_relaxed);
    _ack_received_at_ms.store(nowMs, std::memory_order_relaxed);
    _ack_generation.fetch_add(1, std::memory_order_release);
    ESP_LOGI(Tag, "ack status=%u detail=%u session=%u", static_cast<unsigned>(status),
             static_cast<unsigned>(detail), session);
    return true;
}

void CapsuleLink::noteTransportFailure(uint16_t session, uint32_t nowMs)
{
    if (session == 0) {
        return;
    }
    _packed_ack.store(packAck(session, FridayCapsuleAckError, FridayCapsuleAckDetailMissingFrame),
                      std::memory_order_relaxed);
    _ack_received_at_ms.store(nowMs, std::memory_order_relaxed);
    _ack_generation.fetch_add(1, std::memory_order_release);
}

void CapsuleLink::setConnected(bool connected)
{
    _connected.store(connected, std::memory_order_release);
    if (!connected) {
        _subscribed.store(false, std::memory_order_release);
        _streaming.store(false, std::memory_order_release);
    }
}

void CapsuleLink::setSubscribed(bool subscribed)
{
    _subscribed.store(subscribed, std::memory_order_release);
}

}  // namespace friday::capsule
