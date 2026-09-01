# Friday for M5Stack StopWatch

Friday is an original, monochrome desk companion built on the M5Stack
StopWatch user demo. It boots directly into a responsive two-eye face while
preserving the upstream launcher and hardware evaluation apps.

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

## Build

### Fetch Dependencies

```bash
python3 ./fetch_repos.py
```

### Tool Chains

[ESP-IDF v5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/index.html)

### Build

```bash
idf.py build
```

### Flash

```bash
idf.py flash
```

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
