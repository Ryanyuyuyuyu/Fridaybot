// SPDX-License-Identifier: MIT
#include "main/services/codex_micro/deferred_control.h"
#include "main/services/codex_micro/usb_write.h"

#include <cassert>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

using namespace codex_micro;

namespace {
struct Command {
    uint32_t sequence;
    uint32_t epoch;
    bool release;
};
struct Batch {
    unsigned consumed = 0;
    unsigned attempts = 0;
    bool deferred = false;
};
struct Fixture {
    static constexpr size_t capacity = 24;
    std::deque<Command> normal;
    std::deque<Command> releases;
    DeferredControlGate gate;
    ControlRouteStamp route{3, 0xFFFE, 7};
    usb::WriteBudget budget;
    int64_t now = 0;
    bool ready = true;
    bool usbRoute = true;
    bool held = false;
    bool expireDuringSerialization = false;
    bool failAfterAttempt = false;
    unsigned disconnects = 0;
    std::vector<std::string> wire;

    bool enqueue(Command command)
    {
        auto& queue = command.release ? releases : normal;
        if (queue.size() == capacity) return false;
        queue.push_back(command);
        return true;
    }
    void cycle(int64_t start) { now = start; budget.start(now); }
    Batch drain()
    {
        Batch batch;
        for (unsigned i = 0; i < 16 && (!normal.empty() || !releases.empty()); ++i) {
            const bool release = releaseHeadFirst(!normal.empty(), normal.empty() ? 0 : normal.front().sequence,
                                                   !releases.empty(), releases.empty() ? 0 : releases.front().sequence);
            auto& source = release ? releases : normal;
            const Command command = source.front();
            const auto stamp = route;
            const auto decision = gate.inspect(command.sequence, command.epoch, stamp, ready, usbRoute,
                                               budget.available(now));
            if (decision == ControlAdmission::Defer) { batch.deferred = true; break; }
            if (decision == ControlAdmission::Dispatch) {
                if (expireDuringSerialization) {
                    now = budget.deadline();
                    expireDuringSerialization = false;
                }
                // Mirrors the final sendJson USB preflight, which can expire
                // AFTER admission while JSON is being serialized.
                if (usbRoute && !budget.available(now)) {
                    gate.complete(command.sequence, ControlAttempt::DeferredBeforeAttempt, stamp);
                    batch.deferred = true;
                    break;
                }
                if (!command.release || held) {
                    ++batch.attempts;
                    const std::string json = std::string("{\"method\":\"v.oai.hid\",\"params\":{\"k\":\"AG00\",\"act\":") +
                                             (command.release ? "0" : "1") + ",\"ag\":0}}\n";
                    std::string received;
                    const bool delivered = usb::sendFramedMessage(json.data(), json.size(), [&](const uint8_t* body) {
                        if (failAfterAttempt) return false;
                        received.append(reinterpret_cast<const char*>(body + 2), body[1]);
                        now += 1000;
                        return true;
                    }, [&] { ++disconnects; ready = false; });
                    if (delivered) {
                        assert(received == json);
                        wire.push_back(received);
                        held = !command.release;
                    }
                }
                gate.complete(command.sequence, ControlAttempt::Consumed, stamp);
            }
            source.pop_front();
            ++batch.consumed;
        }
        return batch;
    }
};
}

int main()
{
    // RPC has exhausted this cycle before a tap reaches the worker. Neither
    // press nor release is removed, attempted or retried in a tight inner loop.
    Fixture tap;
    assert(tap.enqueue({1, 3, false}) && tap.enqueue({2, 3, true}));
    tap.cycle(0);
    tap.now = 8000;
    const auto blocked = tap.drain();
    assert(blocked.deferred && blocked.consumed == 0 && blocked.attempts == 0);
    assert(tap.gate.waiting() && tap.normal.size() == 1 && tap.releases.size() == 1);
    assert(tap.wire.empty() && tap.disconnects == 0 && tap.budget.deadline() == 8000);
    // The owner yields and reserves first use of the next cycle for this head.
    tap.cycle(10000);
    const auto recovered = tap.drain();
    assert(!recovered.deferred && recovered.consumed == 2 && recovered.attempts == 2);
    assert(tap.wire.size() == 2 && tap.wire[0].find("\"act\":1") != std::string::npos);
    assert(tap.wire[1].find("\"act\":0") != std::string::npos && !tap.held && !tap.gate.waiting());

    // Covers the admission/serialization timing gap: only zero-attempt writes
    // may stay queued. They retain the original route stamp across retries.
    Fixture late;
    assert(late.enqueue({1, 3, false}) && late.enqueue({2, 3, true}));
    late.cycle(0);
    late.expireDuringSerialization = true;
    assert(late.drain().deferred && late.normal.size() == 1 && late.wire.empty());
    late.cycle(10000);
    assert(late.drain().consumed == 2 && late.wire.size() == 2);

    // A manual host change invalidates both original input edges.
    Fixture switched;
    assert(switched.enqueue({1, 3, false}) && switched.enqueue({2, 3, true}));
    switched.cycle(0); switched.now = 8000;
    assert(switched.drain().deferred);
    switched.route = {4, 2, 9}; switched.usbRoute = false;
    switched.cycle(10000);
    assert(switched.drain().consumed == 2 && switched.wire.empty() && !switched.gate.waiting());

    // Reusing the same USB endpoint without the service control epoch having
    // caught up still cannot dispatch the retained press into a new session.
    Fixture replug;
    assert(replug.enqueue({1, 3, false}) && replug.enqueue({2, 3, true}));
    replug.cycle(0); replug.now = 8000;
    assert(replug.drain().deferred);
    replug.route.linkEpoch = 8;
    replug.cycle(10000);
    assert(replug.drain().consumed == 2 && replug.wire.empty() && !replug.gate.waiting());

    // Full press storage cannot consume the separately reserved release slots.
    Fixture full;
    for (uint32_t i = 0; i < Fixture::capacity; ++i) assert(full.enqueue({i * 2, 3, false}));
    assert(!full.enqueue({48, 3, false}));
    for (uint32_t i = 0; i < Fixture::capacity; ++i) assert(full.enqueue({i * 2 + 1, 3, true}));
    assert(!full.enqueue({49, 3, true}));
    full.cycle(0); full.now = 8000;
    assert(full.drain().deferred && full.normal.size() == 24 && full.releases.size() == 24);
    assert(releaseHeadFirst(true, 0, true, UINT32_MAX));
    assert(!releaseHeadFirst(true, UINT32_MAX, true, 0));
    assert(releaseHeadFirst(false, 0, true, 0));

    // An actual uncertain USB write is consumed, disconnects and is never
    // retained as a retry that might duplicate a press on the next Mac.
    Fixture failure;
    assert(failure.enqueue({1, 3, false}) && failure.enqueue({2, 3, true}));
    failure.cycle(0); failure.failAfterAttempt = true;
    assert(failure.drain().consumed == 2 && failure.disconnects == 1);
    assert(failure.wire.empty() && !failure.gate.waiting());

    // Enumeration without identity/RPC registration is not a ready route.
    Fixture noIdentity;
    assert(noIdentity.enqueue({1, 3, false}));
    noIdentity.ready = false;
    noIdentity.cycle(0);
    assert(noIdentity.drain().consumed == 1 && noIdentity.wire.empty());
}
