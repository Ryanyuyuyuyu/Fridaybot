/*
 * SPDX-FileCopyrightText: 2026 Codex Micro for StopWatch contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include <assets/assets.h>

#include <cstddef>
#include <cstdint>

namespace {

constexpr int kWidth  = 200;
constexpr int kHeight = 200;

constexpr uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    return static_cast<uint16_t>(((red & 0xF8U) << 8U) | ((green & 0xFCU) << 3U) | (blue >> 3U));
}

constexpr int square(int value)
{
    return value * value;
}

constexpr bool inCircle(int x, int y, int centerX, int centerY, int radius)
{
    return square(x - centerX) + square(y - centerY) <= square(radius);
}

constexpr bool onSegment(int x, int y, int x0, int y0, int x1, int y1, int halfWidth)
{
    const int segmentX = x1 - x0;
    const int segmentY = y1 - y0;
    const int pointX   = x - x0;
    const int pointY   = y - y0;
    const int length2  = square(segmentX) + square(segmentY);
    const int dot      = pointX * segmentX + pointY * segmentY;
    const int cross    = pointX * segmentY - pointY * segmentX;

    return dot >= 0 && dot <= length2 && square(cross) <= square(halfWidth) * length2;
}

constexpr uint16_t pixelAt(int x, int y)
{
    constexpr uint16_t black       = rgb565(0x00, 0x00, 0x00);
    constexpr uint16_t panel       = rgb565(0x11, 0x17, 0x22);
    constexpr uint16_t panelLight  = rgb565(0x1C, 0x21, 0x32);
    constexpr uint16_t accent      = rgb565(0xB8, 0xA7, 0xFF);
    constexpr uint16_t accentLight = rgb565(0xE0, 0xD9, 0xFF);
    constexpr uint16_t white       = rgb565(0xF4, 0xF7, 0xFB);

    const int centerDistance2 = square(x - 100) + square(y - 100);
    uint16_t color            = black;
    if (centerDistance2 <= square(94)) {
        color = panel;
    }
    if (centerDistance2 <= square(88)) {
        color = panelLight;
    }
    if (centerDistance2 <= square(82)) {
        color = panel;
    }

    // Six nodes echo the six configurable Agent keys on the dashboard.
    if (centerDistance2 >= square(49) && centerDistance2 <= square(68) &&
        (inCircle(x, y, 100, 41, 8) || inCircle(x, y, 151, 70, 8) || inCircle(x, y, 151, 130, 8) ||
         inCircle(x, y, 100, 159, 8) || inCircle(x, y, 49, 130, 8) || inCircle(x, y, 49, 70, 8))) {
        color = inCircle(x, y, 100, 41, 8) ? accentLight : accent;
    }

    // A terminal-style prompt keeps the mark legible at launcher scale.
    if (centerDistance2 <= square(46)) {
        color = black;
    }
    if (centerDistance2 <= square(46) && centerDistance2 > square(41)) {
        color = accent;
    }
    if (x >= 69 && x <= 100 && y >= 74 && y <= 126 &&
        (onSegment(x, y, 73, 78, 96, 100, 4) || onSegment(x, y, 96, 100, 73, 122, 4))) {
        color = white;
    }
    if (x >= 100 && x <= 136 && y >= 116 && y <= 124) {
        color = accentLight;
    }

    return color;
}

struct IconPixels {
    uint16_t data[kWidth * kHeight];
};

constexpr IconPixels makeIcon()
{
    IconPixels pixels = {};
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            pixels.data[y * kWidth + x] = pixelAt(x, y);
        }
    }
    return pixels;
}

// Compile-time drawing keeps this source compact while preserving the native
// 200 x 200 RGB565 format used by the factory launcher.
alignas(4) constexpr auto kIconPixels = makeIcon();

}  // namespace

const lv_image_dsc_t icon_codex_micro = [] {
    lv_image_dsc_t image = {};
    image.header.cf      = LV_COLOR_FORMAT_RGB565;
    image.header.magic   = LV_IMAGE_HEADER_MAGIC;
    image.header.w       = kWidth;
    image.header.h       = kHeight;
    image.data_size      = sizeof(kIconPixels.data);
    image.data           = reinterpret_cast<const uint8_t*>(kIconPixels.data);
    return image;
}();
