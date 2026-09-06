#!/usr/bin/env python3
"""Compile the actual service queue/command functions against deterministic I/O.

No SDK, USB or BLE device is opened. This exercises the production peek/commit
integration, not a second implementation of the dispatcher.
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
    depth = 1
    cursor = opening + 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[start:cursor]


harness = r'''
#include "main/services/codex_micro/deferred_control.h"
#include "main/services/codex_micro/ble_sessions.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#define portENTER_CRITICAL(x) (void)(x)
#define portEXIT_CRITICAL(x) (void)(x)
namespace codex_micro {
namespace usb {
    bool available = true;
    bool attached = true;
    uint32_t epoch = 7;
    bool serviceBudgetAvailable() { return available; }
    bool mounted() { return attached; }
    uint32_t sessionEpoch() { return epoch; }
}
enum class CommandType { kSelectHost, kBattery, kKey, kJoystick, kFridayTransfer, kFridayTravelMode };
struct Command {
    CommandType type = CommandType::kKey;
    uint32_t sequence = 0, controlEpoch = 3;
    uint8_t action = 0, percentage = 0;
    int8_t agent = 0;
    char key[64] = "AG00", hostId[97] = {};
    float angle = 0, distance = 0;
    bool charging = false, fridayTravelActive = false;
    uint8_t fridayPacket[16] = {};
};
using QueueHandle_t = std::deque<Command>*;
constexpr int pdTRUE = 1;
int xQueuePeek(QueueHandle_t queue, Command* command, int) {
    if (queue->empty()) return 0;
    *command = queue->front(); return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue, Command* command, int) {
    if (queue->empty()) return 0;
    *command = queue->front(); queue->pop_front(); return pdTRUE;
}
struct Connection {
    uint32_t linkGeneration = 0;
    uint16_t id = 0xFFFE;
    uint8_t address[6] = {};
    bool active = true, secure = true, inputNotifications = true, rpcReady = true, usb = true;
};
struct HeldKey { bool active = false; int8_t agent = 0; char key[64] = "AG00"; };
struct Service { struct Impl; };
struct Service::Impl {
    struct Selection { bool selectHost(const char*) { return true; } } hostSelection;
    struct Resume { void manuallySelected(const char*) {} } usbResumePreference;
    std::deque<Command> normal, release;
    QueueHandle_t commandQueue = &normal, releaseQueue = &release;
    std::atomic<uint32_t> controlEpoch{3};
    DeferredControlGate deferredControls;
    int32_t routedConnection = 0xFFFE;
    uint32_t usbEpoch = 7;
    int callbackSessionMux = 0;
    BleSessions callbackSessions;
    Connection connection;
    std::array<HeldKey, 8> heldKeys{};
    bool joystickHeld = false, fridayTravelActive = false;
    bool expireInSend = false, actualFailure = false;
    unsigned attempts = 0, pendingReleases = 0, fridayCalls = 0;
    std::vector<uint8_t> actions;
    std::vector<int32_t> destinations;
    Connection* findConnection(uint16_t id) { return id == connection.id ? &connection : nullptr; }
    void refreshRouting() {
        ++controlEpoch; routedConnection = connection.id = 2; connection.usb = false;
        connection.linkGeneration = callbackSessions.connect(2, connection.address);
        heldKeys.fill({}); joystickHeld = false;
    }
    void setBatteryNow(uint8_t, bool) { ++fridayCalls; }
    void sendFridayTransferNow(const uint8_t*, size_t) { ++fridayCalls; }
    void updateFridayConnectionIntervals(bool) { ++fridayCalls; }
    void cancelOlderPendingRelease(const Command&) {}
    void schedulePendingRelease(const Command&) { ++pendingReleases; }
    bool sendKeyNow(const char*, uint8_t action, int8_t, bool* deferred) {
        *deferred = false;
        if (expireInSend) { usb::available = false; expireInSend = false; }
        if (connection.usb && !usb::available) { *deferred = true; return false; }
        ++attempts;
        if (actualFailure) { usb::attached = false; ++usb::epoch; return false; }
        actions.push_back(action); destinations.push_back(routedConnection);
        heldKeys[0].active = action != 0;
        return true;
    }
    bool sendJoystickNow(float, float distance, bool* deferred) {
        const bool delivered = sendKeyNow("", distance > 0 ? 1 : 0, 0, deferred);
        if (delivered) joystickHeld = distance > 0;
        return delivered;
    }
    bool processCommand(const Command&);
    bool processQueuedCommands();
};
'''
harness += method('bool Service::Impl::processCommand(const Command& command)') + '\n'
harness += method('bool Service::Impl::processQueuedCommands()') + '\n'
harness += r'''
} // namespace codex_micro
using namespace codex_micro;
Command input(uint32_t sequence, bool released, CommandType type = CommandType::kKey) {
    Command command;
    command.sequence = sequence; command.action = released ? 0 : 1;
    command.distance = released ? 0 : 1; command.type = type;
    return command;
}
void restoreUsb() { usb::available = usb::attached = true; usb::epoch = 7; }
int main() {
    restoreUsb();
    for (CommandType type : {CommandType::kKey, CommandType::kJoystick}) {
        Service::Impl service;
        service.normal.push_back(input(1, false, type));
        service.release.push_back(input(2, true, type));
        usb::available = false;
        assert(!service.processQueuedCommands());
        assert(service.normal.size() == 1 && service.release.size() == 1);
        assert(service.attempts == 0 && service.deferredControls.waiting());
        usb::available = true;
        assert(service.processQueuedCommands());
        assert(service.normal.empty() && service.release.empty());
        assert((service.actions == std::vector<uint8_t>{1, 0}));
        assert(!service.deferredControls.waiting() && service.pendingReleases == 0);
    }
    // Budget expiring after gate admission must not remove either edge or
    // create held state/pending releases for a write that never happened.
    Service::Impl late;
    late.normal.push_back(input(1, false)); late.release.push_back(input(2, true));
    late.expireInSend = true;
    assert(!late.processQueuedCommands());
    assert(late.normal.size() == 1 && late.release.size() == 1 && late.attempts == 0);
    assert(!late.heldKeys[0].active && late.pendingReleases == 0);
    usb::available = true;
    assert(late.processQueuedCommands() && late.actions.size() == 2);

    // A selected-host command is still ordered with the original input edges;
    // controls queued for the previous control epoch never reach the new route.
    Service::Impl switcher;
    Command choose; choose.sequence = 1; choose.type = CommandType::kSelectHost;
    switcher.normal.push_back(choose); switcher.normal.push_back(input(2, false));
    switcher.release.push_back(input(3, true));
    assert(switcher.processQueuedCommands());
    assert(switcher.attempts == 0 && switcher.routedConnection == 2);

    // USB can reset before its queued lifecycle event is consumed. Validate
    // against the live epoch and reject ALL queued actions on the old session.
    Service::Impl replug;
    replug.normal.push_back(input(1, false)); replug.normal.push_back(input(3, false));
    replug.release.push_back(input(2, true)); replug.release.push_back(input(4, true));
    usb::available = false;
    assert(!replug.processQueuedCommands());
    usb::available = true; ++usb::epoch;
    assert(replug.processQueuedCommands());
    assert(replug.normal.empty() && replug.release.empty() && replug.attempts == 0);
    restoreUsb();

    // An uncertain actual write is consumed once. The disconnected route then
    // drops later edges; it is never reclassified as a deferred retry.
    Service::Impl fail;
    fail.normal.push_back(input(1, false)); fail.release.push_back(input(2, true));
    fail.actualFailure = true;
    assert(fail.processQueuedCommands() && fail.attempts == 1);
    assert(fail.normal.empty() && fail.release.empty() && !fail.deferredControls.waiting());
    restoreUsb();

    // Friday work and BLE controls do not depend on the USB deadline.
    Service::Impl friday;
    Command transfer; transfer.type = CommandType::kFridayTransfer; transfer.sequence = 1;
    friday.normal.push_back(transfer);
    usb::available = false;
    assert(friday.processQueuedCommands() && friday.fridayCalls == 1);
    Service::Impl ble;
    ble.refreshRouting();
    Command press = input(1, false), release = input(2, true);
    press.controlEpoch = release.controlEpoch = 4;
    ble.normal.push_back(press); ble.release.push_back(release);
    assert(ble.processQueuedCommands() && ble.actions.size() == 2);
    assert((ble.destinations == std::vector<int32_t>{2, 2}));
}
'''
with tempfile.TemporaryDirectory(prefix='friday-deferred-service.') as directory:
    cpp = Path(directory) / 'actual_service_test.cpp'
    binary = Path(directory) / 'actual_service_test'
    cpp.write_text(harness)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                    '-I', str(root), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('deferred_control_service_test: PASS')
