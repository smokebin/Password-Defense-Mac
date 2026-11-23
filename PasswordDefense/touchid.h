// touchid.h — Touch ID unlock: per-vault master password in the Keychain,
// released only after a successful Touch ID.
//
// Storage (data-protection keychain, so it needs the signed app's
// application-identifier entitlement; unsigned dev builds get
// errSecMissingEntitlement):
//   service ...touchid          secret item, kSecAccessControlBiometryCurrentSet
//                               + kSecAttrAccessibleWhenUnlockedThisDeviceOnly.
//                               Invalidated by the OS if fingerprints change.
//   service ...touchid.enabled  plain marker, no ACL. "Touch ID is on for this
//                               vault" — survives secret invalidation so the
//                               next password unlock can re-store it.
// account = per-vault id (vault_db sync_state "touchid_vault_id").
//
// Async calls run on a background queue (a biometric Keychain read blocks
// until the user responds) and invoke `done` on that background thread —
// callers marshal back to the UI thread themselves.
#pragma once

#include <functional>
#include <string>

namespace touchid {

enum class Status { Ok, Cancelled, NotFound, Unavailable, Error };

using Callback = std::function<void(Status status, std::string secret_or_error)>;

// Touch ID hardware present and a finger enrolled.
bool is_available();

// Whether the app is frontmost — don't pop a Touch ID sheet over another app.
bool app_is_active();

// Marker present for this vault (no prompt).
bool is_enabled(const std::string& account);

// Confirms with one Touch ID prompt, then stores secret + marker.
void enable_async(const std::string& account, const std::string& secret,
                  const std::string& reason, Callback done);

// Stores/replaces the secret without prompting (re-arm after a password
// unlock). Leaves the marker as is. Returns false + error on failure.
bool store_secret(const std::string& account, const std::string& secret,
                  std::string* error);

// Prompts for Touch ID and returns the secret.
void read_async(const std::string& account, const std::string& reason,
                Callback done);

// Removes secret + marker for one vault.
void disable(const std::string& account);

// Removes every Touch ID item for every vault (sign-out).
void disable_all();

}  // namespace touchid
