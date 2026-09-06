// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Codex Micro for StopWatch contributors

#include "host_selection.h"

#include <algorithm>
#include <utility>

namespace codex_micro {

bool HostSelection::validIdentity(const std::string& id, const std::string& name)
{
    return !id.empty() && id.size() <= kMaxHostIdBytes && name.size() <= kMaxHostNameBytes &&
           id.find('\0') == std::string::npos && name.find('\0') == std::string::npos;
}

bool HostSelection::hasRoute(const std::string& id) const
{
    return std::any_of(routes_.begin(), routes_.end(), [&id](const ReadyRoute& route) {
        return route.hostId == id;
    });
}

bool HostSelection::rememberHost(const std::string& id, const std::string& name)
{
    if (!validIdentity(id, name)) {
        return false;
    }
    auto host = std::find_if(hosts_.begin(), hosts_.end(), [&id](const RememberedHost& item) {
        return item.id == id;
    });
    if (host == hosts_.end()) {
        if (hosts_.size() == kMaxHosts) {
            auto oldest = hosts_.end();
            for (auto item = hosts_.begin(); item != hosts_.end(); ++item) {
                if (item->id != preferredHostId_ && !hasRoute(item->id) &&
                    (oldest == hosts_.end() || item->lastSeen < oldest->lastSeen)) {
                    oldest = item;
                }
            }
            if (oldest == hosts_.end()) {
                return false;
            }
            hosts_.erase(oldest);
        }
        hosts_.push_back({id, name.empty() ? "Mac" : name, ++sequence_});
    } else {
        if (!name.empty()) {
            host->name = name;
        }
        host->lastSeen = ++sequence_;
    }
    return true;
}

bool HostSelection::restorePreferredHost(const std::string& id, const std::string& name)
{
    if (!rememberHost(id, name)) {
        return false;
    }
    preferredHostId_ = id;
    return true;
}

bool HostSelection::selectHost(const std::string& id)
{
    const auto host = std::find_if(hosts_.begin(), hosts_.end(), [&id](const RememberedHost& item) {
        return item.id == id;
    });
    if (host == hosts_.end()) {
        return false;
    }
    preferredHostId_ = id;
    host->lastSeen = ++sequence_;
    return true;
}

bool HostSelection::migrateIdentity(const std::string& alias, const std::string& id, const std::string& name)
{
    if (!validIdentity(alias, {}) || !validIdentity(id, name)) {
        return false;
    }
    if (alias == id) {
        return rememberHost(id, name);
    }
    auto known = std::find_if(hosts_.begin(), hosts_.end(), [&id](const RememberedHost& host) {
        return host.id == id;
    });
    auto legacy = std::find_if(hosts_.begin(), hosts_.end(), [&alias](const RememberedHost& host) {
        return host.id == alias;
    });
    if (legacy != hosts_.end()) {
        if (known == hosts_.end()) {
            legacy->id = id;
            if (!name.empty()) legacy->name = name;
            legacy->lastSeen = ++sequence_;
        } else {
            if (!name.empty()) known->name = name;
            known->lastSeen = ++sequence_;
            hosts_.erase(legacy);
        }
    } else if (!rememberHost(id, name)) {
        // Capacity failure leaves every route and preference untouched.
        return false;
    }
    for (auto& route : routes_) {
        if (route.hostId == alias) {
            route.hostId = id;
            route.legacyIdentity = false;
        }
    }
    if (preferredHostId_ == alias) {
        preferredHostId_ = id;
    }
    return true;
}

bool HostSelection::registerReady(int32_t endpoint, Transport transport, const std::string& id,
                                  const std::string& name, bool legacyIdentity)
{
    if (endpoint < 0 || !validIdentity(id, name)) {
        return false;
    }
    auto existing = std::find_if(routes_.begin(), routes_.end(), [endpoint](const ReadyRoute& route) {
        return route.route.endpoint == endpoint;
    });
    if (existing == routes_.end() && routes_.size() == kMaxRoutes) {
        return false;
    }
    // A late native-HID fallback on this same live endpoint must not downgrade
    // the stable identity supplied by a companion handshake.
    if (existing != routes_.end() && !existing->legacyIdentity && legacyIdentity &&
        existing->route.transport == transport) {
        return true;
    }
    const bool upgrading = existing != routes_.end() && existing->legacyIdentity && !legacyIdentity &&
                           existing->route.transport == transport;
    const bool sameSession = existing != routes_.end() && existing->route.transport == transport &&
                             (existing->hostId == id || upgrading);
    if (upgrading && existing->hostId != id) {
        // Copy before migration: migration rewrites the route's hostId too.
        const std::string alias = existing->hostId;
        if (!migrateIdentity(alias, id, name)) {
            return false;
        }
    }
    if (!rememberHost(id, name)) {
        return false;
    }
    ReadyRoute ready{{transport, endpoint}, id, legacyIdentity};
    if (existing == routes_.end()) {
        routes_.push_back(std::move(ready));
    } else {
        *existing = std::move(ready);
    }
    if (preferredHostId_.empty() || (transport == Transport::Usb && !sameSession)) {
        preferredHostId_ = id;
    }
    return true;
}

bool HostSelection::disconnect(int32_t endpoint)
{
    const auto route = std::find_if(routes_.begin(), routes_.end(), [endpoint](const ReadyRoute& item) {
        return item.route.endpoint == endpoint;
    });
    if (route == routes_.end()) {
        return false;
    }
    routes_.erase(route);
    return true;
}

std::optional<HostRoute> HostSelection::routeForHost(const std::string& id) const
{
    std::optional<HostRoute> selected;
    for (const auto& route : routes_) {
        if (route.hostId == id && (!selected || route.route.transport == Transport::Usb)) {
            // Keep the first ready route within a transport, avoiding a route
            // switch when a duplicate same-host BLE connection becomes ready.
            selected = route.route;
            if (route.route.transport == Transport::Usb) {
                break;
            }
        }
    }
    return selected;
}

std::optional<HostRoute> HostSelection::selectedRoute() const
{
    return routeForHost(preferredHostId_);
}

std::string HostSelection::selectedName() const
{
    const auto host = std::find_if(hosts_.begin(), hosts_.end(), [this](const RememberedHost& item) {
        return item.id == preferredHostId_;
    });
    return host == hosts_.end() ? std::string{} : host->name;
}

std::vector<HostInfo> HostSelection::hosts() const
{
    std::vector<HostInfo> result;
    result.reserve(hosts_.size());
    for (const auto& host : hosts_) {
        HostInfo info{host.id, host.name, false, host.id == preferredHostId_, false, false};
        for (const auto& route : routes_) {
            if (route.hostId == host.id) {
                info.online = true;
                info.usbAvailable = info.usbAvailable || route.route.transport == Transport::Usb;
                info.bleAvailable = info.bleAvailable || route.route.transport == Transport::Ble;
            }
        }
        result.push_back(std::move(info));
    }
    return result;
}

}  // namespace codex_micro
