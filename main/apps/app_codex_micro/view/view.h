/*
 * SPDX-FileCopyrightText: 2026 Codex Micro for StopWatch contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <lvgl.h>
#include <string>
#include <type_traits>
#include <services/codex_micro/host_selection.h>

namespace codex_micro_app::view {

enum class TouchIntentType : uint8_t {
    AgentPress,
    AgentRelease,
    SendPress,
    SendRelease,
    TouchCancel,
    AnalogPulse,
    DeviceMenuOpened,
    DeviceMenuClosed,
    SelectHost,
};

struct TouchIntent {
    TouchIntentType type = TouchIntentType::SendRelease;
    int8_t agent         = -1;
    float angle          = 0.0f;
    char hostId[97]       = {};
};
static_assert(std::is_trivially_copyable<TouchIntent>::value, "FreeRTOS intents must be copied as plain bytes");

struct AgentVisual {
    uint32_t color   = 0;
    float brightness = 0.0f;
    bool focused     = false;
};

struct DashboardModel {
    std::array<AgentVisual, 6> agents = {};
    std::string connectionText;
    std::string transportText = "Offline";
    uint32_t connectionColor = 0x7E8797;
    std::string batteryText;
    std::vector<codex_micro::HostInfo> hosts;
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
    void update(const DashboardModel& model);
    bool deviceMenuOpen() const { return _device_menu_open.load(); }
    // Call under the LVGL lock. Forget all old-host input and wait for the
    // finger to lift so a release/gesture cannot cross a connection change.
    void cancelTouch();
    void closeDeviceMenu();
    void showSelectionError();

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

    struct HostBinding {
        DashboardView* owner = nullptr;
        std::string hostId;
    };

    static void applyLimitAnimationValue(void* context, int32_t value);
    static void handleLimitAnimationCompleted(lv_anim_t* animation);
    static void applySendPulseAnimationValue(void* context, int32_t value);
    static void handleTouchEvent(lv_event_t* event);
    static void handleCircleHitTest(lv_event_t* event);
    static void handleGestureEvent(lv_event_t* event);
    static void handleDeviceMenuEvent(lv_event_t* event);
    static void handleHostEvent(lv_event_t* event);
    void openDeviceMenu();
    void rebuildDeviceRows();
    void updateLimitIndicator(LimitIndicator& indicator, float usedPercent, bool available, uint32_t durationMs,
                              uint32_t delayMs);
    void playSendPulse();
    void restoreSendPulseVisuals();
    void enqueueIntent(const TouchIntent& intent);

    lv_obj_t* _root             = nullptr;
    lv_obj_t* _connection_label = nullptr;
    lv_obj_t* _transport_label = nullptr;
    lv_obj_t* _connection_button = nullptr;
    lv_obj_t* _connection_dot = nullptr;
    lv_obj_t* _device_overlay = nullptr;
    lv_obj_t* _device_list = nullptr;
    lv_obj_t* _device_menu_message = nullptr;
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
    std::array<HostBinding, codex_micro::HostSelection::kMaxHosts> _host_bindings = {};
    std::vector<codex_micro::HostInfo> _hosts;
    std::atomic<bool> _device_menu_open{false};
    QueueHandle_t _intent_queue                 = nullptr;
};

}  // namespace codex_micro_app::view
