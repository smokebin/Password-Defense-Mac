#!/usr/bin/env bash
#
# Install the native-messaging host manifest for Password Defense's browser
# extension into each supported browser's per-user NativeMessagingHosts dir.
#
# Usage:
#   scripts/install-bridge-host.sh [--host-path PATH] [--browser all|chrome|edge|brave|firefox]
#
# --host-path  Absolute path to the pw-bridge binary. Defaults to the
#              binary inside /Applications/Password Defense.app, with a
#              fallback to ./build/pw-bridge for developer installs.
# --browser    Which browser to install for. Defaults to `all`.
#
# All paths are per-user (~/Library/Application Support/...). No admin
# required. Uninstall via scripts/uninstall-bridge-host.sh.

set -euo pipefail

# --- Extension ID (derived from manifest.json's embedded RSA key) ---
# If pwmngr-ext's manifest.json "key" field changes, recompute via:
#   python3 -c 'import base64,hashlib,json,sys; k=json.load(open(sys.argv[1]))["key"]; d=base64.b64decode(k); h=hashlib.sha256(d).hexdigest()[:32]; print("".join(chr(ord("a")+int(c,16)) for c in h))' /path/to/pwmngr-ext/manifest.json
EXTENSION_ID="ambjefgkfkdeeggkajdmnpjhlefbkpak"
FIREFOX_ID="addon@passworddefense.com"
HOST_NAME="com.passworddefense.bridge"

# --- Argument parsing ---
HOST_PATH=""
BROWSER="all"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --host-path) HOST_PATH="$2"; shift 2;;
        --browser)   BROWSER="$2";   shift 2;;
        *) echo "Unknown arg: $1" >&2; exit 1;;
    esac
done

# --- Resolve host binary path ---
if [[ -z "$HOST_PATH" ]]; then
    APP_HOST="/Applications/Password Defense.app/Contents/MacOS/pw-bridge"
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    DEV_HOST="$(cd "$SCRIPT_DIR/.." && pwd)/build/pw-bridge"
    if [[ -x "$APP_HOST" ]]; then
        HOST_PATH="$APP_HOST"
    elif [[ -x "$DEV_HOST" ]]; then
        HOST_PATH="$DEV_HOST"
        echo "note: using dev-build pw-bridge at $HOST_PATH" >&2
    else
        echo "error: pw-bridge not found. Build it first:" >&2
        echo "  scripts/build-bridge-host.sh" >&2
        exit 1
    fi
fi

if [[ ! -x "$HOST_PATH" ]]; then
    echo "error: $HOST_PATH is not an executable file" >&2
    exit 1
fi

# --- Browser dir table (keyed by name) ---
# Each entry: dir and whether it uses the Firefox allowed_extensions schema.
install_for() {
    local label="$1"
    local dir="$2"
    local firefox_schema="$3"   # "true" or "false"

    local parent
    parent="$(dirname "$dir")"
    if [[ ! -d "$parent" ]]; then
        # Browser isn't installed — skip silently rather than error out.
        return 0
    fi

    mkdir -p "$dir"
    local out="$dir/${HOST_NAME}.json"

    if [[ "$firefox_schema" == "true" ]]; then
        cat > "$out" <<EOF
{
    "name": "${HOST_NAME}",
    "description": "Password Defense native-messaging bridge",
    "path": "${HOST_PATH}",
    "type": "stdio",
    "allowed_extensions": ["${FIREFOX_ID}"]
}
EOF
    else
        cat > "$out" <<EOF
{
    "name": "${HOST_NAME}",
    "description": "Password Defense native-messaging bridge",
    "path": "${HOST_PATH}",
    "type": "stdio",
    "allowed_origins": ["chrome-extension://${EXTENSION_ID}/"]
}
EOF
    fi

    chmod 0644 "$out"
    echo "installed: $label  →  $out"
}

HOME_LIB="$HOME/Library/Application Support"

do_chrome()  { install_for "Chrome"  "$HOME_LIB/Google/Chrome/NativeMessagingHosts"                 "false"; }
do_edge()    { install_for "Edge"    "$HOME_LIB/Microsoft Edge/NativeMessagingHosts"                 "false"; }
do_brave()   { install_for "Brave"   "$HOME_LIB/BraveSoftware/Brave-Browser/NativeMessagingHosts"    "false"; }
do_firefox() { install_for "Firefox" "$HOME_LIB/Mozilla/NativeMessagingHosts"                        "true";  }

case "$BROWSER" in
    all)     do_chrome; do_edge; do_brave; do_firefox;;
    chrome)  do_chrome;;
    edge)    do_edge;;
    brave)   do_brave;;
    firefox) do_firefox;;
    *) echo "Unknown --browser: $BROWSER" >&2; exit 1;;
esac

echo "done. Extension ID pinned: $EXTENSION_ID"
