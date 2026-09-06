# Codex Micro USB transport

The USB variant presents the same vendor HID collection as the Bluetooth
Codex Micro endpoint: VID `303A`, PID `8360`, usage page `FF00`, usage `1`.
Native desktop RPC uses report ID `6`, with 63 body bytes and one Report ID
byte on USB. Input reports use interrupt IN endpoint `0x81`; all host Output
and Feature writes use control `SET_REPORT` on EP0. There is no interrupt OUT
endpoint in the configuration descriptor.
The USB serial string is derived from the chip's factory MAC address.
USB `bcdDevice` is `0x0100`; BLE keeps its existing PnP release `0x0101`.
Work Louder's desktop discovery uses the release's low two bits to distinguish
USB when an explicit transport value is absent, so USB must end in binary `00`.

Static compatibility evidence was checked against the locally installed
`ChatGPT Classic.app` version `26.901.31953` on 2026-09-05. Its `app.asar`
contains `@worklouder/wl-device-kit/dist/index.js` (under `device-kit-oai`):
lines 5189/5195 register PID `33632` (`8360`) and VID `12346` (`303A`),
line 5240 requires usage page `65280` (`FF00`), and line 5247 classifies USB
using `(device.release & 3) === 0`. Its current service bundle
`.vite/build/service-z8uGrRiL.js` prefers explicit `transport === "usb"`,
otherwise uses the same release fallback, then sorts USB interfaces first.
On macOS that service uses a separate native topology enumerator; the bundled
node-hid enumerator inspected locally omits the transport field. Therefore the
release correction fixes a confirmed fallback incompatibility, while native
USB handshake and the cause of an absent report-6 response still require
separate runtime verification. VID/PID/usage matching alone does not prove it.

USB receive-queue overflow ends the damaged USB session and reconnects after
150 ms, using the existing recovery preference policy. It must not wait for a
newline to resume parsing: native desktop requests are JSON without a trailing
newline. No receive overflow was observed in the initial device run; this is a
confirmed recovery defect, not an established cause of that run's RPC timeout.

The separate host identity helper uses atomic Feature reports. It never writes
RPC report 6, so two host processes cannot interleave one JSON RPC message.

| Report | Type | Body bytes | Contents |
| --- | --- | --- | --- |
| 6 | Input / Output | 63 | Existing Codex Micro RPC framing |
| 7 | Feature | 128 | NUL-padded UTF-8 identity JSON |
| 8 | Feature | 256 | NUL-padded UTF-8 quota JSON |

Numbered macOS `IOHIDDeviceSetReport` buffers include the Report ID before the
body. TinyUSB removes that ID for a control SET_REPORT callback. Feature
reports must have the exact advertised body length. The HID interrupt IN
endpoint remains 64 bytes; TinyUSB's
control scratch buffer is 257 bytes to accommodate the quota Feature report.

The IN-only configuration is necessary with the pinned TinyUSB version:
`src/class/hid/hid_device.c` uses `CFG_TUD_HID_EP_BUFSIZE` both for the control
buffer and the length passed to `usbd_edpt_xfer` for interrupt OUT. At 257 bytes,
the DWC2 driver schedules five packets for a 64-byte endpoint and waits for
transfer completion before delivering `tud_hid_set_report_cb`. A single full
64-byte RPC report therefore does not finish that receive transfer. The
Espressif `esp_tinyusb` `test_apps/usb_cv/main/test_usbcv.c` example uses
`TUD_HID_DESCRIPTOR` with only interrupt IN and still implements the Output
`SET_REPORT` callback. This configuration keeps Feature 7/8 intact without
patching the SDK. macOS hidapi sends Output through `IOHIDDeviceSetReport`, so
the host protocol is unchanged. Actual native RPC success requires a device
test after installing this configuration.

USB enumeration does not mean the Codex application is ready. The service owns
the handshake and selection policy. A charge-only connection cannot supply a
Codex RPC handshake. Callbacks only copy reports into a bounded queue; the
service parses them. Mounted/unmounted events identify sessions with an epoch,
so reports from an earlier cable connection cannot update a new host's state.
Queue overflow emits an explicit event for RPC resynchronization.

Outbound reports carry the service's accepted session epoch. A single pending
report is submitted on the TinyUSB task through its Start Of Frame callback,
serializing the final session check with USB lifecycle events. Every USB write
in one shared service-worker iteration uses the same 8 ms deadline, including
all fragments and requests. The final RTOS wait may add one tick (1 ms in this
build). A timeout cancels its request token; any failed attempted write
disconnects USB before a later message can reuse a partial stream or leave an
uncertain single-report key press held. A call rejected before attempting a
write because the cycle budget is already empty does not disconnect USB.
This bounds USB congestion without blocking Friday's BLE context and travel for a
full sequence of per-report timeouts, and prevents an old host's command from
following a newly enumerated host.

## Build separately

The regular configuration keeps USB HID disabled. Use a separate build
directory and sdkconfig file; the following command does not flash anything:

```sh
idf.py -B build-usb \
  -D SDKCONFIG=build-usb/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.usb.defaults' build
```

Use the project's ESP-IDF 5.5.4 environment. The lockfile records the tested
managed USB dependencies. If reusing a previous `build-usb/sdkconfig`, inspect
its values because an existing sdkconfig takes precedence over defaults.

After building, verify the actual ELF configuration and report descriptors:

```sh
python3 tests/usb_configuration_descriptor_test.py build-usb/StopWatch-UserDemo.elf
```

The check reads the linked descriptor bytes, requires exactly one interrupt IN
endpoint and no interrupt OUT endpoint, and verifies report 6 Output plus
Feature reports 7/8. It rejects the previous IN/OUT configuration.

## Hardware and recovery

C152's native USB signals use ESP32-S3 GPIO19 (D−) and GPIO20 (D+). The expansion
connector's UART/USB multiplexer is separate from USB transport selection in
the Codex service. This implementation does not change that multiplexer.
[M5Stack hardware documentation](https://docs.m5stack.com/en/core/StopWatch)
and [C152 schematic](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1242/C152-SCH_Stopwatch_PRJ_Main_VA_20251201_2026_04_24_17_46_22.pdf).

USB OTG and hardware USB Serial/JTAG share the internal PHY. When this image
starts USB HID, the ordinary hardware USB Serial/JTAG port is unavailable.
The USB configuration disables that console and retains UART console output.
It does not change bootloader partitions or burn eFuses.
[Espressif USB documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/usb_device.html).

Before deployment, verify C152's physical download-mode recovery: connect USB,
hold the power button for about two seconds until the green LED lights, then
release. M5Stack documents this as entry to download mode. Confirm the actual
device and rollback image before performing any firmware write.
[M5Stack download-mode instructions](https://docs.m5stack.com/en/core/StopWatch).

This board is battery powered and this integration has no dedicated VBUS sense
GPIO. USB suspend therefore makes the USB endpoint unavailable; resume starts
a fresh session. Wake and software recovery retain a manual host choice only
after the new session confirms the same stable host ID. A different Mac connected
during recovery receives normal USB priority; repeated recovery before identity
arrives retains the previous comparison ID. A normal new attachment receives USB
priority after its identity and RPC handshake. This deliberately treats host sleep and cable removal as
loss of the usable transport. Actual suspend/resume and unplug behavior still
requires hardware verification; compilation alone does not prove it.

## Physical acceptance checks

- Native desktop recognizes the USB device, completes RPC handshake, and the
  screen displays the USB host. Merely seeing an HID device is insufficient.
- With two Macs present, controls and task/quota updates belong to one host;
  connecting a charger does not switch hosts.
- Unplug USB, reconnect it to another Mac, and suspend/wake the USB host. Old
  queued RPC fragments and host state must not survive the session change.
- Switch hosts while a button is held. The old host receives release or the
  old endpoint is disconnected before new controls are enabled.
- Confirm physical download-mode entry and the agreed OTA rollback procedure
  separately from application behavior.
