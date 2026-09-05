// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Codex Micro for StopWatch contributors

#include "services/codex_micro/codex_micro_service.h"

#include "apps/app_friday/capsule/capsule_link.h"
#include "apps/app_friday/capsule/capsule_protocol.h"
#include "apps/app_friday/context_link.h"
#include "services/codex_micro/codex_micro_gatt_db.h"
#include "services/codex_micro/usb_transport.h"
#include "services/codex_micro/usb_write.h"
#include "services/codex_micro/usb_resume_policy.h"
#include "services/codex_micro/ble_sessions.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

namespace codex_micro {
namespace {

constexpr char kTag[]                    = "codex_micro";
constexpr uint16_t kUsbConnectionId = 0xFFFE;
constexpr uint16_t kGattAppId            = 0x0C0D;
constexpr size_t kMaxConnections         = 8;
constexpr size_t kMaxHandlesPerService   = detail::kHidCount;
constexpr UBaseType_t kControlQueueDepth = 32;
constexpr UBaseType_t kWriteQueueDepth   = 12;
constexpr UBaseType_t kCommandQueueDepth = 24;
constexpr UBaseType_t kReleaseQueueDepth = 24;
constexpr UBaseType_t kCapsuleQueueDepth = 24;
constexpr uint32_t kWorkerStackBytes     = 12 * 1024;
constexpr UBaseType_t kWorkerPriority    = 5;
constexpr uint32_t kRpcAssemblyTimeoutMs = 1000;
constexpr uint32_t kReleaseRetryMs       = 25;
constexpr uint32_t kAdvertisingRetryMs   = 500;
constexpr uint32_t kStackRetryMs         = 1000;
constexpr size_t kMaxEffectLength        = 31;
constexpr uint8_t kGattSchemaRevision    = 4;
constexpr char kNvsNamespace[]           = "codex-mic";
constexpr char kNvsGattRevisionKey[]     = "gatt-rev";

uint32_t nowMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

bool addressEquals(const uint8_t* lhs, const uint8_t* rhs)
{
    return std::memcmp(lhs, rhs, ESP_BD_ADDR_LEN) == 0;
}

enum class ControlEventType : uint8_t {
    kRegistered,
    kAttributeTableCreated,
    kServiceStarted,
    kConnected,
    kDisconnected,
    kMtuChanged,
    kCongestionChanged,
    kSecurityRequest,
    kAuthenticationComplete,
    kBondRemoved,
    kAdvertisingDataConfigured,
    kScanResponseConfigured,
    kAdvertisingStarted,
};

struct ControlEvent {
    uint32_t linkGeneration;
    ControlEventType type;
    esp_gatt_if_t gattsIf;
    esp_gatt_status_t gattStatus;
    esp_bt_status_t btStatus;
    uint16_t connId;
    uint16_t value;
    uint8_t instanceId;
    bool flag;
    uint8_t address[ESP_BD_ADDR_LEN];
    uint8_t handleCount;
    uint16_t handles[kMaxHandlesPerService];
};

struct WriteEvent {
    uint32_t linkGeneration;
    esp_gatt_if_t gattsIf;
    uint16_t connId;
    uint16_t handle;
    uint16_t offset;
    uint16_t length;
    bool needResponse;
    bool prepared;
    uint8_t address[ESP_BD_ADDR_LEN];
    uint8_t data[detail::kMaxQuotaSize];
};

enum class CommandType : uint8_t {
    kBattery,
    kSelectHost,
    kKey,
    kJoystick,
    kFridayTransfer,
    kFridayTravelMode,
    kFridayCapsuleMode,
};

struct CapsuleFrame {
    uint16_t length = 0;
    uint8_t data[FRIDAY_CAPSULE_MAX_PACKET_SIZE] = {};
};

struct Command {
    CommandType type;
    uint32_t sequence;
    uint32_t controlEpoch;
    char hostId[97];
    uint8_t percentage;
    bool charging;
    uint8_t action;
    int8_t agent;
    float angle;
    float distance;
    char key[64];
    uint8_t fridayPacket[friday::presence::PacketSize];
    bool fridayTravelActive;
    bool fridayCapsuleActive;
};

struct Connection {
    uint32_t linkGeneration = 0;
    bool usb                         = false;
    bool rpcReady                    = false;
    uint32_t lastRpcAtMs              = 0;
    std::string hostId;
    std::string hostName;
    bool legacyIdentity              = true;
    bool identityConfirmed            = false;
    std::array<Thread, 6> threads{};
    Quota quota{};
    RateLimitUsage rateLimits{};
    bool active                      = false;
    uint16_t id                      = 0;
    uint8_t address[ESP_BD_ADDR_LEN] = {};
    uint16_t mtu                     = 23;
    bool inputNotifications          = false;
    bool batteryNotifications        = false;
    bool congested                   = false;
    bool secure                      = false;
    bool fridayPeer                  = false;
    bool fridayTransferNotifications = false;
    bool fridayCapsuleNotifications  = false;
    uint32_t rpcLastFragmentAtMs     = 0;
    std::string rpcBuffer;
    bool rpcDiscardUntilNewline = false;
};

struct HostBinding {
    std::array<uint8_t, ESP_BD_ADDR_LEN> address{};
    std::string id;
    std::string name;
};

struct HeldKey {
    bool active  = false;
    int8_t agent = -1;
    char key[64] = {};
};

struct PendingRelease {
    bool active        = false;
    uint32_t retryAtMs = 0;
    Command command{};
};

enum class TableId : uint8_t {
    kDeviceInfo = 0,
    kHid        = 1,
    kBattery    = 2,
    kQuota      = 3,
    kFriday     = 4,
    // Append only: original table order/counts preserve paired Friday handles.
    kHostIdentity = 5,
    kCount      = 6,
};

constexpr uint8_t toIndex(TableId id)
{
    return static_cast<uint8_t>(id);
}

}  // namespace

struct Service::Impl {
    Impl() = default;

    bool begin();
    bool enqueueSelectHost(const std::string& hostId);
    bool processUsb();
    void acceptHostIdentity(Connection& connection, const uint8_t* bytes, size_t length);
    void registerHost(Connection& connection);
    void refreshRouting();
    void publishTelemetry();
    bool releaseOldRoute(int32_t endpoint);
    void loadHosts();
    void saveHosts();
    void applyBinding(Connection& connection);
    State snapshot() const;
    void enqueueBattery(uint8_t percentage, bool charging);
    void enqueueKey(const char* key, uint8_t action, int8_t agent, uint32_t expectedEpoch);
    void enqueueJoystick(float angle, float distance, uint32_t expectedEpoch);
    bool enqueueFridayTransfer(const uint8_t* packet, size_t length);
    bool enqueueFridayTravelMode(bool active);
    bool enqueueFridayCapsule(const uint8_t* packet, size_t length);
    bool enqueueFridayCapsuleMode(bool active);

    static void workerEntry(void* context);
    static void gattsCallback(esp_gatts_cb_event_t event, esp_gatt_if_t gattsIf, esp_ble_gatts_cb_param_t* param);
    static void gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);

    void worker();
    bool initializeBluetooth();
    void configureSecurity();
    void createTable(TableId id);
    void configureAdvertising();
    void startAdvertising();
    bool processAdvertisingRetry();
    void scheduleAdvertisingRetry();
    void beginBondMigration();
    void requestNextBondRemoval();
    bool commitGattSchemaRevision();
    void registerGattApplication();
    bool processStackRetry();

    void enqueueControl(const ControlEvent& event);
    void enqueueWrite(esp_gatt_if_t gattsIf, const esp_ble_gatts_cb_param_t& parameters);
    void processControlEvent(const ControlEvent& event);
    void processWrite(const WriteEvent& write);
    void processCommand(const Command& command);
    bool processQueuedCommands();
    bool processCapsuleFrames();
    bool processPendingReleases();
    bool processRpcTimeouts();
    void schedulePendingRelease(const Command& command);
    void cancelOlderPendingRelease(const Command& command);
    void handleCallbackQueueLoss(bool controlLost, bool writeLost);
    void handleReleaseQueueLoss();

    void handleRegistration(const ControlEvent& event);
    void handleTableCreated(const ControlEvent& event);
    void handleServiceStarted(const ControlEvent& event);
    void handleConnection(bool connected, uint16_t connId, const uint8_t* address, uint32_t generation);
    void handleMtu(uint16_t connId, uint16_t mtu);
    void handleCongestion(uint16_t connId, bool congested);
    void handleCccdWrite(const WriteEvent& write);
    void handleOutputReport(const WriteEvent& write);
    void handleQuotaWrite(const WriteEvent& write);
    void handleFridayContextWrite(const WriteEvent& write, Connection& connection);
    void handleFridayTransferWrite(const WriteEvent& write, Connection& connection);
    void handleFridayCapsuleAckWrite(const WriteEvent& write, Connection& connection);

    bool handleRpc(const JsonDocument& request, uint16_t connectionId);
    void noteHostRpc(uint16_t connectionId);
    void updateThreads(JsonArrayConst values, uint16_t connectionId);
    void sendMethodNotFound(JsonVariantConst id, uint16_t connectionId);
    void sendSuccess(JsonVariantConst id, uint16_t connectionId);
    bool sendJson(const std::string& json, int32_t targetConnection = -1);
    bool sendJsonToConnection(const std::string& framed, Connection& connection);
    bool sendReport(const uint8_t* report, Connection& connection);
    bool sendKeyNow(const char* key, uint8_t action, int8_t agent);
    bool sendJoystickNow(float angle, float distance);
    bool sendFridayTransferNow(const uint8_t* packet, size_t length);
    bool sendFridayCapsuleNow(const uint8_t* packet, size_t length);
    void updateFridayConnectionIntervals(bool active);
    void setBatteryNow(uint8_t percentage, bool charging);
    void rememberKeyState(const char* key, uint8_t action, int8_t agent);

    Connection* findConnection(uint16_t connectionId);
    size_t connectionCount() const;
    void clearHostRpcLocked();
    void clearRpcAssembly(Connection& connection);
    void clearAllRpcAssemblies();
    void markFridayPeer(Connection& connection);
    void syncFridayLinkState();

    uint16_t* handlesFor(TableId id);
    size_t handleCountFor(TableId id) const;
    uint16_t serviceHandleFor(TableId id) const;
    TableId tableForServiceHandle(uint16_t serviceHandle) const;

    static std::atomic<Impl*> callbackTarget;

    std::atomic<bool> started{false};
    std::atomic<bool> controlQueueLost{false};
    std::atomic<bool> writeQueueLost{false};
    std::atomic<bool> releaseQueueLost{false};
    std::atomic<uint32_t> nextCommandSequence{1};
    QueueHandle_t controlQueue           = nullptr;
    QueueHandle_t writeQueue             = nullptr;
    QueueHandle_t commandQueue           = nullptr;
    QueueHandle_t releaseQueue           = nullptr;
    QueueHandle_t capsuleQueue           = nullptr;
    mutable SemaphoreHandle_t stateMutex = nullptr;
    TaskHandle_t workerTask              = nullptr;

    State state{};
    HostSelection hostSelection;
    std::vector<HostBinding> hostBindings;
    std::string savedHosts;
    std::atomic<uint32_t> controlEpoch{1};
    BleSessions callbackSessions;
    portMUX_TYPE callbackSessionMux = portMUX_INITIALIZER_UNLOCKED;
    int32_t routedConnection = -1;
    std::string routedHostId;
    Connection usbConnection{};
    uint32_t usbEpoch = 0;
    uint32_t usbReconnectAtMs = 0;
    usb::ResumePreference usbResumePreference;
    std::array<Connection, kMaxConnections> connections{};
    esp_gatt_if_t gattsIf                          = ESP_GATT_IF_NONE;
    uint16_t deviceInfoHandles[detail::kDiCount]   = {};
    uint16_t hidHandles[detail::kHidCount]         = {};
    uint16_t batteryHandles[detail::kBatteryCount] = {};
    uint16_t quotaHandles[detail::kQuotaCount]     = {};
    uint16_t fridayHandles[detail::kFridayCount]   = {};
    uint16_t hostIdentityHandles[detail::kHostIdentityCount] = {};
    std::array<bool, toIndex(TableId::kCount)> servicesStarted{};
    bool advertisingDataPending   = false;
    bool scanResponsePending      = false;
    bool advertisingDataReady     = false;
    bool scanResponseReady        = false;
    bool advertisingActive        = false;
    uint32_t advertisingRetryAtMs = 0;

    bool bondMigrationComplete = false;
    bool bondRemovalInFlight   = false;
    size_t bondRemovalIndex    = 0;
    std::vector<std::array<uint8_t, ESP_BD_ADDR_LEN>> bondsToRemove;
    uint32_t bondMigrationRetryAtMs    = 0;
    bool gattRegistrationRequested     = false;
    bool gattRegistered                = false;
    uint32_t gattRegistrationRetryAtMs = 0;

    uint8_t batteryPercentage    = 100;
    bool charging                = false;
    bool hostRpcConnectionValid  = false;
    uint16_t hostRpcConnectionId = 0;
    std::array<HeldKey, 16> heldKeys{};
    std::array<PendingRelease, 20> pendingReleases{};
    bool joystickHeld       = false;
    float heldJoystickAngle = 0.0f;
    bool fridayTravelActive = false;
    bool fridayCapsuleActive = false;
    uint16_t selectedFridayConnectionId = UINT16_MAX;
};

std::atomic<Service::Impl*> Service::Impl::callbackTarget{nullptr};

namespace {

esp_ble_adv_params_t kAdvertisingParams = {
    .adv_int_min       = 0x20,
    .adv_int_max       = 0x40,
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .peer_addr         = {},
    .peer_addr_type    = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

}  // namespace

bool Service::Impl::begin()
{
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) {
        return true;
    }

    if (stateMutex == nullptr) {
        stateMutex = xSemaphoreCreateMutex();
    }
    if (controlQueue == nullptr) {
        controlQueue = xQueueCreate(kControlQueueDepth, sizeof(ControlEvent));
    }
    if (writeQueue == nullptr) {
        writeQueue = xQueueCreate(kWriteQueueDepth, sizeof(WriteEvent));
    }
    if (commandQueue == nullptr) {
        commandQueue = xQueueCreate(kCommandQueueDepth, sizeof(Command));
    }
    if (releaseQueue == nullptr) {
        releaseQueue = xQueueCreate(kReleaseQueueDepth, sizeof(Command));
    }
    if (capsuleQueue == nullptr) {
        capsuleQueue = xQueueCreate(kCapsuleQueueDepth, sizeof(CapsuleFrame));
    }
    if (stateMutex == nullptr || controlQueue == nullptr || writeQueue == nullptr || commandQueue == nullptr ||
        releaseQueue == nullptr || capsuleQueue == nullptr) {
        ESP_LOGE(kTag, "failed to allocate BLE worker primitives");
        if (controlQueue != nullptr) {
            vQueueDelete(controlQueue);
            controlQueue = nullptr;
        }
        if (writeQueue != nullptr) {
            vQueueDelete(writeQueue);
            writeQueue = nullptr;
        }
        if (commandQueue != nullptr) {
            vQueueDelete(commandQueue);
            commandQueue = nullptr;
        }
        if (releaseQueue != nullptr) {
            vQueueDelete(releaseQueue);
            releaseQueue = nullptr;
        }
        if (capsuleQueue != nullptr) {
            vQueueDelete(capsuleQueue);
            capsuleQueue = nullptr;
        }
        if (stateMutex != nullptr) {
            vSemaphoreDelete(stateMutex);
            stateMutex = nullptr;
        }
        started.store(false);
        return false;
    }

    xQueueReset(controlQueue);
    xQueueReset(writeQueue);
    xQueueReset(commandQueue);
    xQueueReset(releaseQueue);
    xQueueReset(capsuleQueue);
    controlQueueLost.store(false);
    writeQueueLost.store(false);
    releaseQueueLost.store(false);
    callbackTarget.store(this, std::memory_order_release);
    const BaseType_t created =
        xTaskCreate(workerEntry, "codex_micro_ble", kWorkerStackBytes, this, kWorkerPriority, &workerTask);
    if (created != pdPASS) {
        ESP_LOGE(kTag, "failed to create BLE worker task");
        callbackTarget.store(nullptr, std::memory_order_release);
        started.store(false);
        return false;
    }
    return true;
}

State Service::Impl::snapshot() const
{
    State copy;
    if (stateMutex == nullptr) {
        return copy;
    }
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    copy = state;
    xSemaphoreGive(stateMutex);
    return copy;
}

void Service::Impl::enqueueBattery(uint8_t percentage, bool isCharging)
{
    if (!started.load(std::memory_order_acquire) || commandQueue == nullptr) {
        return;
    }
    Command command{};
    command.type       = CommandType::kBattery;
    command.sequence   = nextCommandSequence.fetch_add(1, std::memory_order_relaxed);
    command.percentage = std::min<uint8_t>(percentage, 100);
    command.charging   = isCharging;
    if (xQueueSend(commandQueue, &command, 0) != pdTRUE) {
        ESP_LOGW(kTag, "battery command dropped: queue full");
    }
}

void Service::Impl::enqueueKey(const char* key, uint8_t action, int8_t agent, uint32_t expectedEpoch)
{
    if (!started.load(std::memory_order_acquire) || commandQueue == nullptr || releaseQueue == nullptr) {
        return;
    }
    Command command{};
    command.controlEpoch = expectedEpoch == UINT32_MAX ? controlEpoch.load(std::memory_order_acquire) : expectedEpoch;
    command.type     = CommandType::kKey;
    command.sequence = nextCommandSequence.fetch_add(1, std::memory_order_relaxed);
    command.action   = action;
    command.agent    = agent;
    if (key != nullptr) {
        std::strncpy(command.key, key, sizeof(command.key) - 1);
    }
    QueueHandle_t target = action == 0 ? releaseQueue : commandQueue;
    if (xQueueSend(target, &command, 0) != pdTRUE) {
        if (action == 0) {
            releaseQueueLost.store(true, std::memory_order_release);
            ESP_LOGE(kTag, "key release queue overflow");
        } else {
            ESP_LOGW(kTag, "key press dropped: queue full");
        }
    }
}

void Service::Impl::enqueueJoystick(float angle, float distance, uint32_t expectedEpoch)
{
    if (!started.load(std::memory_order_acquire) || commandQueue == nullptr || releaseQueue == nullptr) {
        return;
    }
    Command command{};
    command.controlEpoch = expectedEpoch == UINT32_MAX ? controlEpoch.load(std::memory_order_acquire) : expectedEpoch;
    command.type         = CommandType::kJoystick;
    command.sequence     = nextCommandSequence.fetch_add(1, std::memory_order_relaxed);
    command.angle        = angle;
    command.distance     = distance;
    QueueHandle_t target = distance <= 0.0f ? releaseQueue : commandQueue;
    if (xQueueSend(target, &command, 0) != pdTRUE) {
        if (distance <= 0.0f) {
            releaseQueueLost.store(true, std::memory_order_release);
            ESP_LOGE(kTag, "joystick release queue overflow");
        } else {
            ESP_LOGW(kTag, "joystick press dropped: queue full");
        }
    }
}

bool Service::Impl::enqueueFridayTransfer(const uint8_t* packet, size_t length)
{
    if (!started.load(std::memory_order_acquire) || commandQueue == nullptr || packet == nullptr ||
        length != friday::presence::PacketSize) {
        return false;
    }
    Command command{};
    command.type     = CommandType::kFridayTransfer;
    command.sequence = nextCommandSequence.fetch_add(1, std::memory_order_relaxed);
    std::memcpy(command.fridayPacket, packet, length);
    if (xQueueSend(commandQueue, &command, 0) != pdTRUE) {
        ESP_LOGW(kTag, "Friday transfer dropped: queue full");
        return false;
    }
    return true;
}

bool Service::Impl::enqueueFridayTravelMode(bool active)
{
    if (!started.load(std::memory_order_acquire) || commandQueue == nullptr) {
        return false;
    }
    Command command{};
    command.type               = CommandType::kFridayTravelMode;
    command.sequence           = nextCommandSequence.fetch_add(1, std::memory_order_relaxed);
    command.fridayTravelActive = active;
    if (xQueueSend(commandQueue, &command, 0) != pdTRUE) {
        ESP_LOGW(kTag, "Friday connection-mode update dropped: queue full");
        return false;
    }
    return true;
}

bool Service::Impl::enqueueFridayCapsule(const uint8_t* packet, size_t length)
{
    if (!started.load(std::memory_order_acquire) || capsuleQueue == nullptr || packet == nullptr ||
        length < 4 || length > FRIDAY_CAPSULE_MAX_PACKET_SIZE ||
        packet[0] != FRIDAY_CAPSULE_PROTOCOL_VERSION) {
        return false;
    }
    CapsuleFrame frame{};
    frame.length = static_cast<uint16_t>(length);
    std::memcpy(frame.data, packet, length);
    if (xQueueSend(capsuleQueue, &frame, 0) != pdTRUE) {
        const uint16_t session = friday_capsule_read_u16(packet + 2);
        friday::capsule::CapsuleLink::instance().noteTransportFailure(session, nowMs());
        ESP_LOGW(kTag, "Friday capsule frame dropped: dedicated queue full");
        return false;
    }
    return true;
}

bool Service::Impl::enqueueFridayCapsuleMode(bool active)
{
    if (!started.load(std::memory_order_acquire) || commandQueue == nullptr) {
        return false;
    }
    Command command{};
    command.type = CommandType::kFridayCapsuleMode;
    command.sequence = nextCommandSequence.fetch_add(1, std::memory_order_relaxed);
    command.fridayCapsuleActive = active;
    if (xQueueSend(commandQueue, &command, 0) != pdTRUE) {
        ESP_LOGW(kTag, "Friday capsule connection-mode update dropped: queue full");
        return false;
    }
    return true;
}

bool Service::Impl::enqueueSelectHost(const std::string& hostId)
{
    if (!started.load() || hostId.empty() || hostId.size() > 96) return false;
    Command command{};
    command.type = CommandType::kSelectHost;
    command.sequence = nextCommandSequence.fetch_add(1);
    std::memcpy(command.hostId, hostId.c_str(), hostId.size() + 1);
    return xQueueSend(commandQueue, &command, 0) == pdTRUE;
}

void Service::Impl::applyBinding(Connection& connection)
{
    for (const auto& binding : hostBindings) {
        if (addressEquals(binding.address.data(), connection.address)) {
            connection.hostId = binding.id;
            connection.hostName = binding.name;
            connection.legacyIdentity = false;
            return;
        }
    }
    char alias[32];
    std::snprintf(alias, sizeof(alias), "ble-%02x%02x%02x%02x%02x%02x", connection.address[0],
                  connection.address[1], connection.address[2], connection.address[3], connection.address[4],
                  connection.address[5]);
    connection.hostId = alias;
    char label[32];
    std::snprintf(label, sizeof(label), "Mac %02X%02X", connection.address[4], connection.address[5]);
    connection.hostName = label;
    connection.legacyIdentity = true;
}

void Service::Impl::acceptHostIdentity(Connection& connection, const uint8_t* bytes, size_t length)
{
    if (bytes == nullptr || length == 0 || length > 128) return;
    // Feature reports are NUL padded; BLE writes contain exactly the JSON.
    const void* end = std::memchr(bytes, 0, length);
    if (end) length = static_cast<const uint8_t*>(end) - bytes;
    JsonDocument doc;
    if (deserializeJson(doc, bytes, length) || !doc.is<JsonObjectConst>() ||
        !doc["version"].is<unsigned>() || doc["version"].as<unsigned>() != 1 ||
        !doc["hostId"].is<const char*>() || !doc["name"].is<const char*>()) return;
    std::string id = doc["hostId"].as<const char*>();
    std::string name = doc["name"].as<const char*>();
    if (id.size() != 36 || name.empty() || name.size() > 64) return;
    for (size_t i = 0; i < id.size(); ++i) {
        const bool hyphen = i == 8 || i == 13 || i == 18 || i == 23;
        if (hyphen ? id[i] != '-' : !std::isxdigit(static_cast<unsigned char>(id[i]))) return;
        id[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(id[i])));
    }
    if (std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; })) return;
    // A live physical transport cannot change identity mid-session. Renames are fine.
    if (connection.identityConfirmed && connection.hostId != id) return;
    if (!connection.usb) {
        char alias[32];
        std::snprintf(alias, sizeof(alias), "ble-%02x%02x%02x%02x%02x%02x", connection.address[0],
                      connection.address[1], connection.address[2], connection.address[3], connection.address[4], connection.address[5]);
        hostSelection.migrateIdentity(alias, id, name);
    }
    connection.identityConfirmed = true;
    connection.hostId = id;
    connection.hostName = name;
    connection.legacyIdentity = false;
    hostSelection.rememberHost(id, name);
    if (!connection.usb) {
        auto binding = std::find_if(hostBindings.begin(), hostBindings.end(), [&connection](const HostBinding& item) {
            return addressEquals(item.address.data(), connection.address);
        });
        if (binding == hostBindings.end()) {
            if (hostBindings.size() == 16) hostBindings.erase(hostBindings.begin());
            hostBindings.push_back(HostBinding{});
            binding = hostBindings.end() - 1;
            std::memcpy(binding->address.data(), connection.address, ESP_BD_ADDR_LEN);
        }
        binding->id = id;
        binding->name = name;
        // CoreBluetooth and the OS HID client can use separate logical links.
        for (auto& peer : connections) {
            if (peer.active && addressEquals(peer.address, connection.address)) {
                peer.identityConfirmed = true;
                peer.hostId = id;
                peer.hostName = name;
                peer.legacyIdentity = false;
                registerHost(peer);
            }
        }
    } else {
        usbResumePreference.identityConfirmed(id);
        registerHost(connection);
    }
    refreshRouting();
}

void Service::Impl::registerHost(Connection& connection)
{
    if (!connection.active || !connection.secure || !connection.inputNotifications || !connection.rpcReady ||
        connection.hostId.empty()) return;
    const bool accepted = hostSelection.registerReady(connection.id, connection.usb ? Transport::Usb : Transport::Ble,
                                                      connection.hostId, connection.hostName, connection.legacyIdentity);
    if (accepted && connection.usb) {
        const std::string preference = usbResumePreference.takeForReadyHost(connection.hostId);
        if (!preference.empty()) hostSelection.selectHost(preference);
    }
}

bool Service::Impl::releaseOldRoute(int32_t endpoint)
{
    if (endpoint < 0 || findConnection(static_cast<uint16_t>(endpoint)) == nullptr) return true;
    bool delivered = true;
    for (const auto& held : heldKeys) {
        if (!held.active) continue;
        JsonDocument message;
        message["method"] = "v.oai.hid";
        auto params = message["params"].to<JsonObject>();
        params["k"] = held.key;
        params["act"] = 0;
        if (held.agent >= 0) params["ag"] = held.agent;
        std::string json;
        serializeJson(message, json);
        delivered = sendJson(json, endpoint) && delivered;
    }
    if (joystickHeld) {
        JsonDocument message;
        message["method"] = "v.oai.rad";
        auto params = message["params"].to<JsonObject>();
        params["a"] = heldJoystickAngle;
        params["d"] = 0;
        std::string json;
        serializeJson(message, json);
        delivered = sendJson(json, endpoint) && delivered;
    }
    return delivered;
}

void Service::Impl::refreshRouting()
{
    const auto selection = hostSelection.selectedRoute();
    const int32_t next = selection ? selection->endpoint : -1;
    const std::string nextHost = hostSelection.selectedHostId();
    if (next != routedConnection || nextHost != routedHostId) {
        // Drain releases to the OLD destination before changing the route. If
        // it cannot receive them, terminate that session rather than carrying
        // any held state into another Mac.
        if (!releaseOldRoute(routedConnection)) {
            if (routedConnection == kUsbConnectionId) {
                usb::disconnect();
                usbReconnectAtMs = nowMs() + 150;
            } else if (routedConnection >= 0 && gattsIf != ESP_GATT_IF_NONE) {
                esp_ble_gatts_close(gattsIf, static_cast<uint16_t>(routedConnection));
            }
        }
        controlEpoch.fetch_add(1, std::memory_order_acq_rel);
        heldKeys.fill(HeldKey{});
        pendingReleases.fill(PendingRelease{});
        joystickHeld = false;
        heldJoystickAngle = 0;
        routedConnection = next;
        routedHostId = nextHost;
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        state.threads = {};
        state.quota = {};
        state.rateLimits = {};
        clearHostRpcLocked();
        state.connectionEpoch = controlEpoch.load();
        xSemaphoreGive(stateMutex);
    }
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    state.hosts = hostSelection.hosts();
    state.activeHostId = nextHost;
    state.activeHostName = hostSelection.selectedName();
    state.preferredHostId = hostSelection.preferredHostId();
    state.transport = selection ? selection->transport : Transport::Ble;
    state.connected = next >= 0;
    state.usbMounted = usbConnection.active;
    ++state.revision;
    xSemaphoreGive(stateMutex);
    publishTelemetry();
    saveHosts();
}

void Service::Impl::publishTelemetry()
{
    const Connection* selected = routedConnection >= 0 ? findConnection(static_cast<uint16_t>(routedConnection)) : nullptr;
    if (!selected) return;
    const Connection* quota = selected;
    // Legacy quota writers use a separate GATT connection. Associate it with
    // the selected Mac, never with whichever Mac wrote most recently globally.
    for (const auto& peer : connections) {
        if (selected->usb && selected->quota.available && nowMs() - selected->quota.receivedAtMs < 120000) break;
        if (peer.active && peer.secure && peer.hostId == selected->hostId && peer.quota.available &&
            (!quota->quota.available || static_cast<int32_t>(peer.quota.receivedAtMs - quota->quota.receivedAtMs) > 0)) {
            quota = &peer;
        }
    }
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    state.threads = selected->threads;
    state.quota = quota->quota;
    state.rateLimits = quota->rateLimits;
    state.hostRpcObserved = selected->rpcReady;
    state.lastHostRpcAtMs = selected->lastRpcAtMs;
    hostRpcConnectionValid = selected->rpcReady;
    hostRpcConnectionId = selected->id;
    ++state.revision;
    xSemaphoreGive(stateMutex);
}

bool Service::Impl::processUsb()
{
    bool worked = false;
    if (usbReconnectAtMs && static_cast<int32_t>(nowMs() - usbReconnectAtMs) >= 0) {
        usbReconnectAtMs = 0;
        usb::connect();
    }
    usb::Event event{};
    for (size_t count = 0; count < 16 && usb::serviceBudgetAvailable() && usb::poll(event); ++count) {
        worked = true;
        if (event.type == usb::EventType::Mounted) {
            if (event.epoch != usb::sessionEpoch() || !usb::mounted()) continue;
            usbResumePreference.mounted(event.resumed, hostSelection.preferredHostId());
            usbConnection = Connection{};
            hostSelection.disconnect(kUsbConnectionId);
            refreshRouting();
            usbEpoch = event.epoch;
            usbConnection.usb = usbConnection.active = usbConnection.secure = usbConnection.inputNotifications = true;
            usbConnection.id = kUsbConnectionId;
            usbConnection.mtu = 517;
            refreshRouting();
        } else if (event.type == usb::EventType::Unmounted) {
            if (event.epoch != usb::sessionEpoch() || usb::mounted()) continue;
            usbConnection = Connection{};
            hostSelection.disconnect(kUsbConnectionId);
            refreshRouting();
        } else if (event.epoch == usbEpoch && usbConnection.active) {
            if (event.type == usb::EventType::Overflow) {
                // Native desktop requests contain no trailing newline, so
                // waiting for one would discard every later RPC forever.
                // End the damaged stream instead of guessing a JSON boundary.
                usb::disconnect();
                usbReconnectAtMs = nowMs() + 150;
                break;
            } else if (event.type == usb::EventType::HostIdentity) {
                acceptHostIdentity(usbConnection, event.data, event.length);
            } else if (event.type == usb::EventType::Quota) {
                if (usbConnection.hostId.empty()) continue;
                WriteEvent report{};
                report.connId = kUsbConnectionId;
                report.length = std::min(event.length, sizeof(report.data));
                const void* terminator = std::memchr(event.data, 0, report.length);
                if (terminator) report.length = static_cast<const uint8_t*>(terminator) - event.data;
                std::memcpy(report.data, event.data, report.length);
                handleQuotaWrite(report);
            } else if (event.type == usb::EventType::OutputReport) {
                WriteEvent report{};
                report.connId = kUsbConnectionId;
                report.length = std::min(event.length, sizeof(report.data));
                std::memcpy(report.data, event.data, report.length);
                handleOutputReport(report);
            }
        }
    }
    // A full callback queue must not keep a removed device selected.
    if (usbConnection.active && (!usb::mounted() || usbEpoch != usb::sessionEpoch())) {
        usbConnection = Connection{};
        hostSelection.disconnect(kUsbConnectionId);
        refreshRouting();
    }
    if (!usbConnection.active && usb::mounted()) {
        usbEpoch = usb::sessionEpoch();
        usbConnection = Connection{};
        usbConnection.usb = usbConnection.active = usbConnection.secure = usbConnection.inputNotifications = true;
        usbConnection.id = kUsbConnectionId;
        usbConnection.mtu = 517;
        refreshRouting();
    }
    return worked;
}

void Service::Impl::loadHosts()
{
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    size_t length = 0;
    if (nvs_get_str(handle, "hosts-v1", nullptr, &length) == ESP_OK && length > 1 && length <= 8192) {
        std::string data(length, '\0');
        if (nvs_get_str(handle, "hosts-v1", data.data(), &length) == ESP_OK) {
            JsonDocument doc;
            if (!deserializeJson(doc, data.c_str())) {
                for (JsonObjectConst host : doc["hosts"].as<JsonArrayConst>()) {
                    hostSelection.rememberHost(host["id"] | "", host["name"] | "");
                }
                hostSelection.restorePreferredHost(doc["preferred"] | "");
                for (JsonObjectConst entry : doc["bindings"].as<JsonArrayConst>()) {
                    if (hostBindings.size() >= 16) break;
                    JsonArrayConst address = entry["address"].as<JsonArrayConst>();
                    if (address.size() != ESP_BD_ADDR_LEN) continue;
                    HostBinding binding;
                    bool valid = true;
                    for (size_t i = 0; i < binding.address.size(); ++i) {
                        if (!address[i].is<uint8_t>()) valid = false;
                        binding.address[i] = address[i].as<uint8_t>();
                    }
                    binding.id = entry["id"] | "";
                    binding.name = entry["name"] | "";
                    if (valid && binding.id.size() == 36 && binding.name.size() <= 64) hostBindings.push_back(binding);
                }
                savedHosts = data.c_str();
            }
        }
    }
    nvs_close(handle);
    refreshRouting();
}

void Service::Impl::saveHosts()
{
    JsonDocument doc;
    doc["preferred"] = hostSelection.preferredHostId();
    auto hosts = doc["hosts"].to<JsonArray>();
    for (const auto& host : hostSelection.hosts()) {
        auto entry = hosts.add<JsonObject>();
        entry["id"] = host.id;
        entry["name"] = host.name;
    }
    auto bindings = doc["bindings"].to<JsonArray>();
    for (const auto& binding : hostBindings) {
        auto entry = bindings.add<JsonObject>();
        entry["id"] = binding.id;
        entry["name"] = binding.name;
        auto address = entry["address"].to<JsonArray>();
        for (uint8_t byte : binding.address) address.add(byte);
    }
    std::string serialized;
    serializeJson(doc, serialized);
    if (serialized == savedHosts) return;
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_str(handle, "hosts-v1", serialized.c_str()) == ESP_OK && nvs_commit(handle) == ESP_OK) {
        savedHosts = serialized;
    } else {
        ESP_LOGW(kTag, "host preference save failed");
    }
    nvs_close(handle);
}

void Service::Impl::workerEntry(void* context)
{
    static_cast<Impl*>(context)->worker();
}

void Service::Impl::worker()
{
    loadHosts();
    usb::begin();
    while (!initializeBluetooth()) {
        // Initialization is asynchronous from begin(). Keep the worker alive
        // and retry partial controller/host setup instead of leaving a
        // permanently-started service with no event consumer.
        ESP_LOGE(kTag, "Bluetooth initialization failed; retrying in 2 seconds");
        xQueueReset(commandQueue);
        xQueueReset(releaseQueue);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    ESP_LOGI(kTag, "BLE worker running");
    while (true) {
        usb::beginServiceCycle();
        bool didWork = processUsb();
        ControlEvent control{};
        for (size_t count = 0; count < kControlQueueDepth && xQueueReceive(controlQueue, &control, 0) == pdTRUE;
             ++count) {
            didWork = true;
            processControlEvent(control);
        }

        WriteEvent write{};
        for (size_t count = 0; count < kWriteQueueDepth && xQueueReceive(writeQueue, &write, 0) == pdTRUE; ++count) {
            didWork = true;
            processWrite(write);
        }

        if (releaseQueueLost.exchange(false)) {
            didWork = true;
            handleReleaseQueueLoss();
        }

        if (processQueuedCommands()) {
            didWork = true;
        }
        if (processCapsuleFrames()) {
            didWork = true;
        }
        if (processPendingReleases()) {
            didWork = true;
        }
        if (processRpcTimeouts()) {
            didWork = true;
        }
        if (processAdvertisingRetry()) {
            didWork = true;
        }
        if (processStackRetry()) {
            didWork = true;
        }

        const bool lostControl = controlQueueLost.exchange(false);
        const bool lostWrite   = writeQueueLost.exchange(false);
        if (lostControl || lostWrite) {
            didWork = true;
            handleCallbackQueueLoss(lostControl, lostWrite);
        }
        if (!didWork) {
            vTaskDelay(pdMS_TO_TICKS(2) == 0 ? 1 : pdMS_TO_TICKS(2));
        }
    }
}

bool Service::Impl::initializeBluetooth()
{
    esp_err_t error                             = ESP_OK;
    esp_bt_controller_status_t controllerStatus = esp_bt_controller_get_status();
    if (controllerStatus == ESP_BT_CONTROLLER_STATUS_IDLE) {
        error = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(kTag, "classic BT memory release failed: %s", esp_err_to_name(error));
        }

        esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        error                             = esp_bt_controller_init(&config);
        if (error != ESP_OK) {
            ESP_LOGE(kTag, "BT controller init failed: %s", esp_err_to_name(error));
            return false;
        }
        controllerStatus = esp_bt_controller_get_status();
    }
    if (controllerStatus == ESP_BT_CONTROLLER_STATUS_INITED) {
        error = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (error != ESP_OK) {
            ESP_LOGE(kTag, "BT controller enable failed: %s", esp_err_to_name(error));
            return false;
        }
    }
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        ESP_LOGE(kTag, "BT controller is not enabled");
        return false;
    }

    esp_bluedroid_status_t bluedroidStatus = esp_bluedroid_get_status();
    if (bluedroidStatus == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        esp_bluedroid_config_t config = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
        error                         = esp_bluedroid_init_with_cfg(&config);
        if (error != ESP_OK) {
            ESP_LOGE(kTag, "Bluedroid init failed: %s", esp_err_to_name(error));
            return false;
        }
        bluedroidStatus = esp_bluedroid_get_status();
    }
    if (bluedroidStatus == ESP_BLUEDROID_STATUS_INITIALIZED) {
        error = esp_bluedroid_enable();
        if (error != ESP_OK) {
            ESP_LOGE(kTag, "Bluedroid enable failed: %s", esp_err_to_name(error));
            return false;
        }
    }
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        ESP_LOGE(kTag, "Bluedroid is not enabled");
        return false;
    }

    error = esp_ble_gap_register_callback(gapCallback);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "GAP callback registration failed: %s", esp_err_to_name(error));
        return false;
    }
    error = esp_ble_gatts_register_callback(gattsCallback);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "GATTS callback registration failed: %s", esp_err_to_name(error));
        return false;
    }

    configureSecurity();
    esp_ble_gap_set_device_name(detail::kDeviceName);
    esp_ble_gap_config_local_icon(0x03C0);

    error = esp_ble_gatt_set_local_mtu(517);
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "local MTU configuration failed: %s", esp_err_to_name(error));
    }
    beginBondMigration();
    return true;
}

void Service::Impl::configureSecurity()
{
    esp_ble_auth_req_t authentication = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t ioCapability     = ESP_IO_CAP_NONE;
    uint8_t keySize                   = 16;
    uint8_t initiatorKeys             = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t responderKeys             = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &authentication, sizeof(authentication));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &ioCapability, sizeof(ioCapability));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &keySize, sizeof(keySize));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &initiatorKeys, sizeof(initiatorKeys));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &responderKeys, sizeof(responderKeys));
}

void Service::Impl::beginBondMigration()
{
    if (bondMigrationComplete || bondRemovalInFlight) {
        return;
    }
    bondMigrationRetryAtMs = 0;

    nvs_handle_t handle = 0;
    esp_err_t error     = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "GATT migration NVS open failed: %s", esp_err_to_name(error));
        bondMigrationRetryAtMs = nowMs() + kStackRetryMs;
        return;
    }
    uint8_t storedRevision = 0;
    error                  = nvs_get_u8(handle, kNvsGattRevisionKey, &storedRevision);
    nvs_close(handle);
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(kTag, "GATT migration NVS read failed: %s", esp_err_to_name(error));
        bondMigrationRetryAtMs = nowMs() + kStackRetryMs;
        return;
    }
    if (storedRevision >= kGattSchemaRevision) {
        bondMigrationComplete = true;
        ESP_LOGI(kTag, "GATT schema revision %u already migrated", storedRevision);
        registerGattApplication();
        return;
    }

    int bondCount = esp_ble_get_bond_device_num();
    if (bondCount < 0) {
        ESP_LOGE(kTag, "failed to query bonded devices");
        bondMigrationRetryAtMs = nowMs() + kStackRetryMs;
        return;
    }
    bondsToRemove.clear();
    bondRemovalIndex = 0;
    if (bondCount > 0) {
        std::vector<esp_ble_bond_dev_t> bonds(static_cast<size_t>(bondCount));
        int listed = bondCount;
        error      = esp_ble_get_bond_device_list(&listed, bonds.data());
        if (error != ESP_OK) {
            ESP_LOGE(kTag, "failed to list bonded devices: %s", esp_err_to_name(error));
            bondMigrationRetryAtMs = nowMs() + kStackRetryMs;
            return;
        }
        bondsToRemove.reserve(static_cast<size_t>(listed));
        for (int index = 0; index < listed; ++index) {
            std::array<uint8_t, ESP_BD_ADDR_LEN> address{};
            std::memcpy(address.data(), bonds[static_cast<size_t>(index)].bd_addr, ESP_BD_ADDR_LEN);
            bondsToRemove.push_back(address);
        }
    }

    if (bondsToRemove.empty()) {
        if (commitGattSchemaRevision()) {
            bondMigrationComplete = true;
            ESP_LOGI(kTag, "GATT schema migration complete; no bonds removed");
            registerGattApplication();
        } else {
            bondMigrationRetryAtMs = nowMs() + kStackRetryMs;
        }
        return;
    }

    ESP_LOGW(kTag, "GATT schema changed; removing %u cached bond(s) once for revision %u",
             static_cast<unsigned>(bondsToRemove.size()), kGattSchemaRevision);
    requestNextBondRemoval();
}

void Service::Impl::requestNextBondRemoval()
{
    if (bondRemovalIndex >= bondsToRemove.size()) {
        if (commitGattSchemaRevision()) {
            bondMigrationComplete = true;
            bondsToRemove.clear();
            ESP_LOGI(kTag, "GATT schema migration complete revision=%u", kGattSchemaRevision);
            registerGattApplication();
        } else {
            bondMigrationRetryAtMs = nowMs() + kStackRetryMs;
        }
        return;
    }
    esp_bd_addr_t address{};
    std::memcpy(address, bondsToRemove[bondRemovalIndex].data(), ESP_BD_ADDR_LEN);
    const esp_err_t error = esp_ble_remove_bond_device(address);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "bond removal request failed: %s", esp_err_to_name(error));
        bondRemovalInFlight    = false;
        bondMigrationRetryAtMs = nowMs() + kStackRetryMs;
        return;
    }
    bondRemovalInFlight = true;
}

bool Service::Impl::commitGattSchemaRevision()
{
    nvs_handle_t handle = 0;
    esp_err_t error     = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_u8(handle, kNvsGattRevisionKey, kGattSchemaRevision);
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "GATT migration NVS commit failed: %s", esp_err_to_name(error));
        return false;
    }
    return true;
}

void Service::Impl::registerGattApplication()
{
    if (!bondMigrationComplete || gattRegistrationRequested || gattRegistered) {
        return;
    }
    gattRegistrationRetryAtMs = 0;
    const esp_err_t error     = esp_ble_gatts_app_register(kGattAppId);
    if (error == ESP_OK) {
        gattRegistrationRequested = true;
    } else {
        ESP_LOGE(kTag, "GATTS app registration failed: %s", esp_err_to_name(error));
        gattRegistrationRetryAtMs = nowMs() + kStackRetryMs;
    }
}

bool Service::Impl::processStackRetry()
{
    const uint32_t currentTime = nowMs();
    if (bondMigrationRetryAtMs != 0 && static_cast<int32_t>(currentTime - bondMigrationRetryAtMs) >= 0) {
        bondMigrationRetryAtMs = 0;
        bondRemovalInFlight    = false;
        beginBondMigration();
        return true;
    }
    if (gattRegistrationRetryAtMs != 0 && static_cast<int32_t>(currentTime - gattRegistrationRetryAtMs) >= 0) {
        gattRegistrationRetryAtMs = 0;
        registerGattApplication();
        return true;
    }
    return false;
}

void Service::Impl::gattsCallback(esp_gatts_cb_event_t event, esp_gatt_if_t callbackIf, esp_ble_gatts_cb_param_t* param)
{
    Impl* self = callbackTarget.load(std::memory_order_acquire);
    if (self == nullptr || param == nullptr) {
        return;
    }

    ControlEvent queued{};
    queued.gattsIf = callbackIf;
    switch (event) {
        case ESP_GATTS_REG_EVT:
            queued.type       = ControlEventType::kRegistered;
            queued.gattStatus = param->reg.status;
            self->enqueueControl(queued);
            break;
        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            queued.type       = ControlEventType::kAttributeTableCreated;
            queued.gattStatus = param->add_attr_tab.status;
            queued.instanceId = param->add_attr_tab.svc_inst_id;
            queued.handleCount =
                static_cast<uint8_t>(std::min<size_t>(param->add_attr_tab.num_handle, kMaxHandlesPerService));
            if (param->add_attr_tab.handles != nullptr) {
                std::memcpy(queued.handles, param->add_attr_tab.handles, queued.handleCount * sizeof(uint16_t));
            }
            self->enqueueControl(queued);
            break;
        case ESP_GATTS_START_EVT:
            queued.type       = ControlEventType::kServiceStarted;
            queued.gattStatus = param->start.status;
            queued.value      = param->start.service_handle;
            self->enqueueControl(queued);
            break;
        case ESP_GATTS_CONNECT_EVT:
            queued.type   = ControlEventType::kConnected;
            queued.connId = param->connect.conn_id;
            std::memcpy(queued.address, param->connect.remote_bda, ESP_BD_ADDR_LEN);
            self->enqueueControl(queued);
            break;
        case ESP_GATTS_DISCONNECT_EVT:
            queued.type   = ControlEventType::kDisconnected;
            queued.connId = param->disconnect.conn_id;
            std::memcpy(queued.address, param->disconnect.remote_bda, ESP_BD_ADDR_LEN);
            self->enqueueControl(queued);
            break;
        case ESP_GATTS_MTU_EVT:
            queued.type   = ControlEventType::kMtuChanged;
            queued.connId = param->mtu.conn_id;
            queued.value  = param->mtu.mtu;
            self->enqueueControl(queued);
            break;
        case ESP_GATTS_CONGEST_EVT:
            queued.type   = ControlEventType::kCongestionChanged;
            queued.connId = param->congest.conn_id;
            queued.flag   = param->congest.congested;
            self->enqueueControl(queued);
            break;
        case ESP_GATTS_WRITE_EVT:
            self->enqueueWrite(callbackIf, *param);
            break;
        default:
            break;
    }
}

void Service::Impl::gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param)
{
    Impl* self = callbackTarget.load(std::memory_order_acquire);
    if (self == nullptr || param == nullptr) {
        return;
    }

    ControlEvent queued{};
    switch (event) {
        case ESP_GAP_BLE_SEC_REQ_EVT:
            queued.type = ControlEventType::kSecurityRequest;
            std::memcpy(queued.address, param->ble_security.ble_req.bd_addr, ESP_BD_ADDR_LEN);
            self->enqueueControl(queued);
            break;
        case ESP_GAP_BLE_AUTH_CMPL_EVT:
            queued.type  = ControlEventType::kAuthenticationComplete;
            queued.flag  = param->ble_security.auth_cmpl.success;
            queued.value = static_cast<uint16_t>(param->ble_security.auth_cmpl.fail_reason);
            std::memcpy(queued.address, param->ble_security.auth_cmpl.bd_addr, ESP_BD_ADDR_LEN);
            self->enqueueControl(queued);
            break;
        case ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT:
            queued.type     = ControlEventType::kBondRemoved;
            queued.btStatus = param->remove_bond_dev_cmpl.status;
            std::memcpy(queued.address, param->remove_bond_dev_cmpl.bd_addr, ESP_BD_ADDR_LEN);
            self->enqueueControl(queued);
            break;
        case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
            queued.type     = ControlEventType::kAdvertisingDataConfigured;
            queued.btStatus = param->adv_data_raw_cmpl.status;
            self->enqueueControl(queued);
            break;
        case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
            queued.type     = ControlEventType::kScanResponseConfigured;
            queued.btStatus = param->scan_rsp_data_raw_cmpl.status;
            self->enqueueControl(queued);
            break;
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            queued.type     = ControlEventType::kAdvertisingStarted;
            queued.btStatus = param->adv_start_cmpl.status;
            self->enqueueControl(queued);
            break;
        default:
            break;
    }
}

void Service::Impl::enqueueControl(const ControlEvent& event)
{
    ControlEvent queued = event;
    portENTER_CRITICAL(&callbackSessionMux);
    if (event.type == ControlEventType::kConnected) {
        queued.linkGeneration = callbackSessions.connect(event.connId, event.address);
    } else if (event.type == ControlEventType::kDisconnected) {
        queued.linkGeneration = callbackSessions.disconnect(event.connId, event.address);
    } else {
        queued.linkGeneration = callbackSessions.token(event.connId);
    }
    portEXIT_CRITICAL(&callbackSessionMux);
    if (controlQueue == nullptr || xQueueSend(controlQueue, &queued, 0) != pdTRUE) {
        controlQueueLost.store(true, std::memory_order_release);
    }
}

void Service::Impl::enqueueWrite(esp_gatt_if_t callbackIf, const esp_ble_gatts_cb_param_t& parameters)
{
    const auto& write = parameters.write;
    if (writeQueue == nullptr || write.value == nullptr || write.len == 0 || write.len > detail::kMaxQuotaSize) {
        return;
    }
    WriteEvent queued{};
    portENTER_CRITICAL(&callbackSessionMux);
    queued.linkGeneration = callbackSessions.token(write.conn_id, write.bda);
    portEXIT_CRITICAL(&callbackSessionMux);
    queued.gattsIf      = callbackIf;
    queued.connId       = write.conn_id;
    queued.handle       = write.handle;
    queued.offset       = write.offset;
    queued.length       = write.len;
    queued.needResponse = write.need_rsp;
    queued.prepared     = write.is_prep;
    std::memcpy(queued.address, write.bda, ESP_BD_ADDR_LEN);
    std::memcpy(queued.data, write.value, write.len);
    if (xQueueSend(writeQueue, &queued, 0) != pdTRUE) {
        writeQueueLost.store(true, std::memory_order_release);
    }
}

void Service::Impl::processControlEvent(const ControlEvent& event)
{
    switch (event.type) {
        case ControlEventType::kRegistered:
            handleRegistration(event);
            break;
        case ControlEventType::kAttributeTableCreated:
            handleTableCreated(event);
            break;
        case ControlEventType::kServiceStarted:
            handleServiceStarted(event);
            break;
        case ControlEventType::kConnected:
            handleConnection(true, event.connId, event.address, event.linkGeneration);
            break;
        case ControlEventType::kDisconnected:
            handleConnection(false, event.connId, event.address, event.linkGeneration);
            break;
        case ControlEventType::kMtuChanged:
            handleMtu(event.connId, event.value);
            break;
        case ControlEventType::kCongestionChanged:
            handleCongestion(event.connId, event.flag);
            break;
        case ControlEventType::kSecurityRequest:
            esp_ble_gap_security_rsp(const_cast<uint8_t*>(event.address), true);
            break;
        case ControlEventType::kAuthenticationComplete:
            if (event.flag) {
                bool fridayHidLinkReady = false;
                for (Connection& connection : connections) {
                    if (connection.active && addressEquals(connection.address, event.address)) {
                        connection.secure = true;
                        fridayHidLinkReady =
                            fridayHidLinkReady || (connection.fridayPeer && connection.inputNotifications);
                    }
                }
                if (fridayHidLinkReady) {
                    updateFridayConnectionIntervals(fridayTravelActive || fridayCapsuleActive);
                }
                syncFridayLinkState();
                ESP_LOGI(kTag, "BLE pairing complete");
            } else {
                ESP_LOGW(kTag, "BLE pairing failed reason=0x%02x", event.value);
                for (const Connection& connection : connections) {
                    if (connection.active && addressEquals(connection.address, event.address) &&
                        gattsIf != ESP_GATT_IF_NONE) {
                        esp_ble_gatts_close(gattsIf, connection.id);
                    }
                }
            }
            break;
        case ControlEventType::kBondRemoved:
            if (!bondRemovalInFlight || bondRemovalIndex >= bondsToRemove.size() ||
                !addressEquals(bondsToRemove[bondRemovalIndex].data(), event.address)) {
                ESP_LOGW(kTag, "unexpected bond removal completion");
                break;
            }
            bondRemovalInFlight = false;
            if (event.btStatus != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(kTag, "bond removal failed status=%d; migration will retry", event.btStatus);
                bondMigrationRetryAtMs = nowMs() + kStackRetryMs;
                break;
            }
            ++bondRemovalIndex;
            requestNextBondRemoval();
            break;
        case ControlEventType::kAdvertisingDataConfigured:
            advertisingDataPending = false;
            advertisingDataReady   = event.btStatus == ESP_BT_STATUS_SUCCESS;
            if (!advertisingDataReady) {
                ESP_LOGE(kTag, "raw advertising data rejected status=%d", event.btStatus);
                scheduleAdvertisingRetry();
            } else if (scanResponseReady) {
                startAdvertising();
            }
            break;
        case ControlEventType::kScanResponseConfigured:
            scanResponsePending = false;
            scanResponseReady   = event.btStatus == ESP_BT_STATUS_SUCCESS;
            if (!scanResponseReady) {
                ESP_LOGE(kTag, "raw scan response rejected status=%d", event.btStatus);
                scheduleAdvertisingRetry();
            } else if (advertisingDataReady) {
                startAdvertising();
            }
            break;
        case ControlEventType::kAdvertisingStarted:
            if (event.btStatus == ESP_BT_STATUS_SUCCESS) {
                advertisingActive    = true;
                advertisingRetryAtMs = 0;
                ESP_LOGI(kTag, "BLE advertising active");
            } else {
                advertisingActive = false;
                ESP_LOGW(kTag, "BLE advertising start status=%d", event.btStatus);
                scheduleAdvertisingRetry();
            }
            break;
    }
}

void Service::Impl::handleRegistration(const ControlEvent& event)
{
    gattRegistrationRequested = false;
    if (event.gattStatus != ESP_GATT_OK) {
        ESP_LOGE(kTag, "GATTS registration failed status=%d", event.gattStatus);
        gattRegistrationRetryAtMs = nowMs() + kStackRetryMs;
        return;
    }
    gattRegistered = true;
    gattsIf        = event.gattsIf;
    createTable(TableId::kDeviceInfo);
}

void Service::Impl::createTable(TableId id)
{
    const esp_gatts_attr_db_t* table = nullptr;
    size_t count                     = 0;
    switch (id) {
        case TableId::kDeviceInfo:
            table = detail::kDeviceInfoDb;
            count = detail::kDiCount;
            break;
        case TableId::kHid:
            table = detail::kHidDb;
            count = detail::kHidCount;
            break;
        case TableId::kBattery:
            table = detail::kBatteryDb;
            count = detail::kBatteryCount;
            break;
        case TableId::kQuota:
            table = detail::kQuotaDb;
            count = detail::kQuotaCount;
            break;
        case TableId::kFriday:
            table = detail::kFridayDb;
            count = detail::kFridayCount;
            break;
        case TableId::kHostIdentity:
            table = detail::kHostIdentityDb;
            count = detail::kHostIdentityCount;
            break;
        case TableId::kCount:
            return;
    }
    const esp_err_t error = esp_ble_gatts_create_attr_tab(table, gattsIf, static_cast<uint16_t>(count), toIndex(id));
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "attribute table %u request failed: %s", toIndex(id), esp_err_to_name(error));
    }
}

void Service::Impl::handleTableCreated(const ControlEvent& event)
{
    if (event.instanceId >= toIndex(TableId::kCount)) {
        ESP_LOGE(kTag, "unknown attribute table instance=%u", event.instanceId);
        return;
    }
    const TableId id      = static_cast<TableId>(event.instanceId);
    const size_t expected = handleCountFor(id);
    if (event.gattStatus != ESP_GATT_OK || event.handleCount != expected) {
        ESP_LOGE(kTag, "attribute table %u failed status=%d handles=%u expected=%u", event.instanceId, event.gattStatus,
                 event.handleCount, static_cast<unsigned>(expected));
        return;
    }

    std::memcpy(handlesFor(id), event.handles, expected * sizeof(uint16_t));
    const esp_err_t error = esp_ble_gatts_start_service(event.handles[0]);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "service %u start request failed: %s", event.instanceId, esp_err_to_name(error));
        return;
    }

    const uint8_t next = event.instanceId + 1;
    if (next < toIndex(TableId::kCount)) {
        createTable(static_cast<TableId>(next));
    }
}

void Service::Impl::handleServiceStarted(const ControlEvent& event)
{
    const TableId id = tableForServiceHandle(event.value);
    if (id == TableId::kCount) {
        ESP_LOGW(kTag, "start event for unknown service handle=%u", event.value);
        return;
    }
    if (event.gattStatus != ESP_GATT_OK) {
        ESP_LOGE(kTag, "service %u failed to start status=%d", toIndex(id), event.gattStatus);
        return;
    }
    servicesStarted[toIndex(id)] = true;
    if (std::all_of(servicesStarted.begin(), servicesStarted.end(), [](bool value) { return value; })) {
        configureAdvertising();
        ESP_LOGI(kTag, "BLE GATT ready VID=%04X PID=%04X report=%u services=%u", Service::kVendorId, Service::kProductId,
                 Service::kReportId, toIndex(TableId::kCount));
    }
}

void Service::Impl::configureAdvertising()
{
    if (!advertisingDataReady && !advertisingDataPending) {
        const esp_err_t error =
            esp_ble_gap_config_adv_data_raw(detail::kAdvertisingData, sizeof(detail::kAdvertisingData));
        if (error == ESP_OK) {
            advertisingDataPending = true;
        } else {
            ESP_LOGE(kTag, "advertising data request failed: %s", esp_err_to_name(error));
            scheduleAdvertisingRetry();
        }
    }
    if (!scanResponseReady && !scanResponsePending) {
        const esp_err_t error =
            esp_ble_gap_config_scan_rsp_data_raw(detail::kScanResponseData, sizeof(detail::kScanResponseData));
        if (error == ESP_OK) {
            scanResponsePending = true;
        } else {
            ESP_LOGE(kTag, "scan response request failed: %s", esp_err_to_name(error));
            scheduleAdvertisingRetry();
        }
    }
}

void Service::Impl::startAdvertising()
{
    if (!advertisingDataReady || !scanResponseReady || advertisingActive) {
        return;
    }
    const esp_err_t error = esp_ble_gap_start_advertising(&kAdvertisingParams);
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "advertising request failed: %s", esp_err_to_name(error));
        scheduleAdvertisingRetry();
    }
}

void Service::Impl::scheduleAdvertisingRetry()
{
    advertisingRetryAtMs = nowMs() + kAdvertisingRetryMs;
}

bool Service::Impl::processAdvertisingRetry()
{
    if (advertisingRetryAtMs == 0 || static_cast<int32_t>(nowMs() - advertisingRetryAtMs) < 0) {
        return false;
    }
    advertisingRetryAtMs = 0;
    if (!advertisingDataReady || !scanResponseReady) {
        configureAdvertising();
    } else if (!advertisingActive) {
        startAdvertising();
    }
    return true;
}

Connection* Service::Impl::findConnection(uint16_t connectionId)
{
    if (connectionId == kUsbConnectionId) return usbConnection.active ? &usbConnection : nullptr;
    for (Connection& connection : connections) {
        if (connection.active && connection.id == connectionId) {
            return &connection;
        }
    }
    return nullptr;
}

void Service::Impl::markFridayPeer(Connection& connection)
{
    if (connection.fridayPeer) {
        return;
    }
    connection.fridayPeer = true;
    syncFridayLinkState();
    ESP_LOGI(kTag, "Friday companion active id=%u", connection.id);
}

void Service::Impl::syncFridayLinkState()
{
    Connection* selected = nullptr;
    for (Connection& connection : connections) {
        if (!connection.active || !connection.fridayPeer) {
            continue;
        }
        const unsigned score = (connection.fridayCapsuleNotifications ? 2U : 0U) +
                               (connection.fridayTransferNotifications ? 1U : 0U);
        const unsigned selectedScore = selected == nullptr ? 0U :
            (selected->fridayCapsuleNotifications ? 2U : 0U) +
            (selected->fridayTransferNotifications ? 1U : 0U);
        if (selected == nullptr || score > selectedScore) {
            selected = &connection;
        }
    }

    auto& link = friday::context::ContextLink::instance();
    const uint16_t selectedId = selected == nullptr ? UINT16_MAX : selected->id;
    if (selectedId != selectedFridayConnectionId) {
        selectedFridayConnectionId = selectedId;
        fridayTravelActive         = false;
        fridayCapsuleActive        = false;
        // Never replay audio queued for a disconnected/replaced Mac. The
        // Friday app will observe the link loss and surface an honest failure.
        if (capsuleQueue != nullptr) {
            xQueueReset(capsuleQueue);
        }
    }
    link.setConnected(selected != nullptr, selectedId);
    link.setTransferSubscribed(selected != nullptr && selected->fridayTransferNotifications);

    auto& capsuleLink = friday::capsule::CapsuleLink::instance();
    const bool capsuleConnected = selected != nullptr && selected->secure;
    capsuleLink.setConnected(capsuleConnected);
    capsuleLink.setSubscribed(capsuleConnected && selected->fridayCapsuleNotifications);
}

size_t Service::Impl::connectionCount() const
{
    return static_cast<size_t>(std::count_if(connections.begin(), connections.end(),
                                             [](const Connection& connection) { return connection.active; }));
}

void Service::Impl::handleConnection(bool connected, uint16_t connectionId, const uint8_t* address, uint32_t generation)
{
    if (connectionId == kUsbConnectionId || address == nullptr || generation == 0) return;
    Connection* connection = findConnection(connectionId);
    if (connected) {
        advertisingActive = false;
        if (connection && connection->linkGeneration == generation && addressEquals(connection->address, address)) return;
        if (connection) {
            *connection = Connection{};
            hostSelection.disconnect(connectionId);
            refreshRouting();
        } else {
            const auto empty = std::find_if(connections.begin(), connections.end(),
                                           [](const Connection& item) { return !item.active; });
            if (empty == connections.end()) {
                esp_ble_gatts_close(gattsIf, connectionId);
                return;
            }
            connection = &*empty;
        }
        *connection = Connection{};
        connection->active = true;
        connection->id = connectionId;
        connection->linkGeneration = generation;
        std::memcpy(connection->address, address, ESP_BD_ADDR_LEN);
        applyBinding(*connection);
        esp_ble_set_encryption(connection->address, ESP_BLE_SEC_ENCRYPT_NO_MITM);
        refreshRouting();
        startAdvertising();
        ESP_LOGI(kTag, "BLE link connected id=%u count=%u", connectionId, static_cast<unsigned>(connectionCount()));
        return;
    }
    if (!connection || connection->linkGeneration != generation || !addressEquals(connection->address, address)) return;
    // Mark unavailable before routing changes: never release a held key to a
    // new peer that later reuses this connection ID.
    *connection = Connection{};
    hostSelection.disconnect(connectionId);
    refreshRouting();
    syncFridayLinkState();
    startAdvertising();
    ESP_LOGI(kTag, "BLE link disconnected id=%u count=%u", connectionId, static_cast<unsigned>(connectionCount()));
}

void Service::Impl::handleMtu(uint16_t connectionId, uint16_t mtu)
{
    Connection* connection = findConnection(connectionId);
    if (connection != nullptr) {
        connection->mtu = mtu;
        ESP_LOGI(kTag, "connection id=%u MTU=%u", connectionId, mtu);
    }
}

void Service::Impl::handleCongestion(uint16_t connectionId, bool congested)
{
    Connection* connection = findConnection(connectionId);
    if (connection != nullptr) {
        connection->congested = congested;
    }
}

void Service::Impl::processWrite(const WriteEvent& write)
{
    if (write.prepared) {
        ESP_LOGW(kTag, "prepared write ignored handle=%u offset=%u", write.handle, write.offset);
        return;
    }
    Connection* connection = findConnection(write.connId);
    if (connection == nullptr || write.linkGeneration == 0 || connection->linkGeneration != write.linkGeneration ||
        !addressEquals(connection->address, write.address)) {
        ESP_LOGW(kTag, "stale write rejected id=%u handle=%u", write.connId, write.handle);
        return;
    }
    if (write.handle == hidHandles[detail::kHidInputCccd] || write.handle == batteryHandles[detail::kBatteryCccd] ||
        write.handle == fridayHandles[detail::kFridayTransferCccd] ||
        write.handle == fridayHandles[detail::kFridayCapsuleDataCccd]) {
        handleCccdWrite(write);
        return;
    }
    // Friday's companion protocol intentionally remains usable without HID
    // pairing. The unified device may begin Just Works encryption for Codex,
    // but context and travel must not be held behind that asynchronous step.
    if (write.handle == fridayHandles[detail::kFridayContextValue]) {
        handleFridayContextWrite(write, *connection);
        return;
    }
    if (write.handle == fridayHandles[detail::kFridayTransferValue]) {
        handleFridayTransferWrite(write, *connection);
        return;
    }
    if (write.handle == fridayHandles[detail::kFridayCapsuleAckValue]) {
        if (!connection->secure) {
            ESP_LOGW(kTag, "unencrypted capsule acknowledgement rejected id=%u", write.connId);
            return;
        }
        handleFridayCapsuleAckWrite(write, *connection);
        return;
    }
    if (!connection->secure) {
        ESP_LOGW(kTag, "unencrypted write rejected id=%u handle=%u", write.connId, write.handle);
        return;
    }
    if (write.handle == hostIdentityHandles[detail::kHostIdentityValue]) {
        if (write.offset == 0) acceptHostIdentity(*connection, write.data, write.length);
        return;
    }
    if (write.handle == hidHandles[detail::kHidOutputValue]) {
        handleOutputReport(write);
        return;
    }
    if (write.handle == quotaHandles[detail::kQuotaValue]) {
        handleQuotaWrite(write);
        return;
    }
}

void Service::Impl::handleCccdWrite(const WriteEvent& write)
{
    if (write.length != 2) {
        return;
    }
    Connection* connection = findConnection(write.connId);
    if (connection == nullptr) {
        return;
    }
    const uint16_t value =
        static_cast<uint16_t>(write.data[0]) | static_cast<uint16_t>(static_cast<uint16_t>(write.data[1]) << 8);
    if (write.handle == hidHandles[detail::kHidInputCccd]) {
        connection->inputNotifications = (value & 0x0001) != 0;
        if (!connection->inputNotifications) hostSelection.disconnect(connection->id);
        registerHost(*connection);
        refreshRouting();
        if (connection->fridayPeer) {
            updateFridayConnectionIntervals(fridayTravelActive || fridayCapsuleActive);
        }
        ESP_LOGI(kTag, "input notifications id=%u enabled=%d", write.connId, connection->inputNotifications);
    } else if (write.handle == batteryHandles[detail::kBatteryCccd]) {
        connection->batteryNotifications = (value & 0x0001) != 0;
    } else if (write.handle == fridayHandles[detail::kFridayTransferCccd]) {
        markFridayPeer(*connection);
        connection->fridayTransferNotifications = (value & 0x0001) != 0;
        syncFridayLinkState();
        ESP_LOGI(kTag, "Friday travel notifications id=%u enabled=%d", write.connId,
                 connection->fridayTransferNotifications);
    } else if (write.handle == fridayHandles[detail::kFridayCapsuleDataCccd]) {
        markFridayPeer(*connection);
        connection->fridayCapsuleNotifications = (value & 0x0001) != 0;
        syncFridayLinkState();
        ESP_LOGI(kTag, "Friday capsule notifications id=%u enabled=%d", write.connId,
                 connection->fridayCapsuleNotifications);
    }
}

void Service::Impl::handleFridayContextWrite(const WriteEvent& write, Connection& connection)
{
    markFridayPeer(connection);
    if (write.offset != 0 ||
        !friday::context::ContextLink::instance().acceptPacket(write.data, write.length, nowMs())) {
        ESP_LOGW(kTag, "Friday context packet rejected id=%u length=%u", write.connId, write.length);
        return;
    }
    esp_ble_gatts_set_attr_value(fridayHandles[detail::kFridayContextValue], write.length,
                                 const_cast<uint8_t*>(write.data));
}

void Service::Impl::handleFridayTransferWrite(const WriteEvent& write, Connection& connection)
{
    markFridayPeer(connection);
    if (write.offset != 0 ||
        !friday::context::ContextLink::instance().acceptTransferPacket(write.data, write.length, nowMs())) {
        ESP_LOGW(kTag, "Friday transfer packet rejected id=%u length=%u", write.connId, write.length);
        return;
    }
    esp_ble_gatts_set_attr_value(fridayHandles[detail::kFridayTransferValue], write.length,
                                 const_cast<uint8_t*>(write.data));
}

void Service::Impl::handleFridayCapsuleAckWrite(const WriteEvent& write, Connection& connection)
{
    markFridayPeer(connection);
    if (write.offset != 0 ||
        !friday::capsule::CapsuleLink::instance().acceptAck(write.data, write.length, nowMs())) {
        ESP_LOGW(kTag, "Friday capsule acknowledgement rejected id=%u length=%u", write.connId, write.length);
        return;
    }
    esp_ble_gatts_set_attr_value(fridayHandles[detail::kFridayCapsuleAckValue], write.length,
                                 const_cast<uint8_t*>(write.data));
}

void Service::Impl::handleOutputReport(const WriteEvent& write)
{
    Connection* connection = findConnection(write.connId);
    if (connection == nullptr || write.length < 2) {
        return;
    }
    const uint32_t currentTime = nowMs();
    if (!connection->rpcBuffer.empty() &&
        static_cast<uint32_t>(currentTime - connection->rpcLastFragmentAtMs) > kRpcAssemblyTimeoutMs) {
        clearRpcAssembly(*connection);
    }

    size_t offset = write.length >= 3 && write.data[0] == Service::kReportId ? 1 : 0;
    if (write.length < offset + 2 || write.data[offset] != 2) {
        return;
    }
    const size_t payloadLength = std::min<size_t>(write.data[offset + 1], detail::kPayloadSize);
    if (write.length < offset + 2 + payloadLength) {
        return;
    }
    const char* payload = reinterpret_cast<const char*>(write.data + offset + 2);
    if (connection->rpcDiscardUntilNewline) {
        if (std::memchr(payload, '\n', payloadLength)) connection->rpcDiscardUntilNewline = false;
        return;
    }

    constexpr char kTopLevelPrefix[] = "{\"method\"";
    const bool startsTopLevel        = payloadLength >= sizeof(kTopLevelPrefix) - 1 &&
                                std::memcmp(payload, kTopLevelPrefix, sizeof(kTopLevelPrefix) - 1) == 0;
    if (startsTopLevel && !connection->rpcBuffer.empty()) {
        clearRpcAssembly(*connection);
    }

    if (connection->rpcBuffer.empty()) {
        size_t jsonStart = 0;
        while (jsonStart < payloadLength && payload[jsonStart] != '{') {
            ++jsonStart;
        }
        if (jsonStart == payloadLength) {
            return;
        }
        connection->rpcBuffer.assign(payload + jsonStart, payloadLength - jsonStart);
    } else {
        connection->rpcBuffer.append(payload, payloadLength);
    }
    connection->rpcLastFragmentAtMs = currentTime;

    if (connection->rpcBuffer.size() > detail::kMaxRpcSize) {
        ESP_LOGW(kTag, "RPC assembly exceeded %u bytes", static_cast<unsigned>(detail::kMaxRpcSize));
        clearRpcAssembly(*connection);
        return;
    }

    JsonDocument request;
    const DeserializationError error = deserializeJson(request, connection->rpcBuffer);
    if (error == DeserializationError::IncompleteInput) {
        return;
    }
    if (error) {
        ESP_LOGW(kTag, "RPC parse error: %s", error.c_str());
        clearRpcAssembly(*connection);
        return;
    }

    if (handleRpc(request, write.connId)) {
        noteHostRpc(write.connId);
    }
    clearRpcAssembly(*connection);
}

bool Service::Impl::handleRpc(const JsonDocument& request, uint16_t connectionId)
{
    const JsonObjectConst object  = request.as<JsonObjectConst>();
    const char* method            = object["method"] | "";
    const JsonVariantConst id     = object["id"];
    const JsonVariantConst params = object["params"];
    ESP_LOGI(kTag, "RPC method=%s", method);

    JsonDocument response;
    response["id"].set(id);
    if (std::strcmp(method, "sys.version") == 0) {
        JsonObject result = response["result"].to<JsonObject>();
        result["version"] = detail::kFirmwareVersion;
    } else if (std::strcmp(method, "device.status") == 0) {
        JsonObject result       = response["result"].to<JsonObject>();
        result["version"]       = detail::kFirmwareVersion;
        result["profile_index"] = 0;
        result["layer_index"]   = 1;
        result["battery"]       = batteryPercentage;
        result["is_charging"]   = charging;
    } else if (std::strcmp(method, "v.oai.thstatus") == 0 && params.is<JsonArrayConst>()) {
        updateThreads(params.as<JsonArrayConst>(), connectionId);
        JsonObject result = response["result"].to<JsonObject>();
        result["ok"]      = true;
    } else if (std::strcmp(method, "v.oai.rgbcfg") == 0 && params.is<JsonObjectConst>()) {
        JsonObject result = response["result"].to<JsonObject>();
        result["ok"]      = true;
    } else if (std::strcmp(method, "lights.preview") == 0 || std::strcmp(method, "host.focused_app") == 0) {
        JsonObject result = response["result"].to<JsonObject>();
        result["ok"]      = true;
    } else {
        sendMethodNotFound(id, connectionId);
        return false;
    }

    std::string json;
    serializeJson(response, json);
    return sendJson(json, connectionId);
}

void Service::Impl::sendMethodNotFound(JsonVariantConst id, uint16_t connectionId)
{
    JsonDocument response;
    response["id"].set(id);
    JsonObject error = response["error"].to<JsonObject>();
    error["code"]    = -32601;
    error["message"] = "Method not found";
    std::string json;
    serializeJson(response, json);
    sendJson(json, connectionId);
}

void Service::Impl::sendSuccess(JsonVariantConst id, uint16_t connectionId)
{
    JsonDocument response;
    response["id"].set(id);
    JsonObject result = response["result"].to<JsonObject>();
    result["ok"]      = true;
    std::string json;
    serializeJson(response, json);
    sendJson(json, connectionId);
}

void Service::Impl::updateThreads(JsonArrayConst values, uint16_t connectionId)
{
    Connection* connection = findConnection(connectionId);
    if (connection == nullptr) return;
    for (JsonObjectConst value : values) {
        const int id = value["id"] | -1;
        if (id < 0 || id >= static_cast<int>(connection->threads.size())) {
            continue;
        }
        Thread& thread = connection->threads[static_cast<size_t>(id)];
        if (value["c"].is<uint32_t>()) {
            thread.color = value["c"].as<uint32_t>() & 0x00FFFFFFU;
        }
        if (value["b"].is<float>()) {
            const float brightness = value["b"].as<float>();
            if (std::isfinite(brightness)) {
                thread.brightness = std::clamp(brightness, 0.0f, 1.0f);
            }
        }
        if (value["e"].is<const char*>()) {
            const char* effect  = value["e"].as<const char*>();
            size_t effectLength = 0;
            while (effect != nullptr && effectLength <= kMaxEffectLength && effect[effectLength] != '\0') {
                ++effectLength;
            }
            if (effect != nullptr && effectLength <= kMaxEffectLength) {
                thread.effect = effect;
            }
        }
        if (value["s"].is<float>()) {
            const float speed = value["s"].as<float>();
            if (std::isfinite(speed)) {
                thread.speed = std::clamp(speed, 0.0f, 100.0f);
            }
        }
    }
    publishTelemetry();
}

void Service::Impl::noteHostRpc(uint16_t connectionId)
{
    Connection* connection = findConnection(connectionId);
    if (connection == nullptr || !connection->secure) return;
    connection->rpcReady = true;
    connection->lastRpcAtMs = nowMs();
    registerHost(*connection);
    refreshRouting();
    publishTelemetry();
}

void Service::Impl::handleQuotaWrite(const WriteEvent& write)
{
    Connection* connection = findConnection(write.connId);
    if (connection == nullptr) return;
    if (write.length == 0 || write.length > detail::kMaxQuotaSize) {
        return;
    }
    JsonDocument update;
    const DeserializationError error = deserializeJson(update, write.data, write.length);
    if (error || !update.is<JsonObjectConst>()) {
        ESP_LOGW(kTag, "quota update rejected: %s", error.c_str());
        return;
    }

    const JsonObjectConst object     = update.as<JsonObjectConst>();
    const JsonVariantConst remaining = object["remaining_percent"];
    const JsonVariantConst reset     = object["reset_in_seconds"];
    if (!remaining.is<float>() || !reset.is<uint32_t>()) {
        ESP_LOGW(kTag, "quota update rejected: invalid fields");
        return;
    }
    const float percentage = remaining.as<float>();
    if (!std::isfinite(percentage) || percentage < 0.0f || percentage > 100.0f) {
        ESP_LOGW(kTag, "quota update rejected: invalid percentage");
        return;
    }

    const JsonVariantConst fiveHourUsed = object["five_hour_used_percent"];
    const JsonVariantConst weeklyUsed   = object["weekly_used_percent"];
    const auto validUsedPercent         = [](JsonVariantConst value) {
        if (value.isNull() || !value.is<float>() || value.is<bool>()) {
            return false;
        }
        const float used = value.as<float>();
        return std::isfinite(used) && used >= 0.0f && used <= 100.0f;
    };
    const bool fiveHourAvailable = validUsedPercent(fiveHourUsed);
    const bool weeklyAvailable   = validUsedPercent(weeklyUsed);
    if ((!fiveHourUsed.isNull() && !fiveHourAvailable) || (!weeklyUsed.isNull() && !weeklyAvailable)) {
        ESP_LOGW(kTag, "quota update contains invalid rate-limit percentage fields");
    }

    const uint32_t receivedAt = nowMs();

    connection->quota.remainingPercent         = percentage;
    connection->quota.resetInSeconds           = reset.as<uint32_t>();
    connection->quota.receivedAtMs             = receivedAt;
    connection->quota.available                = true;
    connection->rateLimits.fiveHourUsedPercent = fiveHourAvailable ? fiveHourUsed.as<float>() : 0.0f;
    connection->rateLimits.weeklyUsedPercent   = weeklyAvailable ? weeklyUsed.as<float>() : 0.0f;
    connection->rateLimits.receivedAtMs        = receivedAt;
    connection->rateLimits.fiveHourAvailable   = fiveHourAvailable;
    connection->rateLimits.weeklyAvailable     = weeklyAvailable;
    publishTelemetry();
    ESP_LOGI(kTag, "quota remaining=%.1f reset=%lus", percentage, static_cast<unsigned long>(reset.as<uint32_t>()));
    if (fiveHourAvailable || weeklyAvailable) {
        ESP_LOGI(kTag, "rate-limit fields five_hour=%d weekly=%d", fiveHourAvailable, weeklyAvailable);
    }
}

void Service::Impl::processCommand(const Command& command)
{
    if ((command.type == CommandType::kKey || command.type == CommandType::kJoystick) &&
        command.controlEpoch != controlEpoch.load(std::memory_order_acquire)) return;
    switch (command.type) {
        case CommandType::kSelectHost:
            if (hostSelection.selectHost(command.hostId)) usbResumePreference.manuallySelected(command.hostId);
            refreshRouting();
            break;
        case CommandType::kBattery:
            setBatteryNow(command.percentage, command.charging);
            break;
        case CommandType::kKey:
            if (command.action != 0) {
                if (sendKeyNow(command.key, command.action, command.agent)) {
                    cancelOlderPendingRelease(command);
                }
            } else if (std::any_of(heldKeys.begin(), heldKeys.end(), [&command](const HeldKey& held) {
                return held.active && held.agent == command.agent && std::strcmp(held.key, command.key) == 0;
            }) && !sendKeyNow(command.key, command.action, command.agent)) {
                const bool stillHeld = std::any_of(heldKeys.begin(), heldKeys.end(), [&command](const HeldKey& item) {
                    return item.active && item.agent == command.agent &&
                           std::strncmp(item.key, command.key, sizeof(item.key)) == 0;
                });
                if (stillHeld) {
                    schedulePendingRelease(command);
                }
            }
            break;
        case CommandType::kJoystick:
            if (command.distance > 0.0f) {
                if (sendJoystickNow(command.angle, command.distance)) {
                    cancelOlderPendingRelease(command);
                }
            } else if (joystickHeld && !sendJoystickNow(command.angle, command.distance)) {
                schedulePendingRelease(command);
            }
            break;
        case CommandType::kFridayTransfer:
            sendFridayTransferNow(command.fridayPacket, sizeof(command.fridayPacket));
            break;
        case CommandType::kFridayTravelMode:
            fridayTravelActive = command.fridayTravelActive;
            updateFridayConnectionIntervals(fridayTravelActive || fridayCapsuleActive);
            break;
        case CommandType::kFridayCapsuleMode:
            fridayCapsuleActive = command.fridayCapsuleActive;
            updateFridayConnectionIntervals(fridayTravelActive || fridayCapsuleActive);
            break;
    }
}

bool Service::Impl::processQueuedCommands()
{
    bool didWork = false;
    for (size_t count = 0; count < 16; ++count) {
        Command normal{};
        Command release{};
        const bool hasNormal  = xQueuePeek(commandQueue, &normal, 0) == pdTRUE;
        const bool hasRelease = xQueuePeek(releaseQueue, &release, 0) == pdTRUE;
        if (!hasNormal && !hasRelease) {
            break;
        }

        const bool chooseRelease =
            hasRelease && (!hasNormal || static_cast<int32_t>(release.sequence - normal.sequence) < 0);
        Command command{};
        QueueHandle_t source = chooseRelease ? releaseQueue : commandQueue;
        if (xQueueReceive(source, &command, 0) != pdTRUE) {
            continue;
        }
        didWork = true;
        processCommand(command);
    }
    return didWork;
}

bool Service::Impl::processCapsuleFrames()
{
    if (capsuleQueue == nullptr) {
        return false;
    }
    bool didWork = false;
    for (size_t count = 0; count < 4; ++count) {
        CapsuleFrame frame{};
        if (xQueueReceive(capsuleQueue, &frame, 0) != pdTRUE) {
            break;
        }
        didWork = true;
        sendFridayCapsuleNow(frame.data, frame.length);
    }
    return didWork;
}

bool Service::Impl::processPendingReleases()
{
    bool didWork               = false;
    const uint32_t currentTime = nowMs();
    for (PendingRelease& pending : pendingReleases) {
        if (!pending.active || static_cast<int32_t>(currentTime - pending.retryAtMs) < 0) {
            continue;
        }
        didWork        = true;
        bool delivered = false;
        if (pending.command.type == CommandType::kKey) {
            delivered = sendKeyNow(pending.command.key, 0, pending.command.agent);
        } else if (pending.command.type == CommandType::kJoystick) {
            delivered = sendJoystickNow(pending.command.angle, 0.0f);
        } else {
            delivered = true;
        }
        if (delivered) {
            pending = PendingRelease{};
        } else {
            pending.retryAtMs = currentTime + kReleaseRetryMs;
        }
    }
    return didWork;
}

bool Service::Impl::processRpcTimeouts()
{
    bool cleared               = false;
    const uint32_t currentTime = nowMs();
    for (Connection& connection : connections) {
        if (!connection.rpcBuffer.empty() &&
            static_cast<uint32_t>(currentTime - connection.rpcLastFragmentAtMs) > kRpcAssemblyTimeoutMs) {
            clearRpcAssembly(connection);
            cleared = true;
        }
    }
    return cleared;
}

void Service::Impl::schedulePendingRelease(const Command& command)
{
    const auto sameRelease = [&command](const PendingRelease& pending) {
        if (!pending.active || pending.command.type != command.type) {
            return false;
        }
        if (command.type == CommandType::kJoystick) {
            return true;
        }
        return pending.command.agent == command.agent &&
               std::strncmp(pending.command.key, command.key, sizeof(pending.command.key)) == 0;
    };
    auto existing = std::find_if(pendingReleases.begin(), pendingReleases.end(), sameRelease);
    if (existing != pendingReleases.end()) {
        existing->retryAtMs = nowMs() + kReleaseRetryMs;
        existing->command   = command;
        return;
    }
    const auto empty = std::find_if(pendingReleases.begin(), pendingReleases.end(),
                                    [](const PendingRelease& pending) { return !pending.active; });
    if (empty == pendingReleases.end()) {
        ESP_LOGE(kTag, "pending release table full; closing HID connections");
        if (routedConnection == kUsbConnectionId) {
            usb::disconnect();
            usbReconnectAtMs = nowMs() + 150;
        }
        for (const Connection& connection : connections) {
            if (connection.active && connection.secure && connection.inputNotifications &&
                gattsIf != ESP_GATT_IF_NONE) {
                esp_ble_gatts_close(gattsIf, connection.id);
            }
        }
        return;
    }
    empty->active    = true;
    empty->retryAtMs = nowMs() + kReleaseRetryMs;
    empty->command   = command;
}

void Service::Impl::cancelOlderPendingRelease(const Command& command)
{
    for (PendingRelease& pending : pendingReleases) {
        if (!pending.active || pending.command.type != command.type ||
            static_cast<int32_t>(pending.command.sequence - command.sequence) >= 0) {
            continue;
        }
        bool sameControl = command.type == CommandType::kJoystick;
        if (command.type == CommandType::kKey) {
            sameControl = pending.command.agent == command.agent &&
                          std::strncmp(pending.command.key, command.key, sizeof(pending.command.key)) == 0;
        }
        if (sameControl) {
            pending = PendingRelease{};
        }
    }
}

void Service::Impl::setBatteryNow(uint8_t percentage, bool isCharging)
{
    batteryPercentage     = std::min<uint8_t>(percentage, 100);
    charging              = isCharging;
    detail::kBatteryLevel = batteryPercentage;
    if (batteryHandles[detail::kBatteryValue] == 0) {
        return;
    }
    esp_ble_gatts_set_attr_value(batteryHandles[detail::kBatteryValue], 1, &batteryPercentage);
    for (const Connection& connection : connections) {
        if (!connection.active || !connection.batteryNotifications || connection.congested || connection.mtu < 4) {
            continue;
        }
        const esp_err_t error = esp_ble_gatts_send_indicate(
            gattsIf, connection.id, batteryHandles[detail::kBatteryValue], 1, &batteryPercentage, false);
        if (error != ESP_OK) {
            ESP_LOGD(kTag, "battery notify id=%u failed: %s", connection.id, esp_err_to_name(error));
        }
    }
}

bool Service::Impl::sendKeyNow(const char* key, uint8_t action, int8_t agent)
{
    JsonDocument message;
    message["method"] = "v.oai.hid";
    JsonObject params = message["params"].to<JsonObject>();
    params["k"]       = key;
    params["act"]     = action;
    if (agent >= 0) {
        params["ag"] = agent;
    }
    std::string json;
    serializeJson(message, json);
    const bool delivered     = sendJson(json);
    const bool hasSubscriber = routedConnection >= 0;
    if (delivered || (action != 0 && hasSubscriber)) {
        rememberKeyState(key, action, agent);
    }
    ESP_LOGI(kTag, "HID key=%s action=%u", key, action);
    return delivered;
}

bool Service::Impl::sendJoystickNow(float angle, float distance)
{
    JsonDocument message;
    message["method"] = "v.oai.rad";
    JsonObject params = message["params"].to<JsonObject>();
    params["a"]       = angle;
    params["d"]       = distance;
    std::string json;
    serializeJson(message, json);
    const bool delivered     = sendJson(json);
    const bool hasSubscriber = routedConnection >= 0;
    if (delivered || (distance > 0.0f && hasSubscriber)) {
        joystickHeld      = distance > 0.0f;
        heldJoystickAngle = angle;
    }
    return delivered;
}

bool Service::Impl::sendFridayTransferNow(const uint8_t* packet, size_t length)
{
    if (packet == nullptr || length != friday::presence::PacketSize || gattsIf == ESP_GATT_IF_NONE ||
        fridayHandles[detail::kFridayTransferValue] == 0) {
        return false;
    }

    esp_ble_gatts_set_attr_value(fridayHandles[detail::kFridayTransferValue], static_cast<uint16_t>(length),
                                 const_cast<uint8_t*>(packet));
    bool foundRecipient = false;
    bool allDelivered   = true;
    for (Connection& connection : connections) {
        if (!connection.active || !connection.fridayPeer || !connection.fridayTransferNotifications) {
            continue;
        }
        foundRecipient = true;
        if (connection.congested || connection.mtu < length + 3) {
            allDelivered = false;
            continue;
        }
        const esp_err_t error = esp_ble_gatts_send_indicate(
            gattsIf, connection.id, fridayHandles[detail::kFridayTransferValue], static_cast<uint16_t>(length),
            const_cast<uint8_t*>(packet), false);
        if (error != ESP_OK) {
            allDelivered = false;
            ESP_LOGW(kTag, "Friday travel notify id=%u failed: %s", connection.id, esp_err_to_name(error));
        }
    }
    return foundRecipient && allDelivered;
}

bool Service::Impl::sendFridayCapsuleNow(const uint8_t* packet, size_t length)
{
    if (packet == nullptr || length < 4 || length > FRIDAY_CAPSULE_MAX_PACKET_SIZE ||
        packet[0] != FRIDAY_CAPSULE_PROTOCOL_VERSION || gattsIf == ESP_GATT_IF_NONE ||
        fridayHandles[detail::kFridayCapsuleDataValue] == 0) {
        return false;
    }

    const uint16_t session = friday_capsule_read_u16(packet + 2);
    esp_ble_gatts_set_attr_value(fridayHandles[detail::kFridayCapsuleDataValue],
                                 static_cast<uint16_t>(length), const_cast<uint8_t*>(packet));
    bool foundRecipient = false;
    bool delivered = false;
    for (Connection& connection : connections) {
        if (!connection.active || !connection.secure || !connection.fridayPeer ||
            !connection.fridayCapsuleNotifications) {
            continue;
        }
        foundRecipient = true;
        if (connection.congested || connection.mtu < length + 3) {
            continue;
        }
        const esp_err_t error = esp_ble_gatts_send_indicate(
            gattsIf, connection.id, fridayHandles[detail::kFridayCapsuleDataValue],
            static_cast<uint16_t>(length), const_cast<uint8_t*>(packet), false);
        if (error == ESP_OK) {
            delivered = true;
        } else {
            ESP_LOGW(kTag, "Friday capsule notify id=%u failed: %s", connection.id,
                     esp_err_to_name(error));
        }
    }
    if (!foundRecipient || !delivered) {
        friday::capsule::CapsuleLink::instance().noteTransportFailure(session, nowMs());
        return false;
    }
    return true;
}

void Service::Impl::updateFridayConnectionIntervals(bool active)
{
    for (const Connection& connection : connections) {
        if (!connection.active || !connection.fridayPeer) {
            continue;
        }
        esp_ble_conn_update_params_t parameters{};
        std::memcpy(parameters.bda, connection.address, ESP_BD_ADDR_LEN);
        if (active) {
            // Connection interval units are 1.25 ms. Keep travel handoffs
            // responsive, then return to the low-duty office connection.
            parameters.min_int = 12;  // 15 ms
            parameters.max_int = 24;  // 30 ms
            parameters.latency = 0;
        } else if (connection.secure && connection.inputNotifications) {
            // One macOS link may carry both Friday and Codex HID. Do not let
            // Friday's idle policy make Codex button feedback feel sluggish.
            parameters.min_int = 24;  // 30 ms
            parameters.max_int = 40;  // 50 ms
            parameters.latency = 0;
        } else {
            parameters.min_int = 160;  // 200 ms
            parameters.max_int = 320;  // 400 ms
            parameters.latency = 3;
        }
        parameters.timeout = 600;  // 6 seconds, in 10 ms units
        const esp_err_t error = esp_ble_gap_update_conn_params(&parameters);
        if (error != ESP_OK) {
            ESP_LOGW(kTag, "Friday %s interval request id=%u failed: %s", active ? "travel" : "relaxed",
                     connection.id, esp_err_to_name(error));
        }
    }
}

void Service::Impl::rememberKeyState(const char* key, uint8_t action, int8_t agent)
{
    const auto matches = [key, agent](const HeldKey& item) {
        return item.active && item.agent == agent && std::strncmp(item.key, key, sizeof(item.key)) == 0;
    };
    auto existing = std::find_if(heldKeys.begin(), heldKeys.end(), matches);
    if (action == 0) {
        if (existing != heldKeys.end()) {
            *existing = HeldKey{};
        }
        return;
    }
    if (existing != heldKeys.end()) {
        return;
    }
    const auto empty = std::find_if(heldKeys.begin(), heldKeys.end(), [](const HeldKey& item) { return !item.active; });
    if (empty == heldKeys.end()) {
        ESP_LOGW(kTag, "held key table full; key=%s", key);
        return;
    }
    empty->active = true;
    empty->agent  = agent;
    std::strncpy(empty->key, key, sizeof(empty->key) - 1);
}

bool Service::Impl::sendJson(const std::string& json, int32_t targetConnection)
{
    const int32_t endpoint = targetConnection >= 0 ? targetConnection : routedConnection;
    if (endpoint < 0) return false;
    Connection* connection = findConnection(static_cast<uint16_t>(endpoint));
    if (connection == nullptr) return false;
    return sendJsonToConnection(json + "\n", *connection);
}

bool Service::Impl::sendJsonToConnection(const std::string& framed, Connection& connection)
{
    if (!connection.active || !connection.secure || !connection.inputNotifications || connection.congested ||
        connection.mtu < detail::kReportBodySize + 3) {
        return false;
    }
    if (connection.usb) {
        if (!usb::serviceBudgetAvailable()) return false;
        return usb::sendFramedMessage(framed.data(), framed.size(),
            [this, &connection](const uint8_t* report) { return sendReport(report, connection); },
            [this] {
                // Do not let a half-written RPC contaminate a later response.
                // Re-enumeration also cancels old held controls at the host.
                usb::disconnect();
                usbReconnectAtMs = nowMs() + 150;
            });
    }
    for (size_t offset = 0; offset < framed.size();) {
        const size_t chunk                      = std::min(detail::kPayloadSize, framed.size() - offset);
        uint8_t report[detail::kReportBodySize] = {};
        report[0]                               = 2;
        report[1]                               = static_cast<uint8_t>(chunk);
        std::memcpy(report + 2, framed.data() + offset, chunk);
        if (!sendReport(report, connection)) {
            return false;
        }
        offset += chunk;
        if (offset < framed.size()) {
            const TickType_t delayTicks = pdMS_TO_TICKS(4);
            vTaskDelay(delayTicks == 0 ? 1 : delayTicks);
        }
    }
    return true;
}

bool Service::Impl::sendReport(const uint8_t* report, Connection& connection)
{
    if (connection.usb) return usb::sendReport(report, detail::kReportBodySize, usbEpoch);
    portENTER_CRITICAL(&callbackSessionMux);
    const uint32_t generation = callbackSessions.token(connection.id, connection.address);
    portEXIT_CRITICAL(&callbackSessionMux);
    if (generation == 0 || generation != connection.linkGeneration) return false;
    esp_ble_gatts_set_attr_value(hidHandles[detail::kHidInputValue], detail::kReportBodySize, report);
    const esp_err_t error = esp_ble_gatts_send_indicate(gattsIf, connection.id, hidHandles[detail::kHidInputValue],
                                                        detail::kReportBodySize, const_cast<uint8_t*>(report), false);
    if (error != ESP_OK) {
        ESP_LOGD(kTag, "input notify id=%u failed: %s", connection.id, esp_err_to_name(error));
        return false;
    }
    return true;
}

void Service::Impl::handleCallbackQueueLoss(bool controlLost, bool writeLost)
{
    if (writeLost) {
        // A missing fragment invalidates only the current RPC assembly. Quota
        // writes are independent snapshots, so dropping one leaves the last
        // valid snapshot intact.
        for (auto& peer : connections) {
            clearRpcAssembly(peer);
            peer.rpcDiscardUntilNewline = true;
        }
        ESP_LOGE(kTag, "BLE write callback queue overflow; RPC resynchronized");
    }
    if (!controlLost) {
        return;
    }

    // There is no public Bluedroid equivalent of BLEServer::getPeerDevices().
    // Close every connection we still know before failing closed, so the real
    // controller state and our local view cannot intentionally diverge.
    for (Connection& connection : connections) {
        if (connection.active && gattsIf != ESP_GATT_IF_NONE) {
            esp_ble_gatts_close(gattsIf, connection.id);
        }
        hostSelection.disconnect(connection.id);
        connection = Connection{};
    }
    refreshRouting();
    syncFridayLinkState();
    for (auto& peer : connections) clearRpcAssembly(peer);
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    state.connected = routedConnection >= 0;
    if (routedConnection < 0) clearHostRpcLocked();
    ++state.revision;
    xSemaphoreGive(stateMutex);
    advertisingDataPending = false;
    scanResponsePending    = false;
    advertisingActive      = false;
    scheduleAdvertisingRetry();
    ESP_LOGE(kTag, "BLE callback queue overflow; connection state failed closed");
}

void Service::Impl::handleReleaseQueueLoss()
{
    // Drop queued presses before repairing releases. A press that has not yet
    // reached the host must not be delivered after its release was lost.
    xQueueReset(commandQueue);
    xQueueReset(releaseQueue);

    for (const HeldKey& held : heldKeys) {
        if (!held.active) {
            continue;
        }
        Command release{};
        release.type     = CommandType::kKey;
        release.sequence = nextCommandSequence.fetch_add(1, std::memory_order_relaxed);
        release.action   = 0;
        release.agent    = held.agent;
        std::memcpy(release.key, held.key, sizeof(release.key));
        schedulePendingRelease(release);
    }
    if (joystickHeld) {
        Command release{};
        release.type     = CommandType::kJoystick;
        release.sequence = nextCommandSequence.fetch_add(1, std::memory_order_relaxed);
        release.angle    = heldJoystickAngle;
        release.distance = 0.0f;
        schedulePendingRelease(release);
    }
    ESP_LOGE(kTag, "release queue overflow repaired with fail-safe releases");
}

void Service::Impl::clearHostRpcLocked()
{
    state.hostRpcObserved  = false;
    state.lastHostRpcAtMs  = 0;
    hostRpcConnectionId    = 0;
    hostRpcConnectionValid = false;
}

void Service::Impl::clearRpcAssembly(Connection& connection)
{
    connection.rpcBuffer.clear();
    connection.rpcLastFragmentAtMs = 0;
}

void Service::Impl::clearAllRpcAssemblies()
{
    clearRpcAssembly(usbConnection);
    for (Connection& connection : connections) {
        clearRpcAssembly(connection);
    }
}

uint16_t* Service::Impl::handlesFor(TableId id)
{
    switch (id) {
        case TableId::kDeviceInfo:
            return deviceInfoHandles;
        case TableId::kHid:
            return hidHandles;
        case TableId::kBattery:
            return batteryHandles;
        case TableId::kQuota:
            return quotaHandles;
        case TableId::kFriday:
            return fridayHandles;
        case TableId::kHostIdentity:
            return hostIdentityHandles;
        case TableId::kCount:
            return nullptr;
    }
    return nullptr;
}

size_t Service::Impl::handleCountFor(TableId id) const
{
    switch (id) {
        case TableId::kDeviceInfo:
            return detail::kDiCount;
        case TableId::kHid:
            return detail::kHidCount;
        case TableId::kBattery:
            return detail::kBatteryCount;
        case TableId::kQuota:
            return detail::kQuotaCount;
        case TableId::kFriday:
            return detail::kFridayCount;
        case TableId::kHostIdentity:
            return detail::kHostIdentityCount;
        case TableId::kCount:
            return 0;
    }
    return 0;
}

uint16_t Service::Impl::serviceHandleFor(TableId id) const
{
    switch (id) {
        case TableId::kDeviceInfo:
            return deviceInfoHandles[detail::kDiService];
        case TableId::kHid:
            return hidHandles[detail::kHidService];
        case TableId::kBattery:
            return batteryHandles[detail::kBatteryService];
        case TableId::kQuota:
            return quotaHandles[detail::kQuotaService];
        case TableId::kFriday:
            return fridayHandles[detail::kFridayService];
        case TableId::kHostIdentity:
            return hostIdentityHandles[detail::kHostIdentityService];
        case TableId::kCount:
            return 0;
    }
    return 0;
}

TableId Service::Impl::tableForServiceHandle(uint16_t serviceHandle) const
{
    for (uint8_t index = 0; index < toIndex(TableId::kCount); ++index) {
        const TableId id = static_cast<TableId>(index);
        if (serviceHandleFor(id) == serviceHandle) {
            return id;
        }
    }
    return TableId::kCount;
}

Service::Service() : impl_(new Impl())
{
}

Service::~Service() = default;

bool Service::begin()
{
    return impl_->begin();
}

State Service::snapshot() const
{
    return impl_->snapshot();
}

void Service::setBattery(uint8_t percentage, bool charging)
{
    impl_->enqueueBattery(percentage, charging);
}

void Service::sendKey(const char* key, uint8_t action, int8_t agent, uint32_t expectedEpoch)
{
    impl_->enqueueKey(key, action, agent, expectedEpoch);
}

void Service::sendJoystick(float angle, float distance, uint32_t expectedEpoch)
{
    impl_->enqueueJoystick(angle, distance, expectedEpoch);
}

bool Service::selectHost(const std::string& hostId)
{
    return impl_->enqueueSelectHost(hostId);
}

bool Service::sendFridayTransfer(const uint8_t* packet, size_t length)
{
    return impl_->enqueueFridayTransfer(packet, length);
}

bool Service::setFridayTravelActive(bool active)
{
    return impl_->enqueueFridayTravelMode(active);
}

bool Service::sendFridayCapsule(const uint8_t* packet, size_t length)
{
    return impl_->enqueueFridayCapsule(packet, length);
}

bool Service::setFridayCapsuleActive(bool active)
{
    return impl_->enqueueFridayCapsuleMode(active);
}

Service& GetService()
{
    static Service service;
    return service;
}

}  // namespace codex_micro
