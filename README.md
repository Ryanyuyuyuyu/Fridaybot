# Friday + Codex Micro for M5Stack StopWatch

Codex host switching is implemented in `V0.5-friday-codex.6`: one active Mac,
a device picker, optional USB priority, and same-Mac Bluetooth fallback. See
[host switching and verification](docs/codex-host-switching.md),
[USB build configuration](docs/codex-usb-transport.md), and the
[Mac identity/usage helper](companion/macos/host_identity/README.md).
The `.7` image adds USB discovery compatibility and receive-overflow recovery
fixes. It has been written to the verified OTA slot and independently read back,
with protected flash regions verified unchanged. Physical startup, native USB
control, and two-Mac behavior validation of this revision are pending.

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
Friday now also has a Mac-connected Flash Capsule prototype: hold A to stream
the StopWatch microphone into live transcription, then release to save the
text in today's combined todo/memo inbox. Audio is only a temporary recovery
file: successful transcription deletes it immediately, while failed or
low-confidence transcription retains it for at most 24 hours. Live voice
conversation remains a separate future feature.

Flash Capsule audio duration, clarity, and transcription accuracy are still
under hardware validation. A successful build or boot is not an audio-quality
acceptance test.

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
- A-button Flash Capsules with 60-second streaming transcription, temporary
  recovery audio, and one daily todo/memo inbox

[简体中文](README.zh-CN.md)

This branch exposes Friday and Codex Micro as two adjacent, independent Apps in
M5Stack's factory-style StopWatch launcher. Cold boot stays in the launcher so
either experience is one tap away. Codex Micro remains a Codex-only control
surface, while Friday and the other factory Apps keep their own independent UI.

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
| Press or hold physical **A** | `ACT10` | Push to talk starts on press; release stops it |
| Press and release physical **B** | `ACT09` | Short command pulse; Voice Chat is a suggested assignment |
| Tap **A1** through **A6** | `AG00` through `AG05` | Open or focus the six host-assigned Agent conversations |
| Tap the center quota / **SEND** control | `ACT12` | Send the current composer message |
| Swipe up, right, down, or left | Analog direction | Four independently host-configurable actions |
| Hold physical **A+B** together for about 500 ms | Local launcher action | Release controls and close only this App, returning to the factory launcher |

In ChatGPT Desktop, select **Settings > Codex Micro > Agent keys > Custom
assignments** to make A1-A6 stable. The labels are slot numbers; the Bluetooth
payload does not provide project or conversation names.

The A+B action is handled on the StopWatch. It does not send a Back command to
ChatGPT and does not quit ChatGPT on the Mac. The BLE service is process-scoped,
so leaving the Codex App keeps the paired control surface available in the
background while the factory launcher or another StopWatch App is visible.

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

### Friday controls

- Tap, hold, drag, edge-drag and swipe: distinct touch performances
- Hold the yellow A button: record and stream a Flash Capsule to the connected
  Mac; release to finish (60 seconds maximum, under 0.8 seconds is discarded)
- Blue button: upper-right bump, playful chase, feint and celebratory hops
- Drag Friday to the configured bezel edge: send it to the connected Mac
- On Mac: click to interact, hold and drag to move it, or drag it back to its
  outer portal edge to return
- While Friday is on Mac: press B to recall it; A still records a Flash Capsule
- Hold both buttons: return to the original launcher

## macOS Companion

```bash
cd companion/macos
make
open -n build/FridayCompanion.app --args --monitor-side centre
```

The companion does not read keystrokes, documents, URLs, screenshots, camera
frames, or the Mac microphone. During an intentional A-button capture it does
receive the StopWatch microphone stream and stores it locally. See its README
for the complete storage, speech-recognition, privacy, and interaction model.

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
- Codex Micro has one Codex-only dashboard registered beside the factory Apps;
  it has no hidden Chat preview mode.
- In the Codex App, physical A reports `ACT10` directly on press and release. The A+B Home chord
  is evaluated first so entering the launcher cannot leave PTT held down.
- One native ESP-IDF Bluedroid service owns BLE outside both UI lifecycles. It
  publishes Codex HID/quota plus Friday context/travel/Flash Capsule channels
  in one GATT database.
- Flash Capsule audio has its own BLE queue behind Codex commands and releases;
  regression tests also pin the verified Codex UI/controller source hashes.
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
