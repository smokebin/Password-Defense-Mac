#!/usr/bin/env bash
#
# Remove Password Defense's native-messaging host manifests from every
# browser's per-user NativeMessagingHosts dir. Idempotent — missing files
# are skipped silently.
#
# Usage: scripts/uninstall-bridge-host.sh [--browser all|chrome|edge|brave|firefox]

set -euo pipefail

HOST_NAME="com.passworddefense.bridge"
BROWSER="all"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --browser) BROWSER="$2"; shift 2;;
        *) echo "Unknown arg: $1" >&2; exit 1;;
    esac
done

HOME_LIB="$HOME/Library/Application Support"

remove_from() {
    local label="$1"
    local dir="$2"
    local path="$dir/${HOST_NAME}.json"
    if [[ -f "$path" ]]; then
        rm -f "$path"
        echo "removed: $label  →  $path"
    fi
}

do_chrome()  { remove_from "Chrome"  "$HOME_LIB/Google/Chrome/NativeMessagingHosts"; }
do_edge()    { remove_from "Edge"    "$HOME_LIB/Microsoft Edge/NativeMessagingHosts"; }
do_brave()   { remove_from "Brave"   "$HOME_LIB/BraveSoftware/Brave-Browser/NativeMessagingHosts"; }
do_firefox() { remove_from "Firefox" "$HOME_LIB/Mozilla/NativeMessagingHosts"; }

case "$BROWSER" in
    all)     do_chrome; do_edge; do_brave; do_firefox;;
    chrome)  do_chrome;;
    edge)    do_edge;;
    brave)   do_brave;;
    firefox) do_firefox;;
    *) echo "Unknown --browser: $BROWSER" >&2; exit 1;;
esac

echo "done."
