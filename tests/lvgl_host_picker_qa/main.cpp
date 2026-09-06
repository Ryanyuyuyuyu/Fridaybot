// Render the actual LVGL view and exercise its real event callbacks without
// starting firmware, opening a desktop window, or connecting any device.
#include <apps/app_codex_micro/view/view.h>

#include <cassert>
#include <cmath>
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

struct ArcStrings {
    std::string host;
    std::string transport;
    std::string battery;
};

void inspectArcLabels(lv_obj_t* root, ArcStrings& strings)
{
    const int32_t rotation = lv_obj_get_style_transform_rotation(root, LV_PART_MAIN);
    if (lv_obj_check_type(root, &lv_label_class) && rotation != 0 &&
        !lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) {
        const auto* font = lv_obj_get_style_text_font(root, LV_PART_MAIN);
        auto& text = rotation < 0 ? strings.host : font == &lv_font_montserrat_18 ? strings.transport : strings.battery;
        text += lv_label_get_text(root);
        // Check the entire transformed glyph box against the real circular
        // display, so a passing rectangular render cannot conceal clipped text.
        lv_area_t area;
        lv_obj_get_coords(root, &area);
        const float pivotX = area.x1 + lv_obj_get_style_transform_pivot_x(root, LV_PART_MAIN);
        const float pivotY = area.y1 + lv_obj_get_style_transform_pivot_y(root, LV_PART_MAIN);
        const float angle = rotation * 3.14159265358979323846f / 1800.0f;
        for (const int x : {area.x1, area.x2}) {
            for (const int y : {area.y1, area.y2}) {
                const float transformedX = pivotX + (x - pivotX) * std::cos(angle) - (y - pivotY) * std::sin(angle);
                const float transformedY = pivotY + (x - pivotX) * std::sin(angle) + (y - pivotY) * std::cos(angle);
                assert(std::hypot(transformedX - 233, transformedY - 233) < 233);
            }
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        inspectArcLabels(lv_obj_get_child(root, i), strings);
    }
}

ArcStrings arcStrings()
{
    ArcStrings result;
    inspectArcLabels(lv_screen_active(), result);
    return result;
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
    model.connectionText = "Mac W";
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
        {"a", "Mac W", true, true, true, true},
        {"b", "Mac P", true, false, false, true},
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
    assert(arcStrings().host == "Mac W");
    assert(arcStrings().transport == "USB");
    assert(arcStrings().battery == "82%+");

    // A1 retains the original position and hit area; connection metadata must
    // not cover it or turn a normal agent tap into a device-picker action.
    TouchIntent intent;
    tap(233, 72);
    assert(view.popIntent(intent) && intent.type == TouchIntentType::AgentPress && intent.agent == 0);
    assert(view.popIntent(intent) && intent.type == TouchIntentType::AgentRelease && intent.agent == 0);
    assert(!view.popIntent(intent));

    // Original circle-edge taps adjacent to the metadata still reach exactly
    // their agent, including the portion overlapped by the picker rectangle.
    for (const auto& point : std::array<std::array<int, 3>, 3>{{{178, 72, 0}, {100, 101, 5}, {375, 100, 1}}}) {
        tap(point[0], point[1]);
        const bool received = view.popIntent(intent);
        if (!received || intent.type != TouchIntentType::AgentPress || intent.agent != point[2]) {
            std::fprintf(stderr, "Edge tap %d,%d expected A%d received=%d type=%d agent=%d menu=%d\n",
                         point[0], point[1], point[2] + 1, received, static_cast<int>(intent.type), intent.agent,
                         view.deviceMenuOpen());
        }
        assert(received && intent.type == TouchIntentType::AgentPress && intent.agent == point[2]);
        assert(view.popIntent(intent) && intent.type == TouchIntentType::AgentRelease && intent.agent == point[2]);
        assert(!view.popIntent(intent));
        assert(!view.deviceMenuOpen());
    }

    auto* connectionName = findLabel(lv_screen_active(), "M");
    assert(connectionName != nullptr);
    assert(lv_obj_get_style_transform_rotation(connectionName, LV_PART_MAIN) < -200);
    const auto tapConnection = [] { tap(133, 52); };
    tapConnection();
    assert(view.deviceMenuOpen());
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

    model.transportText = "Offline";
    model.connectionColor = 0xFF7685;
    view.update(model);
    save(output, "dashboard-offline");
    assert(arcStrings().transport == "Offline");
    model.connectionText = "Very Long Office MacBook Name";
    view.update(model);
    save(output, "dashboard-offline-long-name");
    // Arc labels keep each glyph on one line and truncate the original name
    // inside its own wedge; Offline remains fully spelled out on the right.
    assert(lv_obj_get_height(connectionName) == lv_font_get_line_height(&lv_font_montserrat_18) + 2);
    assert(findLabel(lv_screen_active(), ".") != nullptr);
    assert(findLabel(lv_screen_active(), "O") != nullptr);
    assert(arcStrings().host.size() < model.connectionText.size());
    assert(arcStrings().host.substr(arcStrings().host.size() - 3) == "...");
    assert(arcStrings().transport == "Offline");

    model.connectionText = "Mac P";
    model.transportText = "BLE";
    model.batteryText = "BAT 100%";
    view.update(model);
    save(output, "dashboard-ble");
    assert(arcStrings().host == "Mac P");
    assert(arcStrings().transport == "BLE");
    assert(arcStrings().battery == "100%");

    // Use the actual route policy and pointer callbacks for the user's mixed
    // topology: W remains plugged into USB while P is selected over BLE.
    codex_micro::HostSelection selection;
    selection.registerReady(7, codex_micro::Transport::Ble, "w", "Mac W");
    selection.registerReady(8, codex_micro::Transport::Ble, "p", "Mac P");
    selection.registerReady(65534, codex_micro::Transport::Usb, "w", "Mac W");
    const auto projectSelection = [&] {
        model.connectionText = selection.selectedName();
        const auto route = selection.selectedRoute();
        model.transportText = !route ? "Offline" : route->transport == codex_micro::Transport::Usb ? "USB" : "BLE";
        model.hosts = selection.hosts();
        view.update(model);
        tick();
    };
    projectSelection();
    tapConnection();
    assert(view.popIntent(intent) && intent.type == TouchIntentType::DeviceMenuOpened);
    assert(findLabel(lv_screen_active(), "USB + BLE") == nullptr);
    assert(findLabel(lv_screen_active(), "USB") != nullptr);
    assert(findLabel(lv_screen_active(), "BLE") != nullptr);
    save(output, "devices-mixed");
    // Both rows remain in their original order; repeated round trips must
    // select the stable Mac ID without delivering any underlying Agent key.
    for (int round = 0; round < 3; ++round) {
        tap(180, 230);
        assert(view.popIntent(intent) && intent.type == TouchIntentType::SelectHost);
        assert(std::string(intent.hostId) == "p" && !view.popIntent(intent));
        assert(selection.selectHost("p"));
        selection.registerReady(65534, codex_micro::Transport::Usb, "w", "Mac W");
        projectSelection();
        view.closeDeviceMenu();
        tick(); // Observe the lifted finger after route-change cancellation.
        assert(arcStrings().host == "Mac P" && arcStrings().transport == "BLE");
        tapConnection();
        assert(view.popIntent(intent) && intent.type == TouchIntentType::DeviceMenuOpened);
        tap(180, 150);
        assert(view.popIntent(intent) && intent.type == TouchIntentType::SelectHost);
        assert(std::string(intent.hostId) == "w" && !view.popIntent(intent));
        assert(selection.selectHost("w"));
        projectSelection();
        view.closeDeviceMenu();
        tick();
        assert(arcStrings().host == "Mac W" && arcStrings().transport == "USB");
        tapConnection();
        assert(view.popIntent(intent) && intent.type == TouchIntentType::DeviceMenuOpened);
    }
    selection.disconnect(65534);
    projectSelection();
    view.closeDeviceMenu();
    tick();
    assert(arcStrings().host == "Mac W" && arcStrings().transport == "BLE");
    selection.disconnect(7);
    projectSelection();
    assert(arcStrings().host == "Mac W" && arcStrings().transport == "Offline");

    model.hosts.clear();
    view.update(model);
    tapConnection();
    assert(view.deviceMenuOpen());
    save(output, "devices-empty");
    std::puts("LVGL host picker: circular text bounds, Agent hit areas, modal isolation, mixed USB/BLE round trips, fallback, offline selection and old-touch cancellation PASS");
    return 0;
}
