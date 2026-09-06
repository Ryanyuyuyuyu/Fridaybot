// SPDX-License-Identifier: MIT
#include "power_policy.h"
#include <math.h>

static double Backoff(CHPowerPolicy *policy)
{
    static const double delays[] = {15, 30, 60, 120};
    unsigned index = policy->scanFailures < 3 ? policy->scanFailures : 3;
    if (policy->scanFailures < 3) ++policy->scanFailures;
    return delays[index];
}

void CHPowerInit(CHPowerPolicy *policy, double now)
{
    *policy = (CHPowerPolicy){.nextScanAt = now, .nextRecoveryAt = now + CHRecoverySeconds,
                             .nextQuotaAt = now, .lastQuotaStartedAt = now - CHQuotaMinimumIntervalSeconds};
}

void CHPowerWake(CHPowerPolicy *policy, double now)
{
    CHPowerDiscoveryLost(policy, now);
    CHPowerRequestIdentity(policy, now);
    CHPowerNewQuotaPath(policy, now);
}

void CHPowerDiscoveryLost(CHPowerPolicy *policy, double now)
{
    policy->nextScanAt = now;
    policy->scanFailures = 0;
}

void CHPowerDiscoveryFailed(CHPowerPolicy *policy, double now)
{
    policy->scanning = false; // Caller stops any scan before recording a failed attempt.
    policy->nextScanAt = now + Backoff(policy);
}

void CHPowerRequestIdentity(CHPowerPolicy *policy, double now) { policy->nextRecoveryAt = now; }
void CHPowerNewQuotaPath(CHPowerPolicy *policy, double now)
{
    // A read already in progress will deliver to the currently ready path.
    // Do not cold-start a second child immediately after that fresh result.
    if (!policy->quotaInFlight)
        policy->nextQuotaAt = fmin(policy->nextQuotaAt, fmax(now, policy->lastQuotaStartedAt + CHQuotaMinimumIntervalSeconds));
}
void CHPowerQuotaFinished(CHPowerPolicy *policy) { policy->quotaInFlight = false; }

CHPowerActions CHPowerEvaluate(CHPowerPolicy *policy, double now, CHPowerInputs inputs)
{
    CHPowerActions actions = {0};
    if (now >= policy->nextRecoveryAt) {
        actions.recoverIdentity = true;
        policy->nextRecoveryAt = now + CHRecoverySeconds;
    }
    bool quiet = !inputs.bluetoothOn || inputs.usbReady || inputs.bleBusy;
    if (quiet) {
        actions.stopScan = policy->scanning;
        policy->scanning = false;
    } else if (policy->scanning && now >= policy->scanUntil) {
        actions.stopScan = true;
        CHPowerDiscoveryFailed(policy, now);
    } else if (!policy->scanning && now >= policy->nextScanAt) {
        actions.startScan = true;
        policy->scanning = true;
        policy->scanUntil = now + CHScanWindowSeconds;
    }
    if (inputs.quotaReady && !policy->quotaInFlight && now >= policy->nextQuotaAt) {
        actions.readQuota = true;
        policy->quotaInFlight = true;
        policy->lastQuotaStartedAt = now;
        policy->nextQuotaAt = now + CHQuotaIntervalSeconds;
    }
    return actions;
}

double CHPowerNextDeadline(const CHPowerPolicy *policy, CHPowerInputs inputs)
{
    double next = policy->nextRecoveryAt;
    if (inputs.bluetoothOn && !inputs.usbReady && !inputs.bleBusy)
        next = fmin(next, policy->scanning ? policy->scanUntil : policy->nextScanAt);
    if (inputs.quotaReady && !policy->quotaInFlight) next = fmin(next, policy->nextQuotaAt);
    return next;
}
