# PasswordDefense — macOS Port

## Overview

PasswordDefense is a cross-platform password manager originally built for Windows (DX11/Win32). This documents the macOS port, which replaces the rendering backend with Metal and the windowing layer with Cocoa/AppKit, while sharing the vast majority of application logic as cross-platform C++.

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
│  sync_manager     – cloud sync (httplib+OpenSSL)  │
│  local_server     – browser extension API         │
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
| `sync_manager.cpp` | Cloud sync — login, register, push/pull, token refresh |
| `local_server.cpp` | Browser extension HTTP API (port 19837) |
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
| OpenSSL | 3.x | TLS for HTTPS (sync, favicons, breach check) |
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

## Favicon System

Implemented in `favicon_stub.mm`:

1. `favicon::Get(domain)` checks cache, returns `ImTextureID` if available
2. On cache miss, starts async download via `NSURLSession` from `https://www.google.com/s2/favicons?domain=...&sz=32`
3. Response decoded: `NSImage` → `CGBitmapContext` (RGBA, un-premultiplied alpha)
4. Metal texture created with `MTLTextureDescriptor` (32x32, RGBA8Unorm)
5. Ownership transferred out of ARC via `__bridge_retained` cast to `ImTextureID`
6. Thread-safe: `std::mutex` protects cache, pending, and failed sets

---

## Sync System

The sync system (`sync_manager.cpp`) is fully cross-platform — uses cpp-httplib with OpenSSL on macOS. Features:

- **Authentication:** Email + password hash login/register
- **Cloud vaults:** List, create, sync
- **Push/pull:** Full vault sync with conflict resolution
- **Token management:** Access + refresh tokens stored in vault DB
- **Anonymous sharing:** Generate shareable vault links
- **Breach checking:** Check passwords against breach databases

---

## Browser Extension

Local HTTP server (`local_server.cpp`) on port **19837**:

- HMAC-SHA256 request validation
- Pairing flow with UUID exchange
- Session tokens with expiration
- Credential autofill endpoints
- Public Suffix List for domain matching (eTLD+1)
- Cross-thread vault switch signaling

---

## Build Notes

- **Xcode project:** `PasswordDefense.xcodeproj`
- **C++ standard:** C++20 (`gnu++20`)
- **ARC:** Enabled for all Objective-C++ files
- **Homebrew deps:** `brew install libsodium openssl@3`
- **Header search paths:** Must include Homebrew's include dirs for libsodium and OpenSSL
- **Library search paths:** Must include Homebrew's lib dirs
- **Preprocessor:** `CPPHTTPLIB_OPENSSL_SUPPORT` must be defined for HTTPS support
- **Sandbox:** Disabled (app needs filesystem access for vaults and backups)
