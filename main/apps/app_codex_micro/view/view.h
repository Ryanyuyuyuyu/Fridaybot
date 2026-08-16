/*
 * SPDX-FileCopyrightText: 2026 Codex Micro for StopWatch contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

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

struct DashboardModel {
    std::array<AgentVisual, 6> agents = {};
    std::string connectionText;
    uint32_t connectionColor = 0x7E8797;
    std::string batteryText;
    std::string quotaText;
    std::string resetText;
    bool quotaAvailable = false;
};

class DashboardView {
public:
    DashboardView() = default;
    ~DashboardView();

    DashboardView(const DashboardView&)            = delete;
    DashboardView& operator=(const DashboardView&) = delete;

    bool init(lv_obj_t* parent = lv_screen_active());
    bool popIntent(TouchIntent& intent);
    void update(const DashboardModel& model);

private:
    struct TouchBinding {
        DashboardView* owner = nullptr;
        int8_t agent         = -1;
        bool isSend          = false;
    };

    static void handleTouchEvent(lv_event_t* event);
    static void handleCircleHitTest(lv_event_t* event);
    static void handleGestureEvent(lv_event_t* event);
    void enqueueIntent(const TouchIntent& intent);

    lv_obj_t* _root                             = nullptr;
    lv_obj_t* _connection_label                 = nullptr;
    lv_obj_t* _battery_label                    = nullptr;
    lv_obj_t* _quota_button                     = nullptr;
    lv_obj_t* _quota_label                      = nullptr;
    lv_obj_t* _reset_label                      = nullptr;
    lv_obj_t* _send_label                       = nullptr;
    lv_obj_t* _home_hint_label                  = nullptr;
    std::array<lv_obj_t*, 6> _agent_buttons     = {};
    std::array<lv_obj_t*, 6> _agent_labels      = {};
    std::array<TouchBinding, 7> _touch_bindings = {};
    QueueHandle_t _intent_queue                 = nullptr;
};

}  // namespace codex_micro_app::view
