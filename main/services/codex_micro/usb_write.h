// SPDX-License-Identifier: MIT
#pragma once

#include "usb_descriptor.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace codex_micro::usb {

// One deadline is shared by every USB write in a service-worker iteration.
// It is never renewed for another RPC or another fragment of the same RPC.
class WriteBudget {
public:
    static constexpr int64_t kDurationUs = 8000;
    void start(int64_t now) { deadline_ = now + kDurationUs; }
    bool available(int64_t now) const { return deadline_ > now; }
    int64_t deadline() const { return deadline_; }
private:
    int64_t deadline_ = 0;
};

// Framing and uncertain-write policy shared by the real service and host tests.
// The sender owns its time budget. No fragment delay or deferred tail is added.
template <typename Sender, typename AbortWrite>
bool sendFramedMessage(const char* message, size_t length, Sender&& send, AbortWrite&& abortWrite)
{
    constexpr size_t payloadSize = kRpcBodySize - 2;
    for (size_t offset = 0; offset < length;) {
        const size_t chunk = std::min(payloadSize, length - offset);
        uint8_t report[kRpcBodySize] = {};
        report[0] = 2;
        report[1] = static_cast<uint8_t>(chunk);
        std::memcpy(report + 2, message + offset, chunk);
        if (!send(report)) {
            // Cancellation may race with the final controller submission.
            // Even one uncertain report can be a key press whose held state
            // the caller did not record. Disconnect on every attempted failure.
            abortWrite();
            return false;
        }
        offset += chunk;
    }
    return true;
}

}  // namespace codex_micro::usb
