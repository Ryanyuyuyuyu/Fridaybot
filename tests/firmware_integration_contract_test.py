#!/usr/bin/env python3
"""Host-only source contracts for the combined Friday + Codex firmware.

These checks intentionally avoid ESP-IDF headers and hardware.  They protect
the integration seams that are otherwise easy to break while resolving the
two original firmwares: launcher registration, one Bluetooth host, Friday's
public protocol identity, legacy advertising limits, and the user-verified
Codex f7076ff (".2") dual-ring data flow.
"""

from __future__ import annotations

import ast
import hashlib
import re
import sys
from pathlib import Path
from typing import Iterable


class Contract:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.failures.append(message)

    def finish(self) -> None:
        if self.failures:
            for failure in self.failures:
                print(f"FAIL: {failure}", file=sys.stderr)
            raise SystemExit(1)
        print("firmware_integration_contract_test: PASS")


def read(root: Path, relative: str) -> str:
    path = root / relative
    try:
        return path.read_text(encoding="utf-8")
    except OSError as error:
        raise SystemExit(f"cannot read {path}: {error}") from error


def sha256(root: Path, relative: str) -> str:
    path = root / relative
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError as error:
        raise SystemExit(f"cannot read {path}: {error}") from error


def strip_cpp_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", source)


def active_config(source: str, key: str) -> bool:
    return re.search(rf"(?m)^\s*{re.escape(key)}\s*=\s*y\s*$", source) is not None


def contains_subsequence(values: Iterable[int], expected: bytes) -> bool:
    data = bytes(values)
    return expected in data


def parse_byte_token(token: str) -> int:
    token = token.strip()
    if re.fullmatch(r"0[xX][0-9A-Fa-f]+|[0-9]+", token):
        value = int(token, 0)
    elif re.fullmatch(r"'(?:\\.|[^\\'])'", token):
        decoded = ast.literal_eval(token)
        value = ord(decoded)
    else:
        raise ValueError(f"unsupported byte literal {token!r}")
    if not 0 <= value <= 0xFF:
        raise ValueError(f"byte literal out of range: {token!r}")
    return value


def byte_arrays(source: str) -> dict[str, list[int]]:
    """Extract simple kNamed byte arrays used by the raw GATT/ADV tables."""

    source = strip_cpp_comments(source)
    result: dict[str, list[int]] = {}
    pattern = re.compile(
        r"\b(?P<name>k[A-Za-z0-9_]+)\s*(?:\[[^\]]*\])?\s*=\s*\{(?P<body>[^{}]*)\}\s*;",
        flags=re.DOTALL,
    )
    for match in pattern.finditer(source):
        tokens = [token.strip() for token in match.group("body").split(",") if token.strip()]
        try:
            result[match.group("name")] = [parse_byte_token(token) for token in tokens]
        except ValueError:
            # Attribute tables and other non-literal initializers also match
            # the broad expression.  They are not raw byte arrays.
            continue
    return result


def advertising_fields(payload: list[int], label: str, contract: Contract) -> list[tuple[int, bytes]]:
    contract.require(len(payload) <= 31, f"{label} is {len(payload)} bytes; legacy BLE permits at most 31")
    fields: list[tuple[int, bytes]] = []
    cursor = 0
    while cursor < len(payload):
        field_length = payload[cursor]
        if field_length == 0:
            contract.require(False, f"{label} contains a zero-length AD field at byte {cursor}")
            break
        end = cursor + field_length + 1
        if end > len(payload):
            contract.require(False, f"{label} AD field at byte {cursor} overruns the payload")
            break
        fields.append((payload[cursor + 1], bytes(payload[cursor + 2 : end])))
        cursor = end
    contract.require(cursor == len(payload), f"{label} does not end on an AD-field boundary")
    return fields


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        return ""
    opening = source.find("{", start)
    if opening < 0:
        return ""
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    return ""


def test_launcher(root: Path, contract: Contract) -> None:
    main = strip_cpp_comments(read(root, "main/main.cpp"))
    apps = read(root, "main/apps/apps.h")
    launcher = strip_cpp_comments(read(root, "main/apps/app_launcher/app_launcher.cpp"))
    positions: dict[str, int] = {}
    app_headers = {
        "AppLauncher": "app_launcher/app_launcher.h",
        "AppFriday": "app_friday/app_friday.h",
        "AppCodexMicro": "app_codex_micro/app_codex_micro.h",
    }
    for app, header in app_headers.items():
        expression = re.compile(
            rf"installApp\s*\(\s*std::make_unique\s*<\s*{app}\s*>\s*\(\s*\)\s*\)\s*;"
        )
        matches = list(expression.finditer(main))
        contract.require(len(matches) == 1, f"main must install {app} exactly once (found {len(matches)})")
        if matches:
            positions[app] = matches[0].start()
        contract.require(header in apps, f"apps.h must include {header}")

    if len(positions) == 3:
        contract.require(
            positions["AppLauncher"] < positions["AppFriday"] and positions["AppLauncher"] < positions["AppCodexMicro"],
            "the launcher must be registered before Friday and Codex",
        )

    installed_apps = re.findall(r"installApp\s*\(\s*std::make_unique\s*<\s*(App[A-Za-z0-9_]+)\s*>", main)
    contract.require(
        installed_apps[:3] == ["AppLauncher", "AppFriday", "AppCodexMicro"],
        "launcher, Friday, and Codex must be the first three adjacent entries in the visible app list",
    )
    contract.require(
        "open_friday_on_boot" not in launcher and "_should_open_friday" not in launcher,
        "the launcher must stay visible on boot instead of forcing Friday open",
    )


def test_single_bluetooth_host(root: Path, contract: Contract) -> None:
    defaults = read(root, "sdkconfig.defaults")
    contract.require(active_config(defaults, "CONFIG_BT_BLUEDROID_ENABLED"), "Bluedroid must be the one BLE host")
    contract.require(active_config(defaults, "CONFIG_BT_BLE_ENABLED"), "BLE must remain enabled")
    contract.require(active_config(defaults, "CONFIG_BT_GATTS_ENABLE"), "the shared Bluedroid GATT server must be enabled")
    contract.require(
        not active_config(defaults, "CONFIG_BT_NIMBLE_ENABLED"),
        "NimBLE and Bluedroid are mutually exclusive; sdkconfig.defaults must not enable NimBLE",
    )

    context_link = strip_cpp_comments(read(root, "main/apps/app_friday/context_link.cpp"))
    forbidden = (
        "<host/ble_",
        "<nimble/",
        "nimble_port_",
        "ble_hs_",
        "ble_gap_",
        "ble_gatts_",
        "ble_svc_",
        "os_mbuf",
    )
    for token in forbidden:
        contract.require(token not in context_link, f"Friday ContextLink still owns NimBLE symbol {token!r}")

    service = strip_cpp_comments(read(root, "main/services/codex_micro/codex_micro_service.cpp"))
    contract.require(
        "app_friday/context_link.h" in service or "apps/app_friday/context_link.h" in service,
        "the shared Codex BLE service must bridge into Friday ContextLink",
    )
    for callback in ("acceptPacket", "acceptTransferPacket", "setConnected", "setTransferSubscribed"):
        contract.require(callback in service, f"shared BLE service does not route Friday {callback} events")

    send_body = function_body(context_link, "bool ContextLink::sendTransfer")
    contract.require(bool(send_body), "ContextLink::sendTransfer implementation is missing")
    contract.require(
        "codex_micro::" in send_body,
        "Friday transfer notifications must be sent through the shared Codex/Bluedroid service",
    )


def test_friday_gatt_and_advertising(root: Path, contract: Contract) -> None:
    gatt = read(root, "main/services/codex_micro/codex_micro_gatt_db.h")
    service = read(root, "main/services/codex_micro/codex_micro_service.cpp")
    companion = read(root, "companion/macos/friday_companion.m")
    arrays = byte_arrays(gatt)

    # ESP-IDF's 128-bit GATT/advertising representation is Bluetooth
    # little-endian, the reverse of the canonical UUID printed by CoreBluetooth.
    quota_uuid = bytes.fromhex("015c0e1af64ebebf714ac22a664e0d7f")
    friday_service = bytes.fromhex("fb349b5f800000800100594144495246")
    friday_context = bytes.fromhex("fb349b5f800000800200594144495246")
    friday_transfer = bytes.fromhex("fb349b5f800000800300594144495246")
    friday_capsule_data = bytes.fromhex("fb349b5f800000800400594144495246")
    friday_capsule_ack = bytes.fromhex("fb349b5f800000800500594144495246")

    literal_arrays = list(arrays.values())
    for name, value in (
        ("service", friday_service),
        ("context characteristic", friday_context),
        ("transfer characteristic", friday_transfer),
        ("Flash Capsule data characteristic", friday_capsule_data),
        ("Flash Capsule acknowledgement characteristic", friday_capsule_ack),
    ):
        contract.require(
            any(contains_subsequence(array, value) for array in literal_arrays),
            f"shared GATT table is missing Friday {name} UUID bytes",
        )

    for canonical in (
        "46524944-4159-0001-8000-00805F9B34FB",
        "46524944-4159-0002-8000-00805F9B34FB",
        "46524944-4159-0003-8000-00805F9B34FB",
        "46524944-4159-0004-8000-00805F9B34FB",
        "46524944-4159-0005-8000-00805F9B34FB",
    ):
        contract.require(canonical in companion, f"Friday companion lost canonical UUID {canonical}")

    contract.require(
        re.search(r"\bFriday[A-Za-z0-9_]*Db\b|\bkFriday[A-Za-z0-9_]*(?:Count|Service)\b", service) is not None,
        "shared BLE service must create/start Friday's GATT attribute table",
    )

    friday_db = function_body(gatt, "inline const esp_gatts_attr_db_t kFridayDb")
    contract.require(bool(friday_db), "Friday GATT attribute table is missing")
    for token, description in (
        ("kPropertyReadWrite", "read/write context characteristic"),
        ("kPropertyWriteNotify", "write/notify transfer characteristic"),
        ("kFridayTransferCccd", "transfer notification CCCD"),
        ("kFridayCapsuleDataCccd", "Flash Capsule notification CCCD"),
        ("ESP_GATT_PERM_READ_ENCRYPTED", "encrypted Flash Capsule audio characteristic"),
        ("ESP_GATT_PERM_WRITE_ENCRYPTED", "encrypted Flash Capsule acknowledgement characteristic"),
    ):
        contract.require(token in friday_db, f"Friday GATT table is missing its {description}")

    advertising = arrays.get("kAdvertisingData")
    scan_response = arrays.get("kScanResponseData")
    contract.require(advertising is not None, "kAdvertisingData must remain a raw, testable byte array")
    contract.require(scan_response is not None, "kScanResponseData must remain a raw, testable byte array")
    if advertising is None or scan_response is None:
        return

    fields = advertising_fields(advertising, "advertising data", contract)
    fields += advertising_fields(scan_response, "scan response", contract)
    uuid128_data = b"".join(data for field_type, data in fields if field_type in (0x06, 0x07))
    uuid16_data = b"".join(data for field_type, data in fields if field_type in (0x02, 0x03))
    names = [data for field_type, data in fields if field_type in (0x08, 0x09)]

    contract.require(quota_uuid in uuid128_data, "Codex quota UUID must remain advertised")
    contract.require(friday_service in uuid128_data, "Friday service UUID must be advertised for companion discovery")
    contract.require(bytes.fromhex("1218") in uuid16_data, "HID service UUID 0x1812 must remain advertised")
    contract.require(b"Codex Micro" in names, "the paired Codex Micro device name must remain in scan response")


def test_flash_capsule_contract(root: Path, contract: Contract) -> None:
    app = strip_cpp_comments(read(root, "main/apps/app_friday/app_friday.cpp"))
    app_header = strip_cpp_comments(read(root, "main/apps/app_friday/app_friday.h"))
    protocol = strip_cpp_comments(read(root, "main/apps/app_friday/capsule/capsule_protocol.h"))
    codec = strip_cpp_comments(read(root, "main/apps/app_friday/capsule/capsule_codec.cpp"))
    service = strip_cpp_comments(read(root, "main/services/codex_micro/codex_micro_service.cpp"))
    store = strip_cpp_comments(read(root, "companion/macos/friday_capsule_store.m"))
    inbox = strip_cpp_comments(read(root, "companion/macos/friday_capsule_inbox.m"))
    companion_makefile = read(root, "companion/macos/Makefile")
    companion_plist = read(root, "companion/macos/Info.plist")

    input_body = function_body(app, "void AppFriday::handleInputs")
    contract.require(bool(input_body), "Friday input handler is missing")
    contract.require("beginCapsule(nowMs)" in input_body, "Friday A press must start a Flash Capsule")
    contract.require(
        "FridayCapsuleEndReleased" in input_body and "finishCapsule(reason, nowMs)" in input_body,
        "Friday A release must end the active Flash Capsule",
    )
    contract.require(
        "Reaction::ButtonA" not in input_body,
        "Friday A must not retain its old short-press animation",
    )
    contract.require(
        "Reaction::ButtonB" in input_body,
        "Friday B interaction must remain available independently of Flash Capsule",
    )
    contract.require(
        re.search(r"CapsuleMinimumMs\s*=\s*800", app_header) is not None,
        "Flash Capsule accidental-press threshold must stay at 800 ms",
    )
    contract.require(
        re.search(r"FRIDAY_CAPSULE_MAX_DURATION_MS\s*=\s*60000", protocol) is not None,
        "Flash Capsule recording limit must stay at 60 seconds",
    )
    contract.require(
        "audioRecord(_capsule_input, FRIDAY_CAPSULE_CAPTURE_CHUNK_MS, CapsuleMicGainDb)" in app,
        "Friday must record built-in microphone audio in streaming chunks with its own gain",
    )
    contract.require(
        re.search(r"CapsuleMicGainDb\s*=\s*12\.0f", app_header) is not None,
        "Flash Capsule microphone gain must remain below the noisy 30 dB HAL default",
    )
    contract.require(
        "friday_capsule_resampler_reset" in app,
        "Flash Capsule must reset its anti-alias resampler at the start of every recording",
    )
    contract.require(
        "int32_t accumulator" in codec and "int64_t accumulator" not in codec,
        "the embedded anti-alias filter must use real-time 32-bit accumulation",
    )
    contract.require(
        "friday_capsule_resample_44100_to_16000" in app and "friday_capsule_adpcm_encode" in app,
        "Friday must resample and encode each microphone chunk before BLE streaming",
    )
    contract.require(
        "_statusItem.button.title" not in inbox and "NSSquareStatusItemLength" in inbox,
        "the Flash Capsule menu bar item must stay icon-only without an item-count badge",
    )
    contract.require(
        "link.ready()" in function_body(app, "void AppFriday::beginCapsule"),
        "Friday must refuse to record when the paired Mac capsule channel is unavailable",
    )

    worker_body = function_body(service, "void Service::Impl::worker()")
    normal_position = worker_body.find("processQueuedCommands()")
    capsule_position = worker_body.find("processCapsuleFrames()")
    contract.require("capsuleQueue" in service, "Flash Capsule must use a dedicated BLE queue")
    contract.require(
        normal_position >= 0 and capsule_position > normal_position,
        "Codex command/release processing must retain priority over Friday audio frames",
    )
    contract.require(
        "connection.secure" in function_body(service, "bool Service::Impl::sendFridayCapsuleNow"),
        "Flash Capsule audio notifications must require an encrypted BLE connection",
    )
    sync_body = function_body(service, "void Service::Impl::syncFridayLinkState")
    contract.require(
        "fridayCapsuleActive" in sync_body and "xQueueReset(capsuleQueue)" in sync_body,
        "a disconnected/replaced Mac must clear Flash Capsule high-rate mode and stale audio",
    )

    for token, description in (
        ("writeData", "incremental temporary audio writes"),
        ("FridayCapsuleWavHeader", "recoverable temporary WAV finalization"),
        ("NSDataWritingAtomic", "atomic daily index updates"),
        ("SFSpeechAudioBufferRecognitionRequest", "live speech transcription"),
        ("appendAudioPCMBuffer", "incremental PCM delivery to speech recognition"),
        ("SFSpeechURLRecognitionRequest", "file-based recovery transcription"),
        ("FridayCapsuleAudioRetentionSeconds", "bounded failed-transcription audio retention"),
        ("audioRetainUntil", "per-item temporary-audio expiry"),
        ("removeItemAtURL", "successful-transcription audio deletion"),
        ("FridayCapsuleCategory", "todo-or-memo classification"),
    ):
        contract.require(token in store, f"Mac Flash Capsule store lost {description}")
    contract.require("今日闪念胶囊" in inbox, "Mac menu-bar inbox must expose today's Flash Capsules")
    contract.require("最多保留 24 小时" in inbox, "Mac inbox must explain bounded recovery-audio retention")
    contract.require("expireStaleCapture" in store, "Mac must delete a stalled partial audio capture")
    contract.require(
        "NSSpeechRecognitionUsageDescription" in companion_plist,
        "the signed Mac app must declare why Speech Recognition is used",
    )
    contract.require(
        "codesign" in companion_makefile and "--identifier com.friday.companion" in companion_makefile,
        "the Mac build must bind Info.plist privacy declarations into the app signature",
    )
    contract.require(
        'open -n "$(APP)"' in companion_makefile,
        "the Mac run target must launch the app bundle so TCC reads its privacy declarations",
    )


def test_codex_ui_source_fingerprint(root: Path, contract: Contract) -> None:
    # User-authorized host switching and its visual integration update this
    # UI/controller baseline, including restoration of the equal Agent circles.
    # Keep a fingerprint boundary for later, unrelated Friday changes. Native
    # LVGL QA and host-selection tests validate the new behavior independently.
    expected = {
        "main/apps/app_codex_micro/app_codex_micro.cpp":
            "ebdf57576ab57da583546e26d40f777ff4e6bc15a0948fd82109b2748bfb53b6",
        "main/apps/app_codex_micro/app_codex_micro.h":
            "d28e3cf1caa41d73627f264c0ac15a0a456937a09d022b5567cf9f57643cec1f",
        "main/apps/app_codex_micro/view/view.cpp":
            "124a16f689fb7f8525abd22c2a69245732239df151ff2add72d10c38454c7c9e",
        "main/apps/app_codex_micro/view/view.h":
            "b2d6a85844074c30b0071d96d99e22d3d3bee7ee9f12bed20e59c6b31c44654d",
    }
    for relative, digest in expected.items():
        contract.require(
            sha256(root, relative) == digest,
            f"Unrelated changes must not modify the verified Codex host-picker baseline: {relative}",
        )


def test_codex_has_no_legacy_chat_dashboard(root: Path, contract: Contract) -> None:
    app_path = "main/apps/app_codex_micro/app_codex_micro.cpp"
    app = strip_cpp_comments(read(root, app_path))
    codex_sources = {
        app_path: app,
        "main/apps/app_codex_micro/app_codex_micro.h": strip_cpp_comments(
            read(root, "main/apps/app_codex_micro/app_codex_micro.h")
        ),
        "main/apps/app_codex_micro/view/view.cpp": strip_cpp_comments(
            read(root, "main/apps/app_codex_micro/view/view.cpp")
        ),
        "main/apps/app_codex_micro/view/view.h": strip_cpp_comments(
            read(root, "main/apps/app_codex_micro/view/view.h")
        ),
    }

    forbidden_tokens = (
        "DashboardMode::Chat",
        "ModeButtonGesture",
        "ToggleMode",
        "toggleDashboardMode",
        "selectChatSlot",
        "ChatSlotVisual",
        "LOCAL PREVIEW",
        "kChatAccentColor",
        "kChatPanelColor",
        "kChatMutedText",
        "_dashboard_mode",
        "_mode_button_gesture",
        "_chat_slots",
        "_selected_chat_slot",
        "local_chat_slots",
        "beginModeTransition",
        "_discard_touch_until_release",
        "_codex_mode_active",
        "model.chats",
    )
    for path, source in codex_sources.items():
        for token in forbidden_tokens:
            contract.require(token not in source, f"{path} still contains legacy Chat token {token!r}")
        contract.require(
            re.search(r"\bDashboardMode\b", source) is None,
            f"{path} still contains the legacy DashboardMode type",
        )

    removed_paths = (
        "main/apps/app_codex_micro/model/chat_mode_model.h",
        "main/apps/app_codex_micro/model/chat_slot_config.h",
        "main/apps/app_codex_micro/model/local_chat_slots.h.example",
        "main/apps/app_codex_micro/model/mode_button_gesture.h",
        "tests/codex_micro_mode_test.cpp",
    )
    for relative in removed_paths:
        contract.require(not (root / relative).exists(), f"legacy Chat-only file still exists: {relative}")

    button_body = function_body(app, "void AppCodexMicro::handlePhysicalButtons")
    contract.require(bool(button_body), "Codex physical-button handler is missing")
    press_guard = re.search(r"if\s*\(\s*left_pressed_edge\s*&&\s*!_left_mic_pressed\s*\)", button_body)
    release_guard = re.search(r"if\s*\(\s*left_released_edge\s*&&\s*_left_mic_pressed\s*\)", button_body)
    press_report = re.search(r"sendKey\s*\(\s*kLeftMicKey\s*,\s*1\s*,\s*-1\s*,\s*_last_connection_epoch\s*\)", button_body)
    release_report = re.search(r"sendKey\s*\(\s*kLeftMicKey\s*,\s*0\s*,\s*-1\s*,\s*_last_connection_epoch\s*\)", button_body)
    contract.require(press_guard is not None and press_report is not None, "physical A must press ACT10 directly")
    contract.require(release_guard is not None and release_report is not None, "physical A must release ACT10 directly")
    chord_position = button_body.find("beginButtonChord()")
    press_position = press_report.start() if press_report is not None else -1
    contract.require(
        chord_position >= 0 and press_position >= 0 and chord_position < press_position,
        "A+B Home chord must be evaluated before physical A can emit ACT10",
    )

    control_contracts = (
        ("beginRightCommandPulse(now)", "physical B command pulse"),
        ("beginAgentPulse(intent.agent, now)", "Agent touch pulse"),
        ("beginSendPulse(now)", "center Send pulse"),
        ("close()", "A+B local launcher return"),
    )
    for token, description in control_contracts:
        contract.require(token in app, f"Codex-only dashboard lost {description}")

    key_contracts = (
        (r'kRightCommandKey\[\]\s*=\s*"ACT09"', "physical B ACT09 mapping"),
        (r'kSendKey\[\]\s*=\s*"ACT12"', "center Send ACT12 mapping"),
        (
            r'"AG00"\s*,\s*"AG01"\s*,\s*"AG02"\s*,\s*"AG03"\s*,\s*"AG04"\s*,\s*"AG05"',
            "six Codex Agent mappings",
        ),
    )
    for pattern, description in key_contracts:
        contract.require(re.search(pattern, app) is not None, f"Codex-only dashboard lost {description}")

    view = codex_sources["main/apps/app_codex_micro/view/view.cpp"]
    contract.require("playSendPulse()" in view, "Codex-only dashboard lost the Send feedback animation")


def test_codex_dot_two_ring_contract(root: Path, contract: Contract) -> None:
    service = strip_cpp_comments(read(root, "main/services/codex_micro/codex_micro_service.cpp"))
    app = strip_cpp_comments(read(root, "main/apps/app_codex_micro/app_codex_micro.cpp"))
    view = strip_cpp_comments(read(root, "main/apps/app_codex_micro/view/view.cpp"))

    for field in ("five_hour_used_percent", "weekly_used_percent"):
        contract.require(f'object["{field}"]' in service, f"Codex .2 input field {field} is missing")

    direct_assignments = (
        r"fiveHourUsedPercent\s*=\s*fiveHourAvailable\s*\?\s*fiveHourUsed\.as\s*<\s*float\s*>\s*\(\s*\)",
        r"weeklyUsedPercent\s*=\s*weeklyAvailable\s*\?\s*weeklyUsed\.as\s*<\s*float\s*>\s*\(\s*\)",
    )
    for expression in direct_assignments:
        contract.require(
            re.search(expression, service) is not None,
            "Codex .2 must preserve incoming used-percent values without the later 100-used conversion",
        )

    for field in ("fiveHourUsedPercent", "weeklyUsedPercent"):
        contract.require(
            re.search(rf"model\.{field}\s*=\s*state\.rateLimits\.{field}\s*;", app) is not None,
            f"Codex .2 app must pass {field} straight to the dashboard",
        )

    update_body = function_body(view, "void DashboardView::updateLimitIndicator")
    contract.require(bool(update_body), "Codex dual-limit update function is missing")
    contract.require(
        re.search(r"std::clamp\s*\(\s*usedPercent\s*,\s*0\.0f\s*,\s*100\.0f\s*\)", update_body) is not None,
        "Codex .2 ring values must clamp the incoming used percentage",
    )
    contract.require(
        re.search(r"100(?:\.0)?f?\s*-\s*std::clamp", update_body) is None,
        "do not mix the later remaining-percentage experiment into the verified Codex .2 UI",
    )

    expected_calls = (
        r"updateLimitIndicator\s*\(\s*_five_hour_limit\s*,\s*model\.fiveHourUsedPercent",
        r"updateLimitIndicator\s*\(\s*_weekly_limit\s*,\s*model\.weeklyUsedPercent",
    )
    for expression in expected_calls:
        contract.require(re.search(expression, view) is not None, "both Codex .2 limit rings must update together")

    for expression, description in (
        (r"makeLimitArc\s*\(\s*_quota_button\s*,\s*200\s*,", "200 px weekly outer ring"),
        (r"makeLimitArc\s*\(\s*_quota_button\s*,\s*182\s*,", "182 px five-hour inner ring"),
        (r'make_readout\s*\(\s*_five_hour_limit\s*,\s*"5H"', "5H readout"),
        (r'make_readout\s*\(\s*_weekly_limit\s*,\s*"WK"', "WK readout"),
    ):
        contract.require(re.search(expression, view) is not None, f"Codex .2 is missing its {description}")


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {Path(sys.argv[0]).name} REPOSITORY_ROOT")
    root = Path(sys.argv[1]).resolve()
    contract = Contract()
    test_launcher(root, contract)
    test_single_bluetooth_host(root, contract)
    test_friday_gatt_and_advertising(root, contract)
    test_flash_capsule_contract(root, contract)
    test_codex_ui_source_fingerprint(root, contract)
    test_codex_has_no_legacy_chat_dashboard(root, contract)
    test_codex_dot_two_ring_contract(root, contract)
    contract.finish()


if __name__ == "__main__":
    main()
