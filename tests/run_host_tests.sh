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

compile_and_run face_model_test \
    "$test_repo_root/tests/face_model_test.cpp" \
    "$test_repo_root/main/apps/app_friday/face_model.cpp"

compile_and_run context_protocol_test \
    "$test_repo_root/tests/context_protocol_test.cpp"

compile_and_run presence_protocol_test \
    "$test_repo_root/tests/presence_protocol_test.cpp"

compile_and_run capsule_codec_test \
    "$test_repo_root/tests/capsule_codec_test.cpp" \
    "$test_repo_root/main/apps/app_friday/capsule/capsule_codec.cpp"

compile_and_run context_link_bridge_test \
    "$test_repo_root/tests/context_link_bridge_test.cpp" \
    "$test_repo_root/main/apps/app_friday/context_link.cpp"

"$test_python" "$test_repo_root/tests/firmware_integration_contract_test.py" "$test_repo_root"
