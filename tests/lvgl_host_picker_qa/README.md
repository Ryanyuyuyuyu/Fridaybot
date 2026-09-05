# Native LVGL device-picker QA

Run from the repository root:

```sh
sh tests/run_lvgl_host_picker_qa.sh /tmp/friday-lvgl-qa
```

This builds the checked-in LVGL library and actual Codex dashboard view with a
small, single-threaded FreeRTOS queue stub. It needs a native C/C++ compiler,
CMake, and Make; it can reuse the local ESP-IDF CMake installation. Set `CMAKE`
to a different executable or `QA_JOBS` to adjust build parallelism. Reusing the
same output directory makes subsequent builds incremental.

The executable uses real LVGL pointer input to check opening and closing the
menu, choosing an offline host by stable ID, and canceling a held touch across a
route change. It also checks delayed control callbacks cannot bypass the modal,
scrolling works, and original A1/A2/A6 edge taps cannot open the device menu.
Every rotated character's transformed bounds must remain inside the circular
screen. Short aliases, long-name truncation, USB/BLE/Offline labels, and compact
battery text with its charging mark are checked separately.
Assertions remain enabled for release builds.

Seven 466 × 466 PPM frames show USB and BLE dashboards, offline short and long
names, populated list, scrolled list, and empty list. The Mac P/W preview labels
are explicit synthetic fixtures; the view never assigns a private/work role to
a real host. On macOS the runner also creates PNG copies. The
grey corners indicate the area outside the circular hardware display.

This test opens no desktop window, serial port, Bluetooth session, or hardware
connection. It validates view behavior and layout; physical A/B buttons,
FreeRTOS concurrency, BLE/USB transport, and the app-to-service epoch guard still
require their separate tests or device validation.
