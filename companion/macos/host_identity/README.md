# Codex Host Identity for macOS

This background helper gives the StopWatch one stable identity for the current
Mac over USB and Bluetooth. The firmware uses that identity to choose a single
Codex host, prefer a data-capable USB connection, and return to the same Mac over
Bluetooth after USB is unplugged. The native Codex app continues handling its
normal HID controls and task state. An identity announcement alone does not
prove that Codex is ready to handle controls.

The helper also reads Codex usage limits and sends them over USB when available,
so the device's quota display can work with Bluetooth off. Install this helper
on each Mac that should appear by name in the device selector. Friday's existing
companion remains separate and is not changed by this helper.

## Build and offline verification

Requires macOS 13 or later, Xcode Command Line Tools, and Python 3 for the fake
app-server test. From this directory:

```sh
make
make test
# Repackage explicitly after a build:
make package
```

`make` / `make package` produces `build/CodexHostIdentity.zip`; it does not launch
or install the app. Packaging copies the generated app without resource forks or
extended attributes into a temporary directory outside iCloud, ad-hoc signs and
verifies that copy, then verifies the ZIP after extracting it into a second
temporary directory. The `sign` target is an alias for this packaging workflow.

Use the ZIP as the deliverable. The intermediate `build/CodexHostIdentity.app`
inside Documents can acquire iCloud/File Provider metadata after compilation or
signing, which makes strict signature checks fail. Its signature is not the
packaged-app verification result. `make test` checks Unicode-safe identity encoding, file
permissions/persistence, quota selection and malformed inputs, and four local
fake app-server scenarios. Tests neither connect to hardware nor start real
Codex, request permissions, read credentials, or create the real host identity.

## Run manually

Extract `build/CodexHostIdentity.zip` outside iCloud and copy the extracted app to
`~/Applications/`. The following manual commands extract it directly there.
After the new firmware has been installed and its USB/Bluetooth links are ready,
verify the installed copy and launch it:

```sh
mkdir -p "$HOME/Applications"
ditto -x -k --norsrc --noextattr build/CodexHostIdentity.zip "$HOME/Applications"
codesign --verify --strict "$HOME/Applications/CodexHostIdentity.app"
open "$HOME/Applications/CodexHostIdentity.app"
```

The app has no Dock icon. macOS may ask for Bluetooth permission. Allow **Codex
Host Identity** in System Settings > Privacy & Security > Bluetooth. Launching
with `open` associates that permission with the app bundle. Stop the process
`codex-host-identity` in Activity Monitor when needed. Only one copy runs per
macOS user; a second process exits without connecting.

Optional commands:

```sh
# Show CLI options or run offline checks.
"$HOME/Applications/CodexHostIdentity.app/Contents/MacOS/codex-host-identity" --help
"$HOME/Applications/CodexHostIdentity.app/Contents/MacOS/codex-host-identity" --self-test

# Print/create this Mac user's identity without accessing hardware.
"$HOME/Applications/CodexHostIdentity.app/Contents/MacOS/codex-host-identity" --show-identity

# Identity only, for use alongside an existing quota helper.
open "$HOME/Applications/CodexHostIdentity.app" --args --no-quota

# Explicit Codex executable when automatic discovery cannot find it.
open "$HOME/Applications/CodexHostIdentity.app" --args --codex-bin /absolute/path/to/codex
```

Stop any running copy before changing startup arguments. Default Codex discovery
checks executable entries in `PATH`, installed Codex/ChatGPT app resources, and
the usual Homebrew/local binary paths. Quota reads use the Mac's existing Codex
sign-in through `app-server`, with only `initialize`, `initialized`, and
`account/rateLimits/read`. Each temporary child exits after the read. No login,
chat, task, approval, purchase, reset, or thread API is called. Failures leave
identity/control operation available and are logged; unavailable quota is not
reported as zero. Reads start after a compatible device accepts identity, then
repeat at most once a minute. Use this helper as the quota writer when testing
USB-only operation; older BLE-only quota helpers do not prioritize USB.

For automatic startup after manual installation, optionally add the app in
System Settings > General > Login Items. This repository does not install a LaunchAgent or change
Login Items automatically. Rebuilding an ad-hoc signed app may require renewing
its Bluetooth permission.

## Protocol

| Path | Payload | Limits |
| --- | --- | --- |
| BLE identity service `7F0D4E66-2AC2-4A71-BFBE-4EF61A0E5C04`, characteristic `...5C03` | `{"version":1,"hostId":"generated-uuid","name":"LocalHostName"}` | Write with response; firmware requires encryption; JSON shorter than 128 bytes |
| USB HID feature report 7 | Same identity JSON, followed by NUL padding | 128-byte body; macOS buffer is report-ID byte plus body, 129 bytes total |
| USB HID feature report 8 | Quota JSON, followed by NUL padding | 256-byte body; macOS buffer is 257 bytes total |
| BLE quota service `...5C01`, characteristic `...5C02` | Same quota JSON, without padding | Used when no USB identity report is being delivered |

USB matching requires transport USB, VID `0x303A`, PID `0x8360`, usage page
`0xFF00`, usage 1, and feature report 7. The helper opens devices without seizing
them and never sends or subscribes to Codex report 6. Identity is refreshed every
five seconds on both transports. BLE service discovery retries after connection
failures, with a 30-second handshake timeout. It sends complete JSON only when
CoreBluetooth's negotiated write limit permits it.

The identity service is appended as the sixth GATT service. The existing device
information, HID, battery, quota, and Friday service order and handle counts stay
unchanged, preserving the cached Friday handles of already paired Macs. The
helper scans the existing advertised quota service, then discovers the separate
identity and quota services. A missing identity service triggers one explicit
service-UUID discovery retry. This upgrade does not clear Bluetooth bonds or
device NVS. USB report numbers and payloads are unchanged.

Quota fields are `version`, `remaining_percent`, `reset_in_seconds`, and optional
`five_hour_used_percent` / `weekly_used_percent`. Named rings require matching
300-minute and 10080-minute windows; ambiguous or missing windows are omitted.
The helper does not change which Mac is active. The firmware makes that choice
and ignores controls/status from other hosts.

The UUID is generated locally once and stored with mode `0600` in:

```text
~/Library/Application Support/Friday/Codex Host Identity/host-id
```

The containing directory is private (`0700`). Identity survives helper/app
updates. The displayed name follows macOS `LocalHostName`, shortened to 31 UTF-8
bytes without splitting Unicode characters. Separate macOS user accounts keep
separate identities. Do not copy the identity file to another Mac. If it is
corrupted, restore it from a local backup; the helper refuses to silently create
a replacement because that would change remembered device selection. These
identity announcements are routing metadata, not authentication credentials;
BLE pairing and the physical USB connection remain the transport trust boundary.

## Verification status and device checks

Compilation and offline tests do not establish physical USB enumeration,
CoreBluetooth pairing, native Codex report-6 compatibility, quota delivery, or
cross-Mac switching. Those require the matching firmware and real-device tests:

1. Run one helper on each Mac, pair BLE, and verify distinct device names.
2. Select Mac A on the StopWatch and check that only A receives one complete
   press/release; ensure B's task updates cannot overwrite A's display.
3. Connect USB to B while native Codex is running. Verify B becomes active and
   report-6 controls/task state still work. With Bluetooth off, check USB quota.
4. Unplug USB. Verify the device returns only to B over BLE, or remains offline
   if B is unavailable. It should not silently control A.
5. Repeat a manual override and plug-only charging test, then hold/release an
   input across a deliberate switch and check for stuck actions.
