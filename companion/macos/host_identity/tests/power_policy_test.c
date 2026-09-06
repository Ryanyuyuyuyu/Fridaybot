// SPDX-License-Identifier: MIT
#include "../power_policy.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    CHPowerPolicy p;
    CHPowerInputs none = {0}, ble = {.bluetoothOn = true};
    CHPowerInit(&p, 0);
    assert(!CHPowerEvaluate(&p, 0, none).readQuota);
    assert(!p.scanning && CHPowerNextDeadline(&p, none) == 60);
    assert(CHPowerEvaluate(&p, 0, ble).startScan);
    assert(!CHPowerEvaluate(&p, 7, ble).stopScan);
    assert(CHPowerEvaluate(&p, 8, ble).stopScan && p.nextScanAt == 23);
    assert(!CHPowerEvaluate(&p, 22, ble).startScan);
    assert(CHPowerEvaluate(&p, 23, ble).startScan);
    assert(CHPowerEvaluate(&p, 31, ble).stopScan && p.nextScanAt == 61);
    assert(CHPowerEvaluate(&p, 61, ble).startScan);
    assert(CHPowerEvaluate(&p, 69, ble).stopScan && p.nextScanAt == 129);
    assert(CHPowerEvaluate(&p, 129, ble).startScan);
    assert(CHPowerEvaluate(&p, 137, ble).stopScan && p.nextScanAt == 257);
    for (unsigned i = 0; i < 4; ++i) {
        double start = p.nextScanAt;
        assert(CHPowerEvaluate(&p, start, ble).startScan);
        assert(CHPowerEvaluate(&p, start + 8, ble).stopScan && p.nextScanAt == start + 128);
    }

    CHPowerInit(&p, 0);
    assert(CHPowerEvaluate(&p, 0, ble).startScan);
    CHPowerInputs usb = {.bluetoothOn = true, .usbReady = true, .quotaReady = true};
    CHPowerActions a = CHPowerEvaluate(&p, 1, usb);
    assert(a.stopScan && a.readQuota && !p.scanning);
    CHPowerNewQuotaPath(&p, 2); // A BLE fallback becomes ready during the same fresh read.
    assert(p.nextQuotaAt == 181);
    assert(!CHPowerEvaluate(&p, 2, usb).readQuota); // Only one app-server at a time.
    CHPowerQuotaFinished(&p);
    for (int reconnect = 3; reconnect < 31; ++reconnect) {
        CHPowerNewQuotaPath(&p, reconnect);
        assert(!CHPowerEvaluate(&p, reconnect, usb).readQuota); // Flapping cannot cold-start a child per event.
    }
    assert(CHPowerEvaluate(&p, 31, usb).readQuota);
    CHPowerQuotaFinished(&p);
    assert(!CHPowerEvaluate(&p, 60, usb).readQuota && !p.scanning);
    assert(!CHPowerEvaluate(&p, 210, usb).readQuota);
    assert(CHPowerEvaluate(&p, 211, usb).readQuota);
    CHPowerQuotaFinished(&p);
    assert(!CHPowerEvaluate(&p, 500, none).readQuota); // No device, no child even overdue.
    CHPowerNewQuotaPath(&p, 501);
    assert(CHPowerEvaluate(&p, 501, usb).readQuota); // Prompt on reconnect.
    CHPowerQuotaFinished(&p);

    CHPowerInputs connectedBLE = {.bluetoothOn = true, .bleBusy = true, .quotaReady = true};
    assert(!CHPowerEvaluate(&p, 502, connectedBLE).startScan); // Keep an existing BLE fallback.
    CHPowerDiscoveryLost(&p, 503);
    assert(CHPowerEvaluate(&p, 503, ble).startScan); // USB/BLE gone: immediate new window.
    CHPowerInputs connecting = {.bluetoothOn = true, .bleBusy = true};
    assert(CHPowerEvaluate(&p, 504, connecting).stopScan);
    CHPowerDiscoveryFailed(&p, 505);
    assert(!CHPowerEvaluate(&p, 519, ble).startScan);
    assert(CHPowerEvaluate(&p, 520, ble).startScan);

    CHPowerInit(&p, 0);
    assert(CHPowerEvaluate(&p, 0, ble).startScan);
    assert(CHPowerEvaluate(&p, 1, none).stopScan); // Bluetooth off stops even an active discovery window.
    CHPowerDiscoveryLost(&p, 50);
    assert(CHPowerEvaluate(&p, 50, ble).startScan);

    CHPowerInit(&p, 0);
    assert(!CHPowerEvaluate(&p, 59, usb).recoverIdentity);
    CHPowerQuotaFinished(&p);
    assert(CHPowerEvaluate(&p, 60, usb).recoverIdentity); // Recovery even without a new attach callback.
    CHPowerWake(&p, 100);
    a = CHPowerEvaluate(&p, 100, usb);
    assert(a.recoverIdentity && a.readQuota && !a.startScan); // Resume re-identifies a USB-active Mac.
    CHPowerQuotaFinished(&p);
    assert(!CHPowerEvaluate(&p, 101, usb).readQuota);
    CHPowerRequestIdentity(&p, 105); // Name change / native BLE HID appearance under USB.
    assert(CHPowerEvaluate(&p, 105, usb).recoverIdentity && !p.scanning);
    CHPowerDiscoveryLost(&p, 106);
    assert(!CHPowerEvaluate(&p, 106, connectedBLE).startScan); // USB unplug does not tear down BLE.
    CHPowerWake(&p, 3600);
    a = CHPowerEvaluate(&p, 3600, none);
    assert(a.recoverIdentity && !a.readQuota && !a.startScan); // No catch-up child burst after sleep.
    puts("power policy: PASS (scan windows/backoff, USB/BLE reconnect, wake, identity, quota cadence)");
    return 0;
}
