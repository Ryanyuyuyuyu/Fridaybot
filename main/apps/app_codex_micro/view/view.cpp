/*
 * SPDX-FileCopyrightText: 2026 Codex Micro for StopWatch contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "view.h"

#include <algorithm>
#include <array>
#include <assets/assets.h>
#include <cmath>
#include <cstdio>
#include <src/core/lv_obj_event_private.h>

namespace codex_micro_app::view {
namespace {

constexpr uint32_t kBackgroundColor   = 0x000000;
constexpr uint32_t kPanelColor        = 0x111820;
constexpr uint32_t kPanelPressed      = 0x1D2935;
constexpr uint32_t kPrimaryText       = 0xE8EEF5;
constexpr uint32_t kSecondaryText     = 0x7D8A98;
constexpr uint32_t kTrackColor        = 0x26323D;
constexpr uint32_t kCenterSurface     = 0x070B0E;
constexpr uint32_t kWeeklyTrackColor  = 0x202B33;
constexpr uint32_t kFiveHourTrack     = 0x182229;
constexpr uint32_t kWeeklyLimitColor  = 0xAAB7C0;
constexpr uint32_t kFiveHourColor     = 0x65C7A9;
constexpr uint32_t kWeeklyHighlight   = 0xE0E7EB;
constexpr uint32_t kFiveHourHighlight = 0x9DE1CC;
constexpr uint32_t kSendTextColor     = 0xEDF2F4;
constexpr uint32_t kMetricDivider     = 0x34414A;
constexpr uint32_t kReticleColor      = 0x4C5B65;
constexpr uint32_t kChatAccentColor   = 0xA78BFA;
constexpr uint32_t kChatPanelColor    = 0x211B35;
constexpr uint32_t kChatMutedText     = 0xB9A9E8;
constexpr size_t kIntentQueueDepth    = 24;
constexpr int32_t kLimitArcWidth      = 9;
constexpr int32_t kLimitValueScale    = 10;
constexpr int32_t kLimitValueMaximum  = 100 * kLimitValueScale;
constexpr float kPi                   = 3.14159265358979323846f;

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

uint32_t blendColor(uint32_t from, uint32_t to, float amount)
{
    amount                   = std::clamp(amount, 0.0f, 1.0f);
    const auto blend_channel = [&](int shift) {
        const float start = static_cast<float>(colorChannel(from, shift));
        const float end   = static_cast<float>(colorChannel(to, shift));
        return static_cast<uint32_t>(std::lround(start + (end - start) * amount));
    };

    return (blend_channel(16) << 16) | (blend_channel(8) << 8) | blend_channel(0);
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
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t* makeLimitArc(lv_obj_t* parent, int32_t size, uint32_t trackColor, uint32_t color)
{
    auto* arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    lv_obj_center(arc);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, kLimitValueMaximum);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, kLimitArcWidth, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(trackColor), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, kLimitArcWidth, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
    return arc;
}

lv_obj_t* makeHighlightArc(lv_obj_t* parent, int32_t size, uint32_t color)
{
    auto* arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    lv_obj_center(arc);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_angles(arc, 0, 0);
    lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
    return arc;
}

lv_obj_t* makeDecorativeObject(lv_obj_t* parent, int32_t width, int32_t height, uint32_t color,
                               lv_opa_t opacity = LV_OPA_COVER)
{
    auto* object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(object, opacity, LV_PART_MAIN);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

void setVisualVisible(lv_obj_t* object, bool visible)
{
    if (object == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace

void DashboardView::applyLimitAnimationValue(void* context, int32_t value)
{
    auto* indicator = static_cast<LimitIndicator*>(context);
    if (indicator == nullptr || indicator->arc == nullptr) {
        return;
    }

    value                     = std::clamp<int32_t>(value, 0, kLimitValueMaximum);
    indicator->displayedValue = value;
    lv_arc_set_value(indicator->arc, value);

    if (!indicator->available) {
        lv_obj_set_style_arc_opa(indicator->arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
        setVisualVisible(indicator->highlight, false);
        setVisualVisible(indicator->head, false);
        setVisualVisible(indicator->percentLabel, false);
        if (indicator->valueLabel != nullptr) {
            lv_label_set_text(indicator->valueLabel, "--");
        }
        indicator->displayedWhole = -1;
        return;
    }

    const int32_t wholePercent = (value + (kLimitValueScale / 2)) / kLimitValueScale;
    if (indicator->valueLabel != nullptr && wholePercent != indicator->displayedWhole) {
        char text[4] = {};
        std::snprintf(text, sizeof(text), "%ld", static_cast<long>(wholePercent));
        lv_label_set_text(indicator->valueLabel, text);
        indicator->displayedWhole = wholePercent;
    }
    setVisualVisible(indicator->percentLabel, true);

    const bool hasProgress = value > 0;
    lv_obj_set_style_arc_opa(indicator->arc,
                             hasProgress ? static_cast<lv_opa_t>(LV_OPA_COVER) : static_cast<lv_opa_t>(LV_OPA_TRANSP),
                             LV_PART_INDICATOR);
    setVisualVisible(indicator->highlight, hasProgress);
    setVisualVisible(indicator->head, hasProgress);
    if (!hasProgress) {
        return;
    }

    const int32_t endAngle   = (value * 360 + (kLimitValueMaximum / 2)) / kLimitValueMaximum;
    const int32_t startAngle = std::max<int32_t>(0, endAngle - 5);
    lv_arc_set_angles(indicator->highlight, startAngle, endAngle);
    lv_obj_set_style_arc_opa(indicator->highlight, LV_OPA_COVER, LV_PART_INDICATOR);

    const float radians = (static_cast<float>(endAngle) - 90.0f) * kPi / 180.0f;
    const int32_t x =
        104 + static_cast<int32_t>(std::lround(static_cast<float>(indicator->headRadius) * std::cos(radians)));
    const int32_t y =
        104 + static_cast<int32_t>(std::lround(static_cast<float>(indicator->headRadius) * std::sin(radians)));
    lv_obj_set_pos(indicator->head, x - 3, y - 3);
}

void DashboardView::handleLimitAnimationCompleted(lv_anim_t* animation)
{
    if (animation == nullptr) {
        return;
    }
    auto* indicator = static_cast<LimitIndicator*>(animation->var);
    if (indicator == nullptr) {
        return;
    }
    indicator->animating = false;
    applyLimitAnimationValue(indicator, indicator->targetValue);
}

void DashboardView::applySendPulseAnimationValue(void* context, int32_t value)
{
    auto* view = static_cast<DashboardView*>(context);
    if (view == nullptr || !view->_codex_mode_active || view->_send_label == nullptr || view->_send_ripple == nullptr) {
        return;
    }

    const float progress = std::clamp(static_cast<float>(value) / 1000.0f, 0.0f, 1.0f);
    const float energy   = std::sin(progress * kPi);
    lv_obj_set_style_text_letter_space(view->_send_label, 4 + static_cast<int32_t>(std::lround(2.0f * energy)),
                                       LV_PART_MAIN);

    lv_obj_set_style_transform_scale(view->_send_ripple, 225 + static_cast<int32_t>(std::lround(241.0f * progress)),
                                     LV_PART_MAIN);
    lv_obj_set_style_border_opa(view->_send_ripple, static_cast<lv_opa_t>(std::lround(155.0f * energy)), LV_PART_MAIN);

    const float blend = 0.38f * energy;
    lv_obj_set_style_arc_color(
        view->_weekly_limit.arc,
        lv_color_hex(blendColor(view->_weekly_limit.color, view->_weekly_limit.highlightColor, blend)),
        LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(
        view->_five_hour_limit.arc,
        lv_color_hex(blendColor(view->_five_hour_limit.color, view->_five_hour_limit.highlightColor, blend)),
        LV_PART_INDICATOR);

    if (value >= 1000) {
        view->restoreSendPulseVisuals();
    }
}

void DashboardView::updateLimitIndicator(LimitIndicator& indicator, float usedPercent, bool available,
                                         uint32_t durationMs, uint32_t delayMs)
{
    if (indicator.arc == nullptr) {
        return;
    }

    lv_obj_set_style_arc_opa(indicator.arc, available ? LV_OPA_COVER : LV_OPA_30, LV_PART_MAIN);
    if (!available) {
        if (indicator.animating) {
            lv_anim_delete(&indicator, applyLimitAnimationValue);
        }
        indicator.available   = false;
        indicator.animating   = false;
        indicator.targetValue = -1;
        applyLimitAnimationValue(&indicator, 0);
        return;
    }

    const bool wasAvailable = indicator.available;
    indicator.available     = true;
    const int32_t target =
        static_cast<int32_t>(std::lround(std::clamp(usedPercent, 0.0f, 100.0f) * static_cast<float>(kLimitValueScale)));
    if (target == indicator.targetValue && (indicator.animating || indicator.displayedValue == target)) {
        return;
    }

    if (indicator.animating) {
        lv_anim_delete(&indicator, applyLimitAnimationValue);
        indicator.animating = false;
    }
    if (!wasAvailable) {
        indicator.displayedValue = 0;
        indicator.displayedWhole = -1;
    }
    indicator.targetValue = target;
    setVisualVisible(indicator.percentLabel, true);

    if (indicator.displayedValue == target) {
        applyLimitAnimationValue(&indicator, target);
        return;
    }

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, &indicator);
    lv_anim_set_exec_cb(&animation, applyLimitAnimationValue);
    lv_anim_set_values(&animation, indicator.displayedValue, target);
    lv_anim_set_duration(&animation, durationMs);
    lv_anim_set_delay(&animation, delayMs);
    lv_anim_set_early_apply(&animation, delayMs == 0);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&animation, handleLimitAnimationCompleted);
    indicator.animating = true;
    lv_anim_start(&animation);
}

void DashboardView::setLimitIndicatorVisible(LimitIndicator& indicator, bool visible)
{
    setVisualVisible(indicator.arc, visible);
    setVisualVisible(indicator.nameLabel, visible);
    setVisualVisible(indicator.valueLabel, visible);
    setVisualVisible(indicator.percentLabel, visible && indicator.available);
    setVisualVisible(indicator.highlight, visible && indicator.available && indicator.displayedValue > 0);
    setVisualVisible(indicator.head, visible && indicator.available && indicator.displayedValue > 0);
}

void DashboardView::setCodexInstrumentVisible(bool visible)
{
    setVisualVisible(_center_surface, visible);
    setVisualVisible(_metric_divider, visible);
    setVisualVisible(_send_ripple, visible);
    setVisualVisible(_send_left_line, visible);
    setVisualVisible(_send_right_line, visible);
    setVisualVisible(_send_left_dot, visible);
    setVisualVisible(_send_right_dot, visible);
    setVisualVisible(_send_mark, visible);
    setLimitIndicatorVisible(_weekly_limit, visible);
    setLimitIndicatorVisible(_five_hour_limit, visible);
}

void DashboardView::playSendPulse()
{
    if (!_codex_mode_active || _send_ripple == nullptr) {
        return;
    }

    lv_anim_delete(this, applySendPulseAnimationValue);
    restoreSendPulseVisuals();
    setVisualVisible(_send_ripple, true);

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, this);
    lv_anim_set_exec_cb(&animation, applySendPulseAnimationValue);
    lv_anim_set_values(&animation, 0, 1000);
    lv_anim_set_duration(&animation, 620);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
}

void DashboardView::restoreSendPulseVisuals()
{
    if (_send_label != nullptr) {
        lv_obj_set_style_text_letter_space(_send_label, _codex_mode_active ? 4 : 0, LV_PART_MAIN);
    }
    if (_send_ripple != nullptr) {
        lv_obj_set_style_transform_scale(_send_ripple, 256, LV_PART_MAIN);
        lv_obj_set_style_border_opa(_send_ripple, LV_OPA_TRANSP, LV_PART_MAIN);
    }
    if (_weekly_limit.arc != nullptr) {
        lv_obj_set_style_arc_color(_weekly_limit.arc, lv_color_hex(_weekly_limit.color), LV_PART_INDICATOR);
    }
    if (_five_hour_limit.arc != nullptr) {
        lv_obj_set_style_arc_color(_five_hour_limit.arc, lv_color_hex(_five_hour_limit.color), LV_PART_INDICATOR);
    }
}

DashboardView::~DashboardView()
{
    lv_anim_delete(&_weekly_limit, applyLimitAnimationValue);
    lv_anim_delete(&_five_hour_limit, applyLimitAnimationValue);
    lv_anim_delete(this, applySendPulseAnimationValue);
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
    lv_obj_set_style_border_width(_quota_button, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(_quota_button, lv_color_hex(kTrackColor), LV_PART_MAIN);
    lv_obj_set_style_transform_scale(_quota_button, 249, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(_quota_button, lv_color_hex(kPanelPressed), LV_STATE_PRESSED);
    lv_obj_clear_flag(_quota_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_quota_button, LV_OBJ_FLAG_GESTURE_BUBBLE);

    _touch_bindings[6] = TouchBinding{this, -1, true};
    lv_obj_add_event_cb(_quota_button, handleTouchEvent, LV_EVENT_ALL, &_touch_bindings[6]);
    lv_obj_add_flag(_quota_button, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_event_cb(_quota_button, handleCircleHitTest, LV_EVENT_HIT_TEST, &_touch_bindings[6]);

    // The central SEND control remains one 208 px touch target. Its two visual
    // lanes touch with no black seam: Weekly is outside, Five Hour is inside.
    _center_surface = makeDecorativeObject(_quota_button, 175, 175, kCenterSurface);
    lv_obj_set_style_radius(_center_surface, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_center(_center_surface);

    _send_ripple = makeDecorativeObject(_quota_button, 80, 80, kFiveHourHighlight, LV_OPA_TRANSP);
    lv_obj_set_style_radius(_send_ripple, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(_send_ripple, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(_send_ripple, lv_color_hex(kFiveHourHighlight), LV_PART_MAIN);
    lv_obj_set_style_border_opa(_send_ripple, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_center(_send_ripple);

    _weekly_limit.color          = kWeeklyLimitColor;
    _weekly_limit.highlightColor = kWeeklyHighlight;
    _weekly_limit.headRadius     = 96;
    _weekly_limit.arc            = makeLimitArc(_quota_button, 200, kWeeklyTrackColor, _weekly_limit.color);

    _five_hour_limit.color          = kFiveHourColor;
    _five_hour_limit.highlightColor = kFiveHourHighlight;
    _five_hour_limit.headRadius     = 87;
    _five_hour_limit.arc            = makeLimitArc(_quota_button, 182, kFiveHourTrack, _five_hour_limit.color);

    _weekly_limit.highlight    = makeHighlightArc(_quota_button, 200, _weekly_limit.highlightColor);
    _five_hour_limit.highlight = makeHighlightArc(_quota_button, 182, _five_hour_limit.highlightColor);

    const auto make_head = [&](LimitIndicator& indicator) {
        indicator.head = makeDecorativeObject(_quota_button, 6, 6, indicator.highlightColor);
        lv_obj_set_style_radius(indicator.head, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(indicator.head, 6, LV_PART_MAIN);
        lv_obj_set_style_shadow_spread(indicator.head, 1, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(indicator.head, lv_color_hex(indicator.highlightColor), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(indicator.head, 42, LV_PART_MAIN);
        setVisualVisible(indicator.head, false);
    };
    make_head(_weekly_limit);
    make_head(_five_hour_limit);

    _quota_label = lv_label_create(_quota_button);
    makeTransparentLabel(_quota_label, &lv_font_montserrat_36, kPrimaryText);
    lv_obj_set_size(_quota_label, 166, lv_font_get_line_height(&lv_font_montserrat_36));
    lv_label_set_long_mode(_quota_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(_quota_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(_quota_label, "--");
    lv_obj_align(_quota_label, LV_ALIGN_CENTER, 0, -29);

    _reset_label = lv_label_create(_quota_button);
    makeTransparentLabel(_reset_label, &lv_font_montserrat_16, kSecondaryText);
    lv_obj_set_size(_reset_label, 174, lv_font_get_line_height(&lv_font_montserrat_16));
    lv_label_set_long_mode(_reset_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(_reset_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(_reset_label, "QUOTA WAITING");
    lv_obj_align(_reset_label, LV_ALIGN_CENTER, 0, 9);

    const auto make_readout = [&](LimitIndicator& indicator, const char* name, int32_t centerX) {
        indicator.nameLabel = lv_label_create(_quota_button);
        makeTransparentLabel(indicator.nameLabel, &lv_font_montserrat_10, indicator.color);
        lv_obj_set_size(indicator.nameLabel, 34, lv_font_get_line_height(&lv_font_montserrat_10));
        lv_obj_set_style_text_align(indicator.nameLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(indicator.nameLabel, 1, LV_PART_MAIN);
        lv_label_set_text(indicator.nameLabel, name);
        lv_obj_set_pos(indicator.nameLabel, centerX - 17, 70);

        indicator.valueLabel = lv_label_create(_quota_button);
        makeTransparentLabel(indicator.valueLabel, &MontserratSemiBold26, kSendTextColor);
        lv_obj_set_size(indicator.valueLabel, 44, lv_font_get_line_height(&MontserratSemiBold26));
        lv_obj_set_style_text_align(indicator.valueLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(indicator.valueLabel, -1, LV_PART_MAIN);
        lv_label_set_text(indicator.valueLabel, "--");
        lv_obj_set_pos(indicator.valueLabel, centerX - 25, 86);

        indicator.percentLabel = lv_label_create(_quota_button);
        makeTransparentLabel(indicator.percentLabel, &lv_font_montserrat_10, indicator.color);
        lv_label_set_text(indicator.percentLabel, "%");
        lv_obj_set_pos(indicator.percentLabel, centerX + 16, 84);
        setVisualVisible(indicator.percentLabel, false);
    };
    make_readout(_five_hour_limit, "5H", 72);
    make_readout(_weekly_limit, "WK", 134);

    _metric_divider = makeDecorativeObject(_quota_button, 1, 36, kMetricDivider);
    lv_obj_set_pos(_metric_divider, 103, 75);

    _send_label = lv_label_create(_quota_button);
    makeTransparentLabel(_send_label, &lv_font_montserrat_16, kSendTextColor);
    lv_obj_set_style_text_letter_space(_send_label, 4, LV_PART_MAIN);
    lv_label_set_text(_send_label, "SEND");
    lv_obj_align(_send_label, LV_ALIGN_CENTER, 1, 29);

    _send_left_line = makeDecorativeObject(_quota_button, 17, 1, kReticleColor);
    lv_obj_set_pos(_send_left_line, 48, 128);
    _send_right_line = makeDecorativeObject(_quota_button, 17, 1, kReticleColor);
    lv_obj_set_pos(_send_right_line, 143, 128);
    _send_left_dot = makeDecorativeObject(_quota_button, 3, 3, kFiveHourColor);
    lv_obj_set_style_radius(_send_left_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_pos(_send_left_dot, 70, 127);
    _send_right_dot = makeDecorativeObject(_quota_button, 3, 3, kFiveHourColor);
    lv_obj_set_style_radius(_send_right_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_pos(_send_right_dot, 135, 127);
    _send_mark = makeDecorativeObject(_quota_button, 14, 2, kFiveHourColor);
    lv_obj_set_style_radius(_send_mark, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_pos(_send_mark, 97, 144);

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
        lv_obj_set_size(label, 92, lv_font_get_line_height(&lv_font_montserrat_22));
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
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

void DashboardView::beginModeTransition(bool touchActive)
{
    if (_intent_queue != nullptr) {
        xQueueReset(_intent_queue);
    }
    _discard_touch_until_release = touchActive;
}

void DashboardView::update(const DashboardModel& model)
{
    if (_root == nullptr) {
        return;
    }

    lv_label_set_text(_connection_label, model.connectionText.c_str());
    lv_obj_set_style_text_color(_connection_label, lv_color_hex(model.connectionColor), LV_PART_MAIN);
    lv_label_set_text(_battery_label, model.batteryText.c_str());

    const bool codexMode = model.mode == codex_micro_app::model::DashboardMode::Codex;
    if (codexMode != _codex_mode_active) {
        lv_anim_delete(this, applySendPulseAnimationValue);
        if (!codexMode) {
            lv_anim_delete(&_weekly_limit, applyLimitAnimationValue);
            lv_anim_delete(&_five_hour_limit, applyLimitAnimationValue);
            _weekly_limit.animating    = false;
            _five_hour_limit.animating = false;
        }
        _codex_mode_active = codexMode;
        restoreSendPulseVisuals();
    }

    if (model.mode == codex_micro_app::model::DashboardMode::Chat) {
        setCodexInstrumentVisible(false);
        lv_obj_clear_flag(_quota_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_reset_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_home_hint_label, LV_OBJ_FLAG_HIDDEN);
        const ChatSlotVisual* selected = nullptr;
        for (const auto& chat : model.chats) {
            if (chat.available && chat.selected) {
                selected = &chat;
                break;
            }
        }
        lv_obj_set_style_text_font(_quota_label, &lv_font_montserrat_28, LV_PART_MAIN);
        lv_label_set_text(_quota_label, selected != nullptr ? selected->title.c_str() : "NO CHATS");
        lv_label_set_text(_reset_label, selected != nullptr ? selected->project.c_str() : "ADD LOCAL SLOTS");
        lv_label_set_text(_send_label, "LOCAL PREVIEW");
        lv_obj_set_style_text_font(_send_label, &lv_font_montserrat_18, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(_send_label, 0, LV_PART_MAIN);
        lv_obj_align(_send_label, LV_ALIGN_CENTER, 0, 49);
        lv_obj_set_style_text_color(_send_label, lv_color_hex(kChatAccentColor), LV_PART_MAIN);
        lv_obj_set_style_border_width(_quota_button, 10, LV_PART_MAIN);
        lv_obj_set_style_border_color(_quota_button, lv_color_hex(selected != nullptr ? kChatAccentColor : kTrackColor),
                                      LV_PART_MAIN);
    } else {
        setCodexInstrumentVisible(true);
        updateLimitIndicator(_five_hour_limit, model.fiveHourUsedPercent, model.fiveHourLimitAvailable, 520, 0);
        updateLimitIndicator(_weekly_limit, model.weeklyUsedPercent, model.weeklyLimitAvailable, 620, 70);
        lv_obj_add_flag(_quota_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_reset_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_home_hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(_send_label, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(_send_label, 4, LV_PART_MAIN);
        lv_label_set_text(_send_label, "SEND");
        lv_obj_align(_send_label, LV_ALIGN_CENTER, 1, 29);
        lv_obj_set_style_text_color(_send_label, lv_color_hex(kSendTextColor), LV_PART_MAIN);
        lv_obj_set_style_border_width(_quota_button, 0, LV_PART_MAIN);
    }

    for (size_t i = 0; i < _agent_buttons.size(); ++i) {
        if (model.mode == codex_micro_app::model::DashboardMode::Chat) {
            const auto& chat     = model.chats[i];
            const uint32_t fill  = chat.available ? (chat.selected ? 0x6552C7 : kChatPanelColor) : kPanelColor;
            const uint32_t edge  = chat.available ? kChatAccentColor : kTrackColor;
            const uint32_t label = chat.selected ? kPrimaryText : (chat.available ? kChatMutedText : kSecondaryText);

            lv_label_set_text(_agent_labels[i], chat.available ? chat.alias.c_str() : "--");
            lv_obj_set_style_text_font(_agent_labels[i], &lv_font_montserrat_18, LV_PART_MAIN);
            lv_obj_set_style_bg_color(_agent_buttons[i], lv_color_hex(fill), LV_PART_MAIN);
            lv_obj_set_style_border_color(_agent_buttons[i], lv_color_hex(edge), LV_PART_MAIN);
            lv_obj_set_style_outline_color(_agent_buttons[i], lv_color_hex(kPrimaryText), LV_PART_MAIN);
            lv_obj_set_style_outline_opa(_agent_buttons[i], chat.selected ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_shadow_color(_agent_buttons[i], lv_color_hex(kChatAccentColor), LV_PART_MAIN);
            lv_obj_set_style_shadow_opa(
                _agent_buttons[i], chat.selected ? static_cast<lv_opa_t>(70) : static_cast<lv_opa_t>(LV_OPA_TRANSP),
                LV_PART_MAIN);
            lv_obj_set_style_text_color(_agent_labels[i], lv_color_hex(label), LV_PART_MAIN);
            continue;
        }

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
        char agent_name[4] = {};
        std::snprintf(agent_name, sizeof(agent_name), "A%u", static_cast<unsigned>(i + 1));
        lv_label_set_text(_agent_labels[i], agent_name);
        lv_obj_set_style_text_font(_agent_labels[i], &lv_font_montserrat_22, LV_PART_MAIN);
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

    if (binding->owner->_discard_touch_until_release) {
        if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            binding->owner->_discard_touch_until_release = false;
        }
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
        if (pressed) {
            binding->owner->playSendPulse();
        }
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
    if (owner->_discard_touch_until_release) {
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
