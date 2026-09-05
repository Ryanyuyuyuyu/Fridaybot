# Codex Micro USB transport

The USB variant presents the same vendor HID collection as the Bluetooth
Codex Micro endpoint: VID `303A`, PID `8360`, usage page `FF00`, usage `1`.
Native desktop RPC uses report ID `6`, with 63 body bytes and one Report ID
byte on USB. Both interrupt OUT and control SET_REPORT writes are accepted.
The USB serial string is derived from the chip's factory MAC address.

The separate host identity helper uses atomic Feature reports. It never writes
RPC report 6, so two host processes cannot interleave one JSON RPC message.

| Report | Type | Body bytes | Contents |
| --- | --- | --- | --- |
| 6 | Input / Output | 63 | Existing Codex Micro RPC framing |
| 7 | Feature | 128 | NUL-padded UTF-8 identity JSON |
| 8 | Feature | 256 | NUL-padded UTF-8 quota JSON |

Numbered macOS `IOHIDDeviceSetReport` buffers include the Report ID before the
body. TinyUSB removes that ID for a control SET_REPORT callback. Interrupt OUT
callbacks are normalized by the transport. Feature reports must have the exact
advertised body length. The HID interrupt endpoints remain 64 bytes; TinyUSB's
control scratch buffer is 257 bytes to accommodate the quota Feature report.

USB enumeration does not mean the Codex application is ready. The service owns
the handshake and selection policy. A charge-only connection cannot supply a
Codex RPC handshake. Callbacks only copy reports into a bounded queue; the
service parses them. Mounted/unmounted events identify sessions with an epoch,
so reports from an earlier cable connection cannot update a new host's state.
Queue overflow emits an explicit event for RPC resynchronization.

Outbound reports carry the service's accepted session epoch. A single pending
report is submitted on the TinyUSB task through its Start Of Frame callback,
serializing the final session check with USB lifecycle events. The caller waits
at most 100 ms and cancels the request token on timeout. This prevents a command
queued for the old USB host from following a newly enumerated host.

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
a fresh session. This deliberately treats host sleep and cable removal as
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
