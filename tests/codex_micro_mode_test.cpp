#include "main/apps/app_codex_micro/model/chat_mode_model.h"
#include "main/apps/app_codex_micro/model/mode_button_gesture.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

using codex_micro_app::model::ChatEntry;
using codex_micro_app::model::ChatSlot;
using codex_micro_app::model::ChatSlots;
using codex_micro_app::model::DashboardMode;
using codex_micro_app::model::ModeButtonEvent;
using codex_micro_app::model::ModeButtonGesture;

#define CHECK(condition)                                                                 \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            std::cerr << "check failed at line " << __LINE__ << ": " #condition << '\n'; \
            std::exit(1);                                                                \
        }                                                                                \
    } while (false)

void testChatComposition()
{
    ChatSlots configured{};
    configured[2] = ChatSlot{ChatEntry{"fixed", "FIXED", "WORK", "Pinned chat"}, true, true};
    configured[4] = ChatSlot{ChatEntry{"fixed", "COPY", "WORK", "Duplicate pin"}, true, true};

    const std::array<ChatEntry, 8> recent = {
        ChatEntry{"recent-a", "A", "P1", "Recent A"},        ChatEntry{"fixed", "FIXED", "WORK", "Pinned duplicate"},
        ChatEntry{"recent-b", "B", "P2", "Recent B"},        ChatEntry{"recent-b", "B2", "P2", "Recent duplicate"},
        ChatEntry{"bad", "TOO-LONG", "P3", "Invalid alias"}, ChatEntry{"recent-c", "C", "P3", "Recent C"},
        ChatEntry{"recent-d", "D", "P4", "Recent D"},        ChatEntry{"recent-e", "E", "P5", "Recent E"},
    };

    const auto slots = codex_micro_app::model::composeChatSlots(configured, recent);
    CHECK(slots[2].available && slots[2].pinned && slots[2].chat.id == "fixed");
    CHECK(!slots[4].pinned);
    CHECK(slots[0].chat.id == "recent-a");
    CHECK(slots[1].chat.id == "recent-b");
    CHECK(slots[3].chat.id == "recent-c");
    CHECK(slots[4].chat.id == "recent-d");
    CHECK(slots[5].chat.id == "recent-e");
    CHECK(codex_micro_app::model::firstAvailableChat(slots) == 0);
    CHECK(codex_micro_app::model::toggledMode(DashboardMode::Codex) == DashboardMode::Chat);
    CHECK(codex_micro_app::model::toggledMode(DashboardMode::Chat) == DashboardMode::Codex);
}

void testEmptyAndInvalidChatComposition()
{
    ChatSlots configured{};
    configured[0] = ChatSlot{ChatEntry{"not-pinned", "LOCAL", "P1", "Ignored local"}, true, false};
    configured[1] = ChatSlot{ChatEntry{"bad-title", "BAD", "P1", "Line\nbreak"}, true, true};
    constexpr std::array<ChatEntry, 0> noRecent{};

    const auto slots = codex_micro_app::model::composeChatSlots(configured, noRecent);
    for (const auto& slot : slots) {
        CHECK(!slot.available);
    }
    CHECK(codex_micro_app::model::firstAvailableChat(slots) == slots.size());
}

void testDoubleTapWithoutPttLeak()
{
    ModeButtonGesture gesture;
    CHECK(gesture.update(100, true, true, false) == ModeButtonEvent::None);
    CHECK(gesture.update(170, false, false, true) == ModeButtonEvent::None);
    CHECK(gesture.update(300, true, true, false) == ModeButtonEvent::None);
    CHECK(gesture.update(370, false, false, true) == ModeButtonEvent::ToggleMode);
}

void testSingleTapExpires()
{
    ModeButtonGesture gesture;
    CHECK(gesture.update(100, true, true, false) == ModeButtonEvent::None);
    CHECK(gesture.update(170, false, false, true) == ModeButtonEvent::None);
    CHECK(gesture.update(471, false, false, false) == ModeButtonEvent::None);
    CHECK(gesture.update(500, true, true, false) == ModeButtonEvent::None);
    CHECK(gesture.update(570, false, false, true) == ModeButtonEvent::None);
}

void testTapGapBoundary()
{
    ModeButtonGesture withinBoundary;
    CHECK(withinBoundary.update(100, true, true, false) == ModeButtonEvent::None);
    CHECK(withinBoundary.update(170, false, false, true) == ModeButtonEvent::None);
    CHECK(withinBoundary.update(470, true, true, false) == ModeButtonEvent::None);
    CHECK(withinBoundary.update(540, false, false, true) == ModeButtonEvent::ToggleMode);

    ModeButtonGesture outsideBoundary;
    CHECK(outsideBoundary.update(100, true, true, false) == ModeButtonEvent::None);
    CHECK(outsideBoundary.update(170, false, false, true) == ModeButtonEvent::None);
    CHECK(outsideBoundary.update(471, true, true, false) == ModeButtonEvent::None);
    CHECK(outsideBoundary.update(541, false, false, true) == ModeButtonEvent::None);
}

void testHoldBecomesPtt()
{
    ModeButtonGesture gesture;
    CHECK(gesture.update(100, true, true, false) == ModeButtonEvent::None);
    CHECK(gesture.update(279, true, false, false) == ModeButtonEvent::None);
    CHECK(gesture.update(280, true, false, false) == ModeButtonEvent::PttPress);
    CHECK(gesture.update(410, false, false, true) == ModeButtonEvent::PttRelease);
}

void testSecondHoldCancelsToggle()
{
    ModeButtonGesture gesture;
    CHECK(gesture.update(100, true, true, false) == ModeButtonEvent::None);
    CHECK(gesture.update(170, false, false, true) == ModeButtonEvent::None);
    CHECK(gesture.update(250, true, true, false) == ModeButtonEvent::None);
    CHECK(gesture.update(430, true, false, false) == ModeButtonEvent::PttPress);
    CHECK(gesture.update(500, false, false, true) == ModeButtonEvent::PttRelease);
}

void testBounceAndCancel()
{
    ModeButtonGesture gesture;
    CHECK(gesture.update(100, true, true, false) == ModeButtonEvent::None);
    CHECK(gesture.update(120, false, false, true) == ModeButtonEvent::None);
    CHECK(gesture.update(200, true, true, false) == ModeButtonEvent::None);
    CHECK(gesture.update(380, true, false, false) == ModeButtonEvent::PttPress);
    CHECK(gesture.cancel() == ModeButtonEvent::PttRelease);
    CHECK(gesture.cancel() == ModeButtonEvent::None);
}

void testTouchCancelsToggleButPreservesPtt()
{
    ModeButtonGesture pendingToggle;
    CHECK(pendingToggle.update(100, true, true, false) == ModeButtonEvent::None);
    CHECK(pendingToggle.update(170, false, false, true) == ModeButtonEvent::None);
    pendingToggle.cancelPendingToggle();
    CHECK(pendingToggle.update(250, true, true, false) == ModeButtonEvent::None);
    CHECK(pendingToggle.update(320, false, false, true) == ModeButtonEvent::None);

    ModeButtonGesture pendingPtt;
    CHECK(pendingPtt.update(500, true, true, false) == ModeButtonEvent::None);
    pendingPtt.cancelPendingToggle();
    CHECK(pendingPtt.update(680, true, false, false) == ModeButtonEvent::PttPress);
    CHECK(pendingPtt.update(720, false, false, true) == ModeButtonEvent::PttRelease);

    ModeButtonGesture activePtt;
    CHECK(activePtt.update(800, true, true, false) == ModeButtonEvent::None);
    CHECK(activePtt.update(980, true, false, false) == ModeButtonEvent::PttPress);
    activePtt.cancelPendingToggle();
    CHECK(activePtt.update(1000, true, false, false) == ModeButtonEvent::None);
    CHECK(activePtt.update(1050, false, false, true) == ModeButtonEvent::PttRelease);
}

void testMillisRollover()
{
    ModeButtonGesture gesture;
    constexpr uint32_t firstPress    = UINT32_MAX - 100;
    constexpr uint32_t firstRelease  = UINT32_MAX - 30;
    constexpr uint32_t secondPress   = 80;
    constexpr uint32_t secondRelease = 150;

    CHECK(gesture.update(firstPress, true, true, false) == ModeButtonEvent::None);
    CHECK(gesture.update(firstRelease, false, false, true) == ModeButtonEvent::None);
    CHECK(gesture.update(secondPress, true, true, false) == ModeButtonEvent::None);
    CHECK(gesture.update(secondRelease, false, false, true) == ModeButtonEvent::ToggleMode);
}

}  // namespace

int main()
{
    testChatComposition();
    testEmptyAndInvalidChatComposition();
    testDoubleTapWithoutPttLeak();
    testSingleTapExpires();
    testTapGapBoundary();
    testHoldBecomesPtt();
    testSecondHoldCancelsToggle();
    testBounceAndCancel();
    testTouchCancelsToggleButPreservesPtt();
    testMillisRollover();
    std::cout << "codex_micro_mode_test: PASS\n";
    return 0;
}
