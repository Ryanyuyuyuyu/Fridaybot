# USB connected but buttons did not control the Mac

On 2026-09-06 the user reported local button feedback with no Mac action. The
USB Codex Micro interface was present, but the installed CodexHostIdentity
helper was not running. The helper had previously been launched manually and
had no installed login startup job.

The firmware requires both native RPC readiness and a host identity for each
USB session. RPC responses go directly to the requesting connection; button
notifications use the selected route. Successful initialization therefore does
not prove that physical controls have a destination.

Starting the existing helper delivered its USB Feature 7 identity and confirmed
its encrypted BLE identity. A passive listener then captured physical A1, A2,
and A3 presses and releases over USB (`AG00`, `AG01`, `AG02`, actions 1 and 0).
The native app followed these events with task-status RPCs. The user confirmed
that task switching now worked. No synthetic key events were sent.

The fix installs the dedicated user's
`com.friday.codex-host-identity` LaunchAgent using
[`manage-startup.sh`](../companion/macos/host_identity/scripts/manage-startup.sh).
It launches the installed helper at login, retries abnormal exits with a
30-second throttle, and writes logs to `~/Library/Logs/CodexHostIdentity/`.
The script checks for a manually running instance before takeover. Repeating
installation with the same loaded job leaves its process unchanged.

Validation completed:

- Shell syntax and generated plist checks passed.
- The actual LaunchAgent reached `running`; only one helper process existed.
- USB identity delivery and encrypted BLE identity acceptance succeeded after
  launchd took over.
- Repeated installation was idempotent, and the saved UUID was unchanged.
- The user confirmed physical task switching before the startup handover.

This fix changes only the dedicated helper's startup setup and documentation.
It does not reflash the hardware, change firmware, replace Friday Companion,
or modify other login jobs. Firmware remains the previously verified `.8`.
Two-Mac exclusivity and unplug/fallback behavior remain separate tests.

Use `manage-startup.sh --uninstall` to stop and remove this startup job while
preserving the app, identity, alias, and logs. See the helper's
[startup and troubleshooting instructions](../companion/macos/host_identity/README.md).
