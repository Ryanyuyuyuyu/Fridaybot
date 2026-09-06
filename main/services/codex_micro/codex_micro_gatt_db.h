// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Codex Micro for StopWatch contributors

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_gatt_defs.h"

namespace codex_micro::detail {

inline constexpr char kDeviceName[]      = "Codex Micro";
inline constexpr char kManufacturer[]    = "Work Louder";
inline constexpr char kFirmwareVersion[] = "0.1.0-stopwatch-port";
inline constexpr size_t kPayloadSize     = 61;
inline constexpr size_t kReportBodySize  = 63;
inline constexpr size_t kMaxRpcSize      = 4096;
inline constexpr size_t kMaxQuotaSize    = 512;

// ESP-IDF/Bluedroid consumes 128-bit UUIDs in Bluetooth little-endian order.
inline uint8_t kQuotaServiceUuid[ESP_UUID_LEN_128] = {
    0x01, 0x5c, 0x0e, 0x1a, 0xf6, 0x4e, 0xbe, 0xbf, 0x71, 0x4a, 0xc2, 0x2a, 0x66, 0x4e, 0x0d, 0x7f,
};
inline uint8_t kQuotaWriteUuid[ESP_UUID_LEN_128] = {
    0x02, 0x5c, 0x0e, 0x1a, 0xf6, 0x4e, 0xbe, 0xbf, 0x71, 0x4a, 0xc2, 0x2a, 0x66, 0x4e, 0x0d, 0x7f,
};
inline uint8_t kHostIdentityUuid[ESP_UUID_LEN_128] = {
    0x03, 0x5c, 0x0e, 0x1a, 0xf6, 0x4e, 0xbe, 0xbf, 0x71, 0x4a, 0xc2, 0x2a, 0x66, 0x4e, 0x0d, 0x7f,
};
// Append this service after the five original tables. Never insert attributes
// into Quota/Friday: paired Macs cache their existing absolute GATT handles.
// Canonical service UUID: 7F0D4E66-2AC2-4A71-BFBE-4EF61A0E5C04.
inline uint8_t kHostIdentityServiceUuid[ESP_UUID_LEN_128] = {
    0x04, 0x5c, 0x0e, 0x1a, 0xf6, 0x4e, 0xbe, 0xbf, 0x71, 0x4a, 0xc2, 0x2a, 0x66, 0x4e, 0x0d, 0x7f,
};
// Canonical UUIDs, stored in the Bluetooth little-endian byte order used by
// ESP-IDF/Bluedroid:
//   46524944-4159-0001-8000-00805F9B34FB (Friday service)
//   46524944-4159-0002-8000-00805F9B34FB (context characteristic)
//   46524944-4159-0003-8000-00805F9B34FB (presence transfer characteristic)
inline uint8_t kFridayServiceUuid[ESP_UUID_LEN_128] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x01, 0x00, 0x59, 0x41, 0x44, 0x49, 0x52, 0x46,
};
inline uint8_t kFridayContextUuid[ESP_UUID_LEN_128] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x02, 0x00, 0x59, 0x41, 0x44, 0x49, 0x52, 0x46,
};
inline uint8_t kFridayTransferUuid[ESP_UUID_LEN_128] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x03, 0x00, 0x59, 0x41, 0x44, 0x49, 0x52, 0x46,
};
// Retired UUIDs occupy inert slots to keep the following Codex host-identity
// service at its released handles. No read, write, or notification capability.
inline uint8_t kFridayReservedUuid4[ESP_UUID_LEN_128] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x04, 0x00, 0x59, 0x41, 0x44, 0x49, 0x52, 0x46,
};
inline uint8_t kFridayReservedUuid5[ESP_UUID_LEN_128] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x05, 0x00, 0x59, 0x41, 0x44, 0x49, 0x52, 0x46,
};

inline uint8_t kReportMap[] = {
    0x06, 0x00, 0xFF,  // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,        // Usage (1)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x06,        // Report ID (6)
    0x15, 0x00,        // Logical Minimum (0)
    0x26, 0xFF, 0x00,  // Logical Maximum (255)
    0x75, 0x08,        // Report Size (8)
    0x95, 0x3F,        // Report Count (63)
    0x09, 0x01,        // Usage (1)
    0x81, 0x02,        // Input (Data, Variable, Absolute)
    0x95, 0x3F,        // Report Count (63)
    0x09, 0x02,        // Usage (2)
    0x91, 0x02,        // Output (Data, Variable, Absolute)
    0xC0,              // End Collection
};

enum DeviceInfoIndex : uint8_t {
    kDiService,
    kDiPnpDeclaration,
    kDiPnpValue,
    kDiManufacturerDeclaration,
    kDiManufacturerValue,
    kDiCount,
};

enum HidIndex : uint8_t {
    kHidService,
    kHidInfoDeclaration,
    kHidInfoValue,
    kHidReportMapDeclaration,
    kHidReportMapValue,
    kHidControlDeclaration,
    kHidControlValue,
    kHidProtocolDeclaration,
    kHidProtocolValue,
    kHidInputDeclaration,
    kHidInputValue,
    kHidInputCccd,
    kHidInputReference,
    kHidOutputDeclaration,
    kHidOutputValue,
    kHidOutputReference,
    kHidCount,
};

enum BatteryIndex : uint8_t {
    kBatteryService,
    kBatteryDeclaration,
    kBatteryValue,
    kBatteryCccd,
    kBatteryFormat,
    kBatteryCount,
};

enum QuotaIndex : uint8_t {
    kQuotaService,
    kQuotaDeclaration,
    kQuotaValue,
    kQuotaCount,
};

enum FridayIndex : uint8_t {
    kFridayService,
    kFridayContextDeclaration,
    kFridayContextValue,
    kFridayTransferDeclaration,
    kFridayTransferValue,
    kFridayTransferCccd,
    kFridayReservedDeclaration4,
    kFridayReservedValue4,
    kFridayReservedDescriptor4,
    kFridayReservedDeclaration5,
    kFridayReservedValue5,
    kFridayCount,
};

enum HostIdentityIndex : uint8_t {
    kHostIdentityService,
    kHostIdentityDeclaration,
    kHostIdentityValue,
    kHostIdentityCount,
};

inline uint16_t kPrimaryServiceUuid            = ESP_GATT_UUID_PRI_SERVICE;
inline uint16_t kCharacteristicDeclarationUuid = ESP_GATT_UUID_CHAR_DECLARE;
inline uint16_t kCccdUuid                      = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
inline uint16_t kReportReferenceUuid           = ESP_GATT_UUID_RPT_REF_DESCR;
inline uint16_t kPresentationFormatUuid        = ESP_GATT_UUID_CHAR_PRESENT_FORMAT;

inline uint16_t kDeviceInfoServiceUuid = ESP_GATT_UUID_DEVICE_INFO_SVC;
inline uint16_t kPnpUuid               = ESP_GATT_UUID_PNP_ID;
inline uint16_t kManufacturerUuid      = ESP_GATT_UUID_MANU_NAME;
inline uint16_t kHidServiceUuid        = ESP_GATT_UUID_HID_SVC;
inline uint16_t kHidInfoUuid           = ESP_GATT_UUID_HID_INFORMATION;
inline uint16_t kHidReportMapUuid      = ESP_GATT_UUID_HID_REPORT_MAP;
inline uint16_t kHidControlUuid        = ESP_GATT_UUID_HID_CONTROL_POINT;
inline uint16_t kHidProtocolUuid       = ESP_GATT_UUID_HID_PROTO_MODE;
inline uint16_t kHidReportUuid         = ESP_GATT_UUID_HID_REPORT;
inline uint16_t kBatteryServiceUuid    = ESP_GATT_UUID_BATTERY_SERVICE_SVC;
inline uint16_t kBatteryLevelUuid      = ESP_GATT_UUID_BATTERY_LEVEL;

inline uint8_t kPropertyRead                = ESP_GATT_CHAR_PROP_BIT_READ;
inline uint8_t kPropertyDisabled            = 0;
inline uint8_t kPropertyWriteNoResponse     = ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
inline uint8_t kPropertyReadWriteNoResponse = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
inline uint8_t kPropertyReadNotify          = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
inline uint8_t kPropertyReadWrite =
    ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
inline uint8_t kPropertyWrite = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
inline uint8_t kPropertyWriteNotify =
    ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR | ESP_GATT_CHAR_PROP_BIT_NOTIFY;

inline uint8_t kPnpValue[]                   = {0x02, 0x3A, 0x30, 0x60, 0x83, 0x01, 0x01};
inline uint8_t kManufacturerValue[]          = "Work Louder";
inline uint8_t kHidInfo[]                    = {0x11, 0x01, 0x00, 0x01};
inline uint8_t kHidControlPoint              = 0;
inline uint8_t kHidProtocolMode              = 1;
inline uint8_t kInputReport[kReportBodySize] = {};
// HOGP normally omits Report ID 6 and writes the 63-byte body. One extra byte
// permits compatibility hosts that prepend the Report ID to the same body.
inline uint8_t kOutputReport[kReportBodySize + 1] = {};
inline uint8_t kInputCccdValue[]                  = {0x00, 0x00};
inline uint8_t kBatteryCccdValue[]                = {0x00, 0x00};
inline uint8_t kInputReportReference[]            = {0x06, 0x01};
inline uint8_t kOutputReportReference[]           = {0x06, 0x02};
inline uint8_t kBatteryLevel                      = 100;
inline uint8_t kBatteryPresentation[]             = {
    0x04,        // uint8
    0x00,        // exponent
    0xAD, 0x27,  // percentage (0x27AD)
    0x01,        // Bluetooth SIG namespace
    0x00, 0x00,  // no description
};
inline uint8_t kQuotaInitialValue[kMaxQuotaSize] = {};
inline uint8_t kHostIdentityInitialValue[128] = {};
inline uint8_t kFridayContextInitialValue[4]      = {1, 0, 0, 0};
inline uint8_t kFridayTransferInitialValue[10]    = {};
inline uint8_t kFridayTransferCccdValue[2]        = {0x00, 0x00};
inline uint8_t kFridayReservedValue = 0;
inline uint8_t kFridayReservedDescriptor[2] = {0x00, 0x00};

#define CODEX_ATTR16(uuid_ptr, permissions, max_len, current_len, value_ptr)                          \
    {                                                                                                 \
        {ESP_GATT_AUTO_RSP},                                                                          \
        {                                                                                             \
            ESP_UUID_LEN_16, reinterpret_cast<uint8_t*>(uuid_ptr), permissions, max_len, current_len, \
                reinterpret_cast<uint8_t*>(value_ptr)                                                 \
        }                                                                                             \
    }

#define CODEX_ATTR128(uuid_ptr, permissions, max_len, current_len, value_ptr)                          \
    {                                                                                                  \
        {ESP_GATT_AUTO_RSP},                                                                           \
        {                                                                                              \
            ESP_UUID_LEN_128, reinterpret_cast<uint8_t*>(uuid_ptr), permissions, max_len, current_len, \
                reinterpret_cast<uint8_t*>(value_ptr)                                                  \
        }                                                                                              \
    }

inline const esp_gatts_attr_db_t kDeviceInfoDb[kDiCount] = {
    CODEX_ATTR16(&kPrimaryServiceUuid, ESP_GATT_PERM_READ, sizeof(kDeviceInfoServiceUuid),
                 sizeof(kDeviceInfoServiceUuid), &kDeviceInfoServiceUuid),
    CODEX_ATTR16(&kCharacteristicDeclarationUuid, ESP_GATT_PERM_READ, sizeof(kPropertyRead), sizeof(kPropertyRead),
                 &kPropertyRead),
    CODEX_ATTR16(&kPnpUuid, ESP_GATT_PERM_READ, sizeof(kPnpValue), sizeof(kPnpValue), kPnpValue),
    CODEX_ATTR16(&kCharacteristicDeclarationUuid, ESP_GATT_PERM_READ, sizeof(kPropertyRead), sizeof(kPropertyRead),
                 &kPropertyRead),
    CODEX_ATTR16(&kManufacturerUuid, ESP_GATT_PERM_READ, sizeof(kManufacturerValue) - 1, sizeof(kManufacturerValue) - 1,
                 kManufacturerValue),
};

inline const esp_gatts_attr_db_t kHidDb[kHidCount] = {
    CODEX_ATTR16(&kPrimaryServiceUuid, ESP_GATT_PERM_READ, sizeof(kHidServiceUuid), sizeof(kHidServiceUuid),
                 &kHidServiceUuid),
    CODEX_ATTR16(&kCharacteristicDeclarationUuid, ESP_GATT_PERM_READ, sizeof(kPropertyRead), sizeof(kPropertyRead),
                 &kPropertyRead),
    CODEX_ATTR16(&kHidInfoUuid, ESP_GATT_PERM_READ, sizeof(kHidInfo), sizeof(kHidInfo), kHidInfo),
    CODEX_ATTR16(&kCharacteristicDeclarationUuid, ESP_GATT_PERM_READ, sizeof(kPropertyRead), sizeof(kPropertyRead),
                 &kPropertyRead),
    CODEX_ATTR16(&kHidReportMapUuid, ESP_GATT_PERM_READ, sizeof(kReportMap), sizeof(kReportMap), kReportMap),
    CODEX_ATTR16(&kCharacteristicDeclarationUuid, ESP_GATT_PERM_READ, sizeof(kPropertyWriteNoResponse),
                 sizeof(kPropertyWriteNoResponse), &kPropertyWriteNoResponse),
    CODEX_ATTR16(&kHidControlUuid, ESP_GATT_PERM_WRITE, sizeof(kHidControlPoint), sizeof(kHidControlPoint),
                 &kHidControlPoint),
    CODEX_ATTR16(&kCharacteristicDeclarationUuid, ESP_GATT_PERM_READ, sizeof(kPropertyReadWriteNoResponse),
                 sizeof(kPropertyReadWriteNoResponse), &kPropertyReadWriteNoResponse),
    CODEX_ATTR16(&kHidProtocolUuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(kHidProtocolMode),
                 sizeof(kHidProtocolMode), &kHidProtocolMode),
    CODEX_ATTR16(&kCharacteristicDeclarationUuid, ESP_GATT_PERM_READ, sizeof(kPropertyReadNotify),
                 sizeof(kPropertyReadNotify), &kPropertyReadNotify),
    CODEX_ATTR16(&kHidReportUuid, ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED, sizeof(kInputReport),
                 sizeof(kInputReport), kInputReport),
    CODEX_ATTR16(&kCccdUuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(kInputCccdValue), sizeof(kInputCccdValue),
                 kInputCccdValue),
    CODEX_ATTR16(&kReportReferenceUuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(kInputReportReference),
                 sizeof(kInputReportReference), kInputReportReference),
    CODEX_ATTR16(&kCharacteristicDeclarationUuid, ESP_GATT_PERM_READ, sizeof(kPropertyReadWrite),
                 sizeof(kPropertyReadWrite), &kPropertyReadWrite),
    CODEX_ATTR16(&kHidReportUuid, ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED, sizeof(kOutputReport),
                 kReportBodySize, kOutputReport),
    CODEX_ATTR16(&kReportReferenceUuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(kOutputReportReference),
                 sizeof(kOutputReportReference), kOutputReportReference),
};

inline const esp_gatts_attr_db_t kBatteryDb[kBatteryCount] = {
    CODEX_ATTR16(&kPrimaryServiceUuid, ESP_GATT_PERM_READ, sizeof(kBatteryServiceUuid), sizeof(kBatteryServiceUuid),
                 &kBatteryServiceUuid),
    CODEX_ATTR16(&kCharacteristicDeclarationUuid, ESP_GATT_PERM_READ, sizeof(kPropertyReadNotify),
                 sizeof(kPropertyReadNotify), &kPropertyReadNotify),
    CODEX_ATTR16(&kBatteryLevelUuid, ESP_GATT_PERM_READ, sizeof(kBatteryLevel), sizeof(kBatteryLevel), &kBatteryLevel),
    CODEX_ATTR16(&kCccdUuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(kBatteryCccdValue),
                 sizeof(kBatteryCccdValue), kBatteryCccdValue),
    CODEX_ATTR16(&kPresentationFormatUuid, ESP_GATT_PERM_READ, sizeof(kBatteryPresentation),
                 sizeof(kBatteryPresentation), kBatteryPresentation),
};

inline const esp_gatts_attr_db_t kQuotaDb[kQuotaCount] = {
    CODEX_ATTR16(&kPrimaryServiceUuid, ESP_GATT_PERM_READ, sizeof(kQuotaServiceUuid), sizeof(kQuotaServiceUuid),
                 kQuotaServiceUuid),
    CODEX_ATTR16(&kCharacteristicDeclarationUuid, ESP_GATT_PERM_READ, sizeof(kPropertyWrite), sizeof(kPropertyWrite),
                 &kPropertyWrite),
    CODEX_ATTR128(kQuotaWriteUuid, ESP_GATT_PERM_WRITE_ENCRYPTED, sizeof(kQuotaInitialValue), 0, kQuotaInitialValue),
};

inline const esp_gatts_attr_db_t kFridayDb[kFridayCount] = {
    CODEX_ATTR16(&kPrimaryServiceUuid, ESP_GATT_PERM_READ, sizeof(kFridayServiceUuid), sizeof(kFridayServiceUuid),
                 kFridayServiceUuid),
    CODEX_ATTR16(&kCharacteristicDeclarationUuid, ESP_GATT_PERM_READ, sizeof(kPropertyReadWrite),
                 sizeof(kPropertyReadWrite), &kPropertyReadWrite),
    CODEX_ATTR128(kFridayContextUuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                  sizeof(kFridayContextInitialValue), sizeof(kFridayContextInitialValue),
                  kFridayContextInitialValue),
    CODEX_ATTR16(&kCharacteristicDeclarationUuid, ESP_GATT_PERM_READ, sizeof(kPropertyWriteNotify),
                 sizeof(kPropertyWriteNotify), &kPropertyWriteNotify),
    CODEX_ATTR128(kFridayTransferUuid, ESP_GATT_PERM_WRITE, sizeof(kFridayTransferInitialValue),
                  sizeof(kFridayTransferInitialValue), kFridayTransferInitialValue),
    CODEX_ATTR16(&kCccdUuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(kFridayTransferCccdValue),
                 sizeof(kFridayTransferCccdValue), kFridayTransferCccdValue),
    // Do not compact these five retired attributes: doing so shifts the
    // paired Mac's cached Codex host-identity handles. All operations are denied.
    CODEX_ATTR16(&kCharacteristicDeclarationUuid, ESP_GATT_PERM_READ, sizeof(kPropertyDisabled),
                 sizeof(kPropertyDisabled), &kPropertyDisabled),
    CODEX_ATTR128(kFridayReservedUuid4, 0, sizeof(kFridayReservedValue), 0, &kFridayReservedValue),
    CODEX_ATTR16(&kCccdUuid, 0, sizeof(kFridayReservedDescriptor),
                 sizeof(kFridayReservedDescriptor), kFridayReservedDescriptor),
    CODEX_ATTR16(&kCharacteristicDeclarationUuid, ESP_GATT_PERM_READ, sizeof(kPropertyDisabled),
                 sizeof(kPropertyDisabled), &kPropertyDisabled),
    CODEX_ATTR128(kFridayReservedUuid5, 0, sizeof(kFridayReservedValue), 0, &kFridayReservedValue),
};

inline const esp_gatts_attr_db_t kHostIdentityDb[kHostIdentityCount] = {
    CODEX_ATTR16(&kPrimaryServiceUuid, ESP_GATT_PERM_READ, sizeof(kHostIdentityServiceUuid),
                 sizeof(kHostIdentityServiceUuid), kHostIdentityServiceUuid),
    CODEX_ATTR16(&kCharacteristicDeclarationUuid, ESP_GATT_PERM_READ, sizeof(kPropertyWrite), sizeof(kPropertyWrite),
                 &kPropertyWrite),
    CODEX_ATTR128(kHostIdentityUuid, ESP_GATT_PERM_WRITE_ENCRYPTED, sizeof(kHostIdentityInitialValue), 0,
                  kHostIdentityInitialValue),
};

#undef CODEX_ATTR16
#undef CODEX_ATTR128

inline uint8_t kAdvertisingData[] = {
    0x02, 0x01, 0x06,        // flags
    0x03, 0x19, 0xC0, 0x03,  // generic HID appearance (0x03C0)
    0x03, 0x03, 0x12, 0x18,  // complete 16-bit UUID list: HID
    0x11, 0x06,              // incomplete 128-bit UUID list: Friday companion service
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x01, 0x00, 0x59, 0x41, 0x44, 0x49, 0x52, 0x46,
};

inline uint8_t kScanResponseData[] = {
    0x0C, 0x09, 'C',  'o',  'd',  'e',  'x',  ' ',  'M',  'i', 'c', 'r', 'o',
    0x11, 0x06,  // incomplete 128-bit UUID list: Codex quota service
    0x01, 0x5c, 0x0e, 0x1a, 0xf6, 0x4e, 0xbe, 0xbf, 0x71, 0x4a, 0xc2, 0x2a, 0x66, 0x4e, 0x0d, 0x7f,
};

static_assert(sizeof(kAdvertisingData) <= 31);
static_assert(sizeof(kScanResponseData) <= 31);
static_assert(sizeof(kReportMap) == 29);
static_assert(sizeof(kInputReport) == kReportBodySize);
// Existing bonded Friday/Codex Macs must retain all five original handle ranges.
static_assert(kDiCount == 5 && kHidCount == 16 && kBatteryCount == 5 && kQuotaCount == 3 && kFridayCount == 11);
static_assert(kHostIdentityCount == 3);
static_assert(static_cast<uint8_t>(kFridayCount) <= static_cast<uint8_t>(kHidCount));

}  // namespace codex_micro::detail
