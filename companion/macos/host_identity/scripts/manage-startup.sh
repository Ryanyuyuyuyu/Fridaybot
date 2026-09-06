#!/bin/sh
# SPDX-License-Identifier: MIT
# Optional per-user startup for the dedicated identity helper only.
set -eu
umask 077

startup_label=com.friday.codex-host-identity
startup_app="$HOME/Applications/CodexHostIdentity.app"
startup_binary="$startup_app/Contents/MacOS/codex-host-identity"
startup_agents="$HOME/Library/LaunchAgents"
startup_plist="$startup_agents/$startup_label.plist"
startup_logs="$HOME/Library/Logs/CodexHostIdentity"
startup_target="gui/$(id -u)/$startup_label"

usage() {
    cat <<'USAGE'
Usage: manage-startup.sh --print | --install | --uninstall
  --print      Print and validate the proposed plist; do not install or launch.
  --install    Start this installed helper at login and after an abnormal exit.
  --uninstall  Stop/remove only this startup job; keep the app, identity and logs.

Install the app in ~/Applications and grant its Bluetooth permission first.
Before --install, stop only a manually running copy of codex-host-identity.
No Friday Companion, native Codex app, firmware, or other login item is changed.
USAGE
}

fail() { printf '%s\n' "$*" >&2; exit 1; }

[ "$#" -eq 1 ] || { usage >&2; exit 2; }
case "$1" in
    --help|-h) usage; exit 0 ;;
    --print|--install|--uninstall) startup_action=$1 ;;
    *) usage >&2; exit 2 ;;
esac

# Never overwrite/remove an unrelated file merely because it uses this name.
managed_plist() {
    [ -f "$startup_plist" ] && [ ! -L "$startup_plist" ] &&
    [ "$(/usr/bin/plutil -extract Label raw -o - "$startup_plist" 2>/dev/null)" = "$startup_label" ] &&
    [ "$(/usr/bin/plutil -extract ProgramArguments.0 raw -o - "$startup_plist" 2>/dev/null)" = "$startup_binary" ]
}

job_loaded() { /bin/launchctl print "$startup_target" >/dev/null 2>&1; }

if [ "$startup_action" = --uninstall ]; then
    if [ -e "$startup_plist" ] || [ -L "$startup_plist" ]; then
        managed_plist || fail "Refusing to remove an unrecognized plist: $startup_plist"
        if job_loaded; then /bin/launchctl bootout "$startup_target"; fi
        /bin/rm "$startup_plist"
        printf 'Removed startup job: %s\nApp, UUID, display name and logs were kept.\n' "$startup_label"
    elif job_loaded; then
        fail "Job exists without its managed plist; inspect $startup_target before removing it."
    else
        printf 'Startup job is not installed: %s\n' "$startup_label"
    fi
    exit 0
fi

# Stage outside Documents/iCloud. --print only creates this temporary file.
startup_staged=$(/usr/bin/mktemp /private/tmp/codex-host-startup.XXXXXX)
trap '[ ! -f "$startup_staged" ] || /bin/rm "$startup_staged"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
/usr/bin/plutil -create xml1 "$startup_staged"
/usr/bin/plutil -insert Label -string "$startup_label" "$startup_staged"
/usr/bin/plutil -insert ProgramArguments -array "$startup_staged"
/usr/bin/plutil -insert ProgramArguments.0 -string "$startup_binary" "$startup_staged"
/usr/bin/plutil -insert RunAtLoad -bool YES "$startup_staged"
/usr/bin/plutil -insert KeepAlive -dictionary "$startup_staged"
/usr/bin/plutil -insert KeepAlive.SuccessfulExit -bool NO "$startup_staged"
/usr/bin/plutil -insert ThrottleInterval -integer 30 "$startup_staged"
/usr/bin/plutil -insert LimitLoadToSessionType -string Aqua "$startup_staged"
/usr/bin/plutil -insert ProcessType -string Background "$startup_staged"
/usr/bin/plutil -insert StandardOutPath -string "$startup_logs/stdout.log" "$startup_staged"
/usr/bin/plutil -insert StandardErrorPath -string "$startup_logs/stderr.log" "$startup_staged"
/usr/bin/plutil -lint -s "$startup_staged"

if [ "$startup_action" = --print ]; then
    /bin/cat "$startup_staged"
    exit 0
fi

[ -x "$startup_binary" ] || fail "Install the helper app first: $startup_app"
/usr/bin/codesign --verify --strict "$startup_app"
if [ -e "$startup_plist" ] || [ -L "$startup_plist" ]; then
    managed_plist || fail "Refusing to replace an unrecognized plist: $startup_plist"
    /usr/bin/cmp -s "$startup_staged" "$startup_plist" ||
        fail "Existing startup settings differ; use --uninstall before installing these settings."
fi
if job_loaded; then
    managed_plist || fail "A job already uses $startup_target without this managed plist."
    printf 'Startup job is already loaded: %s\n' "$startup_label"
    exit 0
fi

# Exact executable matching avoids stopping another app or a similarly named job.
# Refuse takeover: a second copy exits nonzero on the helper's singleton lock,
# which would otherwise trigger repeated launchd retries while the manual copy runs.
startup_manual_pids=$(/bin/ps -axo pid=,comm= | /usr/bin/awk -v executable="$startup_binary" '
    { pid=$1; sub(/^[[:space:]]*[0-9]+[[:space:]]+/, ""); if ($0 == executable) print pid }')
[ -z "$startup_manual_pids" ] || fail "Manual helper is running (PID: $startup_manual_pids). Stop only that exact helper, then run --install again."
[ ! -L "$startup_agents" ] && [ ! -L "$startup_logs" ] || fail "LaunchAgents and log directories must not be symlinks."
/bin/mkdir -p "$startup_agents" "$startup_logs"
/bin/chmod 700 "$startup_logs"
/bin/chmod 600 "$startup_staged"
/bin/mv "$startup_staged" "$startup_plist"
if ! /bin/launchctl bootstrap "gui/$(id -u)" "$startup_plist"; then
    /bin/rm "$startup_plist"
    fail "Could not load the startup job; its plist was removed. Run from this user's logged-in desktop session."
fi
printf 'Loaded startup job: %s\nLogs: %s\n' "$startup_label" "$startup_logs"
printf 'RunAtLoad restores it at login; abnormal exits retry after a 30-second throttle.\n'
printf 'Use --uninstall to stop automatic startup before a manual app replacement.\n'
