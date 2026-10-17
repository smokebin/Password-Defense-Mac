#!/usr/bin/env bash
# release-ios-prep.sh — pre-archive housekeeping for the iOS release flow.
#
# App Store Connect rejects uploads whose CFBundleVersion isn't strictly
# greater than the previous build for the same MARKETING_VERSION. This
# script bumps CURRENT_PROJECT_VERSION across every target in the iOS
# Xcode project so the next archive uploads cleanly.
#
# Usage:
#   ./scripts/release-ios-prep.sh           # bump by 1
#   ./scripts/release-ios-prep.sh 5         # bump to specific number (e.g. 5)
#
# After running, open Xcode → Product → Archive → Distribute App →
# App Store Connect → Upload. The version that prints below is what
# TestFlight / App Store will display.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IOS_PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)/../PasswordDefense-IOS"

if [[ ! -d "$IOS_PROJECT_DIR/PasswordDefense-IOS.xcodeproj" ]]; then
    echo "Could not locate iOS xcodeproj at $IOS_PROJECT_DIR" >&2
    exit 1
fi

PBXPROJ="$IOS_PROJECT_DIR/PasswordDefense-IOS.xcodeproj/project.pbxproj"

# Read the current build number from the first CURRENT_PROJECT_VERSION line.
# All targets share the same value in this project; if that ever diverges,
# this script needs to grow per-target awareness.
CURRENT="$(grep -m1 -E "CURRENT_PROJECT_VERSION = " "$PBXPROJ" | sed -E 's/.*= ([0-9]+);.*/\1/')"

if [[ -z "$CURRENT" || ! "$CURRENT" =~ ^[0-9]+$ ]]; then
    echo "Could not parse current build number from pbxproj." >&2
    exit 1
fi

if [[ $# -ge 1 ]]; then
    NEXT="$1"
    if [[ ! "$NEXT" =~ ^[0-9]+$ ]]; then
        echo "Build number must be an integer." >&2
        exit 1
    fi
    if (( NEXT <= CURRENT )); then
        echo "Refusing to set build number to $NEXT (current is $CURRENT). App Store requires monotonic increase." >&2
        exit 1
    fi
else
    NEXT=$((CURRENT + 1))
fi

# Rewrite every CURRENT_PROJECT_VERSION line in-place. sed -i '' for BSD/macOS.
sed -i '' -E "s/CURRENT_PROJECT_VERSION = [0-9]+;/CURRENT_PROJECT_VERSION = $NEXT;/g" "$PBXPROJ"

# Quick sanity check: ensure pbxproj is still parseable.
if ! plutil -lint "$PBXPROJ" >/dev/null 2>&1; then
    echo "WARNING: pbxproj failed plutil lint after bump. You may need to revert." >&2
fi

MARKETING="$(grep -m1 -E "MARKETING_VERSION = " "$PBXPROJ" | sed -E 's/.*= ([0-9.]+);.*/\1/')"

printf "\n\033[1;32m✓\033[0m iOS build bumped: %s → %s   (marketing version %s)\n" "$CURRENT" "$NEXT" "$MARKETING"
printf "\nNext: open Xcode → Product → Archive → Distribute App → App Store Connect.\n"
