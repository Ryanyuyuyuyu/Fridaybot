/*
 * SPDX-FileCopyrightText: 2026 Codex Micro for StopWatch contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace codex_micro_app::model {

constexpr size_t kChatSlotCount = 6;

enum class DashboardMode : uint8_t {
    Codex,
    Chat,
};

constexpr DashboardMode toggledMode(DashboardMode mode)
{
    return mode == DashboardMode::Codex ? DashboardMode::Chat : DashboardMode::Codex;
}

struct ChatEntry {
    std::string_view id;
    std::string_view alias;
    std::string_view project;
    std::string_view title;
};

struct ChatSlot {
    ChatEntry chat;
    bool available = false;
    bool pinned    = false;
};

using ChatSlots = std::array<ChatSlot, kChatSlotCount>;

constexpr bool isPrintableAscii(std::string_view value, size_t maximumLength)
{
    if (value.empty() || value.size() > maximumLength) {
        return false;
    }
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20 || byte > 0x7E) {
            return false;
        }
    }
    return true;
}

constexpr bool isValidChat(const ChatEntry& chat)
{
    return isPrintableAscii(chat.id, 48) && isPrintableAscii(chat.alias, 7) && isPrintableAscii(chat.project, 20) &&
           isPrintableAscii(chat.title, 32);
}

constexpr bool containsChat(const ChatSlots& slots, std::string_view id)
{
    for (const auto& slot : slots) {
        if (slot.available && slot.chat.id == id) {
            return true;
        }
    }
    return false;
}

template <size_t RecentCount>
constexpr ChatSlots composeChatSlots(const ChatSlots& configured, const std::array<ChatEntry, RecentCount>& recent)
{
    ChatSlots result{};

    // Explicitly pinned positions win. Invalid or duplicate private entries
    // fail closed instead of leaking malformed labels into the UI.
    for (size_t index = 0; index < result.size(); ++index) {
        const auto& candidate = configured[index];
        if (!candidate.pinned || !candidate.available || !isValidChat(candidate.chat) ||
            containsChat(result, candidate.chat.id)) {
            continue;
        }
        result[index] = candidate;
    }

    // The remaining positions follow global recency, independent of Project.
    // A selected Chat is deliberately not reordered inside the active session;
    // callers rebuild this snapshot only on a later refresh/open.
    for (const auto& chat : recent) {
        if (!isValidChat(chat) || containsChat(result, chat.id)) {
            continue;
        }
        for (auto& slot : result) {
            if (slot.available) {
                continue;
            }
            slot.chat      = chat;
            slot.available = true;
            slot.pinned    = false;
            break;
        }
    }

    return result;
}

constexpr size_t firstAvailableChat(const ChatSlots& slots)
{
    for (size_t index = 0; index < slots.size(); ++index) {
        if (slots[index].available) {
            return index;
        }
    }
    return slots.size();
}

}  // namespace codex_micro_app::model
