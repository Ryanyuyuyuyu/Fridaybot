// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>

enum { CHScanWindowSeconds = 8, CHRecoverySeconds = 60, CHQuotaIntervalSeconds = 180,
       CHQuotaMinimumIntervalSeconds = 30, CHUSBRetrySeconds = 5, CHHandshakeSeconds = 30 };

typedef struct {
    bool bluetoothOn;
    bool usbReady;
    bool bleBusy; // An existing connection or bounded connection attempt.
    bool quotaReady;
} CHPowerInputs;

typedef struct {
    bool startScan;
    bool stopScan;
    bool recoverIdentity;
    bool readQuota;
} CHPowerActions;

typedef struct {
    double nextScanAt;
    double scanUntil;
    double nextRecoveryAt;
    double nextQuotaAt;
    double lastQuotaStartedAt;
    unsigned scanFailures;
    bool scanning;
    bool quotaInFlight;
} CHPowerPolicy;

void CHPowerInit(CHPowerPolicy *policy, double now);
void CHPowerWake(CHPowerPolicy *policy, double now);
void CHPowerDiscoveryLost(CHPowerPolicy *policy, double now);
void CHPowerDiscoveryFailed(CHPowerPolicy *policy, double now);
void CHPowerRequestIdentity(CHPowerPolicy *policy, double now);
void CHPowerNewQuotaPath(CHPowerPolicy *policy, double now);
void CHPowerQuotaFinished(CHPowerPolicy *policy);
CHPowerActions CHPowerEvaluate(CHPowerPolicy *policy, double now, CHPowerInputs inputs);
double CHPowerNextDeadline(const CHPowerPolicy *policy, CHPowerInputs inputs);
