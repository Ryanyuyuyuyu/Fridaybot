// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace codex_micro {
// Callback-side session tokens. Caller serializes access. A different peer's
// reconnect must never invalidate this peer's already-accepted CCCD/write.
class BleSessions {
public:
    uint32_t connect(uint16_t id, const uint8_t* address) {
        Entry* slot = nullptr;
        for (auto& entry : entries_) if (entry.used && entry.id == id) { slot = &entry; break; }
        if (!slot) for (auto& entry : entries_) if (!entry.active) { slot = &entry; break; }
        if (!slot) return 0;
        if (slot->active && std::memcmp(slot->address.data(), address, 6) == 0) return slot->token;
        if (++sequence_ == 0) ++sequence_;
        *slot = Entry{};
        slot->used = slot->active = true;
        slot->id = id;
        slot->token = sequence_;
        std::memcpy(slot->address.data(), address, 6);
        return slot->token;
    }
    uint32_t token(uint16_t id, const uint8_t* address = nullptr) const {
        for (const auto& entry : entries_) {
            if (entry.active && entry.id == id && (!address || std::memcmp(entry.address.data(), address, 6) == 0)) return entry.token;
        }
        return 0;
    }
    uint32_t disconnect(uint16_t id, const uint8_t* address) {
        for (auto& entry : entries_) {
            if (entry.active && entry.id == id && std::memcmp(entry.address.data(), address, 6) == 0) {
                entry.active = false;
                return entry.token;
            }
        }
        return 0;
    }
private:
    struct Entry { bool used=false,active=false; uint16_t id=0; uint32_t token=0; std::array<uint8_t,6> address{}; };
    std::array<Entry,8> entries_{};
    uint32_t sequence_=0;
};
}
