#!/bin/sh
# SPDX-License-Identifier: MIT
# Sign outside Documents/iCloud, where File Provider may restore Finder xattrs.
set -eu

if [ "$#" -ne 2 ]; then
    printf 'Usage: package.sh INPUT_APP OUTPUT_ZIP\n' >&2
    exit 2
fi

INPUT_APP=$1
OUTPUT_ZIP=$2
PACKAGE_DIR=$(mktemp -d /private/tmp/codex-host-identity-package.XXXXXX)
VERIFY_DIR=
cleanup() {
    rm -rf "$PACKAGE_DIR"
    if [ -n "$VERIFY_DIR" ]; then rm -rf "$VERIFY_DIR"; fi
}
trap cleanup EXIT HUP INT TERM
VERIFY_DIR=$(mktemp -d /private/tmp/codex-host-identity-verify.XXXXXX)
STAGED_APP="$PACKAGE_DIR/CodexHostIdentity.app"
STAGED_ZIP="$PACKAGE_DIR/CodexHostIdentity.zip"

# These options avoid copying resource forks and File Provider metadata. Only
# this generated temporary app is stripped; the source and user folders remain.
ditto --norsrc --noextattr "$INPUT_APP" "$STAGED_APP"
xattr -cr "$STAGED_APP"
codesign --force --sign - --identifier com.friday.codex-host-identity "$STAGED_APP"
codesign --verify --strict --verbose=2 "$STAGED_APP"
COPYFILE_DISABLE=1 ditto -c -k --norsrc --noextattr --keepParent "$STAGED_APP" "$STAGED_ZIP"

# Verify archive contents after an independent extraction, not just the app
# that was signed. This catches accidental AppleDouble/extended-attribute data.
ditto -x -k --norsrc --noextattr "$STAGED_ZIP" "$VERIFY_DIR"
codesign --verify --strict --verbose=2 "$VERIFY_DIR/CodexHostIdentity.app"
cmp "$STAGED_APP/Contents/MacOS/codex-host-identity" \
    "$VERIFY_DIR/CodexHostIdentity.app/Contents/MacOS/codex-host-identity"

mkdir -p "$(dirname "$OUTPUT_ZIP")"
cp -X "$STAGED_ZIP" "$OUTPUT_ZIP"
printf 'Verified signed package: %s\n' "$OUTPUT_ZIP"
shasum -a 256 "$OUTPUT_ZIP"
