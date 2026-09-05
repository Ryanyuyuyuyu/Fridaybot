// SPDX-License-Identifier: MIT
#include "main/services/codex_micro/usb_write.h"

#include <cassert>
#include <cstdint>
#include <string>

using codex_micro::usb::WriteBudget;
using codex_micro::usb::sendFramedMessage;

int main()
{
    WriteBudget budget;
    int64_t now = 0;
    assert(!budget.available(now));
    budget.start(now);
    const std::string message = std::string(180, 'a') + '\n';
    std::string received;
    unsigned aborts = 0;
    const bool sent = sendFramedMessage(message.data(), message.size(), [&](const uint8_t* report) {
        assert(budget.available(now));
        assert(report[0] == 2 && report[1] <= 61);
        received.append(reinterpret_cast<const char*>(report + 2), report[1]);
        now += 1000;
        return true;
    }, [&] { ++aborts; });
    assert(sent && received == message && aborts == 0 && now == 3000);
    assert(budget.deadline() == 8000);  // Fragmenting must not renew the deadline.

    // A 4096-byte echoed RPC identifier may need dozens of HID reports. It
    // still shares one deadline, aborts its partial message and sends no tail.
    now = 0;
    budget.start(now);
    unsigned accepted = 0;
    const std::string longMessage(4096, 'b');
    assert(!sendFramedMessage(longMessage.data(), longMessage.size(), [&](const uint8_t*) {
        if (!budget.available(now)) return false;
        now += 1000;
        ++accepted;
        return true;
    }, [&] { ++aborts; }));
    assert(now == 8000 && accepted == 8 && aborts == 1);
    assert(!budget.available(now));

    // Congestion while the host keeps writing requests must not multiply the
    // deadline by backlog length. Model polling at both 1 ms and 10 ms ticks,
    // including a phase where the last sleep starts just before the deadline.
    for (const int64_t tick : {int64_t{1000}, int64_t{10000}}) {
        for (int64_t phase = 1000; phase <= tick; phase += 1000) {
            now = 0;
            budget.start(now);
            int64_t nextTick = phase;
            unsigned handled = 0;
            unsigned disconnects = 0;
            for (unsigned request = 0; request < 16 && budget.available(now); ++request) {
                ++handled;
                assert(!sendFramedMessage(longMessage.data(), longMessage.size(), [&](const uint8_t*) {
                    while (budget.available(now)) {
                        now = nextTick;
                        nextTick += tick;
                    }
                    return false;
                }, [&] { ++disconnects; }));
            }
            assert(handled == 1 && disconnects == 1);
            assert(now <= WriteBudget::kDurationUs + tick);
            assert(now < 240000);  // Friday's 24 x 10 ms packet capacity.
        }
    }

    // A timed-out report can race with final controller submission. One report
    // can contain a key press, so both single- and multi-report failures close
    // the link before later controls can be processed.
    const std::string shortMessage = "{\"id\":1}\n";
    aborts = 0;
    assert(!sendFramedMessage(shortMessage.data(), shortMessage.size(), [](const uint8_t*) { return false; },
                             [&] { ++aborts; }));
    assert(aborts == 1);
    assert(!sendFramedMessage(message.data(), message.size(), [](const uint8_t*) { return false; },
                             [&] { ++aborts; }));
    assert(aborts == 2);

    // Starting the next worker iteration is the only operation that restores
    // time. The failed transaction has no retained or deferred message tail.
    now = 50000;
    assert(!budget.available(now));
    budget.start(now);
    assert(budget.deadline() == 58000 && budget.available(now));
}
