// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace codex_micro::usb {

// A wake/software recovery must retain a manual choice only when the new USB
// session identifies the same Mac. A cable can move during a software reset.
// All methods run on the service worker, after session-epoch validation.
class ResumePreference {
public:
    void mounted(bool resumed, const std::string& preference)
    {
        savedPreference_ = resumed ? preference : std::string{};
        // Congestion may cause another reset before Feature 7 arrives. Keep
        // the earlier confirmed identity across such unfinished recoveries.
        if (!resumed) previousHostId_.clear();
        else if (!confirmedHostId_.empty()) previousHostId_ = confirmedHostId_;
        confirmedHostId_.clear();
    }

    void identityConfirmed(const std::string& id) { confirmedHostId_ = id; }

    void manuallySelected(const std::string& preference)
    {
        if (!savedPreference_.empty()) savedPreference_ = preference;
    }

    // Call only after this session has a stable identity AND a valid RPC
    // handshake. Consuming it earlier would depend on report arrival order.
    std::string takeForReadyHost(const std::string& id)
    {
        const std::string result = !id.empty() && id == previousHostId_ ? savedPreference_ : std::string{};
        savedPreference_.clear();
        previousHostId_.clear();
        return result;
    }

private:
    std::string confirmedHostId_;
    std::string previousHostId_;
    std::string savedPreference_;
};

}  // namespace codex_micro::usb
