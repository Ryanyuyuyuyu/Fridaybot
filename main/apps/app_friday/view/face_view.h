/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "../face_model.h"

#include <functional>
#include <memory>
#include <smooth_lvgl.hpp>
#include <uitk/short_namespace.hpp>

namespace friday::view {

enum class TouchGesture : uint8_t {
    Tap,
    Hold,
    Drag,
    Swipe,
};

struct TouchGestureEvent {
    TouchGesture gesture = TouchGesture::Tap;
    float x              = 0.0f;
    float y              = 0.0f;
    float deltaX         = 0.0f;
    float deltaY         = 0.0f;
};

class FaceView {
public:
    std::function<void(bool active, float x, float y)> onContactChanged;
    std::function<void(const TouchGestureEvent&)> onGesture;

    void init(lv_obj_t* parent);
    void setPose(const FacePose& pose);
    void setFaceVisible(bool visible);

private:
    struct RenderedEye {
        int width    = -1;
        int height   = -1;
        int x        = -1000;
        int y        = -1000;
    };

    std::unique_ptr<uitk::lvgl_cpp::Container> _panel;
    std::unique_ptr<uitk::lvgl_cpp::Container> _left_eye;
    std::unique_ptr<uitk::lvgl_cpp::Container> _right_eye;
    std::unique_ptr<uitk::lvgl_cpp::Container> _touch_surface;
    RenderedEye _rendered_left;
    RenderedEye _rendered_right;
    lv_point_t _touch_start{};
    lv_point_t _touch_latest{};
    uint32_t _touch_start_ms = 0;
    bool _touch_tracking     = false;
    bool _touch_dragged      = false;
    bool _face_visible       = true;

    void configureEye(uitk::lvgl_cpp::Container& eye);
    void applyEye(uitk::lvgl_cpp::Container& eye, RenderedEye& rendered, int width, int height, int x, int y);
    static void handleTouchEvent(lv_event_t* event);
    void handleTouch(lv_event_t* event);
    static float normaliseTouchCoordinate(int coordinate);
};

}  // namespace friday::view
