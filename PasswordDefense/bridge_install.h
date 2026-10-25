// bridge_install.h
//
// In-process installer for the native-messaging host manifests the browser
// needs to spawn pw-bridge. Same job as scripts/install-bridge-host.sh,
// callable from the Settings UI. Both implementations produce identical
// JSON files.
//
// Scope: per-user manifests under ~/Library/Application Support/<Browser>/
// NativeMessagingHosts/. No admin privileges needed.

#pragma once

#include <string>
#include <vector>

namespace bridge_install {

struct BrowserStatus
{
    std::string name;           // "Chrome", "Edge", "Brave", "Firefox"
    std::string short_key;      // "chrome" | "edge" | "brave" | "firefox"
    bool browser_installed;     // the browser's root dir exists
    bool host_registered;       // our com.passworddefense.bridge.json is present
};

// List every supported browser with its install state.
std::vector<BrowserStatus> Scan();

// Install the host manifest for a single browser. Returns true on success,
// false (with err_out populated) on failure. Setting force=true writes the
// manifest even if the browser's root dir doesn't exist.
bool Install(const std::string& short_key, bool force,
             std::string* err_out = nullptr);

// Remove the host manifest for a single browser. Idempotent — succeeds
// whether the manifest was present or not.
bool Uninstall(const std::string& short_key);

// Convenience: install for every detected browser, uninstall for every
// detected browser. Return the count that changed.
int InstallAll();
int UninstallAll();

// The absolute path pw-bridge should live at. Prefers inside the running
// app bundle (/Applications/.../Contents/MacOS/pw-bridge); falls back to
// a dev-build path. If neither exists, returns empty string.
std::string ResolveHostBinaryPath();

} // namespace bridge_install
