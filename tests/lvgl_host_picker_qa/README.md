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
scrolling works, and the transport label remains separate from long host names.
Assertions remain enabled for release builds.

Five 466 × 466 PPM frames show the dashboard, offline long name, populated list,
scrolled list, and empty list. On macOS the runner also creates PNG copies. The
grey corners indicate the area outside the circular hardware display.

This test opens no desktop window, serial port, Bluetooth session, or hardware
connection. It validates view behavior and layout; physical A/B buttons,
FreeRTOS concurrency, BLE/USB transport, and the app-to-service epoch guard still
require their separate tests or device validation.
