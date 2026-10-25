#!/usr/bin/env bash
#
# Build the pw-bridge native-messaging shim.
#
# Usage:
#   scripts/build-bridge-host.sh [--output PATH] [--arch arm64|x86_64|universal]
#
# Defaults:
#   --output: ./build/pw-bridge
#   --arch:   arm64 (matches scripts/release-mac.sh — see
#             project_mac_release_arm64_only memory for why)
#
# Called from:
#   - developer shell for ad-hoc testing
#   - Xcode "Run Script" build phase (places the binary inside the .app)
#   - scripts/release-mac.sh (bundles into the final DMG)

set -euo pipefail

OUTPUT=""
ARCH="arm64"
DEPLOYMENT_TARGET="13.5"   # match PasswordDefense target

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output) OUTPUT="$2"; shift 2;;
        --arch)   ARCH="$2";   shift 2;;
        *) echo "Unknown arg: $1" >&2; exit 1;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC="$PROJECT_ROOT/bridge-host/main.cpp"

if [[ -z "$OUTPUT" ]]; then
    OUTPUT="$PROJECT_ROOT/build/pw-bridge"
fi

mkdir -p "$(dirname "$OUTPUT")"

ARCH_FLAGS=()
case "$ARCH" in
    arm64)     ARCH_FLAGS=("-arch" "arm64");;
    x86_64)    ARCH_FLAGS=("-arch" "x86_64");;
    universal) ARCH_FLAGS=("-arch" "arm64" "-arch" "x86_64");;
    *) echo "Unknown arch: $ARCH" >&2; exit 1;;
esac

exec clang++ \
    -std=c++17 \
    -O2 \
    -Wall -Wextra -Wno-unused-parameter \
    "${ARCH_FLAGS[@]}" \
    -mmacosx-version-min="$DEPLOYMENT_TARGET" \
    -o "$OUTPUT" \
    "$SRC"
