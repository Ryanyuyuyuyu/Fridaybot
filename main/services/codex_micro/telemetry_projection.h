// SPDX-License-Identifier: MIT
#pragma once

#include "host_selection.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace codex_micro {

// The helper refreshes quota every three minutes. Ten minutes leaves room for
// sleep/reconnection without showing an old snapshot indefinitely.
inline constexpr uint32_t kQuotaFreshForMs = 10 * 60 * 1000;
inline constexpr size_t kNoTelemetrySource = std::numeric_limits<size_t>::max();

enum class ThreadField : size_t { Color = 0, Brightness = 1, Effect = 2, Speed = 3 };
struct FieldReceipt {
    bool available = false;
    uint32_t atMs = 0;
};
using ThreadFieldMetadata = std::array<std::array<FieldReceipt, 4>, 6>;
using ThreadFieldSources = std::array<std::array<size_t, 4>, 6>;

inline ThreadFieldSources emptyThreadSources()
{
    ThreadFieldSources result;
    for (auto& thread : result) thread.fill(kNoTelemetrySource);
    return result;
}

// Metadata only; actual Agent and quota values remain in worker-owned
// Connection objects. A source may be a native control connection or a
// separate authenticated helper connection belonging to the same stable ID.
struct TelemetrySource {
    int32_t endpoint = -1;
    std::string_view hostId;
    Transport transport = Transport::Ble;
    bool active = false;
    bool secure = false;
    bool rpcReady = false;
    const ThreadFieldMetadata* threadFields = nullptr;
    bool quotaAvailable = false;
    uint32_t quotaAtMs = 0;
    bool fiveHourAvailable = false;
    bool weeklyAvailable = false;
    uint32_t limitsAtMs = 0;
};

// Resolve a complete same-Mac field cache independently of which Mac currently
// controls the device. Calculate these indices once, then copy each selected
// field AND its original receipt to every authenticated live peer for hostId.
// Call after valid status updates and after confirming a transport's identity,
// before any source can be destroyed by a disconnect. This preserves the last
// known state through same-Mac USB/BLE failover without persisting offline data.
inline ThreadFieldSources selectThreadFields(std::string_view hostId, const TelemetrySource* sources,
                                             size_t count, uint32_t nowMs,
                                             size_t preferredSource = kNoTelemetrySource)
{
    auto result = emptyThreadSources();
    if (hostId.empty() || sources == nullptr) return result;
    for (size_t index = 0; index < count; ++index) {
        const auto& source = sources[index];
        if (!source.active || !source.secure || source.endpoint < 0 || source.hostId != hostId ||
            source.threadFields == nullptr) continue;
        for (size_t agent = 0; agent < result.size(); ++agent) {
            for (size_t field = 0; field < result[agent].size(); ++field) {
                const auto& receipt = (*source.threadFields)[agent][field];
                if (!receipt.available) continue;
                auto& current = result[agent][field];
                bool choose = current == kNoTelemetrySource;
                if (!choose) {
                    const uint32_t candidateAge = nowMs - receipt.atMs;
                    const uint32_t currentAge = nowMs - (*sources[current].threadFields)[agent][field].atMs;
                    choose = candidateAge < currentAge ||
                        (candidateAge == currentAge && index == preferredSource);
                }
                if (choose) current = index;
            }
        }
    }
    return result;
}

struct TelemetryProjection {
    size_t routeIndex = kNoTelemetrySource;
    ThreadFieldSources threadIndices = emptyThreadSources();
    size_t quotaIndex = kNoTelemetrySource;
    size_t fiveHourIndex = kNoTelemetrySource;
    size_t weeklyIndex = kNoTelemetrySource;
};

// This function has no allocation, mutation, or transport side effects. On an
// offline/mismatched selected route every index stays empty; the caller clears
// the public telemetry together instead of leaving values from a previous Mac.
inline TelemetryProjection selectTelemetry(std::string_view selectedHostId,
                                           const std::optional<HostRoute>& selectedRoute,
                                           const TelemetrySource* sources, size_t count, uint32_t nowMs)
{
    TelemetryProjection result;
    if (selectedHostId.empty() || !selectedRoute || selectedRoute->endpoint < 0 || sources == nullptr) return result;
    const auto sameHost = [&](size_t index) {
        const auto& source = sources[index];
        return source.active && source.secure && source.endpoint >= 0 && source.hostId == selectedHostId;
    };
    for (size_t index = 0; index < count; ++index) {
        const auto& source = sources[index];
        if (sameHost(index) && source.rpcReady && source.endpoint == selectedRoute->endpoint &&
            source.transport == selectedRoute->transport) {
            result.routeIndex = index;
            break;
        }
    }
    if (result.routeIndex == kNoTelemetrySource) return result;
    result.threadIndices = selectThreadFields(selectedHostId, sources, count, nowMs, result.routeIndex);

    // Compare unsigned ages, so boot-time zero and the millis() wrap are both
    // valid timestamps. On a tie, prefer the current control route, otherwise
    // retain source order to avoid a needless display change.
    const auto newer = [&](size_t candidate, size_t current, uint32_t TelemetrySource::*receivedAt) {
        if (current == kNoTelemetrySource) return true;
        const uint32_t candidateAge = nowMs - sources[candidate].*receivedAt;
        const uint32_t currentAge = nowMs - sources[current].*receivedAt;
        return candidateAge < currentAge || (candidateAge == currentAge && candidate == result.routeIndex);
    };
    for (size_t index = 0; index < count; ++index) {
        if (!sameHost(index)) continue;
        const auto& source = sources[index];
        if (source.quotaAvailable && nowMs - source.quotaAtMs < kQuotaFreshForMs &&
            newer(index, result.quotaIndex, &TelemetrySource::quotaAtMs)) {
            result.quotaIndex = index;
        }
        if (nowMs - source.limitsAtMs >= kQuotaFreshForMs) continue;
        if (source.fiveHourAvailable && newer(index, result.fiveHourIndex, &TelemetrySource::limitsAtMs)) {
            result.fiveHourIndex = index;
        }
        if (source.weeklyAvailable && newer(index, result.weeklyIndex, &TelemetrySource::limitsAtMs)) {
            result.weeklyIndex = index;
        }
    }
    return result;
}

inline uint32_t remainingResetSeconds(uint32_t receivedRemainingSeconds, uint32_t receivedAtMs, uint32_t nowMs)
{
    const uint32_t elapsedSeconds = (nowMs - receivedAtMs) / 1000;
    return elapsedSeconds >= receivedRemainingSeconds ? 0 : receivedRemainingSeconds - elapsedSeconds;
}

}  // namespace codex_micro
