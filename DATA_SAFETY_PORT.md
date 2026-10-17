# Data-Safety Port — pw-windows v1.0.2 → offline macOS fork

Port the pw-windows **v1.0.2 "data-safety + UTF-8 path hardening"** batch into this
offline Mac fork. This fork was forked from the older 2.2.1-era code (via
`pw-mac-cloud`) and predates the batch, so this is a **manual, function-by-function
port — not a `git apply`/cherry-pick** (the trees have diverged and the Mac layout is
flat, no `credentials/` or `tools/` subdirs).

Opus can write/port the C++; **you run each `Build.` step on the Mac** (Xcode — Opus
can't compile ObjC++/Metal).

---

## Source of truth — and the trap

- **Port FROM:** `A:\prjcts\pwmngr\pw-windows` @ commit **`3f17c0a`** (Windows HEAD).
  This is the **FIXED** code — Opus fixed 7 real bugs found in review and committed them
  here.
- **Do NOT port from** `pw-windows-cloud` branch `fix/data-loss-prevention`. That is the
  **same batch still carrying the 7 bugs** (authored by Sonnet+Gemini, never fixed there).
- See the batch diff: `git -C A:\prjcts\pwmngr\pw-windows show 3f17c0a`.

---

## 7 fixed-bug invariants — preserve these; do not reintroduce the buggy design

1. **Recovery snapshot is written SYNCHRONOUSLY on save failure.** No detached/retry
   worker thread. (The buggy version raced `vault_db::close()` on the shared `g_db`
   handle → use-after-free + status-map data race.)
2. **Recovery (de)serialization uses the canonical `cred_ops::to_json/from_json`** — the
   *same* field list as the DB blob. (Buggy version hand-rolled a shorter list → dropped
   card/identity/history/tags and resurrected trashed creds.)
3. **self-destruct mode-3 `rmdir recovery`** is **hoisted OUT of the per-vault loop,
   length-guarded, and runs unconditionally (once).** (Buggy version ran it per-vault
   and conditionally.)
4. **Recovery paths go through `PathToU8`** (macOS: UTF-8 passthrough, see `u8path.h`),
   never raw `path::string()` at a conversion boundary.
5. One additional **Gemini-flagged issue** was also fixed in `3f17c0a`. To inherit it
   automatically, **port by reading the functions as they stand in `3f17c0a`** rather than
   reconstructing them from the batch description.
6. **Do NOT port the dead code** that `3f17c0a` deleted: `EnforceBackupRetention` and
   `sweep_recovery_directory`.

---

## Scope — file by file

### A. Ports VERBATIM (pure cross-platform C++)

| Windows (`3f17c0a`) | Mac target | Notes |
|---|---|---|
| `credentials/credential_ops.cpp/.h` (+423/+79) | `credential_ops.cpp/.h` | Canonical `to_json`/`from_json`. **Struct parity confirmed**: Mac's `Credential` matches Windows except two sync-only extras (`is_dirty`, `server_rev`) that the serializer already ignores — safe. Also switch the DB-blob read/write and the snapshot path to call these. |
| `credentials/crypto/vault_crypto.cpp/.h` (+19/+5) | `vault_crypto.cpp/.h` | `make_key_check` / `verify_key_check` (libsodium `encrypt_credential` of a fixed plaintext under its own AAD). Pure. |

### B. Platform-ADAPTED

| Windows | Mac target | Adaptation |
|---|---|---|
| `tools/u8path.h` (new) | `u8path.h` | **DONE / prepped** — Mac version added (UTF-8 passthrough; no Win32). Still needs adding to the Xcode project. Inert until call sites include it. |
| `tools/save.cpp` (+177) | `save.cpp` | `save_all` transactional write + **synchronous** recovery-snapshot-on-fail. Port `get_recovery_directory_path()` with a Mac branch: Windows `%LOCALAPPDATA%\PasswordDefense\recovery` → Mac `~/Library/Application Support/PasswordDefense/recovery` (reuse the existing `cfg` dir logic). **Signature change:** returns `std::string` on Mac, not `std::wstring` (`save.h`). |
| self-destruct **mode-3** | `save.cpp` ~`self_destruct_atexit_handler` (~L1027) | Fold the FIXED mode-3 into the existing Mac `fork`/`exec` handler. Today it only `rm`s `*.db`; add the recovery-dir cleanup (invariant #3: hoisted, length-guarded, once). |
| `credentials/vault_db.cpp/.h` (+235/+37) | `vault_db.cpp/.h` | Port the **key-check column** + verify-before-overwrite + save-path hardening. Mac `vault_db` carries extra cloud columns — port only the data-safety bits, skip sync. |
| `tools/utility.cpp/.h` (+232/+11) | `utility_stub.mm/.h` | Most of this diff is `PathToU8` at Win32 boundaries = **N/A on Mac**. Secure-zero is already `sodium_memzero` here. Port only any genuinely new file/helper logic. **Lowest priority.** |

### C. UI (cross-platform ImGui — line-match into Mac's consolidated files)

| Windows | Mac target | Notes |
|---|---|---|
| `application.cpp` (+873) | `application.cpp` | The recovery **lifecycle**: `restore_recovery_snapshot_if_newer()` + the save-fail trigger. Mac already has `saved_snapshot`/`apply_snapshot_json` (that's the per-row **highlight** snapshot — reuse the JSON shape, but the recovery-on-fail wiring is new). ⚠️ Mac also has an **unrelated** cloud `recovery_key`/`recovery_blob` (2.2.1) — different feature, do not touch it. |
| `ui_modals.cpp` (+38) | `ui_modals.cpp` | Recovery-restore prompt modal. |
| `ui_internal.h` (+9) | `ui_internal.h` | New decls. |
| `ui_controls.cpp` (+2) | `ui_controls.cpp` | Small wiring. |
| `ui_views_table.cpp` (+2), `ui_views_threepane.cpp` (+14) | `ui_views.cpp` | Mac merged the views into one file — find the equivalent spots. |
| `app_import_export.cpp` (+132) | `app_import_export.cpp` | Port **only** the switch to canonical `to_json`/`from_json`. Keep Mac's `NSOpenPanel`/`NSSavePanel` dialogs. |

### D. SKIP on Mac

- `app.manifest` — Windows UTF-8 code page / `longPathAware`. N/A.
- `program.vcxproj` — add new files to `PasswordDefenseOffline.xcodeproj` instead.
- `tools/config_migrate.h` (+66) — Windows `%APPDATA%`→`%LOCALAPPDATA%` migration. This
  fork is brand new and always uses `~/Library`, so there is **nothing to migrate**. Skip.
- `favicon.cpp` `PathToU8` bits — Mac uses `favicon_stub.mm`.

---

## Ordered steps (each = one small concrete move; `Build.` = you, on the Mac)

1. **[prepped]** `u8path.h` added — add it to the Xcode project target. (No behavior change.)
2. Port canonical `cred_ops::to_json/from_json` (verbatim) and route the DB blob + snapshot
   through them. **Build.**
3. Port `vault_crypto::make/verify_key_check` + the `vault_db` key-check column +
   verify-before-overwrite. **Build.**
4. Port `save_all` transactional write + synchronous recovery snapshot; add the Mac
   `get_recovery_directory_path()`. **Build.**
5. Port the `application.cpp` recovery lifecycle (`restore_..._if_newer` + save-fail
   trigger). **Build.**
6. Port the recovery-restore modal + decls (`ui_modals.cpp`, `ui_internal.h`,
   `ui_controls.cpp`, `ui_views.cpp`). **Build.**
7. Add self-destruct **mode-3** recovery-dir cleanup (fixed/hoisted) to the Mac handler.
   **Build.**
8. *(Later, separate pass)* any leftover `utility`/`import-export` bits.

---

## Acceptance checks (on the Mac, after the port)

- **Wrong master key** on unlock → `verify_key_check` fails → vault is **not** overwritten.
- **Kill the app mid-save** → next launch offers the recovery snapshot → restoring keeps
  cards, identity fields, password history, and tags, and **trashed creds stay trashed**.
- **self-destruct mode-3** wipes the recovery dir exactly once (not per-vault), and only
  within the app's own data dir (length guard).
