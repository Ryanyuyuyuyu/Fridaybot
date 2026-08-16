/*
 * SPDX-FileCopyrightText: 2026 Codex Micro for StopWatch contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "view.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <src/core/lv_obj_event_private.h>

namespace codex_micro_app::view {
namespace {

constexpr uint32_t kBackgroundColor = 0x000000;
constexpr uint32_t kPanelColor      = 0x111820;
constexpr uint32_t kPanelPressed    = 0x1D2935;
constexpr uint32_t kPrimaryText     = 0xE8EEF5;
constexpr uint32_t kSecondaryText   = 0x7D8A98;
constexpr uint32_t kTrackColor      = 0x26323D;
constexpr uint32_t kAccentColor     = 0x12D6B2;
constexpr size_t kIntentQueueDepth  = 24;

enum class AgentStatus : uint8_t {
    Unassigned,
    Idle,
    Thinking,
    Complete,
    RequiresInput,
    Error,
    Active,
};

struct Point {
    int16_t x;
    int16_t y;
};

constexpr std::array<Point, 6> kAgentCenters = {
    Point{233, 72}, Point{375, 154}, Point{375, 318}, Point{233, 400}, Point{91, 318}, Point{91, 154},
};

uint8_t colorChannel(uint32_t color, int shift)
{
    return static_cast<uint8_t>((color >> shift) & 0xFF);
}

uint32_t scaleColor(uint32_t color, float brightness)
{
    brightness               = std::clamp(brightness, 0.0f, 1.0f);
    const auto scale_channel = [&](int shift) {
        return static_cast<uint32_t>(std::lround(static_cast<float>(colorChannel(color, shift)) * brightness));
    };

    return (scale_channel(16) << 16) | (scale_channel(8) << 8) | scale_channel(0);
}

AgentStatus classifyAgent(const AgentVisual& agent)
{
    if (agent.brightness <= 0.01f) {
        return AgentStatus::Unassigned;
    }

    const int red     = static_cast<int>((agent.color >> 16) & 0xFF);
    const int green   = static_cast<int>((agent.color >> 8) & 0xFF);
    const int blue    = static_cast<int>(agent.color & 0xFF);
    const int maximum = std::max(red, std::max(green, blue));
    const int minimum = std::min(red, std::min(green, blue));

    if (maximum - minimum < 48 && maximum > 150) {
        return AgentStatus::Idle;
    }
    if (green > red * 1.12f && green > blue * 1.15f) {
        return AgentStatus::Complete;
    }
    if (blue > red * 1.12f && blue > green * 1.05f) {
        return AgentStatus::Thinking;
    }
    if (red > 150 && green > 70 && green > blue * 1.45f) {
        return AgentStatus::RequiresInput;
    }
    if (red > green * 1.20f && red > blue * 1.20f) {
        return AgentStatus::Error;
    }
    return AgentStatus::Active;
}

uint32_t statusFillColor(AgentStatus status, const AgentVisual& agent)
{
    switch (status) {
        case AgentStatus::Idle:
            return 0xB7C2CD;
        case AgentStatus::Thinking:
            return 0x4292F5;
        case AgentStatus::Complete:
            return 0x2BC96E;
        case AgentStatus::RequiresInput:
            return 0xF7AC42;
        case AgentStatus::Error:
            return 0xF55A68;
        default:
            return agent.color & 0xFFFFFF;
    }
}

uint32_t agentLabelColor(uint32_t fill)
{
    const int red       = static_cast<int>((fill >> 16) & 0xFF);
    const int green     = static_cast<int>((fill >> 8) & 0xFF);
    const int blue      = static_cast<int>(fill & 0xFF);
    const int luminance = (red * 77 + green * 150 + blue * 29) >> 8;
    return luminance > 150 ? 0x0D1218 : kPrimaryText;
}

void makeTransparentLabel(lv_obj_t* label, const lv_font_t* font, uint32_t color)
{
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);
}

}  // namespace

DashboardView::~DashboardView()
{
    if (_root != nullptr) {
        lv_obj_delete(_root);
        _root = nullptr;
    }
    if (_intent_queue != nullptr) {
        vQueueDelete(_intent_queue);
        _intent_queue = nullptr;
    }
}

bool DashboardView::init(lv_obj_t* parent)
{
    if (parent == nullptr) {
        return false;
    }

    _intent_queue = xQueueCreate(kIntentQueueDepth, sizeof(TouchIntent));
    if (_intent_queue == nullptr) {
        return false;
    }

    _root = lv_obj_create(parent);
    lv_obj_remove_style_all(_root);
    lv_obj_set_size(_root, 466, 466);
    lv_obj_align(_root, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_root, lv_color_hex(kBackgroundColor), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);
    // Child controls bubble gestures to this dashboard, not through it to the
    // active screen left behind by the launcher lifecycle.
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_root, handleGestureEvent, LV_EVENT_GESTURE, this);

    // Create the central instrument first so the six restored 112 px Agent
    // keys remain the top-most touch targets where their edges nearly meet.
    _quota_button = lv_button_create(_root);
    lv_obj_set_size(_quota_button, 208, 208);
    lv_obj_align(_quota_button, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_radius(_quota_button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_quota_button, lv_color_hex(kBackgroundColor), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_quota_button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(_quota_button, 10, LV_PART_MAIN);
    lv_obj_set_style_border_color(_quota_button, lv_color_hex(kTrackColor), LV_PART_MAIN);
    lv_obj_set_style_transform_scale(_quota_button, 249, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(_quota_button, lv_color_hex(kPanelPressed), LV_STATE_PRESSED);
    lv_obj_clear_flag(_quota_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_quota_button, LV_OBJ_FLAG_GESTURE_BUBBLE);

    _touch_bindings[6] = TouchBinding{this, -1, true};
    lv_obj_add_event_cb(_quota_button, handleTouchEvent, LV_EVENT_ALL, &_touch_bindings[6]);
    lv_obj_add_flag(_quota_button, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_event_cb(_quota_button, handleCircleHitTest, LV_EVENT_HIT_TEST, &_touch_bindings[6]);

    _quota_label = lv_label_create(_quota_button);
    makeTransparentLabel(_quota_label, &lv_font_montserrat_36, kPrimaryText);
    lv_label_set_text(_quota_label, "--");
    lv_obj_align(_quota_label, LV_ALIGN_CENTER, 0, -29);

    _reset_label = lv_label_create(_quota_button);
    makeTransparentLabel(_reset_label, &lv_font_montserrat_16, kSecondaryText);
    lv_label_set_text(_reset_label, "QUOTA WAITING");
    lv_obj_align(_reset_label, LV_ALIGN_CENTER, 0, 9);

    _send_label = lv_label_create(_quota_button);
    makeTransparentLabel(_send_label, &lv_font_montserrat_18, kAccentColor);
    lv_label_set_text(_send_label, "TAP TO SEND");
    lv_obj_align(_send_label, LV_ALIGN_CENTER, 0, 49);

    _home_hint_label = lv_label_create(_quota_button);
    makeTransparentLabel(_home_hint_label, &lv_font_montserrat_10, 0x626C7C);
    lv_label_set_text(_home_hint_label, "HOLD A+B FOR HOME");
    lv_obj_align(_home_hint_label, LV_ALIGN_CENTER, 0, 78);

    for (size_t i = 0; i < _agent_buttons.size(); ++i) {
        auto* button      = lv_button_create(_root);
        _agent_buttons[i] = button;
        lv_obj_set_size(button, 112, 112);
        lv_obj_set_pos(button, kAgentCenters[i].x - 56, kAgentCenters[i].y - 56);
        lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(button, lv_color_hex(kPanelColor), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(button, 4, LV_PART_MAIN);
        lv_obj_set_style_border_color(button, lv_color_hex(kTrackColor), LV_PART_MAIN);
        lv_obj_set_style_outline_width(button, 4, LV_PART_MAIN);
        lv_obj_set_style_outline_pad(button, 0, LV_PART_MAIN);
        lv_obj_set_style_outline_opa(button, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(button, 8, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(button, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_transform_scale(button, 250, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(button, 184, LV_STATE_PRESSED);
        lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_flag(button, LV_OBJ_FLAG_ADV_HITTEST);
        lv_obj_set_ext_click_area(button, 4);

        _touch_bindings[i] = TouchBinding{this, static_cast<int8_t>(i), false};
        lv_obj_add_event_cb(button, handleTouchEvent, LV_EVENT_ALL, &_touch_bindings[i]);
        lv_obj_add_event_cb(button, handleCircleHitTest, LV_EVENT_HIT_TEST, &_touch_bindings[i]);

        auto* label      = lv_label_create(button);
        _agent_labels[i] = label;
        makeTransparentLabel(label, &lv_font_montserrat_22, kPrimaryText);
        char agent_name[4] = {};
        std::snprintf(agent_name, sizeof(agent_name), "A%u", static_cast<unsigned>(i + 1));
        lv_label_set_text(label, agent_name);
        lv_obj_center(label);
    }

    _connection_label = lv_label_create(_root);
    makeTransparentLabel(_connection_label, &lv_font_montserrat_16, kSecondaryText);
    lv_label_set_text(_connection_label, "OFFLINE");
    lv_obj_align(_connection_label, LV_ALIGN_TOP_LEFT, 18, 12);

    _battery_label = lv_label_create(_root);
    makeTransparentLabel(_battery_label, &lv_font_montserrat_16, kSecondaryText);
    lv_label_set_text(_battery_label, "BAT --%");
    lv_obj_align(_battery_label, LV_ALIGN_TOP_RIGHT, -18, 12);

    return true;
}

bool DashboardView::popIntent(TouchIntent& intent)
{
    return _intent_queue != nullptr && xQueueReceive(_intent_queue, &intent, 0) == pdTRUE;
}

void DashboardView::update(const DashboardModel& model)
{
    if (_root == nullptr) {
        return;
    }

    lv_label_set_text(_connection_label, model.connectionText.c_str());
    lv_obj_set_style_text_color(_connection_label, lv_color_hex(model.connectionColor), LV_PART_MAIN);
    lv_label_set_text(_battery_label, model.batteryText.c_str());
    lv_label_set_text(_quota_label, model.quotaText.c_str());
    lv_label_set_text(_reset_label, model.resetText.c_str());
    lv_obj_set_style_border_color(_quota_button, lv_color_hex(model.quotaAvailable ? kAccentColor : kTrackColor),
                                  LV_PART_MAIN);

    for (size_t i = 0; i < _agent_buttons.size(); ++i) {
        const auto& agent        = model.agents[i];
        const AgentStatus status = classifyAgent(agent);
        const float brightness   = std::clamp(agent.brightness, 0.0f, 1.0f);
        const bool assigned      = status != AgentStatus::Unassigned;
        const uint32_t base      = assigned ? statusFillColor(status, agent) : kPanelColor;
        const uint32_t fill      = assigned ? scaleColor(base, brightness) : kPanelColor;
        const uint32_t edge      = assigned ? scaleColor(base, brightness * 0.55f) : kTrackColor;
        const bool emphasized    = status == AgentStatus::Thinking || status == AgentStatus::Complete ||
                                status == AgentStatus::RequiresInput || status == AgentStatus::Error;

        lv_obj_set_style_bg_color(_agent_buttons[i], lv_color_hex(fill), LV_PART_MAIN);
        lv_obj_set_style_border_color(_agent_buttons[i], lv_color_hex(edge), LV_PART_MAIN);
        lv_obj_set_style_outline_color(_agent_buttons[i], lv_color_hex(kPrimaryText), LV_PART_MAIN);
        lv_obj_set_style_outline_opa(_agent_buttons[i], agent.focused ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(_agent_buttons[i], lv_color_hex(base), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(
            _agent_buttons[i],
            emphasized ? static_cast<lv_opa_t>(std::lround(55.0f * brightness)) : static_cast<lv_opa_t>(LV_OPA_TRANSP),
            LV_PART_MAIN);
        lv_obj_set_style_text_color(_agent_labels[i], lv_color_hex(assigned ? agentLabelColor(fill) : kSecondaryText),
                                    LV_PART_MAIN);
    }
}

void DashboardView::handleTouchEvent(lv_event_t* event)
{
    const auto code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) {
        return;
    }

    auto* binding = static_cast<TouchBinding*>(lv_event_get_user_data(event));
    if (binding == nullptr || binding->owner == nullptr) {
        return;
    }

    TouchIntent intent;
    if (code == LV_EVENT_PRESS_LOST) {
        intent.type  = TouchIntentType::TouchCancel;
        intent.agent = binding->agent;
        binding->owner->enqueueIntent(intent);
        return;
    }

    const bool pressed = code == LV_EVENT_PRESSED;
    if (binding->isSend) {
        intent.type = pressed ? TouchIntentType::SendPress : TouchIntentType::SendRelease;
    } else {
        intent.type  = pressed ? TouchIntentType::AgentPress : TouchIntentType::AgentRelease;
        intent.agent = binding->agent;
    }
    binding->owner->enqueueIntent(intent);
}

void DashboardView::handleCircleHitTest(lv_event_t* event)
{
    auto* binding = static_cast<TouchBinding*>(lv_event_get_user_data(event));
    auto* info    = lv_event_get_hit_test_info(event);
    auto* target  = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    if (binding == nullptr || info == nullptr || info->point == nullptr || target == nullptr) {
        return;
    }

    lv_area_t coordinates;
    lv_obj_get_coords(target, &coordinates);
    const int32_t center_x = (coordinates.x1 + coordinates.x2 + 1) / 2;
    const int32_t center_y = (coordinates.y1 + coordinates.y2 + 1) / 2;
    const int32_t delta_x  = info->point->x - center_x;
    const int32_t delta_y  = info->point->y - center_y;
    const int32_t radius   = binding->isSend ? 104 : 60;
    info->res              = delta_x * delta_x + delta_y * delta_y <= radius * radius;
}

void DashboardView::handleGestureEvent(lv_event_t* event)
{
    auto* owner = static_cast<DashboardView*>(lv_event_get_user_data(event));
    auto* indev = lv_indev_active();
    if (owner == nullptr || indev == nullptr) {
        return;
    }

    TouchIntent intent;
    intent.type = TouchIntentType::AnalogPulse;
    switch (lv_indev_get_gesture_dir(indev)) {
        case LV_DIR_RIGHT:
            intent.angle = 0.0f;
            break;
        case LV_DIR_BOTTOM:
            intent.angle = 0.25f;
            break;
        case LV_DIR_LEFT:
            intent.angle = 0.5f;
            break;
        case LV_DIR_TOP:
            intent.angle = 0.75f;
            break;
        default:
            return;
    }
    owner->enqueueIntent(intent);
}

void DashboardView::enqueueIntent(const TouchIntent& intent)
{
    if (_intent_queue == nullptr) {
        return;
    }

    if (xQueueSend(_intent_queue, &intent, 0) != pdTRUE) {
        // A lost release could otherwise leave a host control held. Clear the
        // stale queue and send a release intent that the App interprets as a
        // request to release every locally tracked control.
        xQueueReset(_intent_queue);
        TouchIntent recovery;
        recovery.type  = TouchIntentType::AgentRelease;
        recovery.agent = -1;
        xQueueSend(_intent_queue, &recovery, 0);
    }
}

}  // namespace codex_micro_app::view
