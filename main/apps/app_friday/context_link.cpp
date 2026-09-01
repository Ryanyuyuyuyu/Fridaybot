/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "context_link.h"

#include <array>
#include <cstring>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_hs_adv.h>
#include <host/ble_uuid.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <os/os_mbuf.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

namespace friday::context {
namespace {

constexpr char Tag[] = "Friday-BLE";

// Canonical UUIDs:
//   46524944-4159-0001-8000-00805F9B34FB (service)
//   46524944-4159-0002-8000-00805F9B34FB (context characteristic)
//   46524944-4159-0003-8000-00805F9B34FB (presence transfer characteristic)
const ble_uuid128_t ServiceUuid = BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                                                   0x01, 0x00, 0x59, 0x41, 0x44, 0x49, 0x52, 0x46);
const ble_uuid128_t ContextUuid = BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                                                   0x02, 0x00, 0x59, 0x41, 0x44, 0x49, 0x52, 0x46);
const ble_uuid128_t TransferUuid = BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                                                    0x03, 0x00, 0x59, 0x41, 0x44, 0x49, 0x52, 0x46);

uint8_t OwnAddressType = 0;
uint16_t TransferValueHandle = 0;

uint32_t monotonicMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

int characteristicAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* context, void*)
{
    auto& link = ContextLink::instance();
    if (context->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        std::array<uint8_t, PacketSize> packet{};
        uint16_t length = 0;
        const int result = ble_hs_mbuf_to_flat(context->om, packet.data(), packet.size(), &length);
        if (result != 0 || !link.acceptPacket(packet.data(), length, monotonicMs())) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        return 0;
    }

    if (context->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const Snapshot current = link.snapshot(monotonicMs());
        const Packet packet = encode(current.context.state, current.context.workDirection, current.sequence);
        return os_mbuf_append(context->om, packet.data(), packet.size()) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

int transferAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* context, void*)
{
    if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    std::array<uint8_t, presence::PacketSize> packet{};
    uint16_t length = 0;
    const int result = ble_hs_mbuf_to_flat(context->om, packet.data(), packet.size(), &length);
    if (result != 0 || !ContextLink::instance().acceptTransferPacket(packet.data(), length, monotonicMs())) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    return 0;
}

const ble_gatt_chr_def Characteristics[] = {
    {
        .uuid         = &ContextUuid.u,
        .access_cb    = characteristicAccess,
        .arg          = nullptr,
        .descriptors  = nullptr,
        .flags        = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .min_key_size = 0,
        .val_handle   = nullptr,
        .cpfd         = nullptr,
    },
    {
        .uuid         = &TransferUuid.u,
        .access_cb    = transferAccess,
        .arg          = nullptr,
        .descriptors  = nullptr,
        .flags        = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle   = &TransferValueHandle,
        .cpfd         = nullptr,
    },
    {},
};

const ble_gatt_svc_def Services[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &ServiceUuid.u,
        .includes        = nullptr,
        .characteristics = Characteristics,
    },
    {},
};

void advertise();

int gapEvent(ble_gap_event* event, void*)
{
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ContextLink::instance().setConnected(true, event->connect.conn_handle);
                ESP_LOGI(Tag, "companion connected");
            } else {
                advertise();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ContextLink::instance().setConnected(false);
            ESP_LOGI(Tag, "companion disconnected; returning to local mode");
            advertise();
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == TransferValueHandle) {
                ContextLink::instance().setTransferSubscribed(event->subscribe.cur_notify != 0);
                ESP_LOGI(Tag, "travel notifications %s", event->subscribe.cur_notify ? "ready" : "disabled");
            }
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            advertise();
            return 0;

        default:
            return 0;
    }
}

void advertise()
{
    ble_hs_adv_fields fields{};
    fields.flags                 = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name                  = reinterpret_cast<uint8_t*>(const_cast<char*>("Friday"));
    fields.name_len              = 6;
    fields.name_is_complete      = 1;
    fields.uuids128              = const_cast<ble_uuid128_t*>(&ServiceUuid);
    fields.num_uuids128          = 1;
    fields.uuids128_is_complete  = 1;

    int result = ble_gap_adv_set_fields(&fields);
    if (result != 0) {
        ESP_LOGE(Tag, "cannot set advertising data: %d", result);
        return;
    }

    ble_gap_adv_params parameters{};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    parameters.itvl_min  = BLE_GAP_ADV_ITVL_MS(1000);
    parameters.itvl_max  = BLE_GAP_ADV_ITVL_MS(1200);
    result = ble_gap_adv_start(OwnAddressType, nullptr, BLE_HS_FOREVER, &parameters, gapEvent, nullptr);
    if (result != 0 && result != BLE_HS_EALREADY) {
        ESP_LOGE(Tag, "cannot start advertising: %d", result);
    }
}

void onReset(int reason)
{
    ContextLink::instance().setConnected(false);
    ESP_LOGW(Tag, "host reset: %d", reason);
}

void onSync()
{
    // The same StopWatch previously advertised other firmwares (for example
    // Codex Micro) under its public controller address. A distinct,
    // deterministic static-random address keeps macOS from reusing that stale
    // GATT cache while remaining stable across Friday reboots.
    std::array<uint8_t, 6> publicAddress{};
    int result = esp_read_mac(publicAddress.data(), ESP_MAC_BT);
    std::array<uint8_t, 6> fridayAddress{
        publicAddress[5],
        publicAddress[4],
        publicAddress[3],
        publicAddress[2],
        publicAddress[1],
        static_cast<uint8_t>((publicAddress[0] & 0x3FU) | 0xC0U),
    };
    // Presence protocol v1 adds a GATT characteristic. Change the stable
    // Friday identity once so CoreBluetooth cannot reuse the previous cached
    // service layout from the context-only firmware.
    fridayAddress[1] ^= 0x02U;
    if (result == ESP_OK) {
        result = ble_hs_id_set_rnd(fridayAddress.data());
    }
    if (result == 0) {
        OwnAddressType = BLE_OWN_ADDR_RANDOM;
    }
    if (result != 0) {
        ESP_LOGE(Tag, "cannot select BLE address: %d", result);
        return;
    }
    advertise();
}

void hostTask(void*)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

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

    const esp_err_t initResult = nimble_port_init();
    if (initResult != ESP_OK) {
        ESP_LOGE(Tag, "NimBLE init failed: %s", esp_err_to_name(initResult));
        _started.store(false);
        return;
    }

    ble_hs_cfg.reset_cb = onReset;
    ble_hs_cfg.sync_cb  = onSync;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int result = ble_gatts_count_cfg(Services);
    if (result == 0) {
        result = ble_gatts_add_svcs(Services);
    }
    if (result == 0) {
        result = ble_svc_gap_device_name_set("Friday");
    }
    if (result != 0) {
        ESP_LOGE(Tag, "GATT setup failed: %d", result);
        _started.store(false);
        return;
    }

    nimble_port_freertos_init(hostTask);
    ESP_LOGI(Tag, "context service ready");
}

Snapshot ContextLink::snapshot(uint32_t nowMs) const
{
    Snapshot result;
    result.connected = _connected.load(std::memory_order_relaxed);
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
        !_transfer_subscribed.load(std::memory_order_acquire) || handle == UINT16_MAX ||
        TransferValueHandle == 0) {
        return false;
    }

    const presence::Packet packet = presence::encode(transfer);
    os_mbuf* payload = ble_hs_mbuf_from_flat(packet.data(), packet.size());
    if (payload == nullptr) {
        return false;
    }
    const int result = ble_gatts_notify_custom(handle, TransferValueHandle, payload);
    if (result != 0) {
        ESP_LOGW(Tag, "travel notification failed: %d", result);
        return false;
    }
    ESP_LOGI(Tag, "travel command %u sent, sequence %u", static_cast<unsigned>(transfer.command),
             transfer.sequence);
    return true;
}

void ContextLink::setTravelActive(bool active)
{
    const bool previous = _travel_active.exchange(active, std::memory_order_acq_rel);
    const uint16_t handle = _connection_handle.load(std::memory_order_relaxed);
    if (previous == active || !_connected.load(std::memory_order_acquire) || handle == UINT16_MAX) {
        return;
    }

    ble_gap_upd_params parameters{};
    if (active) {
        // Travel packets are tiny, but an idle latency of three events can add
        // visible black gaps between screens. Use a responsive link only for
        // the handoff itself, then return to the low-duty office connection.
        parameters.itvl_min = BLE_GAP_CONN_ITVL_MS(15);
        parameters.itvl_max = BLE_GAP_CONN_ITVL_MS(30);
        parameters.latency  = 0;
        _relaxed_connection_requested.store(false, std::memory_order_relaxed);
    } else {
        parameters.itvl_min = BLE_GAP_CONN_ITVL_MS(200);
        parameters.itvl_max = BLE_GAP_CONN_ITVL_MS(400);
        parameters.latency  = 3;
        _relaxed_connection_requested.store(true, std::memory_order_relaxed);
    }
    parameters.supervision_timeout = 600;
    const int result = ble_gap_update_params(handle, &parameters);
    if (result != 0) {
        ESP_LOGW(Tag, "%s connection interval request failed: %d", active ? "travel" : "relaxed", result);
    }
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

    // Keep the default fast connection interval for GATT discovery and the
    // first context write. Only then ask macOS for a relaxed, low-duty link.
    bool expected = false;
    const uint16_t handle = _connection_handle.load(std::memory_order_relaxed);
    if (handle != UINT16_MAX && !_travel_active.load(std::memory_order_acquire) &&
        _relaxed_connection_requested.compare_exchange_strong(expected, true)) {
        ble_gap_upd_params parameters{};
        parameters.itvl_min            = BLE_GAP_CONN_ITVL_MS(200);
        parameters.itvl_max            = BLE_GAP_CONN_ITVL_MS(400);
        parameters.latency             = 3;
        parameters.supervision_timeout = 600;
        const int result = ble_gap_update_params(handle, &parameters);
        if (result != 0) {
            ESP_LOGW(Tag, "connection interval request failed: %d", result);
        }
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
    _connected.store(connected, std::memory_order_release);
    _connection_handle.store(connected ? connectionHandle : UINT16_MAX, std::memory_order_relaxed);
    _relaxed_connection_requested.store(false, std::memory_order_relaxed);
    _travel_active.store(false, std::memory_order_relaxed);
    if (!connected) {
        _last_packet_ms.store(0, std::memory_order_release);
        _transfer_subscribed.store(false, std::memory_order_release);
    }
}

void ContextLink::setTransferSubscribed(bool subscribed)
{
    _transfer_subscribed.store(subscribed, std::memory_order_release);
}

}  // namespace friday::context
