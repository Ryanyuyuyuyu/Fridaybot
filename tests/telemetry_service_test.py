#!/usr/bin/env python3
"""Exercise production telemetry functions with typed JSON and clock fixtures.

This checks field writes, projection, identity/RPC arrival order and expiry;
it does not claim to test ArduinoJson parsing or a physical transport.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
source = (root / 'main/services/codex_micro/codex_micro_service.cpp').read_text()


def method(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, cursor = 1, opening + 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[start:cursor].replace('Service::Impl::', 'Fixture::')


harness = r'''
#include "main/services/codex_micro/codex_micro_service.h"
#include "main/services/codex_micro/telemetry_projection.h"
#include "main/services/codex_micro/usb_resume_policy.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>
#define ESP_LOGI(...) ((void)0)
namespace codex_micro {
uint32_t fakeNow = 0;
uint32_t nowMs() { return fakeNow; }
unsigned takes = 0, gives = 0;
bool locked = false;
constexpr int portMAX_DELAY = 0;
void xSemaphoreTake(int, int) { assert(!locked); locked = true; ++takes; }
void xSemaphoreGive(int) { assert(locked); locked = false; ++gives; }
struct JsonValue {
    std::variant<std::monostate, int, uint32_t, float, std::string, bool> value;
    JsonValue() = default;
    JsonValue(int v): value(v) {}
    JsonValue(uint32_t v): value(v) {}
    JsonValue(float v): value(v) {}
    JsonValue(const char* v): value(std::string(v)) {}
    JsonValue(std::string v): value(std::move(v)) {}
    JsonValue(bool v): value(v) {}
    template<typename T> bool is() const {
        if constexpr (std::is_same_v<T, const char*>) return std::holds_alternative<std::string>(value);
        else if constexpr (std::is_same_v<T, float>) return std::holds_alternative<float>(value) ||
            std::holds_alternative<int>(value) || std::holds_alternative<uint32_t>(value);
        else if constexpr (std::is_same_v<T, uint32_t>) return std::holds_alternative<uint32_t>(value) ||
            (std::holds_alternative<int>(value) && std::get<int>(value) >= 0);
        else return std::holds_alternative<T>(value);
    }
    template<typename T> T as() const {
        if constexpr (std::is_same_v<T, const char*>) return std::get<std::string>(value).c_str();
        else if constexpr (std::is_same_v<T, float>) {
            if (std::holds_alternative<float>(value)) return std::get<float>(value);
            if (std::holds_alternative<int>(value)) return static_cast<float>(std::get<int>(value));
            return static_cast<float>(std::get<uint32_t>(value));
        } else if constexpr (std::is_same_v<T, uint32_t>) {
            if (std::holds_alternative<uint32_t>(value)) return std::get<uint32_t>(value);
            return static_cast<uint32_t>(std::get<int>(value));
        } else return std::get<T>(value);
    }
    int operator|(int fallback) const { return std::holds_alternative<int>(value) ? std::get<int>(value) : fallback; }
};
struct JsonObjectConst {
    std::map<std::string, JsonValue> fields;
    JsonObjectConst(std::initializer_list<std::pair<const std::string, JsonValue>> v): fields(v) {}
    const JsonValue& operator[](const char* key) const {
        static const JsonValue missing;
        auto found = fields.find(key); return found == fields.end() ? missing : found->second;
    }
};
using JsonArrayConst = std::vector<JsonObjectConst>;
constexpr size_t kMaxConnections = 3;
constexpr size_t kMaxEffectLength = 31;
constexpr uint32_t kRpcAssemblyTimeoutMs = 1000;
struct Connection {
    uint16_t id = 0;
    std::string hostId, hostName;
    bool usb = false, active = false, secure = false, rpcReady = false, inputNotifications = false;
    bool legacyIdentity = false;
    bool fridayPeer = false, batteryNotifications = false, fridayTransferNotifications = false;
    uint32_t lastRpcAtMs = 0;
    std::array<Thread, 6> threads{};
    ThreadFieldMetadata threadFields{};
    Quota quota;
    RateLimitUsage rateLimits;
    std::string rpcBuffer;
    uint32_t rpcLastFragmentAtMs = 0;
};
struct WriteEvent { uint16_t connId = 0, handle = 100, length = 2; uint8_t data[2] = {}; };
namespace detail { constexpr size_t kHidInputCccd = 0, kBatteryCccd = 0, kFridayTransferCccd = 0; }
struct Fixture {
    uint16_t hidHandles[1] = {100}, batteryHandles[1] = {101}, fridayHandles[1] = {102};
    bool fridayTravelActive = false;
    void markFridayPeer(Connection& peer) { peer.fridayPeer = true; }
    void updateFridayConnectionIntervals(bool) {}
    void syncFridayLinkState() {}
    HostSelection hostSelection;
    Connection usbConnection;
    std::array<Connection, kMaxConnections> connections{};
    std::atomic<uint32_t> controlEpoch{1};
    int32_t routedConnection = -1;
    std::string routedHostId;
    State state;
    uint32_t telemetryExpiresAtMs = 0;
    int stateMutex = 0;
    bool hostRpcConnectionValid = false;
    uint16_t hostRpcConnectionId = 0;
    unsigned refreshes = 0;
    usb::ResumePreference usbResumePreference;
    Connection* findConnection(uint16_t id) {
        if (usbConnection.active && usbConnection.id == id) return &usbConnection;
        for (auto& peer : connections) if (peer.active && peer.id == id) return &peer;
        return nullptr;
    }
    void refreshRouting() {
        ++refreshes;
        auto route = hostSelection.selectedRoute();
        const int32_t endpoint = route ? route->endpoint : -1;
        if (endpoint != routedConnection || hostSelection.selectedHostId() != routedHostId) ++controlEpoch;
        routedConnection = endpoint; routedHostId = hostSelection.selectedHostId();
        publishTelemetry();
    }
    void clearRpcAssembly(Connection& connection) { connection.rpcBuffer.clear(); }
    void registerHost(Connection&);
    void noteHostRpc(uint16_t);
    void updateThreads(JsonArrayConst, uint16_t);
    void publishTelemetry();
    void synchronizeThreadTelemetry(const std::string&, int32_t preferredEndpoint = -1);
    bool processRpcTimeouts();
    void handleCccdWrite(const WriteEvent&);
    // Typed identity fixture uses the same production registration calls as
    // acceptHostIdentity after its UUID/security validation.
    void identity(Connection& connection, const char* id, const char* name) {
        connection.hostId = id; connection.hostName = name;
        hostSelection.rememberHost(id, name);
        if (connection.usb) usbResumePreference.identityConfirmed(id);
        registerHost(connection); synchronizeThreadTelemetry(id); refreshRouting();
    }
};
'''
for signature in [
    'void Service::Impl::registerHost(Connection& connection)',
    'void Service::Impl::noteHostRpc(uint16_t connectionId)',
    'void Service::Impl::updateThreads(JsonArrayConst values, uint16_t connectionId)',
    'void Service::Impl::publishTelemetry()',
    'void Service::Impl::synchronizeThreadTelemetry(',
    'bool Service::Impl::processRpcTimeouts()',
    'void Service::Impl::handleCccdWrite(const WriteEvent& write)',
]:
    harness += method(signature) + '\n'
harness += r'''
} // namespace codex_micro
using namespace codex_micro;
constexpr const char* privateMac = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
constexpr const char* workMac = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
Connection native(uint16_t id, bool usb) {
    Connection connection; connection.id = id; connection.usb = usb;
    connection.active = connection.secure = connection.inputNotifications = true;
    return connection;
}
void receipt(const Connection& source, size_t agent, ThreadField field, uint32_t at) {
    const auto& metadata = source.threadFields[agent][static_cast<size_t>(field)];
    assert(metadata.available && metadata.atMs == at);
}
int main() {
    for (bool identityFirst : {false, true}) {
        fakeNow = 100;
        Fixture test;
        test.usbConnection = native(0xFFFE, true);
        test.usbResumePreference.mounted(false, {});
        if (identityFirst) test.identity(test.usbConnection, privateMac, "Mac P");
        // Early sparse native data is stored with receipts, but cannot claim
        // a route before BOTH stable identity and native RPC readiness exist.
        test.updateThreads({{{"id", 0}, {"c", uint32_t{0xFFAABBCC}}, {"b", 0.4f}, {"e", "breath"}, {"s", 2.5f}}}, 0xFFFE);
        assert(!test.state.connected && test.state.threads[0].color == 0);
        for (size_t field = 0; field < 4; ++field) receipt(test.usbConnection, 0, static_cast<ThreadField>(field), 100);
        test.noteHostRpc(0xFFFE);
        if (!identityFirst) {
            assert(!test.state.connected);
            test.identity(test.usbConnection, privateMac, "Mac P");
        }
        assert(test.state.connected && test.state.transport == Transport::Usb && test.state.activeHostId == privateMac);
        assert(test.state.threads[0].color == 0xAABBCC && test.state.threads[0].brightness == 0.4f);
        assert(test.state.threads[0].effect == "breath" && test.state.threads[0].speed == 2.5f);
        // Invalid/missing fields do not overwrite values or their receipts.
        const auto revision = test.state.revision;
        fakeNow = 200;
        test.updateThreads({{{"id", -1}, {"c", uint32_t{12}}}, {{"id", 6}, {"c", uint32_t{12}}},
                            {{"id", 0}, {"c", -1}, {"b", std::numeric_limits<float>::quiet_NaN()},
                             {"e", std::string(32, 'x')}, {"s", std::numeric_limits<float>::infinity()}}}, 0xFFFE);
        assert(test.state.revision == revision);
        for (size_t field = 0; field < 4; ++field) receipt(test.usbConnection, 0, static_cast<ThreadField>(field), 100);
        test.updateThreads({{{"id", 0}, {"b", 4.0f}, {"s", -4.0f}}}, 0xFFFE);
        assert(test.state.threads[0].color == 0xAABBCC && test.state.threads[0].brightness == 1.0f);
        assert(test.state.threads[0].effect == "breath" && test.state.threads[0].speed == 0.0f);
        receipt(test.usbConnection, 0, ThreadField::Color, 100);
        receipt(test.usbConnection, 0, ThreadField::Brightness, 200);
        receipt(test.usbConnection, 0, ThreadField::Effect, 100);
        receipt(test.usbConnection, 0, ThreadField::Speed, 200);

        // Heartbeats only refresh liveness; they do not re-register or redraw.
        const auto refreshes = test.refreshes, heartbeatRevision = test.state.revision;
        fakeNow = 250; test.noteHostRpc(0xFFFE);
        assert(test.refreshes == refreshes && test.state.revision == heartbeatRevision && test.state.lastHostRpcAtMs == 250);
        // Full BLE native values and sparse USB values merge by field only for
        // this Mac; the newer Work Mac update stays isolated.
        test.connections[0] = native(1, false);
        test.identity(test.connections[0], privateMac, "Mac P"); test.noteHostRpc(1);
        fakeNow = 300;
        test.updateThreads({{{"id", 1}, {"c", uint32_t{0x123456}}, {"b", 0.8f}, {"e", "solid"}, {"s", 4.0f}}}, 1);
        fakeNow = 350;
        test.updateThreads({{{"id", 1}, {"b", 0.2f}}}, 0xFFFE);
        test.connections[1] = native(2, false);
        test.identity(test.connections[1], workMac, "Mac W"); test.noteHostRpc(2);
        fakeNow = 400;
        test.updateThreads({{{"id", 1}, {"c", uint32_t{0xFFFFFF}}, {"b", 0.9f}, {"e", "other"}, {"s", 90.0f}}}, 2);
        assert(test.state.activeHostId == privateMac && test.state.threads[1].color == 0x123456);
        assert(test.state.threads[1].brightness == 0.2f && test.state.threads[1].effect == "solid" && test.state.threads[1].speed == 4.0f);
        assert(test.hostSelection.selectHost(workMac)); test.refreshRouting();
        assert(test.state.activeHostId == workMac && test.state.transport == Transport::Ble);
        assert(test.state.threads[1].color == 0xFFFFFF && test.state.threads[1].effect == "other");
        // A remembered offline selection publishes cleared telemetry and route
        // identity together. Its previous live values cannot bleed into USB.
        test.hostSelection.disconnect(2); test.connections[1].active = false; test.refreshRouting();
        assert(!test.state.connected && test.state.activeHostId == workMac && test.state.threads[1].color == 0);
        assert(!test.state.quota.available && !test.hostRpcConnectionValid);
        assert(test.state.connectionEpoch == test.controlEpoch && takes == gives && !locked);
    }
    // Keep the latest DISPLAYED fields through same-host USB -> BLE failover.
    // Copies retain the original native receipt, not the detach/identity time.
    Fixture fallback; fakeNow = 100;
    fallback.connections[0] = native(1, false);
    fallback.identity(fallback.connections[0], privateMac, "Mac P"); fallback.noteHostRpc(1);
    fallback.updateThreads({{{"id", 2}, {"c", uint32_t{0x112233}}, {"b", 0.3f}, {"e", "breath"}, {"s", 7.0f}}}, 1);
    fallback.usbConnection = native(0xFFFE, true); fallback.usbResumePreference.mounted(false, privateMac);
    fallback.identity(fallback.usbConnection, privateMac, "Mac P"); fallback.noteHostRpc(0xFFFE);
    fakeNow = 200;
    fallback.updateThreads({{{"id", 2}, {"c", uint32_t{0x445566}}, {"b", 0.8f}}}, 0xFFFE);
    receipt(fallback.connections[0], 2, ThreadField::Color, 200);
    receipt(fallback.connections[0], 2, ThreadField::Brightness, 200);
    receipt(fallback.connections[0], 2, ThreadField::Effect, 100);
    receipt(fallback.connections[0], 2, ThreadField::Speed, 100);
    const auto epochBeforeDetach = fallback.state.connectionEpoch;
    fakeNow = 300;
    fallback.usbConnection = Connection{}; fallback.hostSelection.disconnect(0xFFFE); fallback.refreshRouting();
    assert(fallback.state.connected && fallback.state.transport == Transport::Ble && fallback.state.connectionEpoch != epochBeforeDetach);
    assert(fallback.state.threads[2].color == 0x445566 && fallback.state.threads[2].brightness == 0.8f);
    assert(fallback.state.threads[2].effect == "breath" && fallback.state.threads[2].speed == 7.0f);
    receipt(fallback.connections[0], 2, ThreadField::Color, 200);
    // A later actual BLE update replaces only its supplied field.
    fakeNow = 400;
    fallback.updateThreads({{{"id", 2}, {"b", 0.1f}}}, 1);
    assert(fallback.state.threads[2].color == 0x445566 && fallback.state.threads[2].brightness == 0.1f);
    receipt(fallback.connections[0], 2, ThreadField::Color, 200);
    receipt(fallback.connections[0], 2, ThreadField::Brightness, 400);
    // All sessions leave: clear the public state and never seed a fresh native
    // connection with an expired cache from a previous physical session.
    fallback.connections[0] = Connection{}; fallback.hostSelection.disconnect(1); fallback.refreshRouting();
    assert(!fallback.state.connected && fallback.state.threads[2].color == 0 && fallback.state.threads[2].effect == "off");
    fallback.connections[0] = native(4, false); fallback.identity(fallback.connections[0], privateMac, "Mac P");
    fallback.noteHostRpc(4);
    assert(fallback.state.connected && fallback.state.threads[2].color == 0 && fallback.state.threads[2].brightness == 0.0f);

    // Delayed identity combines independent fields without promoting cached
    // values to the arrival time. A newer other-Mac source must never be copied.
    Fixture delayed; fakeNow = 100;
    delayed.connections[0] = native(1, false);
    delayed.identity(delayed.connections[0], privateMac, "Mac P"); delayed.noteHostRpc(1);
    delayed.updateThreads({{{"id", 0}, {"c", uint32_t{0x111111}}, {"b", 0.1f}}}, 1);
    delayed.usbConnection = native(0xFFFE, true); delayed.usbResumePreference.mounted(false, privateMac);
    fakeNow = 200;
    delayed.updateThreads({{{"id", 0}, {"c", uint32_t{0x222222}}, {"b", 0.2f}, {"e", "usb"}, {"s", 2.0f}}}, 0xFFFE);
    delayed.noteHostRpc(0xFFFE); // Unknown USB remains excluded.
    assert(delayed.state.transport == Transport::Ble && delayed.connections[0].threads[0].color == 0x111111);
    fakeNow = 300;
    delayed.updateThreads({{{"id", 0}, {"c", uint32_t{0x333333}}}}, 1);
    delayed.connections[1] = native(2, false);
    delayed.identity(delayed.connections[1], workMac, "Mac W"); delayed.noteHostRpc(2);
    fakeNow = 400;
    delayed.updateThreads({{{"id", 0}, {"c", uint32_t{0xFFFFFF}}, {"b", 0.9f}, {"e", "work"}, {"s", 9.0f}}}, 2);
    fakeNow = 500;
    delayed.identity(delayed.usbConnection, privateMac, "Mac P");
    assert(delayed.state.transport == Transport::Usb && delayed.state.activeHostId == privateMac);
    assert(delayed.state.threads[0].color == 0x333333 && delayed.state.threads[0].brightness == 0.2f);
    assert(delayed.state.threads[0].effect == "usb" && delayed.state.threads[0].speed == 2.0f);
    for (const auto* peer : {&delayed.usbConnection, &delayed.connections[0]}) {
        receipt(*peer, 0, ThreadField::Color, 300);
        receipt(*peer, 0, ThreadField::Brightness, 200);
        receipt(*peer, 0, ThreadField::Effect, 200);
        receipt(*peer, 0, ThreadField::Speed, 200);
    }
    assert(delayed.connections[1].threads[0].color == 0xFFFFFF && delayed.connections[1].threads[0].brightness == 0.9f);
    receipt(delayed.connections[1], 0, ThreadField::Color, 400);
    // On same-tick native arrivals the last RECEIVED update wins, even when
    // the other endpoint is the control route. Identity/cache copies never win
    // merely because their destination is the current route.
    fakeNow = 600;
    delayed.updateThreads({{{"id", 0}, {"c", uint32_t{0x616161}}}}, 0xFFFE);
    delayed.updateThreads({{{"id", 0}, {"c", uint32_t{0x626262}}}}, 1);
    assert(delayed.state.threads[0].color == 0x626262 && delayed.usbConnection.threads[0].color == 0x626262);
    delayed.updateThreads({{{"id", 0}, {"c", uint32_t{0x636363}}}}, 0xFFFE);
    assert(delayed.state.threads[0].color == 0x636363 && delayed.connections[0].threads[0].color == 0x636363);
    receipt(delayed.usbConnection, 0, ThreadField::Color, 600);
    receipt(delayed.connections[0], 0, ThreadField::Color, 600);
    assert(delayed.connections[1].threads[0].color == 0xFFFFFF); // Work Mac untouched.

    // An authenticated identity/quota helper can stay connected after every
    // native HID session leaves. It must neither retain nor resurrect Agents.
    Fixture helperOnly; fakeNow = 100;
    helperOnly.connections[0] = native(1, false);
    helperOnly.identity(helperOnly.connections[0], privateMac, "Mac P"); helperOnly.noteHostRpc(1);
    helperOnly.connections[1] = native(2, false);
    helperOnly.connections[1].inputNotifications = false; // Identity/quota-only client.
    helperOnly.identity(helperOnly.connections[1], privateMac, "Mac P");
    helperOnly.connections[1].quota = {80, 60, 100, true};
    helperOnly.updateThreads({{{"id", 3}, {"c", uint32_t{0xABCDE0}}, {"e", "native"}}}, 1);
    assert(helperOnly.state.threads[3].color == 0xABCDE0 && helperOnly.state.quota.available);
    for (const auto& agent : helperOnly.connections[1].threadFields)
        for (const auto& field : agent) assert(!field.available);
    assert(helperOnly.connections[1].threads[3].color == 0);
    helperOnly.usbConnection = native(0xFFFE, true); helperOnly.usbResumePreference.mounted(false, privateMac);
    helperOnly.identity(helperOnly.usbConnection, privateMac, "Mac P"); helperOnly.noteHostRpc(0xFFFE);
    fakeNow = 200;
    helperOnly.updateThreads({{{"id", 3}, {"c", uint32_t{0xABCDE1}}}}, 0xFFFE);
    helperOnly.usbConnection = {}; helperOnly.hostSelection.disconnect(0xFFFE); helperOnly.refreshRouting();
    assert(helperOnly.state.transport == Transport::Ble && helperOnly.state.threads[3].color == 0xABCDE1);
    // Execute the real CCCD handler. Turning HID notifications off invalidates
    // native readiness and receipts even though the BLE transport remains up.
    const auto beforeUnsubscribe = helperOnly.state.connectionEpoch;
    helperOnly.handleCccdWrite(WriteEvent{1, 100, 2, {0, 0}});
    assert(!helperOnly.state.connected && helperOnly.state.connectionEpoch != beforeUnsubscribe);
    assert(helperOnly.connections[0].active && !helperOnly.connections[0].rpcReady);
    assert(helperOnly.connections[0].lastRpcAtMs == 0 && helperOnly.connections[0].threads[3].color == 0);
    for (const auto& peer : helperOnly.connections)
        for (const auto& agent : peer.threadFields)
            for (const auto& field : agent) assert(!field.available);
    assert(helperOnly.connections[1].active && helperOnly.connections[1].quota.available);
    assert(helperOnly.state.threads[3].color == 0 && helperOnly.state.threads[3].effect == "off");
    helperOnly.handleCccdWrite(WriteEvent{1, 100, 2, {1, 0}});
    assert(!helperOnly.state.connected && !helperOnly.connections[0].rpcReady);
    helperOnly.noteHostRpc(1); // Subscription alone was insufficient; new RPC permits readiness.
    assert(helperOnly.state.connected && helperOnly.state.threads[3].color == 0);
    assert(helperOnly.state.quota.available); // Helper quota remains independently eligible.
    fakeNow = 300;
    helperOnly.updateThreads({{{"id", 3}, {"c", uint32_t{0xABCDE2}}}}, 1);
    assert(helperOnly.state.threads[3].color == 0xABCDE2);
    // Real disconnect/reconnect also cannot borrow old Agent data from helper.
    helperOnly.connections[0] = {}; helperOnly.hostSelection.disconnect(1); helperOnly.refreshRouting();
    assert(!helperOnly.state.connected && helperOnly.state.threads[3].color == 0);
    helperOnly.connections[0] = native(4, false); helperOnly.identity(helperOnly.connections[0], privateMac, "Mac P");
    assert(!helperOnly.state.connected);
    helperOnly.noteHostRpc(4);
    assert(helperOnly.state.connected && helperOnly.state.threads[3].color == 0);
    // Defensive source filter also ignores an old non-native receipt. The
    // source is excluded from both synchronization and public projection.
    helperOnly.connections[1].threads[3].color = 0xEEEEEE;
    helperOnly.connections[1].threadFields[3][0] = {true, 400};
    fakeNow = 400;
    helperOnly.synchronizeThreadTelemetry(privateMac); helperOnly.publishTelemetry();
    assert(helperOnly.connections[0].threads[3].color == 0 && helperOnly.state.threads[3].color == 0);

    // Independent quota source expiries are driven by production timeout code.
    Fixture expiry; fakeNow = 1000;
    expiry.usbConnection = native(0xFFFE, true); expiry.usbResumePreference.mounted(false, {});
    expiry.identity(expiry.usbConnection, privateMac, "Mac P"); expiry.noteHostRpc(0xFFFE);
    expiry.connections[0] = native(1, false); expiry.identity(expiry.connections[0], privateMac, "Mac P");
    expiry.usbConnection.quota = {50, 100, 1000, true};
    expiry.usbConnection.rateLimits = {20, 0, 1000, true, false};
    expiry.connections[0].rateLimits = {0, 30, 2000, false, true};
    fakeNow = 3000;
    const unsigned lockBefore = takes; expiry.publishTelemetry();
    assert(takes == lockBefore + 1 && takes == gives);
    assert(expiry.state.rateLimits.fiveHourUsedPercent == 20 && expiry.state.rateLimits.weeklyUsedPercent == 30);
    assert(expiry.telemetryExpiresAtMs == 601000);
    fakeNow = 601000; assert(expiry.processRpcTimeouts());
    assert(!expiry.state.quota.available && !expiry.state.rateLimits.fiveHourAvailable && expiry.state.rateLimits.weeklyAvailable);
    assert(expiry.telemetryExpiresAtMs == 602000);
    fakeNow = 602000; assert(expiry.processRpcTimeouts());
    assert(!expiry.state.rateLimits.weeklyAvailable && expiry.telemetryExpiresAtMs == 0);
    assert(!expiry.processRpcTimeouts());
}
'''
with tempfile.TemporaryDirectory(prefix='friday-telemetry-service.') as directory:
    cpp = Path(directory) / 'actual_telemetry_test.cpp'
    binary = Path(directory) / 'actual_telemetry_test'
    cpp.write_text(harness)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                    '-I', str(root), str(cpp), str(root / 'main/services/codex_micro/host_selection.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('telemetry_service_test: PASS')
