// Render the actual LVGL view and exercise its real event callbacks without
// starting firmware, opening a desktop window, or connecting any device.
#include <apps/app_codex_micro/view/view.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace codex_micro_app::view;

namespace {

constexpr int kSize = 466;
std::vector<unsigned char> pixels(kSize * kSize * 3);
lv_point_t pointerPosition{0, 0};
bool pointerPressed = false;

void readPointer(lv_indev_t*, lv_indev_data_t* data)
{
    data->point = pointerPosition;
    data->state = pointerPressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void tick(int steps = 4)
{
    for (int i = 0; i < steps; ++i) {
        lv_tick_inc(20);
        lv_timer_handler();
    }
}

void tap(int x, int y)
{
    pointerPosition = {x, y};
    pointerPressed = true;
    tick();
    pointerPressed = false;
    tick();
}

void flush(lv_display_t* display, const lv_area_t* area, uint8_t* map)
{
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            const int source = ((y - area->y1) * (area->x2 - area->x1 + 1) + (x - area->x1)) * 4;
            const int destination = (y * kSize + x) * 3;
            // Mark the area outside the physical circular display in grey.
            if ((x - 233) * (x - 233) + (y - 233) * (y - 233) > 233 * 233) {
                pixels[destination] = pixels[destination + 1] = pixels[destination + 2] = 28;
            } else {
                pixels[destination] = map[source + 2];
                pixels[destination + 1] = map[source + 1];
                pixels[destination + 2] = map[source];
            }
        }
    }
    lv_display_flush_ready(display);
}

lv_obj_t* findLabel(lv_obj_t* root, const char* text)
{
    if (lv_obj_check_type(root, &lv_label_class) && std::strcmp(lv_label_get_text(root), text) == 0) {
        return root;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        if (auto* result = findLabel(lv_obj_get_child(root, i), text)) {
            return result;
        }
    }
    return nullptr;
}

void save(const std::filesystem::path& directory, const char* name)
{
    lv_obj_invalidate(lv_screen_active());
    tick(60);
    const auto path = directory / (std::string(name) + ".ppm");
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    assert(file != nullptr);
    std::fprintf(file, "P6\n%d %d\n255\n", kSize, kSize);
    assert(std::fwrite(pixels.data(), 1, pixels.size(), file) == pixels.size());
    assert(std::fclose(file) == 0);
}

DashboardModel previewModel()
{
    DashboardModel model;
    model.connectionText = "Office Mac";
    model.transportText = "USB";
    model.connectionColor = 0x77E6A5;
    model.batteryText = "BAT 82% +";
    model.fiveHourLimitAvailable = true;
    model.weeklyLimitAvailable = true;
    model.fiveHourUsedPercent = 34;
    model.weeklyUsedPercent = 72;
    model.agents[0] = {0xB7C2CD, 0.90f, false};
    model.agents[1] = {0x4292F5, 1.0f, true};
    model.agents[2] = {0x2BC96E, 1.0f, false};
    model.agents[3] = {0xF7AC42, 1.0f, false};
    model.agents[4] = {0xF55A68, 1.0f, false};
    model.hosts = {
        {"a", "Office Mac", true, true, true, true},
        {"b", "Travel MacBook", true, false, false, true},
        {"c", "Living Room Mac", false, false, false, false},
        {"d", "Studio Mac", false, false, false, false},
        {"e", "Test MacBook Pro", false, false, false, false},
    };
    return model;
}

}  // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path output = argc > 1 ? argv[1] : ".";
    std::filesystem::create_directories(output);
    lv_init();
    auto* display = lv_display_create(kSize, kSize);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    static std::vector<uint8_t> buffer(kSize * kSize * 4);
    lv_display_set_buffers(display, buffer.data(), nullptr, buffer.size(), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    auto* pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, readPointer);

    DashboardView view;
    assert(view.init());
    auto model = previewModel();
    view.update(model);
    save(output, "dashboard");

    tap(225, 38);
    assert(view.deviceMenuOpen());
    TouchIntent intent;
    assert(view.popIntent(intent) && intent.type == TouchIntentType::DeviceMenuOpened);
    assert(!view.popIntent(intent));

    // Controls below a modal must reject even a delayed/injected callback.
    auto* send = findLabel(lv_screen_active(), "SEND");
    assert(send != nullptr);
    lv_obj_send_event(lv_obj_get_parent(send), LV_EVENT_PRESSED, nullptr);
    lv_obj_send_event(lv_obj_get_parent(send), LV_EVENT_RELEASED, nullptr);
    assert(!view.popIntent(intent));
    save(output, "devices");

    // Actual pointer input on an offline row must emit its stable ID, not an
    // agent/send event, and the menu stays open until service confirmation.
    tap(180, 304);
    assert(view.popIntent(intent));
    assert(intent.type == TouchIntentType::SelectHost && std::string(intent.hostId) == "c");
    assert(!view.popIntent(intent));
    assert(view.deviceMenuOpen());

    auto* rowName = findLabel(lv_screen_active(), "Living Room Mac");
    assert(rowName != nullptr);
    auto* list = lv_obj_get_parent(lv_obj_get_parent(rowName));
    pointerPosition = {230, 314};
    pointerPressed = true;
    tick(2);
    for (int y = 284; y >= 134; y -= 30) {
        pointerPosition.y = y;
        tick(2);
    }
    pointerPressed = false;
    tick();
    save(output, "devices-scrolled");
    assert(lv_obj_get_scroll_y(list) > 0);
    assert(!view.popIntent(intent));  // A list swipe is never a joystick action.

    view.cancelTouch();
    assert(!view.popIntent(intent));
    tick();  // Let the pointer driver observe the release after cancellation.
    tap(233, 387);
    assert(!view.deviceMenuOpen());
    assert(view.popIntent(intent) && intent.type == TouchIntentType::DeviceMenuClosed);
    assert(!view.popIntent(intent));

    // A held finger is forgotten on route changes: its later release cannot
    // become a new-host SEND action. A fresh subsequent tap works normally.
    pointerPosition = {233, 240};
    pointerPressed = true;
    tick();
    assert(view.popIntent(intent) && intent.type == TouchIntentType::SendPress);
    view.cancelTouch();
    pointerPressed = false;
    tick();
    assert(!view.popIntent(intent));
    tap(233, 240);
    assert(view.popIntent(intent) && intent.type == TouchIntentType::SendPress);
    assert(view.popIntent(intent) && intent.type == TouchIntentType::SendRelease);
    assert(!view.popIntent(intent));

    model.connectionText = "Very Long Office MacBook Name";
    model.transportText = "Offline";
    model.connectionColor = 0xFF7685;
    view.update(model);
    save(output, "dashboard-offline-long-name");
    auto* longName = findLabel(lv_screen_active(), model.connectionText.c_str());
    assert(longName != nullptr);
    assert(findLabel(lv_obj_get_parent(longName), "Offline") != nullptr);

    model.hosts.clear();
    view.update(model);
    tap(225, 38);
    assert(view.deviceMenuOpen());
    save(output, "devices-empty");
    std::puts("LVGL host picker: real pointer input, modal isolation, offline selection, scroll and old-touch cancellation PASS");
    return 0;
}
