// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

namespace codex_micro::usb {

inline constexpr uint16_t kVendorId = 0x303a;
inline constexpr uint16_t kProductId = 0x8360;
inline constexpr uint8_t kRpcReportId = 6;
inline constexpr uint8_t kIdentityReportId = 7;
inline constexpr uint8_t kQuotaReportId = 8;
inline constexpr size_t kRpcBodySize = 63;
inline constexpr size_t kIdentityBodySize = 128;
inline constexpr size_t kQuotaBodySize = 256;

// The RPC collection matches Codex Micro's BLE vendor HID collection. Feature
// report 7 is a separate atomic identity channel, so the identity helper cannot
// interleave fragments with the desktop application's RPC writer.
inline constexpr uint8_t kReportDescriptor[] = {
    0x06, 0x00, 0xff,       // Usage Page (Vendor 0xff00)
    0x09, 0x01,             // Usage (1)
    0xa1, 0x01,             // Application collection
    0x85, kRpcReportId,
    0x15, 0x00,
    0x26, 0xff, 0x00,
    0x75, 0x08,
    0x95, kRpcBodySize,
    0x09, 0x01,
    0x81, 0x02,             // Input: 63 bytes
    0x95, kRpcBodySize,
    0x09, 0x02,
    0x91, 0x02,             // Output: 63 bytes
    0x85, kIdentityReportId,
    0x95, kIdentityBodySize,
    0x09, 0x03,
    0xb1, 0x02,             // Feature: 128 bytes
    0x85, kQuotaReportId,
    0x96, 0x00, 0x01,       // Report count 256 (16-bit value)
    0x09, 0x04,
    0xb1, 0x02,             // Feature: quota JSON, 256 bytes
    0xc0,
};

}  // namespace codex_micro::usb
