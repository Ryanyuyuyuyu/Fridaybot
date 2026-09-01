/*
 * SPDX-FileCopyrightText: 2026 Codex Micro for StopWatch contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <apps/app_codex_micro/model/chat_mode_model.h>
#include <array>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <lvgl.h>
#include <string>

namespace codex_micro_app::view {

enum class TouchIntentType : uint8_t {
    AgentPress,
    AgentRelease,
    SendPress,
    SendRelease,
    TouchCancel,
    AnalogPulse,
};

struct TouchIntent {
    TouchIntentType type = TouchIntentType::SendRelease;
    int8_t agent         = -1;
    float angle          = 0.0f;
};

struct AgentVisual {
    uint32_t color   = 0;
    float brightness = 0.0f;
    bool focused     = false;
};

struct ChatSlotVisual {
    std::string alias;
    std::string project;
    std::string title;
    bool available = false;
    bool pinned    = false;
    bool selected  = false;
};

struct DashboardModel {
    model::DashboardMode mode           = model::DashboardMode::Codex;
    std::array<AgentVisual, 6> agents   = {};
    std::array<ChatSlotVisual, 6> chats = {};
    std::string connectionText;
    uint32_t connectionColor = 0x7E8797;
    std::string batteryText;
    float fiveHourUsedPercent   = 0.0f;
    float weeklyUsedPercent     = 0.0f;
    bool fiveHourLimitAvailable = false;
    bool weeklyLimitAvailable   = false;
};

class DashboardView {
public:
    DashboardView() = default;
    ~DashboardView();

    DashboardView(const DashboardView&)            = delete;
    DashboardView& operator=(const DashboardView&) = delete;

    bool init(lv_obj_t* parent = lv_screen_active());
    bool popIntent(TouchIntent& intent);
    void beginModeTransition(bool touchActive);
    void update(const DashboardModel& model);

private:
    struct LimitIndicator {
        lv_obj_t* arc           = nullptr;
        lv_obj_t* highlight     = nullptr;
        lv_obj_t* head          = nullptr;
        lv_obj_t* nameLabel     = nullptr;
        lv_obj_t* valueLabel    = nullptr;
        lv_obj_t* percentLabel  = nullptr;
        int32_t displayedValue  = 0;
        int32_t targetValue     = -1;
        int32_t displayedWhole  = -1;
        int32_t headRadius      = 0;
        uint32_t color          = 0;
        uint32_t highlightColor = 0;
        bool available          = false;
        bool animating          = false;
    };

    struct TouchBinding {
        DashboardView* owner = nullptr;
        int8_t agent         = -1;
        bool isSend          = false;
    };

    static void applyLimitAnimationValue(void* context, int32_t value);
    static void handleLimitAnimationCompleted(lv_anim_t* animation);
    static void applySendPulseAnimationValue(void* context, int32_t value);
    static void handleTouchEvent(lv_event_t* event);
    static void handleCircleHitTest(lv_event_t* event);
    static void handleGestureEvent(lv_event_t* event);
    void updateLimitIndicator(LimitIndicator& indicator, float usedPercent, bool available, uint32_t durationMs,
                              uint32_t delayMs);
    void setLimitIndicatorVisible(LimitIndicator& indicator, bool visible);
    void setCodexInstrumentVisible(bool visible);
    void playSendPulse();
    void restoreSendPulseVisuals();
    void enqueueIntent(const TouchIntent& intent);

    lv_obj_t* _root             = nullptr;
    lv_obj_t* _connection_label = nullptr;
    lv_obj_t* _battery_label    = nullptr;
    lv_obj_t* _quota_button     = nullptr;
    lv_obj_t* _quota_label      = nullptr;
    lv_obj_t* _reset_label      = nullptr;
    lv_obj_t* _center_surface   = nullptr;
    lv_obj_t* _metric_divider   = nullptr;
    lv_obj_t* _send_ripple      = nullptr;
    lv_obj_t* _send_left_line   = nullptr;
    lv_obj_t* _send_right_line  = nullptr;
    lv_obj_t* _send_left_dot    = nullptr;
    lv_obj_t* _send_right_dot   = nullptr;
    lv_obj_t* _send_mark        = nullptr;
    lv_obj_t* _send_label       = nullptr;
    lv_obj_t* _home_hint_label  = nullptr;
    LimitIndicator _weekly_limit;
    LimitIndicator _five_hour_limit;
    std::array<lv_obj_t*, 6> _agent_buttons     = {};
    std::array<lv_obj_t*, 6> _agent_labels      = {};
    std::array<TouchBinding, 7> _touch_bindings = {};
    QueueHandle_t _intent_queue                 = nullptr;
    bool _discard_touch_until_release           = false;
    bool _codex_mode_active                     = false;
};

}  // namespace codex_micro_app::view
