#!/bin/sh
set -eu

qa_repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
qa_output_dir=${1:-$(mktemp -d "${TMPDIR:-/tmp}/friday-lvgl-qa.XXXXXX")}
mkdir -p "$qa_output_dir"
qa_output_dir=$(CDPATH= cd -- "$qa_output_dir" && pwd)
qa_cmake=${CMAKE:-cmake}
if ! command -v "$qa_cmake" >/dev/null 2>&1; then
    # Reuse a local ESP-IDF CMake installation when it is outside PATH.
    for qa_candidate in "$qa_repo_root"/../.toolchains/espressif-tools/tools/cmake/*/CMake.app/Contents/bin/cmake; do
        if [ -x "$qa_candidate" ]; then
            qa_cmake=$qa_candidate
            break
        fi
    done
fi

if ! command -v "$qa_cmake" >/dev/null 2>&1; then
    printf '%s\n' 'CMake is required. Set CMAKE to an existing cmake executable.' >&2
    exit 1
fi

"$qa_cmake" -S "$qa_repo_root/tests/lvgl_host_picker_qa" -B "$qa_output_dir/build" \
    -DCMAKE_BUILD_TYPE=Release
"$qa_cmake" --build "$qa_output_dir/build" --target host_picker_qa --parallel "${QA_JOBS:-2}"
"$qa_output_dir/build/host_picker_qa" "$qa_output_dir"
if command -v sips >/dev/null 2>&1; then
    for qa_frame in "$qa_output_dir"/*.ppm; do
        sips -s format png "$qa_frame" --out "${qa_frame%.ppm}.png" >/dev/null
    done
fi
printf 'LVGL screenshots: %s\n' "$qa_output_dir"
