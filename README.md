# M5Stack StopWatch + Codex Micro

[简体中文](README.zh-CN.md)

This branch adds a Codex Micro-compatible control surface as a normal App in
M5Stack's factory-style StopWatch launcher. The same App now contains parallel
Codex and Chat dashboards, while the other factory Apps remain available.

> [!WARNING]
> This is an experimental, unofficial compatibility layer for the **M5Stack
> StopWatch Dev Kit C152**. It is not endorsed by M5Stack, OpenAI, or Work
> Louder. The Codex Micro protocol is not a documented public API and may change.
> A successful build is not evidence that Bluetooth, input, power, or recovery
> has been validated on physical hardware.

The Codex-only baseline was validated on one physical C152 for cold boot, the
factory-style launcher, Codex Micro controls, BLE connectivity, center Send,
and local A+B Home. The new dual-dashboard revision has only passed host unit
tests and a complete firmware build; it has not been flashed to hardware.

## Controls

The host owns the final assignment for every reported Codex Micro input.
Recommended mappings are shown below.

| StopWatch input | Reported control | Behavior / recommended host assignment |
| --- | --- | --- |
| Double-tap physical **A** | Local mode action | Toggle between the Codex and Chat dashboards without adding an on-screen button |
| Hold physical **A** | `ACT10` | Push to talk after a roughly 180 ms gesture-arbitration delay; release stops it |
| Press and release physical **B** | `ACT09` | Short command pulse; Voice Chat is a suggested assignment |
| Tap **A1** through **A6** | `AG00` through `AG05` | Open or focus the six host-assigned Agent conversations |
| Tap the center quota / **SEND** control in Codex mode | `ACT12` | Send the current composer message; Chat mode keeps this local until host targeting exists |
| Swipe up, right, down, or left in Codex mode | Analog direction | Four independently host-configurable actions; swipes are intentionally ignored in Chat mode |
| Hold physical **A+B** together for about 500 ms | Local launcher action | Release controls and close only this App, returning to the factory launcher |

In ChatGPT Desktop, select **Settings > Codex Micro > Agent keys > Custom
assignments** to make A1-A6 stable. The labels are slot numbers; the Bluetooth
payload does not provide project or conversation names.

The A+B action is handled on the StopWatch. It does not send a Back command to
ChatGPT and does not quit ChatGPT on the Mac. The BLE service is process-scoped,
so leaving the Codex App keeps the paired control surface available in the
background while the factory launcher or another StopWatch App is visible.

### Chat dashboard milestone

Chat mode reuses the same six circular positions and center Send control as
Codex mode, with a violet accent and short aliases. Its model supports one
cross-project global recent list plus pinned slots; pinned positions remain
stable for the active session. A slot tap currently selects and previews that
chat locally and deliberately does **not** emit `AG00`-`AG05`, which would open
an Agent by mistake. The center is marked `LOCAL PREVIEW` and suppresses
`ACT12` in Chat mode until the selected target can be acknowledged by the host,
preventing a message from being sent into the wrong composer.

This milestone does not yet open a ChatGPT consumer chat on the Mac. The
current host protocol supplies six Agent states but no public chat catalog or
stable chat target identifier, so host navigation requires a separately
reviewed bridge before it can be wired safely.

The committed build uses neutral sample metadata. To test private aliases,
copy `main/apps/app_codex_micro/model/local_chat_slots.h.example` to
`local_chat_slots.h`, edit the compile-time entries, and rebuild. The private
header is git-ignored. Store only short aliases/project/title metadata there;
never put chat URLs, conversation IDs, cookies, or credentials in firmware.
Labels are intentionally limited to short printable ASCII strings. Git-ignore
prevents an accidental source commit, but the chosen metadata is still compiled
into the firmware image and is not encrypted at rest.

## Bluetooth pairing

The device advertises as **Codex Micro**. If this Mac was paired with an older
StopWatch image, macOS may keep the previous HID/GATT descriptor. If controls do
not appear after installing this image, forget only the **Codex Micro** entry in
macOS Bluetooth settings, restart the StopWatch, and pair it again. Do not
remove unrelated Bluetooth devices.

This compatibility layer uses BLE bonding and an additional project-owned GATT
service for the small quota snapshot. It does not store an OpenAI access token
on the StopWatch.

## Build only (no device write)

Use ESP-IDF v5.5.4 and the ESP32-S3 target. Fetching dependencies and compiling
are non-device operations:

The upstream M5GFX build files used by this branch are patched so source paths
containing spaces are handled correctly.

```bash
python3 ./fetch_repos.py
source "$IDF_PATH/export.sh"
idf.py set-target esp32s3
idf.py build
```

Stop after `idf.py build` if the goal is to prepare and inspect the image. None
of the commands above flashes the StopWatch. Before any later flash, resolve the
currently connected `/dev/cu.*` device again and obtain explicit confirmation
for that exact port; never reuse an earlier port assumption.

## Flashing and factory recovery

Flashing this build changes the firmware and partition layout. Existing NVS,
factory settings, FAT contents, or data from another firmware layout may be
reformatted or become inaccessible. Back up anything important and treat the
device write as destructive.

The official recovery route is M5Stack's
[StopWatch factory firmware recovery guide](https://docs.m5stack.com/en/guide/restore_factory/stopwatch).
Follow that page to restore the current factory image with M5Burner. Recovery
also overwrites the installed image and can clear device data. Keep the recovery
link available before doing a first physical flash.

M5Stack's general StopWatch operating guide is available in the
[official documentation](https://docs.m5stack.com/en/guide/display_device/stopwatch/usage).

## Architecture

- The Codex/Chat UI remains one standard Mooncake `AppAbility` registered
  beside the factory Apps; switching dashboards does not create another App.
- Pure C++ models arbitrate A-button double-tap versus PTT hold and compose the
  six Chat slots, allowing host-side regression tests without LVGL or hardware.
- A native ESP-IDF Bluedroid service owns BLE outside the UI lifecycle.
- LVGL callbacks enqueue touch intents; the App loop sends BLE controls and
  pairs every press with a release.
- Holding A+B uses the factory `KeyManager` Home gesture and calls the local App
  `close()` path.

## Provenance and license

The factory launcher base is
[M5Stack `M5StopWatch-UserDemo`](https://github.com/m5stack/M5StopWatch-UserDemo),
copyright M5Stack Technology CO LTD and licensed under MIT. The Codex control
surface and compatibility work is adapted from
[`digitsisyph/codex-micro-stopwatch`](https://github.com/digitsisyph/codex-micro-stopwatch),
copyright imliubo and codex-micro-4-stopwatch contributors, also under MIT.

See [LICENSE](LICENSE) and [NOTICE](NOTICE). Downloaded components under
`components/` retain their own license files. Product names and trademarks
belong to their respective owners.
