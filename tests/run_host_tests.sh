#!/bin/sh
set -eu

test_repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_build_dir=$(mktemp -d /tmp/friday-codex-host-tests.XXXXXX)
trap 'rm -rf "$test_build_dir"' EXIT HUP INT TERM

test_cxx=${CXX:-c++}
test_python=${PYTHON:-python3}

compile_and_run() {
    test_name=$1
    shift
    "$test_cxx" -std=c++17 -Wall -Wextra -Werror -pedantic \
        -I"$test_repo_root/tests/stubs" \
        -I"$test_repo_root" \
        "$@" \
        -o "$test_build_dir/$test_name"
    "$test_build_dir/$test_name"
    printf '%s\n' "$test_name: PASS"
}

compile_and_run ble_sessions_test \
    "$test_repo_root/tests/ble_sessions_test.cpp"

compile_and_run host_selection_test \
    "$test_repo_root/tests/host_selection_test.cpp" \
    "$test_repo_root/main/services/codex_micro/host_selection.cpp"

compile_and_run telemetry_projection_test \
    "$test_repo_root/tests/telemetry_projection_test.cpp"

compile_and_run deferred_control_test \
    "$test_repo_root/tests/deferred_control_test.cpp"

compile_and_run usb_disabled_test \
    "$test_repo_root/tests/usb_disabled_test.cpp" \
    "$test_repo_root/main/services/codex_micro/usb_transport.cpp"

compile_and_run usb_descriptor_test \
    "$test_repo_root/tests/usb_descriptor_test.cpp"

compile_and_run usb_write_test \
    "$test_repo_root/tests/usb_write_test.cpp"

compile_and_run usb_resume_policy_test \
    "$test_repo_root/tests/usb_resume_policy_test.cpp" \
    "$test_repo_root/main/services/codex_micro/host_selection.cpp"

compile_and_run face_model_test \
    "$test_repo_root/tests/face_model_test.cpp" \
    "$test_repo_root/main/apps/app_friday/face_model.cpp"

compile_and_run context_protocol_test \
    "$test_repo_root/tests/context_protocol_test.cpp"

compile_and_run presence_protocol_test \
    "$test_repo_root/tests/presence_protocol_test.cpp"

compile_and_run context_link_bridge_test \
    "$test_repo_root/tests/context_link_bridge_test.cpp" \
    "$test_repo_root/main/apps/app_friday/context_link.cpp"

"$test_python" "$test_repo_root/tests/firmware_integration_contract_test.py" "$test_repo_root"
"$test_python" "$test_repo_root/tests/deferred_control_service_test.py"
"$test_python" "$test_repo_root/tests/telemetry_service_test.py"
"$test_python" "$test_repo_root/tests/gatt_handle_compatibility_test.py" "$test_repo_root"
