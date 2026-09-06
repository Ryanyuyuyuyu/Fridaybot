/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "face_view.h"

#include <algorithm>
#include <cmath>
#include <hal/hal.h>

namespace friday::view {
namespace {

constexpr int DisplaySize      = 466;
constexpr uint32_t FaceColor   = 0x000000;
constexpr uint32_t EyeColor    = 0xEEF1F3;
constexpr int MinimumEyeHeight = 7;
constexpr int DragThresholdPx  = 18;
constexpr int SwipeThresholdPx = 68;
constexpr uint32_t HoldMs       = 550;
constexpr uint32_t SwipeMaxMs   = 950;
// A slightly raised eye line makes the circular dial read as a complete head
// with an implied lower face, instead of a centred icon floating on a canvas.
constexpr int VerticalFaceShift = -18;

int rounded(float value)
{
    return static_cast<int>(std::lround(value));
}

}  // namespace

void FaceView::init(lv_obj_t* parent)
{
    _panel = std::make_unique<uitk::lvgl_cpp::Container>(parent);
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setSize(DisplaySize, DisplaySize);
    _panel->setBgColor(lv_color_hex(FaceColor));
    _panel->setBgOpa(LV_OPA_COVER);
    _panel->setBorderWidth(0);
    _panel->setOutlineWidth(0);
    _panel->setShadowWidth(0);
    _panel->setPaddingAll(0);
    _panel->setRadius(0);
    _panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _left_eye = std::make_unique<uitk::lvgl_cpp::Container>(_panel->get());
    configureEye(*_left_eye);

    _right_eye = std::make_unique<uitk::lvgl_cpp::Container>(_panel->get());
    configureEye(*_right_eye);

    // The transparent surface owns touch interaction. Keeping it separate from
    // the eyes avoids hit-box changes while the eye geometry is animated.
    _touch_surface = std::make_unique<uitk::lvgl_cpp::Container>(_panel->get());
    _touch_surface->align(LV_ALIGN_CENTER, 0, 0);
    _touch_surface->setSize(DisplaySize, DisplaySize);
    _touch_surface->setBgOpa(LV_OPA_TRANSP);
    _touch_surface->setBorderWidth(0);
    _touch_surface->setOutlineWidth(0);
    _touch_surface->setShadowWidth(0);
    _touch_surface->setPaddingAll(0);
    _touch_surface->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _touch_surface->addFlag(LV_OBJ_FLAG_CLICKABLE);
    _touch_surface->addFlag(LV_OBJ_FLAG_PRESS_LOCK);
    _touch_surface->addEventCb(handleTouchEvent, LV_EVENT_PRESSED, this);
    _touch_surface->addEventCb(handleTouchEvent, LV_EVENT_PRESSING, this);
    _touch_surface->addEventCb(handleTouchEvent, LV_EVENT_RELEASED, this);
    _touch_surface->addEventCb(handleTouchEvent, LV_EVENT_PRESS_LOST, this);
    _touch_surface->moveForeground();

    // The launcher boot label is no longer needed once Friday owns the screen.
    GetHAL().bootLogo.reset();
}

void FaceView::setPose(const FacePose& pose)
{
    if (!_left_eye || !_right_eye) {
        return;
    }

    const int leftWidth   = std::max(1, rounded(pose.leftWidth));
    const int rightWidth  = std::max(1, rounded(pose.rightWidth));
    const int leftHeight  = std::max(MinimumEyeHeight, rounded(pose.leftHeight * pose.blinkScale));
    const int rightHeight = std::max(MinimumEyeHeight, rounded(pose.rightHeight * pose.blinkScale));
    const int centerX     = rounded(pose.gazeX);
    const int centerY     = rounded(pose.gazeY) + VerticalFaceShift;
    const int halfSpacing = rounded(pose.eyeSpacing * 0.5f);

    // Keep the capsules axis-aligned on the embedded renderer. LVGL renders a
    // rotated rounded rectangle through an intermediate transform layer; two
    // continuously changing layers can halve the frame rate while the user is
    // moving the dial. Width, height, spacing and position still carry the
    // expression and head-tilt response without sacrificing the capsule IP.
    applyEye(*_left_eye, _rendered_left, leftWidth, leftHeight, centerX - halfSpacing,
             centerY + rounded(pose.leftOffsetY));
    applyEye(*_right_eye, _rendered_right, rightWidth, rightHeight, centerX + halfSpacing,
             centerY + rounded(pose.rightOffsetY));
}

void FaceView::setFaceVisible(bool visible)
{
    if (!_left_eye || !_right_eye || visible == _face_visible) {
        return;
    }
    _face_visible = visible;
    if (visible) {
        _left_eye->removeFlag(LV_OBJ_FLAG_HIDDEN);
        _right_eye->removeFlag(LV_OBJ_FLAG_HIDDEN);
    } else {
        _left_eye->addFlag(LV_OBJ_FLAG_HIDDEN);
        _right_eye->addFlag(LV_OBJ_FLAG_HIDDEN);
    }
}

void FaceView::configureEye(uitk::lvgl_cpp::Container& eye)
{
    eye.setBgColor(lv_color_hex(EyeColor));
    eye.setBgOpa(LV_OPA_COVER);
    eye.setBorderWidth(0);
    eye.setOutlineWidth(0);
    eye.setShadowWidth(0);
    eye.setPaddingAll(0);
    eye.setRadius(LV_RADIUS_CIRCLE);
    eye.removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    eye.removeFlag(LV_OBJ_FLAG_CLICKABLE);
}

void FaceView::applyEye(uitk::lvgl_cpp::Container& eye, RenderedEye& rendered, int width, int height, int x, int y)
{
    if (rendered.width != width || rendered.height != height) {
        rendered.width  = width;
        rendered.height = height;
        eye.setSize(width, height);
        eye.setRadius(std::min(width, height) / 2);
    }

    if (rendered.x != x || rendered.y != y) {
        rendered.x = x;
        rendered.y = y;
        eye.align(LV_ALIGN_CENTER, x, y);
    }
}

void FaceView::handleTouchEvent(lv_event_t* event)
{
    auto* view = static_cast<FaceView*>(lv_event_get_user_data(event));
    if (view) {
        view->handleTouch(event);
    }
}

void FaceView::handleTouch(lv_event_t* event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    lv_point_t point            = _touch_latest;
    lv_indev_t* indev           = lv_event_get_indev(event);
    if (!indev) {
        indev = lv_indev_active();
    }
    if (indev) {
        lv_indev_get_point(indev, &point);
    }

    if (code == LV_EVENT_PRESSED) {
        _touch_start      = point;
        _touch_latest     = point;
        _touch_start_ms   = lv_tick_get();
        _touch_tracking   = true;
        _touch_dragged    = false;
        if (onContactChanged) {
            onContactChanged(true, normaliseTouchCoordinate(point.x), normaliseTouchCoordinate(point.y));
        }
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        if (!_touch_tracking) {
            return;
        }
        _touch_latest   = point;
        const int dx    = point.x - _touch_start.x;
        const int dy    = point.y - _touch_start.y;
        _touch_dragged |= dx * dx + dy * dy >= DragThresholdPx * DragThresholdPx;
        if (onContactChanged) {
            onContactChanged(true, normaliseTouchCoordinate(point.x), normaliseTouchCoordinate(point.y));
        }
        return;
    }

    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) {
        return;
    }
    if (!_touch_tracking) {
        return;
    }

    _touch_tracking        = false;
    _touch_latest          = point;
    const int dx           = point.x - _touch_start.x;
    const int dy           = point.y - _touch_start.y;
    const float distance   = std::sqrt(static_cast<float>(dx * dx + dy * dy));
    const uint32_t duration = lv_tick_get() - _touch_start_ms;

    if (onContactChanged) {
        onContactChanged(false, normaliseTouchCoordinate(point.x), normaliseTouchCoordinate(point.y));
    }
    if (!onGesture) {
        return;
    }

    TouchGestureEvent gesture;
    gesture.x      = normaliseTouchCoordinate(point.x);
    gesture.y      = normaliseTouchCoordinate(point.y);
    gesture.deltaX = std::max(-1.0f, std::min(1.0f, static_cast<float>(dx) / (DisplaySize * 0.5f)));
    gesture.deltaY = std::max(-1.0f, std::min(1.0f, static_cast<float>(dy) / (DisplaySize * 0.5f)));

    if (distance >= SwipeThresholdPx && duration <= SwipeMaxMs) {
        gesture.gesture = TouchGesture::Swipe;
    } else if (_touch_dragged) {
        gesture.gesture = TouchGesture::Drag;
    } else if (duration >= HoldMs) {
        gesture.gesture = TouchGesture::Hold;
    } else {
        gesture.gesture = TouchGesture::Tap;
    }
    onGesture(gesture);
}

float FaceView::normaliseTouchCoordinate(int coordinate)
{
    constexpr float halfDisplay = DisplaySize * 0.5f;
    return std::max(-1.0f, std::min(1.0f, (static_cast<float>(coordinate) - halfDisplay) / halfDisplay));
}

}  // namespace friday::view
