# Friday Companion for macOS

This local helper turns coarse Mac activity into optional BLE context for
Friday and provides the first StopWatch-to-Mac travel portal. Friday remains
fully functional when the helper is stopped or the BLE link is unavailable.

For presence detection, the helper reads only:

- seconds since the last keyboard or pointing-device event;
- whether the current macOS session is locked.

It does not read keystrokes, window titles, URLs, documents, screenshots,
camera frames or microphone audio. While Friday is visible on the Mac, the
overlay reads the current pointer position so its eyes can look toward it. The
position is neither stored nor sent to the StopWatch.

## Build and run

```sh
cd companion/macos
make
build/FridayCompanion.app/Contents/MacOS/friday-companion --monitor-side left
```

`--monitor-side` describes the monitor's position from Friday's point of view,
not Friday's position from yours. Use `left`, `centre`, or `right`.

macOS may ask for Bluetooth permission the first time. The helper automatically
reconnects to the BLE peripheral named `Friday`. Stop it with Control-C; Friday
will immediately return to local behaviour.

## Cross-device travel prototype

1. Drag Friday to the StopWatch bezel edge on the side facing the Mac and
   release.
2. The StopWatch exit animation completes and its display becomes black.
3. Only after the StopWatch confirms its face is hidden, Friday peeks halfway
   around the matching outer Mac screen edge, watches briefly, retreats, then
   sneaks onto the desktop. Its capsule eyes animate locally at 60 FPS.
4. Click Friday for a small reaction. Drag it around the desktop or through the
   connected Mac displays; drag it back to its original outer portal edge to
   send it home. A single StopWatch A or B button press also recalls it.

The companion runs the full AppKit event loop while remaining an accessory app
without a Dock icon. The transparent non-activating panel explicitly owns its
hit region and mouse tracking session, so holding anywhere on Friday's circular
body keeps the panel attached to the pointer across display boundaries.

With `--monitor-side left` or `right`, only that StopWatch edge is a portal;
the opposite edge keeps its normal spring interaction. With `centre`, either
edge works. Pass `--no-overlay` to retain context sync without enabling travel.

Travel uses a separate ten-byte BLE state packet, never streamed frames. Its
version-2 protocol has visibility barriers in both directions: `DeviceHidden`
permits the Mac's first frame and `HostHidden` permits the StopWatch's first
return frame. One `NSPanel` moves across all connected displays, so a monitor
boundary cannot duplicate Friday. If an acknowledgement times out or BLE
disconnects, Friday automatically returns to the StopWatch. The StopWatch
temporarily requests a responsive BLE interval only while a handoff is moving;
once either endpoint owns an idle Friday, it restores the relaxed low-duty
connection used for presence heartbeats.

To verify context detection without connecting over BLE:

```sh
build/FridayCompanion.app/Contents/MacOS/friday-companion --dry-run
```

For hardware testing, a state can be held without waiting for the normal idle
threshold. This is a diagnostic override, not required for everyday use:

```sh
build/FridayCompanion.app/Contents/MacOS/friday-companion --force-state away
```

## Current inference thresholds

- under 15 seconds idle: `working`;
- 15 seconds to 5 minutes idle with an unlocked screen: `uncertain`;
- five minutes idle or a locked session: `away`;
- first activity after `away`: `returned` for three seconds.

The four-byte context packet is sent only on state changes and as a 15-second
heartbeat. Friday discards it after 45 seconds without a valid packet. This
channel is independent of the travel packet and continues to work when the
overlay is disabled.
