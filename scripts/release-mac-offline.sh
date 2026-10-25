#!/usr/bin/env bash
# release-mac-offline.sh — build, sign, notarize, and package the public
# OFFLINE build of PasswordDefense as a distributable .dmg.
#
# Differs from release-mac.sh only in which Xcode project is built, the
# bundle identifier that gets signed, and the output DMG name. The offline
# project (PasswordDefenseOffline.xcodeproj) bakes PD_OFFLINE_BUILD=1 into
# GCC_PREPROCESSOR_DEFINITIONS, which strips cloud sync, auto-update check,
# and anonymous-share features at compile time. The browser extension still
# works: it talks to the app over a local Unix socket (native messaging),
# not the network.
#
# Bundle ID is com.StackedSoftware.PasswordDefenseOffline so this build can
# coexist with (or cleanly replace) a cloud install.
#
# Required one-time setup (do these once, NOT every release):
#   1. Have a "Developer ID Application" cert in your login keychain.
#        Xcode → Settings → Accounts → Manage Certificates → +
#   2. Store a notarytool credential profile:
#        xcrun notarytool store-credentials "PD_NOTARY" \
#          --apple-id "you@example.com" --team-id "ABCD123456" \
#          --password "abcd-efgh-ijkl-mnop"   # app-specific password
#   3. brew install create-dmg dylibbundler
#
# Required env vars (override defaults below):
#   DEV_TEAM_ID         e.g. ABCD123456
#   DEV_ID_NAME         exact identity name, e.g. "Developer ID Application: Your Name (ABCD123456)"
#   NOTARY_PROFILE      keychain profile name, e.g. PD_NOTARY
#
# Optional:
#   VERSION             marketing version override (default: read from xcodeproj)
#   SKIP_NOTARIZE=1     build + DMG only; skip Apple submission (for dry runs)
#   UNIVERSAL=1         attempt arm64 + x86_64 universal build
#   ARCHS_TO_BUILD=…    raw Xcode ARCHS override (e.g. "arm64")
#
# Architecture note: arm64-only by default. See release-mac.sh for the full
# explanation — same libsodium/Homebrew x86_64 story applies here.

set -euo pipefail

# ---- Config ----------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
XCODEPROJ="$PROJECT_ROOT/PasswordDefenseOffline.xcodeproj"
SCHEME="PasswordDefense"
APP_NAME="PasswordDefense"
BUNDLE_ID="com.StackedSoftware.PasswordDefenseOffline"

BUILD_DIR="$PROJECT_ROOT/build-offline"
ARCHIVE_PATH="$BUILD_DIR/$APP_NAME.xcarchive"
EXPORT_DIR="$BUILD_DIR/export"
DMG_OUT="$BUILD_DIR/${APP_NAME}-Offline.dmg"

DEV_TEAM_ID="${DEV_TEAM_ID:-}"
DEV_ID_NAME="${DEV_ID_NAME:-}"
NOTARY_PROFILE="${NOTARY_PROFILE:-PD_NOTARY}"

# ---- Pre-flight checks -----------------------------------------------------
red()   { printf "\033[31m%s\033[0m\n" "$*" >&2; }
green() { printf "\033[32m%s\033[0m\n" "$*"; }
step()  { printf "\n\033[1;34m▶ %s\033[0m\n" "$*"; }

[[ -d "$XCODEPROJ" ]] || { red "Xcode project not found at $XCODEPROJ"; exit 1; }

if [[ -z "$DEV_TEAM_ID" ]]; then
    red "DEV_TEAM_ID is required (10-char Apple team identifier)."
    red "Find it at developer.apple.com → Membership."
    exit 1
fi

if [[ -z "$DEV_ID_NAME" ]]; then
    # Try to auto-detect from keychain. We capture BOTH the display name
    # (for logs + exportOptions.plist) and the SHA-1 fingerprint (for direct
    # codesign calls that need to disambiguate when the same cert name
    # appears twice — common after a cert renewal where the old cert stays
    # in the keychain alongside the new one).
    #
    # security find-identity line format:
    #   <idx>) <40-char-sha1> "<display name>"
    DEV_ID_LINE="$(security find-identity -v -p codesigning | \
                   grep '"Developer ID Application:' | head -1)"
    if [[ -z "$DEV_ID_LINE" ]]; then
        red "No 'Developer ID Application' identity found in keychain."
        red "Open Xcode → Settings → Accounts → Manage Certificates → + → 'Developer ID Application'"
        exit 1
    fi
    DEV_ID_NAME="$(echo "$DEV_ID_LINE" | grep -o '"[^"]*"' | tr -d '"')"
    DEV_ID_HASH="$(echo "$DEV_ID_LINE" | awk '{print $2}')"
    green "Auto-detected signing identity: $DEV_ID_NAME"
    green "  SHA-1: $DEV_ID_HASH"
fi
# If DEV_ID_NAME was explicitly set via env, we still need a hash. Resolve
# whichever cert matches — if ambiguous, fail loudly rather than randomly
# picking one.
if [[ -z "${DEV_ID_HASH:-}" ]]; then
    MATCHES="$(security find-identity -v -p codesigning | \
               grep -F "\"$DEV_ID_NAME\"" | awk '{print $2}')"
    MATCH_COUNT="$(printf '%s\n' "$MATCHES" | grep -c . || true)"
    if [[ "$MATCH_COUNT" == "0" ]]; then
        red "DEV_ID_NAME='$DEV_ID_NAME' not found in keychain."
        exit 1
    elif [[ "$MATCH_COUNT" != "1" ]]; then
        red "DEV_ID_NAME='$DEV_ID_NAME' matches $MATCH_COUNT certs — specify DEV_ID_HASH to pick one."
        printf '%s\n' "$MATCHES" | sed 's/^/  /'
        exit 1
    fi
    DEV_ID_HASH="$MATCHES"
fi

command -v create-dmg >/dev/null || { red "create-dmg not installed. Run: brew install create-dmg"; exit 1; }

# Version
if [[ -z "${VERSION:-}" ]]; then
    VERSION="$(xcrun agvtool mvers -terse1 -baseDir "$PROJECT_ROOT" 2>/dev/null || true)"
    [[ -z "$VERSION" ]] && VERSION="0.0.0"
fi
green "Releasing $APP_NAME (OFFLINE) $VERSION"
green "  Xcode project: $XCODEPROJ"
green "  Bundle ID:     $BUNDLE_ID"

# ---- Confirm the offline macro is actually set -----------------------------
# Catching a misconfigured project here is better than catching it after a
# user downloads a build that still has cloud entry points.
if ! grep -q "PD_OFFLINE_BUILD=1" "$XCODEPROJ/project.pbxproj"; then
    red "PD_OFFLINE_BUILD=1 not found in $XCODEPROJ/project.pbxproj."
    red "The offline guards won't take effect without it — aborting."
    exit 1
fi
green "  PD_OFFLINE_BUILD=1: confirmed in project settings"

# ---- Data-safety invariant tests -------------------------------------------
# Runs test_invariants.cpp against the core C++ sources (no Cocoa, no Metal,
# no .mm) before the Xcode archive. Covers:
#   1. Exhaustive 28-field serialization symmetry (card_address regression)
#   2. reconcile_after_undo (Empty-Trash-then-Undo doesn't resurrect ghosts)
#   3. Save-fail recovery snapshot roundtrip (AEAD ownership proof)
#   4. Empty stamped vault rejects wrong password at unlock
#   5. Ghost-vault lock-escape (save-time key_check mismatch preserves disk)
#
# Fail-fast placement: before any expensive Xcode work. If an invariant
# regresses, the DMG build aborts here instead of after a 90-second archive.
# Opt out with SKIP_INVARIANT_TESTS=1 if you need an emergency hotfix build
# (not recommended — the whole point is that these never regress silently).
TEST_SRC="$PROJECT_ROOT/PasswordDefense/test_invariants.cpp"
if [[ -f "$TEST_SRC" && "${SKIP_INVARIANT_TESTS:-0}" != "1" ]]; then
    step "Running data-safety invariant tests"
    TEST_BIN="$(mktemp -t pd_invariants_XXXXXX)"
    # Mirror the standalone build command in the test file's header: pure C++20,
    # Homebrew libsodium + system sqlite3, no .mm files, no OpenSSL.
    if ! clang++ -std=gnu++20 -O2 \
            "$TEST_SRC" \
            "$PROJECT_ROOT/PasswordDefense/credential_ops.cpp" \
            "$PROJECT_ROOT/PasswordDefense/vault_crypto.cpp" \
            "$PROJECT_ROOT/PasswordDefense/vault_db.cpp" \
            -I "$PROJECT_ROOT/PasswordDefense" \
            -I /opt/homebrew/include -L /opt/homebrew/lib \
            -lsqlite3 -lsodium \
            -o "$TEST_BIN"; then
        red "Invariant test compile failed — aborting release."
        rm -f "$TEST_BIN"
        exit 1
    fi
    if ! "$TEST_BIN"; then
        red "An invariant test FAILED — aborting release before archive."
        red "This is a hard gate: a failed data-safety invariant means user vault"
        red "data could be lost or silently corrupted by the build you were about to ship."
        rm -f "$TEST_BIN"
        exit 1
    fi
    rm -f "$TEST_BIN"
    green "  All data-safety invariants: PASS"
elif [[ "${SKIP_INVARIANT_TESTS:-0}" == "1" ]]; then
    red "SKIP_INVARIANT_TESTS=1 — skipping data-safety test gate. You are responsible for anything that breaks."
else
    red "Warning: $TEST_SRC not found — skipping invariant tests."
fi

# ---- Clean + archive -------------------------------------------------------
ARCHS_TO_BUILD="${ARCHS_TO_BUILD:-arm64}"
if [[ "${UNIVERSAL:-0}" == "1" ]]; then
    ARCHS_TO_BUILD="arm64 x86_64"
fi

step "Archiving ($ARCHS_TO_BUILD)"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

xcodebuild \
    -project "$XCODEPROJ" \
    -scheme "$SCHEME" \
    -configuration Release \
    -destination "generic/platform=macOS" \
    -archivePath "$ARCHIVE_PATH" \
    -allowProvisioningUpdates \
    ARCHS="$ARCHS_TO_BUILD" \
    ONLY_ACTIVE_ARCH=NO \
    CODE_SIGN_STYLE=Automatic \
    DEVELOPMENT_TEAM="$DEV_TEAM_ID" \
    archive
# Signing notes:
#  - CODE_SIGN_STYLE=Automatic (was Manual) because the app now carries an
#    App Group entitlement (com.apple.security.application-groups). Apple
#    requires a provisioning profile for *any* capability that touches their
#    registration graph (App Groups, iCloud, Push, etc.), and only Automatic
#    signing can request + install one on the fly. Manual mode would need a
#    hand-created "Developer ID Direct Distribution Profile" that nothing in
#    this repo generates.
#  - -allowProvisioningUpdates lets xcodebuild talk to Apple's dev portal to
#    fetch / renew the profile.
#  - CODE_SIGN_IDENTITY + OTHER_CODE_SIGN_FLAGS dropped: automatic signing
#    picks the Development cert for the archive step. The subsequent
#    -exportArchive with method=developer-id (see exportOptions below)
#    re-signs the output with the Developer ID cert, so the user-facing DMG
#    is still Developer ID signed — not Apple Development.

# ---- Export with Developer ID ---------------------------------------------
step "Exporting Developer ID build"
EXPORT_OPTS="$BUILD_DIR/exportOptions.plist"
cat > "$EXPORT_OPTS" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>method</key>           <string>developer-id</string>
    <key>teamID</key>           <string>$DEV_TEAM_ID</string>
    <key>signingStyle</key>     <string>automatic</string>
    <key>signingCertificate</key><string>Developer ID Application</string>
</dict>
</plist>
EOF
# signingStyle=automatic (was manual) for the same App-Groups reason as the
# archive step above: a Developer-ID-with-App-Groups distribution profile
# has to come from Apple's provisioning service, which only Automatic +
# -allowProvisioningUpdates can request.

xcodebuild \
    -exportArchive \
    -archivePath "$ARCHIVE_PATH" \
    -exportOptionsPlist "$EXPORT_OPTS" \
    -exportPath "$EXPORT_DIR" \
    -allowProvisioningUpdates

APP_PATH="$EXPORT_DIR/$APP_NAME.app"
[[ -d "$APP_PATH" ]] || { red "Exported .app not found at $APP_PATH"; exit 1; }

# Verify the signature is Developer ID AND the bundle ID matches the offline one.
SIG_INFO="$(codesign -dvv "$APP_PATH" 2>&1 || true)"
if ! printf '%s\n' "$SIG_INFO" | grep -q "Developer ID Application"; then
    red "Exported binary is NOT signed with Developer ID. Check your identity."
    printf '%s\n' "$SIG_INFO"
    exit 1
fi
if ! printf '%s\n' "$SIG_INFO" | grep -q "Identifier=$BUNDLE_ID"; then
    red "Exported binary has an unexpected bundle identifier."
    red "Expected: $BUNDLE_ID"
    printf '%s\n' "$SIG_INFO"
    exit 1
fi
green "App signed: $APP_PATH"

# ---- Embed pw-bridge native-messaging host ---------------------------------
# The browser extension's installed host manifest points at
# Contents/MacOS/pw-bridge inside the shipped .app. Without this binary,
# connectNative() fails with "Native host has exited" and the extension
# shows "Desktop App Offline" indefinitely.
#
# Built here rather than as an Xcode target because it's a standalone 150-line
# stdio↔socket shim with no Cocoa / Metal / libsodium / ImGui deps — pulling
# it into the Xcode project would add a new target + scheme + signing config
# for ~2k of linked code. The arch(s) must match the main app's $ARCHS_TO_BUILD
# so both ship with the same slice set.
step "Embedding pw-bridge native-messaging host"
BRIDGE_SCRIPT="$SCRIPT_DIR/build-bridge-host.sh"
[[ -x "$BRIDGE_SCRIPT" ]] || { red "$BRIDGE_SCRIPT not found or not executable"; exit 1; }

# Translate xcodebuild's space-separated ARCHS into build-bridge-host.sh's
# single --arch value.
case "$ARCHS_TO_BUILD" in
    *arm64*x86_64*|*x86_64*arm64*) BRIDGE_ARCH="universal" ;;
    *arm64*)                        BRIDGE_ARCH="arm64" ;;
    *x86_64*)                       BRIDGE_ARCH="x86_64" ;;
    *) red "Unknown ARCHS_TO_BUILD: $ARCHS_TO_BUILD"; exit 1 ;;
esac

BRIDGE_PATH="$APP_PATH/Contents/MacOS/pw-bridge"
"$BRIDGE_SCRIPT" --output "$BRIDGE_PATH" --arch "$BRIDGE_ARCH"

# Sign with the same Developer ID + hardened runtime + timestamp as the main
# binary. Pass the SHA-1 instead of the display name because the keychain
# commonly carries two certs with identical names after a renewal, and
# codesign bails with "ambiguous" when it can't pick one.
codesign --force --sign "$DEV_ID_HASH" \
         --options runtime --timestamp \
         "$BRIDGE_PATH"

# Smoke check: the shim must match the main app's arch(s) or users on the
# other platform see "Bad CPU type in executable". file(1) tells us what we got.
file "$BRIDGE_PATH"
green "pw-bridge embedded + signed: $BRIDGE_PATH"

# ---- Bundle non-system dylibs --------------------------------------------
step "Bundling non-system dylibs (fixes Homebrew-link crash on user Macs)"
command -v dylibbundler >/dev/null || {
    red "dylibbundler not installed. Run: brew install dylibbundler"
    exit 1
}

EXEC_PATH="$APP_PATH/Contents/MacOS/$APP_NAME"
FRAMEWORKS_DIR="$APP_PATH/Contents/Frameworks"
mkdir -p "$FRAMEWORKS_DIR"

xattr -cr "$APP_PATH"

dylibbundler \
    -od -b -cd \
    -x "$EXEC_PATH" \
    -d "$FRAMEWORKS_DIR" \
    -p "@executable_path/../Frameworks/"

step "Re-signing bundled dylibs + .app"
find "$FRAMEWORKS_DIR" -type f \( -name "*.dylib" -o -name "*.so" \) -print0 |
    while IFS= read -r -d '' lib; do
        codesign --force --sign "$DEV_ID_HASH" --options runtime --timestamp "$lib"
    done
codesign --force --deep --sign "$DEV_ID_HASH" --options runtime --timestamp "$APP_PATH"

LEFTOVER="$(otool -L "$EXEC_PATH" | awk '{print $1}' | grep -E '^(/opt/homebrew|/usr/local)' || true)"
if [[ -n "$LEFTOVER" ]]; then
    red "Binary still references non-bundled paths:"
    echo "$LEFTOVER"
    exit 1
fi
green "All non-system dylibs bundled inside the .app."

# ---- Build DMG -------------------------------------------------------------
step "Building DMG"
rm -f "$DMG_OUT"

STAGE_DIR="$(mktemp -d "/tmp/PasswordDefense-Offline-dmg.XXXXXX")"

PAYLOAD="$STAGE_DIR/payload"
mkdir -p "$PAYLOAD"
ditto "$APP_PATH" "$PAYLOAD/$APP_NAME.app"
ln -s /Applications "$PAYLOAD/Applications"

xattr -cr "$PAYLOAD" || true

echo "  STAGE_DIR : $STAGE_DIR"
echo "  PAYLOAD   : $PAYLOAD"
ls -la "$PAYLOAD"
if [[ ! -d "$PAYLOAD/$APP_NAME.app" ]]; then
    red "$PAYLOAD/$APP_NAME.app missing — ditto may have failed silently."
    exit 1
fi

RW_DMG="$STAGE_DIR/$APP_NAME-rw.dmg"
STAGE_DMG="$STAGE_DIR/$APP_NAME.dmg"

hdiutil create \
    -volname "$APP_NAME Offline" \
    -srcfolder "$PAYLOAD" \
    -fs HFS+ \
    -format UDRW \
    -ov \
    "$RW_DMG"

hdiutil convert "$RW_DMG" \
    -format UDZO \
    -imagekey zlib-level=9 \
    -o "$STAGE_DMG"

rm -f "$RW_DMG"

mv "$STAGE_DMG" "$DMG_OUT"

codesign --sign "$DEV_ID_HASH" --timestamp "$DMG_OUT"

# ---- Notarize + staple -----------------------------------------------------
if [[ "${SKIP_NOTARIZE:-0}" == "1" ]]; then
    green "SKIP_NOTARIZE=1 → leaving DMG un-notarized at $DMG_OUT"
    exit 0
fi

step "Submitting to Apple notary service (this can take a few minutes)"
xcrun notarytool submit "$DMG_OUT" \
    --keychain-profile "$NOTARY_PROFILE" \
    --wait

step "Stapling notarization ticket"
xcrun stapler staple "$DMG_OUT"
xcrun stapler validate "$DMG_OUT"

spctl --assess --type open --context context:primary-signature -v "$DMG_OUT" || \
    red "Warning: spctl assessment failed — investigate before shipping."

green ""
green "✓ Offline release ready: $DMG_OUT"
green "  Size: $(du -h "$DMG_OUT" | cut -f1)"
green "  Host this file at /dl/PasswordDefense-mac-offline.dmg on your website."
