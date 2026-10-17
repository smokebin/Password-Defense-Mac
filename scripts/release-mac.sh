#!/usr/bin/env bash
# release-mac.sh — build, sign, notarize, and package PasswordDefense as a
# distributable .dmg. One-shot release pipeline.
#
# Required one-time setup (do these once, NOT every release):
#   1. Have a "Developer ID Application" cert in your login keychain.
#        Xcode → Settings → Accounts → Manage Certificates → +
#   2. Store a notarytool credential profile:
#        xcrun notarytool store-credentials "PD_NOTARY" \
#          --apple-id "you@example.com" --team-id "ABCD123456" \
#          --password "abcd-efgh-ijkl-mnop"   # app-specific password
#   3. brew install create-dmg
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
# Architecture note: this script defaults to arm64-only (Apple Silicon).
# Universal builds failed at link time because at least one dependency
# (likely libsodium via Homebrew, which only ships arm64 on Apple Silicon
# machines) has no x86_64 slice. To re-enable universal builds later you'd
# need x86_64 builds of every linked library — easiest path is to install
# them with `arch -x86_64 /usr/local/bin/brew install ...` under Rosetta
# Homebrew, or vendor prebuilt x86_64 binaries into the project. Until then,
# Intel-Mac users will need to either run under Rosetta-translated arm64
# (works fine) or be told to upgrade. Apple Silicon is the majority install
# base as of 2026 so this is a defensible default.

set -euo pipefail

# ---- Config ----------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
XCODEPROJ="$PROJECT_ROOT/PasswordDefense.xcodeproj"
SCHEME="PasswordDefense"
APP_NAME="PasswordDefense"
BUNDLE_ID="com.StackedSoftware.PasswordDefense"

BUILD_DIR="$PROJECT_ROOT/build"
ARCHIVE_PATH="$BUILD_DIR/$APP_NAME.xcarchive"
EXPORT_DIR="$BUILD_DIR/export"
DMG_OUT="$BUILD_DIR/$APP_NAME.dmg"

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
    # Try to auto-detect from keychain
    DEV_ID_NAME="$(security find-identity -v -p codesigning | \
                   grep -o '"Developer ID Application: [^"]*"' | head -1 | tr -d '"')"
    if [[ -z "$DEV_ID_NAME" ]]; then
        red "No 'Developer ID Application' identity found in keychain."
        red "Open Xcode → Settings → Accounts → Manage Certificates → + → 'Developer ID Application'"
        exit 1
    fi
    green "Auto-detected signing identity: $DEV_ID_NAME"
fi

command -v create-dmg >/dev/null || { red "create-dmg not installed. Run: brew install create-dmg"; exit 1; }

# Version
if [[ -z "${VERSION:-}" ]]; then
    VERSION="$(xcrun agvtool mvers -terse1 -baseDir "$PROJECT_ROOT" 2>/dev/null || true)"
    [[ -z "$VERSION" ]] && VERSION="0.0.0"
fi
green "Releasing $APP_NAME $VERSION"

# ---- Clean + archive -------------------------------------------------------
# Arch selection: arm64-only by default (Apple Silicon). Override with
# UNIVERSAL=1 to attempt arm64+x86_64 — only works if every linked dependency
# (libsodium etc.) ships both slices.
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
    ARCHS="$ARCHS_TO_BUILD" \
    ONLY_ACTIVE_ARCH=NO \
    CODE_SIGN_STYLE=Manual \
    DEVELOPMENT_TEAM="$DEV_TEAM_ID" \
    CODE_SIGN_IDENTITY="$DEV_ID_NAME" \
    OTHER_CODE_SIGN_FLAGS="--options=runtime --timestamp" \
    archive

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
    <key>signingStyle</key>     <string>manual</string>
    <key>signingCertificate</key><string>Developer ID Application</string>
</dict>
</plist>
EOF

xcodebuild \
    -exportArchive \
    -archivePath "$ARCHIVE_PATH" \
    -exportOptionsPlist "$EXPORT_OPTS" \
    -exportPath "$EXPORT_DIR"

APP_PATH="$EXPORT_DIR/$APP_NAME.app"
[[ -d "$APP_PATH" ]] || { red "Exported .app not found at $APP_PATH"; exit 1; }

# Sanity: verify the signature is Developer ID, not "Sign to Run Locally".
# Capture to a variable first — piping to `grep -q` triggers a SIGPIPE race
# with pipefail enabled that misreports the check as failing.
SIG_INFO="$(codesign -dvv "$APP_PATH" 2>&1 || true)"
if ! printf '%s\n' "$SIG_INFO" | grep -q "Developer ID Application"; then
    red "Exported binary is NOT signed with Developer ID. Check your identity."
    printf '%s\n' "$SIG_INFO"
    exit 1
fi
green "App signed: $APP_PATH"

# ---- Bundle non-system dylibs --------------------------------------------
# Hardened Runtime + library validation rejects any dylib not signed by
# our team. Anything we pull in from /opt/homebrew, /usr/local, etc. is
# signed by Homebrew (or the upstream maintainer), so it fails to load on
# a user's machine even when the file is present at the same path.
#
# Fix: copy each non-system dependency into Contents/Frameworks/,
# rewrite the binary's references to @executable_path/../Frameworks/...,
# then re-sign the embedded dylibs (and the .app itself) with our
# Developer ID. dylibbundler does the otool / install_name_tool dance.
step "Bundling non-system dylibs (fixes Homebrew-link crash on user Macs)"
command -v dylibbundler >/dev/null || {
    red "dylibbundler not installed. Run: brew install dylibbundler"
    exit 1
}

EXEC_PATH="$APP_PATH/Contents/MacOS/$APP_NAME"
FRAMEWORKS_DIR="$APP_PATH/Contents/Frameworks"
mkdir -p "$FRAMEWORKS_DIR"

# Strip iCloud-attached extended attributes from the .app — dylibbundler's
# ad-hoc re-sign step fails with "resource fork, Finder information, or
# similar detritus not allowed" when these are present.
xattr -cr "$APP_PATH"

# -od overwrite, -b strip codesign of existing, -cd verify after,
# -x main binary, -d frameworks dir, -p install-name prefix.
dylibbundler \
    -od -b -cd \
    -x "$EXEC_PATH" \
    -d "$FRAMEWORKS_DIR" \
    -p "@executable_path/../Frameworks/"

# Re-sign every bundled dylib + the .app itself. install_name_tool edits
# invalidated the prior signatures; without this, codesign --verify fails.
step "Re-signing bundled dylibs + .app"
find "$FRAMEWORKS_DIR" -type f \( -name "*.dylib" -o -name "*.so" \) -print0 |
    while IFS= read -r -d '' lib; do
        codesign --force --sign "$DEV_ID_NAME" --options runtime --timestamp "$lib"
    done
codesign --force --deep --sign "$DEV_ID_NAME" --options runtime --timestamp "$APP_PATH"

# Verify nothing still references /opt/homebrew or /usr/local. If anything
# does, the user will hit the same crash; better to bail here than ship.
LEFTOVER="$(otool -L "$EXEC_PATH" | awk '{print $1}' | grep -E '^(/opt/homebrew|/usr/local)' || true)"
if [[ -n "$LEFTOVER" ]]; then
    red "Binary still references non-bundled paths:"
    echo "$LEFTOVER"
    exit 1
fi
green "All non-system dylibs bundled inside the .app."

# ---- Build DMG -------------------------------------------------------------
# Stage to a local /tmp scratch dir on plain APFS so hdiutil never sees the
# iCloud path. `ditto` is used for the .app copy (handles bundle hardlinks
# and metadata correctly where `cp -R` truncates / silently breaks). Then we
# strip extended attributes that iCloud / quarantine adds — hdiutil refuses
# to create a disk image when xattrs trip its sanity checks ("no mountable
# file systems" with no further detail).
#
# Uses a direct hdiutil call instead of `create-dmg` because the latter
# wraps several extra hdiutil/AppleScript steps that each fail differently
# on iCloud-adjacent paths. Trade-off: no fancy window background or icon
# positioning — just the .app and an Applications symlink. Plenty for
# distribution; revisit when there's bandwidth.
step "Building DMG"
rm -f "$DMG_OUT"

STAGE_DIR="$(mktemp -d "/tmp/PasswordDefense-dmg.XXXXXX")"
# Keep STAGE_DIR around on failure so it's inspectable; only clean on success.

# DMG layout: $STAGE_DIR/payload/PasswordDefense.app + Applications symlink
PAYLOAD="$STAGE_DIR/payload"
mkdir -p "$PAYLOAD"
ditto "$APP_PATH" "$PAYLOAD/$APP_NAME.app"
ln -s /Applications "$PAYLOAD/Applications"

# Strip user-visible xattrs (iCloud markers, quarantine, com.apple.metadata:…).
# System-protected attrs like code signature data live in the binary itself
# and are unaffected.
xattr -cr "$PAYLOAD" || true

# Sanity check before handing off to hdiutil.
echo "  STAGE_DIR : $STAGE_DIR"
echo "  PAYLOAD   : $PAYLOAD"
ls -la "$PAYLOAD"
if [[ ! -d "$PAYLOAD/$APP_NAME.app" ]]; then
    red "$PAYLOAD/$APP_NAME.app missing — ditto may have failed silently."
    exit 1
fi

# Build a writable image first, then convert to compressed read-only. This
# two-step is the conservative path that hdiutil never complains about.
RW_DMG="$STAGE_DIR/$APP_NAME-rw.dmg"
STAGE_DMG="$STAGE_DIR/$APP_NAME.dmg"

hdiutil create \
    -volname "$APP_NAME" \
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

# Move into the build/ directory under the project (iCloud path is fine for
# the *finished* DMG — hdiutil's sensitivity was only during creation).
mv "$STAGE_DMG" "$DMG_OUT"

# Sign the DMG itself (separately from the .app inside it)
codesign --sign "$DEV_ID_NAME" --timestamp "$DMG_OUT"

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

# Final spctl gatekeeper check (simulates what end users hit)
spctl --assess --type open --context context:primary-signature -v "$DMG_OUT" || \
    red "Warning: spctl assessment failed — investigate before shipping."

green ""
green "✓ Release ready: $DMG_OUT"
green "  Size: $(du -h "$DMG_OUT" | cut -f1)"
green "  Host this file at /dl/PasswordDefense-mac.dmg on your website."
