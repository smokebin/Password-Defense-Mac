# PasswordDefense — macOS Port

## Overview

PasswordDefense is a cross-platform password manager originally built for Windows (DX11/Win32). This documents the macOS port, which replaces the rendering backend with Metal and the windowing layer with Cocoa/AppKit, while sharing the vast majority of application logic as cross-platform C++.

> **This is the OFFLINE fork** (`smokebin/Password-Defense-Mac`, public — the "verify yourself" sibling of the Windows offline app). Cloud sync and the browser-extension local server are compiled out: `sync_manager.cpp` and `local_server.cpp` are deleted from this repo and replaced by no-op stubs (`sync_manager_stub.mm`, `local_server_stub.mm`) under the `PD_OFFLINE_BUILD` target. The only online features are opt-in and OFF by default — see [Online Features](#online-features-opt-in). The cloud variant lives in the private `pw-mac-cloud`.

**Rendering:** Metal + ImGui (via `imgui_impl_metal.mm` and `imgui_impl_osx.mm`)
**Language:** C++ (core logic) + Objective-C++ (`.mm` files for macOS-specific code)
**Build:** Xcode, C++20 (`gnu++20`), ARC enabled
**Window size:** 785 x 695 (defined in `GUI.h`)

---

## Architecture

```
┌──────────────────────────────────────────────────┐
│                  Cocoa / Metal                    │
│  main.mm → AppDelegate.mm → ViewController.mm    │
│  (NSApp)    (lifecycle)      (MTKView + ImGui)    │
├──────────────────────────────────────────────────┤
│               ImGui Backends                      │
│  imgui_impl_metal.mm    imgui_impl_osx.mm         │
├──────────────────────────────────────────────────┤
│            macOS Bridge Layer (.mm)                │
│  GUI.mm           – fonts, themes, tray, window   │
│  file_dialogs_mac.mm – NSOpenPanel / NSSavePanel  │
│  favicon_stub.mm  – Metal texture from URL        │
│  utility_stub.mm  – backups, file info            │
├──────────────────────────────────────────────────┤
│         Cross-Platform Core (.cpp / .h)           │
│  application.cpp  – main state machine (225 KB)   │
│  UI.cpp / ui_*.cpp – ImGui UI layer               │
│  credential_ops   – CRUD operations               │
│  vault_db / vault_crypto – SQLite + AES           │
│  sync_manager     – STUBBED (offline: no-op)      │
│  local_server     – STUBBED (offline: no-op)      │
│  save.cpp         – config, paths, self-destruct  │
│  kdbx_export      – KeePass export                │
│  totp / twofa_ops – TOTP generation               │
└──────────────────────────────────────────────────┘
```

---

## File Layout

### macOS-Specific (Objective-C++)

| File | Purpose |
|------|---------|
| `main.mm` | `NSApplicationMain` entry point |
| `AppDelegate.mm` | App lifecycle (`applicationDidFinishLaunching`, `applicationShouldTerminateAfterLastWindowClosed`) |
| `ViewController.mm` | Metal rendering loop (MTKViewDelegate), ImGui frame orchestration, window close/tray handling |
| `GUI.mm` | Fonts, themes, style helpers, always-on-top, system tray (NSStatusItem), window show/hide |
| `file_dialogs_mac.mm` | File open/save dialogs — DB, CSV, PWM, KDBX, backup (`.lbdb`) |
| `favicon_stub.mm` | Async favicon download (NSURLSession) → NSImage → RGBA → Metal texture |
| `utility_stub.mm` | Backup management (create, list, retention), file info via `stat()` |

### Cross-Platform Core (C++)

| File | Purpose |
|------|---------|
| `application.cpp` | Main state machine — vault lifecycle, sync intents, UI event handling |
| `UI.cpp` | Shell layout, screen management, tab bar |
| `ui_views.cpp` | Credential list views (simple, detailed, accordion, 3-pane) |
| `ui_widgets.cpp` | Custom ImGui widgets |
| `ui_controls.cpp` | Footer bar, toolbar, search |
| `ui_modals.cpp` | Modals — trash bin, password generator, share, sync |
| `credential_ops.cpp` | Credential CRUD, undo/redo |
| `vault_db.cpp` | SQLite database abstraction |
| `vault_crypto.cpp` | AES-256 vault encryption/decryption |
| `save.cpp` | Config file I/O, path setup, self-destruct handler |
| `sync_manager_stub.mm` | **Offline no-op** replacing `sync_manager.cpp`. Every method reports "offline"; the free `win_http_request()` helper returns a benign failure. `sync_manager.cpp` is not in this repo. |
| `local_server_stub.mm` | **Offline no-op** replacing `local_server.cpp`. No localhost server, no browser-extension API; `IsRunning()` is always false so UI branches skip cleanly. `local_server.cpp` is not in this repo. |
| `kdbx_export.cpp` | KeePass `.kdbx` export |
| `app_import_export.cpp` | CSV/PWM import and export |
| `app_sharing.cpp` | Anonymous credential sharing |
| `totp.cpp` / `twofa_ops.cpp` | TOTP generation and 2FA management |
| `aes_cipher.cpp` | Low-level AES operations |
| `auth_crypto.cpp` | Authentication key derivation |
| `qrcodegen.cpp` | QR code generation |

---

## Dependencies

### Frameworks (linked via Xcode)

- **Cocoa** — AppKit windows, views, menus, NSStatusItem
- **Metal** — GPU rendering
- **MetalKit** — MTKView, display link
- **QuartzCore** — Core Animation
- **GameController** — Gamepad input (ImGui)
- **UniformTypeIdentifiers** — UTType for file dialogs

### Libraries

| Library | Version | Purpose |
|---------|---------|---------|
| libsodium | 1.0.21 | Key derivation, hashing, encryption |
| OpenSSL | 3.x | TLS for cpp-httplib. Still linked, but the offline build makes no live HTTPS client calls (sync/server stubbed; favicon uses NSURLSession). Can likely be dropped once the breach-check path is resolved — see [Online Features](#online-features-opt-in). |
| SQLite3 | system | Vault database storage |

### Header-Only (embedded in project)

- **cpp-httplib** (`httplib.h`) — HTTP/HTTPS client and server
- **nlohmann/json** (`json.hpp`) — JSON parsing
- **Dear ImGui** — Immediate-mode GUI framework
- **stb_image** (`stb_image.h`) — Image decoding
- **Material Design Icons** (`mdi_font_data.h`) — Icon font

---

## Paths

All data stored in `~/Library/Application Support/PasswordDefense/`:

```
~/Library/Application Support/PasswordDefense/
├── config.json          # App settings
├── vault-0.db           # Default vault
├── vault-1.db           # Additional vaults
└── Backups/
    ├── vault-0_AUTO_20260314_120000.lbdb
    └── vault-0_MANUAL_20260314_130000.lbdb
```

Path setup happens in `cfg::init()` (`save.cpp`), using `$HOME` + `/Library/Application Support/PasswordDefense/`.

---

## Platform Differences (`#ifdef _WIN32` / `#else`)

The codebase uses `#ifdef _WIN32` guards for platform-specific code. All guards have macOS `#else` branches:

| Area | Windows | macOS |
|------|---------|-------|
| Vault path | `GetModuleFileNameA` + exe dir | `~/Library/Application Support/PasswordDefense/` |
| Time functions | `localtime_s`, `gmtime_s` | `localtime_r`, `gmtime_r` |
| File dialogs | `OPENFILENAMEA` / `GetOpenFileNameA` | `NSOpenPanel` / `NSSavePanel` |
| Window centering | `GetDesktopWindow` + `GetWindowRect` | `ImGui::SetNextWindowPos(0,0)` (macOS handles natively) |
| Always-on-top | `SetWindowPos(HWND_TOPMOST)` | `NSWindow setLevel:NSFloatingWindowLevel` |
| System tray | Win32 `Shell_NotifyIcon` | `NSStatusItem` + `NSMenu` |
| App quit | `PostQuitMessage(0)` | `[NSApp terminate:nil]` |
| Sleep | `Sleep(ms)` | `std::this_thread::sleep_for` |
| Self-destruct | `CreateProcessA` | `fork` + `execl` |
| Backup browse | `OPENFILENAMEA` with `.lbdb` filter | `NSOpenPanel` with UTType |

---

## Rendering Pipeline

```
ViewController.drawInMTKView:
  ├── ImGui_ImplMetal_NewFrame(renderPassDescriptor)
  ├── ImGui_ImplOSX_NewFrame(mtkView)
  ├── ImGui::NewFrame()
  ├── render::DisplayProgram()     ← application.cpp (all UI logic)
  ├── ImGui::Render()
  ├── ImGui_ImplMetal_RenderDrawData(...)
  └── [commandBuffer presentDrawable:...]
```

Timer-driven at 60 FPS via MTKView's internal CVDisplayLink.

---

## System Tray (Menu Bar Icon)

Implemented in `GUI.mm` using `NSStatusItem`:

- **Icon:** SF Symbol `lock.shield`
- **Menu items:** Show PasswordDefense, Lock Vault, Quit
- **Behavior:** When `minimize_to_tray` is enabled in settings:
  - Close button hides window (`orderOut:`) instead of quitting
  - "Show" menu item restores window (`makeKeyAndOrderFront:`)
  - "Lock Vault" triggers vault lock and shows window
  - "Quit" removes tray icon and terminates app

**Key implementation detail:** `viewWillDisappear` fires when the window is hidden via `orderOut:`. The ImGui context must NOT be destroyed in this case — only when actually closing. This is guarded by checking `render::IsMinimizeToTrayEnabled()`.

---

## Online Features (opt-in)

Two online features remain; both are **opt-in and OFF by default** (Settings → Network & Privacy), consistent with the Windows offline app. Everything else is fully local.

### Favicon fetch (`favicon_stub.mm`) — working

1. `favicon::Get(domain)` checks cache, returns `ImTextureID` if available
2. On cache miss, async download via `NSURLSession` from `https://img.logo.dev/<domain>?token=...&size=<n>&format=png`
3. Response decoded: `NSImage` → `CGBitmapContext` (RGBA, un-premultiplied alpha)
4. Metal texture created with `MTLTextureDescriptor` (RGBA8Unorm)
5. Ownership transferred out of ARC via `__bridge_retained` cast to `ImTextureID`
6. Thread-safe: `std::mutex` protects cache, pending, and failed sets
7. Gated by `SetNetworkEnabled()` (`g_network_enabled`) — no network calls when the feature is off

### HIBP breach check (`application.cpp` → `CheckBreachedPasswords`) — currently inert

k-anonymity Pwned-Passwords check on a background thread, gated on `shell.online_breach_check`. **Caveat:** its HTTP call goes through `win_http_request()`, which in the offline build is the `sync_manager_stub.mm` no-op — so even when the user opts in, it returns the offline error and never reaches `api.pwnedpasswords.com`. The UI is present but the feature is effectively disabled.

> **Decision needed:** either (a) give the breach path a real `NSURLSession`-based HTTPS call so it works like the Windows offline app, or (b) hide the breach-check UI in the offline build and drop the OpenSSL dependency. Until then, leave the setting off.

---

## Removed in the Offline Build

Both are compiled out under `PD_OFFLINE_BUILD`; the real `.cpp` files are **not in this repo** (deleted so no networking implementation ships). The headers remain so call sites compile against the no-op stubs.

- **Cloud sync** (`sync_manager.cpp` → `sync_manager_stub.mm`): no login/register, cloud vaults, push/pull, token management, or anonymous sharing. `is_logged_in()` always false; status always "Offline".
- **Browser-extension server** (`local_server.cpp` → `local_server_stub.mm`): no localhost HTTP server on port 19837, no pairing, no autofill endpoints.

---

## Build Notes

- **Xcode project:** `PasswordDefenseOffline.xcodeproj`
- **C++ standard:** C++20 (`gnu++20`)
- **ARC:** Enabled for all Objective-C++ files
- **Offline build:** `PD_OFFLINE_BUILD` is defined for the target; it swaps the `*_stub.mm` files in for `sync_manager.cpp` / `local_server.cpp` (which are absent from this repo).
- **Homebrew deps:** `brew install libsodium openssl@3` (OpenSSL only for cpp-httplib; see [Online Features](#online-features-opt-in) — droppable if the breach-check path is removed).
- **Header search paths:** Must include Homebrew's include dirs for libsodium (and OpenSSL while it's still linked)
- **Library search paths:** Must include Homebrew's lib dirs
- **Preprocessor:** `CPPHTTPLIB_OPENSSL_SUPPORT` (only needed while an HTTPS client path remains)
- **Sandbox:** Disabled (app needs filesystem access for vaults and backups)
