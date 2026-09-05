// SPDX-License-Identifier: MIT
#include "usb_transport.h"
#include "usb_write.h"
#include "sdkconfig.h"

#if CONFIG_CODEX_MICRO_USB_HID

#include "codex_micro_gatt_db.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"

#include <cstdio>
#include <cstring>

#if CONFIG_TINYUSB_HID_COUNT != 1
#error "Codex Micro USB requires exactly one HID interface"
#endif
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
#error "USB HID and USB Serial/JTAG cannot share the internal USB PHY"
#endif

namespace codex_micro::usb {
namespace {

static_assert(kRpcBodySize == detail::kReportBodySize);
static_assert(CFG_TUD_HID_EP_BUFSIZE >= kQuotaBodySize + 1);
constexpr char kTag[] = "codex_usb";
constexpr size_t kQueueSize = 96;  // More than one complete 4096-byte RPC.
portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;
Event queue[kQueueSize];
size_t queueHead = 0;
size_t queueCount = 0;
uint32_t epoch = 0;
bool linkMounted = false;
bool started = false;
bool intentionallyDisconnected = false;
bool resumeOnNextMount = false;
bool mountDelivered = false;
bool mountedByResume = false;
struct PendingTx {
    uint32_t token = 0;
    uint32_t epoch = 0;
    int64_t deadline = 0;
    bool pending = false;
    bool complete = false;
    bool accepted = false;
    uint8_t body[kRpcBodySize] = {};
};
PendingTx tx;
uint32_t nextTxToken = 0;
WriteBudget writeBudget;

// Receive/lifecycle callbacks only update this bounded queue; they never parse
// JSON or wait for locks owned by the service task. SOF separately services the
// single nonblocking outbound request on the USB task.
void linkChanged(bool nowMounted, bool resumed = false)
{
    portENTER_CRITICAL(&queueMux);
    if (nowMounted && intentionallyDisconnected) {
        portEXIT_CRITICAL(&queueMux);
        return;
    }
    if (nowMounted) {
        resumed = resumed || resumeOnNextMount;
        resumeOnNextMount = false;
    }
    linkMounted = nowMounted;
    mountDelivered = false;
    mountedByResume = resumed;
    ++epoch;
    tx.pending = false;
    tx.complete = true;
    tx.accepted = false;
    queueHead = 0;
    queueCount = 1;
    queue[0] = Event{};
    queue[0].type = nowMounted ? EventType::Mounted : EventType::Unmounted;
    queue[0].resumed = resumed;
    queue[0].epoch = epoch;
    portEXIT_CRITICAL(&queueMux);
}

void enqueue(EventType type, const uint8_t* body, size_t length)
{
    portENTER_CRITICAL(&queueMux);
    if (!linkMounted || intentionallyDisconnected) {
        portEXIT_CRITICAL(&queueMux);
        return;
    }
    if (queueCount == kQueueSize) {
        // A missing fragment must never join a later message. Invalidate every
        // queued report and notify the owner even if no further packet arrives.
        // Keep the physical session epoch: the owner discards RPC bytes through
        // the next frame boundary, while mounted/identity ownership stays valid.
        queueHead = 0;
        queueCount = 0;
        // Startup can be waiting for Bluetooth while USB has already received
        // many RPC fragments. Do not lose the mount needed to accept any later
        // report when clearing this queue.
        if (!mountDelivered) {
            queue[queueCount] = Event{};
            queue[queueCount].type = EventType::Mounted;
            queue[queueCount].resumed = mountedByResume;
            queue[queueCount++].epoch = epoch;
        }
        queue[queueCount] = Event{};
        queue[queueCount].type = EventType::Overflow;
        queue[queueCount++].epoch = epoch;
        portEXIT_CRITICAL(&queueMux);
        return;
    }
    Event& event = queue[(queueHead + queueCount) % kQueueSize];
    event.type = type;
    event.resumed = false;
    event.epoch = epoch;
    event.length = length;
    std::memcpy(event.data, body, length);
    ++queueCount;
    portEXIT_CRITICAL(&queueMux);
}

void deviceEvent(tinyusb_event_t* event, void*)
{
    if (event->id == TINYUSB_EVENT_ATTACHED) linkChanged(true);
    if (event->id == TINYUSB_EVENT_DETACHED) linkChanged(false);
}

const tusb_desc_device_t deviceDescriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64,
    .idVendor = kVendorId,
    .idProduct = kProductId,
    .bcdDevice = kDeviceRelease,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

const uint8_t configurationDescriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN, 0, 500),
    TUD_HID_INOUT_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE, sizeof(kReportDescriptor), 0x01, 0x81, 64, 1),
};
char serialNumber[13] = {};
const char languageId[] = {0x09, 0x04};
const char* stringDescriptors[] = {
    languageId, "Work Louder", "Codex Micro", serialNumber, "Codex Micro RPC",
};

}  // namespace

bool begin()
{
    if (started) return true;
    uint8_t mac[6] = {};
    if (esp_efuse_mac_get_default(mac) != ESP_OK) return false;
    std::snprintf(serialNumber, sizeof(serialNumber), "%02X%02X%02X%02X%02X%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
    config.descriptor.device = &deviceDescriptor;
    config.descriptor.full_speed_config = configurationDescriptor;
    config.descriptor.string = stringDescriptors;
    config.descriptor.string_count = sizeof(stringDescriptors) / sizeof(stringDescriptors[0]);
    config.event_cb = deviceEvent;
    const esp_err_t result = tinyusb_driver_install(&config);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "USB HID initialization failed: %s", esp_err_to_name(result));
        return false;
    }
    started = true;
    ESP_LOGI(kTag, "USB HID ready; waiting for Codex host handshake");
    return true;
}

bool poll(Event& event)
{
    portENTER_CRITICAL(&queueMux);
    if (queueCount == 0) {
        portEXIT_CRITICAL(&queueMux);
        return false;
    }
    event = queue[queueHead];
    if (event.type == EventType::Mounted && event.epoch == epoch) mountDelivered = true;
    queueHead = (queueHead + 1) % kQueueSize;
    --queueCount;
    portEXIT_CRITICAL(&queueMux);
    return true;
}

bool mounted()
{
    portENTER_CRITICAL(&queueMux);
    const bool result = linkMounted && !intentionallyDisconnected;
    portEXIT_CRITICAL(&queueMux);
    return result;
}

uint32_t sessionEpoch()
{
    portENTER_CRITICAL(&queueMux);
    const uint32_t result = epoch;
    portEXIT_CRITICAL(&queueMux);
    return result;
}

void beginServiceCycle() { writeBudget.start(esp_timer_get_time()); }
bool serviceBudgetAvailable() { return writeBudget.available(esp_timer_get_time()); }

bool sendReport(const uint8_t* body, size_t size, uint32_t expectedEpoch)
{
    if (!started || !body || size != kRpcBodySize || !mounted() || sessionEpoch() != expectedEpoch) return false;
    // This deadline is shared across the entire worker iteration. A long RPC
    // ID or a backlog of requests cannot multiply the wait by fragment count.
    const int64_t deadline = writeBudget.deadline();
    if (esp_timer_get_time() >= deadline) return false;
    portENTER_CRITICAL(&queueMux);
    if (!linkMounted || intentionallyDisconnected || epoch != expectedEpoch) {
        portEXIT_CRITICAL(&queueMux);
        return false;
    }
    const uint32_t token = ++nextTxToken;
    tx = PendingTx{};
    tx.token = token;
    tx.epoch = expectedEpoch;
    tx.deadline = deadline;
    tx.pending = true;
    std::memcpy(tx.body, body, size);
    portEXIT_CRITICAL(&queueMux);

    // Submit from the TinyUSB task's SOF callback, which serializes the epoch
    // check and HID submission with mount/reset callbacks. Sending directly
    // from this worker leaves a gap where a new Mac could mount after a check.
    tud_sof_cb_enable(true);
    bool accepted = false;
    do {
        portENTER_CRITICAL(&queueMux);
        const bool sameRequest = tx.token == token;
        const bool sameSession = epoch == expectedEpoch && linkMounted && !intentionallyDisconnected;
        const bool complete = sameRequest && tx.complete;
        accepted = sameRequest && sameSession && complete && tx.accepted;
        portEXIT_CRITICAL(&queueMux);
        if (!sameRequest || !sameSession || complete) break;
        vTaskDelay(1);
    } while (esp_timer_get_time() < deadline);

    // Only this single producer enables/disables SOF. A late callback cannot
    // disable a newer request or publish its completion into the next token.
    portENTER_CRITICAL(&queueMux);
    if (tx.token == token) {
        accepted = tx.complete && tx.accepted && epoch == expectedEpoch && linkMounted && !intentionallyDisconnected;
        tx.pending = false;
        tx.token = ++nextTxToken;
    }
    portEXIT_CRITICAL(&queueMux);
    tud_sof_cb_enable(false);
    return accepted;
}

// TinyUSB task only. No allocation, waiting or service-owned lock is needed.
void submitPendingReport()
{
    uint8_t body[kRpcBodySize];
    uint32_t token = 0;
    int64_t deadline = 0;
    portENTER_CRITICAL(&queueMux);
    if (!tx.pending || tx.complete) {
        portEXIT_CRITICAL(&queueMux);
        return;
    }
    if (!linkMounted || intentionallyDisconnected || tx.epoch != epoch || esp_timer_get_time() >= tx.deadline) {
        tx.pending = false;
        tx.complete = true;
        tx.accepted = false;
        portEXIT_CRITICAL(&queueMux);
        return;
    }
    token = tx.token;
    deadline = tx.deadline;
    std::memcpy(body, tx.body, sizeof(body));
    portEXIT_CRITICAL(&queueMux);

    if (!tud_hid_ready()) return;  // Retry on the next frame, within the deadline.
    // Recheck cancellation immediately before submission. Lifecycle callbacks
    // cannot run between here and tud_hid_report: all run on this USB task.
    portENTER_CRITICAL(&queueMux);
    const bool valid = tx.token == token && tx.pending && tx.epoch == epoch && linkMounted &&
                       !intentionallyDisconnected && esp_timer_get_time() < deadline;
    portEXIT_CRITICAL(&queueMux);
    if (!valid) return;
    const bool submitted = tud_hid_report(kRpcReportId, body, sizeof(body));
    if (!submitted) return;
    portENTER_CRITICAL(&queueMux);
    if (tx.token == token) {
        tx.pending = false;
        tx.complete = true;
        tx.accepted = true;
    }
    portEXIT_CRITICAL(&queueMux);
}

void disconnect()
{
    if (!started) return;
    portENTER_CRITICAL(&queueMux);
    intentionallyDisconnected = true;
    // A software reset must not claim priority again over a manually chosen
    // Mac. The service compares stable identities in case the cable moved
    // while disconnected; a different Mac still gets normal USB priority.
    resumeOnNextMount = true;
    portEXIT_CRITICAL(&queueMux);
    linkChanged(false);
    tud_disconnect();
}

void connect()
{
    if (!started) return;
    portENTER_CRITICAL(&queueMux);
    intentionallyDisconnected = false;
    portEXIT_CRITICAL(&queueMux);
    tud_connect();
}

// Called by TinyUSB below, always from its own task.
void receiveReport(uint8_t reportId, hid_report_type_t reportType, const uint8_t* body, size_t length)
{
    if (!body || length == 0) return;
    // Interrupt OUT delivers Report ID 0 and includes the real ID in byte 0.
    if (reportId == 0) {
        reportId = body[0];
        ++body;
        --length;
    }
    if (reportId == kRpcReportId && reportType == HID_REPORT_TYPE_OUTPUT) {
        if (length == kRpcBodySize + 1 && body[0] == kRpcReportId) { ++body; --length; }
        if (length == kRpcBodySize) enqueue(EventType::OutputReport, body, length);
    } else if (reportId == kIdentityReportId && reportType == HID_REPORT_TYPE_FEATURE) {
        if (length == kIdentityBodySize + 1 && body[0] == kIdentityReportId) { ++body; --length; }
        if (length == kIdentityBodySize) enqueue(EventType::HostIdentity, body, length);
    } else if (reportId == kQuotaReportId && reportType == HID_REPORT_TYPE_FEATURE) {
        if (length == kQuotaBodySize + 1 && body[0] == kQuotaReportId) { ++body; --length; }
        if (length == kQuotaBodySize) enqueue(EventType::Quota, body, length);
    }
}

void suspend() { linkChanged(false); }
void resume() { if (tud_mounted()) linkChanged(true, true); }

}  // namespace codex_micro::usb

extern "C" uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance)
{
    return instance == 0 ? codex_micro::usb::kReportDescriptor : nullptr;
}

extern "C" uint16_t tud_hid_get_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t*, uint16_t)
{
    // Feature reports are host-to-device only. No saved identity is disclosed.
    return 0;
}

extern "C" void tud_hid_set_report_cb(uint8_t instance, uint8_t reportId, hid_report_type_t type,
                                     uint8_t const* buffer, uint16_t length)
{
    if (instance == 0) codex_micro::usb::receiveReport(reportId, type, buffer, length);
}

extern "C" void tud_suspend_cb(bool)
{
    // C152 has a battery and no dedicated VBUS sense GPIO in this integration.
    // Bus inactivity (sleep or cable removal) must invalidate USB ownership.
    codex_micro::usb::suspend();
}

extern "C" void tud_resume_cb(void) { codex_micro::usb::resume(); }
extern "C" void tud_sof_cb(uint32_t) { codex_micro::usb::submitPendingReport(); }

#else

namespace codex_micro::usb {
bool begin() { return false; }
bool poll(Event&) { return false; }
bool mounted() { return false; }
uint32_t sessionEpoch() { return 0; }
void beginServiceCycle() {}
bool serviceBudgetAvailable() { return false; }
bool sendReport(const uint8_t*, size_t, uint32_t) { return false; }
void disconnect() {}
void connect() {}
}  // namespace codex_micro::usb

#endif
