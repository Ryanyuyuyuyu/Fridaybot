#!/bin/sh
set -eu

test_repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_build_dir=$(mktemp -d /tmp/codex-chat-mode-tests.XXXXXX)
trap 'rm -rf "$test_build_dir"' EXIT

test_cxx=${CXX:-c++}
"$test_cxx" -std=c++17 -Wall -Wextra -Werror -pedantic \
    -I"$test_repo_root" \
    "$test_repo_root/tests/codex_micro_mode_test.cpp" \
    -o "$test_build_dir/codex_micro_mode_test"

"$test_build_dir/codex_micro_mode_test"
