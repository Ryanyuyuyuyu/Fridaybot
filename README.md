# Friday + Codex Micro for M5Stack StopWatch

Friday is an original, monochrome desk companion built on the M5Stack
StopWatch user demo. It opens from the launcher into a responsive two-eye face
while preserving the upstream hardware evaluation apps.

The current milestone is offline-complete: a 60 FPS-class vector face,
mood-specific pose pools and blinking, pseudo-spherical whole-dial turns,
BMI270 tilt/motion reactions, differentiated touch gestures and long button
performances.
An optional privacy-first macOS helper can add coarse work/away/return context
over low-duty BLE. It also lets the same Friday travel between the StopWatch
and every display attached to the Mac, with locally rendered animation and
two-phase visibility barriers that prevent duplicate faces. Disconnecting the
helper leaves Friday's local personality intact.
Voice conversation will be considered after the face and physical interaction
are validated on hardware.

See [`main/apps/app_friday/README.md`](main/apps/app_friday/README.md) for the
interaction map and animation budget.
See [`companion/macos/README.md`](companion/macos/README.md) for optional Mac
context and cross-display setup.
See [`docs/cross-device-presence.md`](docs/cross-device-presence.md) for the
implemented StopWatch-to-Mac travel protocol.

## Highlights

- True-black AMOLED face with silver-white vertical capsule eyes
- 62.5 Hz elapsed-time animation with persistent LVGL objects and no bitmap
  frame streaming
- Touch, buttons and BMI270 motion interpreted as expressive performances
- Autonomous blinking, looking around, play, sleep and context-aware return
- Privacy-first four-byte Mac presence context over BLE
- Seamless StopWatch/Mac ownership handoff with peek, retreat and arrival
  animation
- A single draggable macOS panel that can cross attached displays without
  creating a second Friday

[简体中文](README.zh-CN.md)

This branch exposes Friday and Codex Micro as two adjacent, independent Apps in
M5Stack's factory-style StopWatch launcher. Cold boot stays in the launcher so
either experience is one tap away. The Codex App retains its existing parallel
Codex and Chat dashboards, while the other factory Apps remain available.

> [!WARNING]
> This is an experimental, unofficial compatibility layer for the **M5Stack
> StopWatch Dev Kit C152**. It is not endorsed by M5Stack, OpenAI, or Work
> Louder. The Codex Micro protocol is not a documented public API and may change.
> A successful build is not evidence that Bluetooth, input, power, or recovery
> has been validated on physical hardware.

Friday and the Codex-only baseline were previously validated separately on
physical C152 hardware. This combined build has passed the host protocol/UI
tests and a complete ESP-IDF build, but still requires device validation for
pairing, both launcher entries, controls, travel, sleep/wake, and power.

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

On a development device that already uses the repository's two-slot partition
table, do not use the generic `idf.py flash` command for a rollback-protected
test: it also writes the bootloader, partition table, and OTA metadata. Preserve
a verified full-flash backup and known-good slot, then write only the inactive
OTA App slot with a command derived from the device's current partition table.

### Controls

- Tap, hold, drag, edge-drag and swipe: distinct touch performances
- Yellow button: upper-left bump, wary watch and reluctant spring home
- Blue button: upper-right bump, playful chase, feint and celebratory hops
- Drag Friday to the configured bezel edge: send it to the connected Mac
- On Mac: click to interact, hold and drag to move it, or drag it back to its
  outer portal edge to return
- While Friday is on Mac: press either StopWatch button to recall it
- Hold both buttons: return to the original launcher

## macOS Companion

```bash
cd companion/macos
make
build/FridayCompanion.app/Contents/MacOS/friday-companion --monitor-side centre
```

The companion does not read keystrokes, documents, URLs, screenshots, camera
frames or microphone audio. See its README for the complete privacy and
interaction model.

## Upstream and license

Friday is built on [M5Stack M5StopWatch-UserDemo](https://github.com/m5stack/M5StopWatch-UserDemo)
and retains the original launcher and evaluation apps. The project is released
under the repository's MIT license.

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

- Friday and Codex Micro are separate Mooncake `AppAbility` entries. The
  launcher is the cold-boot surface and places them next to one another.
- The Codex/Chat UI remains one standard Mooncake `AppAbility` registered
  beside the factory Apps; switching dashboards does not create another App.
- Pure C++ models arbitrate A-button double-tap versus PTT hold and compose the
  six Chat slots, allowing host-side regression tests without LVGL or hardware.
- One native ESP-IDF Bluedroid service owns BLE outside both UI lifecycles. It
  publishes Codex HID/quota plus Friday context/travel in one GATT database.
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
