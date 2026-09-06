// SPDX-License-Identifier: MIT
#include "main/services/codex_micro/telemetry_projection.h"

#include <array>
#include <cassert>

using namespace codex_micro;

namespace {
constexpr int kUsb = 0xFFFE;
constexpr const char* kPrivate = "private-stable-id";
constexpr const char* kWork = "work-stable-id";

TelemetrySource native(int endpoint, const char* hostId, Transport transport, uint32_t receivedAt)
{
    TelemetrySource source;
    source.endpoint = endpoint;
    source.hostId = hostId;
    source.transport = transport;
    source.active = source.secure = source.rpcReady = true;
    source.quotaAtMs = source.limitsAtMs = receivedAt;
    return source;
}

void setAllFields(ThreadFieldMetadata& metadata, uint32_t atMs)
{
    for (auto& agent : metadata) for (auto& field : agent) field = {true, atMs};
}

template<size_t Count>
void attachFields(std::array<TelemetrySource, Count>& sources, std::array<ThreadFieldMetadata, Count>& metadata)
{
    for (size_t i = 0; i < Count; ++i) {
        setAllFields(metadata[i], sources[i].quotaAtMs);
        sources[i].threadFields = &metadata[i];
    }
}

void expectThreadSource(const TelemetryProjection& projection, size_t expected)
{
    for (const auto& agent : projection.threadIndices) for (size_t source : agent) assert(source == expected);
}

void expectEmpty(const TelemetryProjection& projection)
{
    assert(projection.routeIndex == kNoTelemetrySource);
    expectThreadSource(projection, kNoTelemetrySource);
    assert(projection.quotaIndex == kNoTelemetrySource);
    assert(projection.fiveHourIndex == kNoTelemetrySource);
    assert(projection.weeklyIndex == kNoTelemetrySource);
}

void selectedMacOwnsEveryFieldAcrossBothTransports()
{
    for (bool privateUsb : {false, true}) {
        const char* usbHost = privateUsb ? kPrivate : kWork;
        const char* otherHost = privateUsb ? kWork : kPrivate;
        std::array<TelemetrySource, 4> sources{
            native(1, usbHost, Transport::Ble, 4000),
            native(2, otherHost, Transport::Ble, 6000),
            native(kUsb, usbHost, Transport::Usb, 3000),
            native(3, otherHost, Transport::Ble, 7000),
        };
        std::array<ThreadFieldMetadata, 4> fields{};
        attachFields(sources, fields);
        for (auto& source : sources) {
            source.quotaAvailable = source.fiveHourAvailable = source.weeklyAvailable = true;
        }
        // A newer other-Mac update never contaminates the selected USB Mac.
        auto projected = selectTelemetry(usbHost, HostRoute{Transport::Usb, kUsb}, sources.data(), sources.size(), 8000);
        assert(projected.routeIndex == 2);
        expectThreadSource(projected, 0);
        assert(projected.quotaIndex == 0 && projected.fiveHourIndex == 0 && projected.weeklyIndex == 0);
        // Switch to the other Mac over BLE while the USB route stays active.
        projected = selectTelemetry(otherHost, HostRoute{Transport::Ble, 2}, sources.data(), sources.size(), 8000);
        assert(projected.routeIndex == 1);
        expectThreadSource(projected, 3);
        assert(projected.quotaIndex == 3 && projected.fiveHourIndex == 3 && projected.weeklyIndex == 3);

        // Same-host USB > BLE controls does not mean older USB telemetry wins.
        setAllFields(fields[2], 7500);
        sources[2].quotaAtMs = sources[2].limitsAtMs = 7500;
        projected = selectTelemetry(usbHost, HostRoute{Transport::Ble, 1}, sources.data(), sources.size(), 8000);
        assert(projected.routeIndex == 0 && projected.quotaIndex == 2);
        expectThreadSource(projected, 2);
        sources[2].active = false;
        projected = selectTelemetry(usbHost, HostRoute{Transport::Ble, 1}, sources.data(), sources.size(), 8000);
        assert(projected.routeIndex == 0 && projected.quotaIndex == 0);
        expectThreadSource(projected, 0);
    }
}

void offlineUnknownAndReusedRoutesCannotRetainTelemetry()
{
    std::array<TelemetrySource, 2> sources{native(1, kPrivate, Transport::Ble, 100), native(kUsb, kWork, Transport::Usb, 200)};
    sources[0].quotaAvailable = sources[0].fiveHourAvailable = sources[0].weeklyAvailable = true;
    for (const auto& host : {kPrivate, kWork}) {
        expectEmpty(selectTelemetry(host, std::nullopt, sources.data(), sources.size(), 300));
    }
    expectEmpty(selectTelemetry("", HostRoute{Transport::Ble, 1}, sources.data(), sources.size(), 300));
    expectEmpty(selectTelemetry(kPrivate, HostRoute{Transport::Ble, 99}, sources.data(), sources.size(), 300));
    expectEmpty(selectTelemetry(kPrivate, HostRoute{Transport::Usb, 1}, sources.data(), sources.size(), 300));
    expectEmpty(selectTelemetry(kPrivate, HostRoute{Transport::Ble, 1}, nullptr, 0, 300));
    sources[0].hostId = kWork;  // Numeric BLE endpoint reused by a different Mac.
    expectEmpty(selectTelemetry(kPrivate, HostRoute{Transport::Ble, 1}, sources.data(), sources.size(), 300));
    sources[0].hostId = kPrivate;
    sources[0].active = false;
    expectEmpty(selectTelemetry(kPrivate, HostRoute{Transport::Ble, 1}, sources.data(), sources.size(), 300));
    sources[0].active = true;
    sources[0].secure = false;
    expectEmpty(selectTelemetry(kPrivate, HostRoute{Transport::Ble, 1}, sources.data(), sources.size(), 300));
    sources[0].secure = true;
    sources[0].rpcReady = false;
    expectEmpty(selectTelemetry(kPrivate, HostRoute{Transport::Ble, 1}, sources.data(), sources.size(), 300));
}

void independentHelperFieldsAndExpiryDoNotEraseValidWindows()
{
    std::array<TelemetrySource, 4> sources{
        native(kUsb, kPrivate, Transport::Usb, 10),
        native(1, kPrivate, Transport::Ble, 10),
        native(2, kPrivate, Transport::Ble, 10),
        native(3, kWork, Transport::Ble, 10),
    };
    std::array<ThreadFieldMetadata, 4> fields{};
    attachFields(sources, fields);
    // The authenticated helper link need not have a native control handshake.
    sources[1].rpcReady = sources[2].rpcReady = false;
    sources[1].quotaAvailable = sources[1].fiveHourAvailable = sources[1].weeklyAvailable = true;
    sources[1].quotaAtMs = sources[1].limitsAtMs = 1000;
    sources[2].quotaAvailable = sources[2].fiveHourAvailable = true;
    sources[2].quotaAtMs = sources[2].limitsAtMs = 2000;
    sources[3].quotaAvailable = sources[3].fiveHourAvailable = sources[3].weeklyAvailable = true;
    sources[3].quotaAtMs = sources[3].limitsAtMs = 3000;
    auto projected = selectTelemetry(kPrivate, HostRoute{Transport::Usb, kUsb}, sources.data(), sources.size(), 4000);
    assert(projected.quotaIndex == 2 && projected.fiveHourIndex == 2 && projected.weeklyIndex == 1);
    // Two missed three-minute updates are tolerated; each field expires based
    // on when that value was received, not a native heartbeat or name refresh.
    projected = selectTelemetry(kPrivate, HostRoute{Transport::Usb, kUsb}, sources.data(), sources.size(), 400000);
    assert(projected.quotaIndex == 2 && projected.fiveHourIndex == 2 && projected.weeklyIndex == 1);
    projected = selectTelemetry(kPrivate, HostRoute{Transport::Usb, kUsb}, sources.data(), sources.size(), 601000);
    assert(projected.quotaIndex == 2 && projected.fiveHourIndex == 2 && projected.weeklyIndex == kNoTelemetrySource);
    projected = selectTelemetry(kPrivate, HostRoute{Transport::Usb, kUsb}, sources.data(), sources.size(), 602000);
    assert(projected.quotaIndex == kNoTelemetrySource && projected.fiveHourIndex == kNoTelemetrySource);
    expectThreadSource(projected, 0);  // An idle native state is still valid.
    sources[2].secure = false;
    projected = selectTelemetry(kPrivate, HostRoute{Transport::Usb, kUsb}, sources.data(), sources.size(), 4000);
    assert(projected.quotaIndex == 1 && projected.fiveHourIndex == 1);
}

void timestampWrapZeroAndEqualTimeStayDeterministic()
{
    std::array<TelemetrySource, 2> sources{
        native(1, kPrivate, Transport::Ble, UINT32_MAX - 300),
        native(kUsb, kPrivate, Transport::Usb, 0),
    };
    std::array<ThreadFieldMetadata, 2> fields{};
    attachFields(sources, fields);
    for (auto& source : sources) {
        source.quotaAvailable = source.fiveHourAvailable = source.weeklyAvailable = true;
    }
    auto projected = selectTelemetry(kPrivate, HostRoute{Transport::Ble, 1}, sources.data(), sources.size(), 200);
    expectThreadSource(projected, 1);
    assert(projected.quotaIndex == 1);
    setAllFields(fields[0], 0);
    sources[0].quotaAtMs = sources[0].limitsAtMs = 0;
    projected = selectTelemetry(kPrivate, HostRoute{Transport::Ble, 1}, sources.data(), sources.size(), 200);
    expectThreadSource(projected, 0);
    assert(projected.quotaIndex == 0);
    projected = selectTelemetry(kPrivate, HostRoute{Transport::Usb, kUsb}, sources.data(), sources.size(), 200);
    expectThreadSource(projected, 1);
    assert(projected.quotaIndex == 1);
    assert(remainingResetSeconds(60, 0, 59000) == 1);
    assert(remainingResetSeconds(60, 0, 60000) == 0);
    assert(remainingResetSeconds(60, 0, 61000) == 0);
    assert(remainingResetSeconds(60, UINT32_MAX - 999, 1000) == 58);
    assert(remainingResetSeconds(0, 0, 0) == 0);
}

void sparseEarlyUpdatesMergePerFieldOnlyAfterIdentityMatches()
{
    std::array<TelemetrySource, 3> sources{
        native(1, kPrivate, Transport::Ble, 100),
        native(kUsb, "", Transport::Usb, 200),
        native(2, kWork, Transport::Ble, 300),
    };
    std::array<ThreadFieldMetadata, 3> fields{};
    attachFields(sources, fields);
    fields[1] = {};  // USB has only received two fields, before helper identity.
    const size_t color = static_cast<size_t>(ThreadField::Color);
    const size_t brightness = static_cast<size_t>(ThreadField::Brightness);
    const size_t effect = static_cast<size_t>(ThreadField::Effect);
    fields[1][0][color] = {true, 200};
    fields[1][1][brightness] = {true, 200};
    auto projected = selectTelemetry(kPrivate, HostRoute{Transport::Ble, 1}, sources.data(), sources.size(), 400);
    expectThreadSource(projected, 0);  // Unknown USB and newer Work Mac excluded.
    sources[1].hostId = kPrivate;  // Feature identity now authenticates the same Mac.
    projected = selectTelemetry(kPrivate, HostRoute{Transport::Usb, kUsb}, sources.data(), sources.size(), 400);
    for (size_t agent = 0; agent < 6; ++agent) {
        for (size_t field = 0; field < 4; ++field) {
            const size_t expected = (agent == 0 && field == color) || (agent == 1 && field == brightness) ? 1 : 0;
            assert(projected.threadIndices[agent][field] == expected);
        }
    }
    // A later BLE update to one different field keeps both newer USB fields.
    fields[0][0][effect] = {true, 350};
    projected = selectTelemetry(kPrivate, HostRoute{Transport::Usb, kUsb}, sources.data(), sources.size(), 400);
    assert(projected.threadIndices[0][color] == 1 && projected.threadIndices[0][effect] == 0);
    assert(projected.threadIndices[1][brightness] == 1);
    // A same-tick conflict chooses the current route consistently.
    fields[0][0][color] = {true, 200};
    projected = selectTelemetry(kPrivate, HostRoute{Transport::Ble, 1}, sources.data(), sources.size(), 400);
    assert(projected.threadIndices[0][color] == 0);
    projected = selectTelemetry(kPrivate, HostRoute{Transport::Usb, kUsb}, sources.data(), sources.size(), 400);
    assert(projected.threadIndices[0][color] == 1);
    // Once the old BLE session leaves, only USB's actually supplied fields
    // remain. Missing fields return to defaults, never to the Work Mac.
    sources[0].active = false;
    projected = selectTelemetry(kPrivate, HostRoute{Transport::Usb, kUsb}, sources.data(), sources.size(), 400);
    assert(projected.threadIndices[0][color] == 1);
    assert(projected.threadIndices[1][brightness] == 1);
    assert(projected.threadIndices[0][effect] == kNoTelemetrySource);
    assert(projected.threadIndices[5][color] == kNoTelemetrySource);
    // A new unreported session must not silently project default fields as if
    // they were authoritative updates received from the selected Mac.
    sources[1].threadFields = nullptr;
    projected = selectTelemetry(kPrivate, HostRoute{Transport::Usb, kUsb}, sources.data(), sources.size(), 400);
    expectThreadSource(projected, kNoTelemetrySource);
}

void synchronizeKnownPeerCachesBeforeUnplugKeepsLatestFields()
{
    std::array<TelemetrySource, 3> sources{
        native(1, kPrivate, Transport::Ble, 100),
        native(kUsb, "", Transport::Usb, 200),
        native(2, kWork, Transport::Ble, 300),
    };
    std::array<ThreadFieldMetadata, 3> receipts{};
    attachFields(sources, receipts);
    using Values = std::array<std::array<int, 4>, 6>;
    std::array<Values, 3> values{};
    for (auto& agent : values[0]) agent.fill(10); // Old BLE state.
    for (auto& agent : values[1]) agent.fill(20); // New USB sparse state.
    for (auto& agent : values[2]) agent.fill(99); // Other Mac must stay separate.
    receipts[1] = {};
    receipts[1][0][0] = {true, 200};
    receipts[1][1][1] = {true, 200};
    const auto synchronize = [&](std::string_view host) {
        const auto indices = selectThreadFields(host, sources.data(), sources.size(), 400, 1);
        for (size_t destination = 0; destination < sources.size(); ++destination) {
            if (!sources[destination].active || !sources[destination].secure || sources[destination].hostId != host) continue;
            for (size_t agent = 0; agent < 6; ++agent) {
                for (size_t field = 0; field < 4; ++field) {
                    const size_t origin = indices[agent][field];
                    if (origin == kNoTelemetrySource) continue;
                    values[destination][agent][field] = values[origin][agent][field];
                    receipts[destination][agent][field] = receipts[origin][agent][field];
                }
            }
        }
    };
    synchronize(kPrivate);
    assert(values[0][0][0] == 10 && values[1][0][0] == 20); // Unknown identity never merges.
    sources[1].hostId = kPrivate;
    synchronize(kPrivate); // Identity arrives after native partial updates.
    assert(values[0] == values[1]);
    assert(values[0][0][0] == 20 && values[0][1][1] == 20 && values[0][5][0] == 10);
    assert(receipts[0][0][0].atMs == 200 && receipts[1][5][0].atMs == 100);
    assert(values[2][0][0] == 99 && receipts[2][0][0].atMs == 300);

    // Destroy the USB Connection just as the worker does on unmount. The
    // remaining BLE cache still carries the newest USB fields and their ages.
    sources[1] = {};
    receipts[1] = {};
    values[1] = {};
    auto projected = selectTelemetry(kPrivate, HostRoute{Transport::Ble, 1}, sources.data(), sources.size(), 500);
    expectThreadSource(projected, 0);
    assert(values[projected.threadIndices[0][0]][0][0] == 20);
    assert(values[projected.threadIndices[5][0]][5][0] == 10);
    sources[0].active = false;
    expectEmpty(selectTelemetry(kPrivate, std::nullopt, sources.data(), sources.size(), 600));
    const auto offline = selectThreadFields(kPrivate, sources.data(), sources.size(), 600);
    for (const auto& agent : offline) for (size_t index : agent) assert(index == kNoTelemetrySource);
    assert(selectThreadFields("", sources.data(), sources.size(), 600) == emptyThreadSources());
    assert(selectThreadFields(kPrivate, nullptr, 0, 600) == emptyThreadSources());
}
}  // namespace

int main()
{
    selectedMacOwnsEveryFieldAcrossBothTransports();
    offlineUnknownAndReusedRoutesCannotRetainTelemetry();
    independentHelperFieldsAndExpiryDoNotEraseValidWindows();
    timestampWrapZeroAndEqualTimeStayDeterministic();
    sparseEarlyUpdatesMergePerFieldOnlyAfterIdentityMatches();
    synchronizeKnownPeerCachesBeforeUnplugKeepsLatestFields();
}
