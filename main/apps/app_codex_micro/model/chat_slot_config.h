/*
 * SPDX-FileCopyrightText: 2026 Codex Micro for StopWatch contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "chat_mode_model.h"

namespace codex_micro_app::model::config {

// Public builds contain only neutral preview data. A private
// local_chat_slots.h may replace both arrays without entering Git.
inline constexpr ChatSlots kPinnedSlots = {};

inline constexpr std::array<ChatEntry, 6> kRecentChats = {
    ChatEntry{"preview-notes", "NOTES", "PERSONAL", "Quick notes"},
    ChatEntry{"preview-plan", "PLAN", "WORK", "Weekly plan"},
    ChatEntry{"preview-learn", "LEARN", "STUDY", "Learning log"},
    ChatEntry{"preview-ideas", "IDEAS", "PERSONAL", "Idea garden"},
    ChatEntry{"preview-draft", "DRAFT", "WORK", "Draft review"},
    ChatEntry{"preview-lab", "LAB", "STUDY", "Research lab"},
};

}  // namespace codex_micro_app::model::config
