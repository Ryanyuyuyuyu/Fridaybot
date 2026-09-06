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
        // A choice made after this attachment started outranks a delayed USB
        // handshake. Carry it through another unfinished software recovery,
        // but never through a later physical attachment or a completed session.
        if (!resumed || !awaitingReady_) explicitPreference_.clear();
        savedPreference_ = resumed ? preference : std::string{};
        // Congestion may cause another reset before Feature 7 arrives. Keep
        // the earlier confirmed identity across such unfinished recoveries.
        if (!resumed) previousHostId_.clear();
        else if (!confirmedHostId_.empty()) previousHostId_ = confirmedHostId_;
        confirmedHostId_.clear();
        awaitingReady_ = true;
    }

    void identityConfirmed(const std::string& id) { confirmedHostId_ = id; }

    void manuallySelected(const std::string& preference)
    {
        if (awaitingReady_) explicitPreference_ = preference;
        if (!savedPreference_.empty()) savedPreference_ = preference;
    }

    // Call only after this session has a stable identity AND a valid RPC
    // handshake. Consuming it earlier would depend on report arrival order.
    std::string takeForReadyHost(const std::string& id)
    {
        // A premature or unrelated ready callback must not consume a pending
        // choice. The worker separately verifies the native RPC handshake.
        if (id.empty() || id != confirmedHostId_) return {};
        const std::string result = !explicitPreference_.empty() ? explicitPreference_ :
            id == previousHostId_ ? savedPreference_ : std::string{};
        savedPreference_.clear();
        previousHostId_.clear();
        explicitPreference_.clear();
        awaitingReady_ = false;
        return result;
    }

private:
    std::string confirmedHostId_;
    std::string previousHostId_;
    std::string savedPreference_;
    std::string explicitPreference_;
    bool awaitingReady_ = false;
};

}  // namespace codex_micro::usb
