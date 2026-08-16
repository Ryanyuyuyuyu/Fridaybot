// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Codex Micro for StopWatch contributors

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace codex_micro {

struct Thread {
    uint32_t color     = 0;
    float brightness   = 0.0f;
    std::string effect = "off";
    float speed        = 0.0f;
};

struct Quota {
    float remainingPercent  = 0.0f;
    uint32_t resetInSeconds = 0;
    uint32_t receivedAtMs   = 0;
    bool available          = false;
};

struct State {
    std::array<Thread, 6> threads{};
    Quota quota{};
    bool connected           = false;
    bool hostRpcObserved     = false;
    uint32_t lastHostRpcAtMs = 0;
    uint32_t connectionEpoch = 0;
    uint32_t revision        = 0;
};

// A process-lifetime BLE service. Closing the Codex Micro UI must not destroy
// this object: begin() starts an independent worker that owns the Bluetooth
// stack, GATT database, RPC parser, and notifications.
class Service {
public:
    static constexpr uint16_t kVendorId  = 0x303A;
    static constexpr uint16_t kProductId = 0x8360;
    static constexpr uint8_t kReportId   = 6;

    bool begin();
    State snapshot() const;
    void setBattery(uint8_t percentage, bool charging);
    void sendKey(const char* key, uint8_t action, int8_t agent = -1);
    void sendJoystick(float angle, float distance);

private:
    Service();
    ~Service();
    Service(const Service&)            = delete;
    Service& operator=(const Service&) = delete;

    struct Impl;
    Impl* impl_;

    friend Service& GetService();
};

Service& GetService();

}  // namespace codex_micro
