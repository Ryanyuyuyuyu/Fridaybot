// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Codex Micro for StopWatch contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace codex_micro {

enum class Transport { Ble, Usb };

struct HostRoute {
    Transport transport = Transport::Ble;
    int32_t endpoint = -1;

    bool operator==(const HostRoute& other) const
    {
        return transport == other.transport && endpoint == other.endpoint;
    }
    bool operator!=(const HostRoute& other) const { return !(*this == other); }
};

struct HostInfo {
    std::string id;
    std::string name;
    bool online = false;
    bool selected = false;
    bool usbAvailable = false;
    bool bleAvailable = false;
};

// Platform-independent selection policy. A transport must finish its data
// handshake before registerReady(); USB power alone is not a ready route.
// IDs are helper-generated installation UUIDs, or temporary legacy BLE aliases.
// This class never reads a hardware serial number or generates a host identity.
// The caller owns synchronization, persistence, and connection-epoch checks:
// disconnect() must not receive a stale event for an already-reused endpoint.
class HostSelection {
public:
    static constexpr size_t kMaxHosts = 8;
    static constexpr size_t kMaxRoutes = 16;
    static constexpr size_t kMaxHostIdBytes = 96;
    static constexpr size_t kMaxHostNameBytes = 128;

    // Endpoint numbers share a namespace: reserve a non-BLE endpoint for USB.
    // Repeated handshakes update metadata without retaking manual selection.
    // Upgrading the same endpoint from a legacy alias to a stable ID also
    // migrates its remembered preference and merges any known host entry.
    bool registerReady(int32_t endpoint, Transport transport, const std::string& id,
                       const std::string& name, bool legacyIdentity = false);
    bool disconnect(int32_t endpoint);

    bool rememberHost(const std::string& id, const std::string& name);
    bool restorePreferredHost(const std::string& id, const std::string& name = {});
    // A trusted, authenticated transport identity may resolve a remembered
    // legacy alias even before its native HID route becomes ready. The caller
    // must only supply an alias known to belong to this host. This preserves
    // another host's manual selection and does not make an offline host ready.
    bool migrateIdentity(const std::string& alias, const std::string& id, const std::string& name);
    // A remembered offline host is selectable and stays offline until that
    // host returns. Other ready hosts are never an implicit fallback.
    bool selectHost(const std::string& id);

    const std::string& preferredHostId() const { return preferredHostId_; }
    const std::string& selectedHostId() const { return preferredHostId_; }
    std::string selectedName() const;
    std::optional<HostRoute> selectedRoute() const;
    std::vector<HostInfo> hosts() const;

private:
    struct RememberedHost {
        std::string id;
        std::string name;
        uint64_t lastSeen = 0;
    };
    struct ReadyRoute {
        HostRoute route;
        std::string hostId;
        bool legacyIdentity = false;
    };

    static bool validIdentity(const std::string& id, const std::string& name);
    bool hasRoute(const std::string& id) const;
    std::optional<HostRoute> routeForHost(const std::string& id) const;

    std::vector<RememberedHost> hosts_;
    std::vector<ReadyRoute> routes_;
    std::string preferredHostId_;
    uint64_t sequence_ = 0;
};

}  // namespace codex_micro
