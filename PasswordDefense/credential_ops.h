// credential_ops.h
// High-level Credential operations (encrypt/decrypt + database)
// Works with decrypted Credential structs

#pragma once

#include "credential.h"
#include "json.hpp"
#include <string>
#include <unordered_set>
#include <vector>
#include <cstdint>

namespace cred_ops {

    // Canonical content-field (de)serialization: the single source of truth for which
    // Credential fields persist. The DB vault blob and the autolock/undo snapshots
    // both go through these, so a new field is covered everywhere at once. uuid and
    // the created/updated/deleted timestamps are NOT included (they are DB columns
    // for the vault blob); callers that store them in the JSON itself, e.g. undo
    // snapshots, add and read them separately.
    nlohmann::json to_json(const Credential& c);
    void           from_json(const nlohmann::json& j, Credential& c);

    // ----------------------------
    // Load/Save operations
    // ----------------------------

    // Load all credentials from SQLite (decrypted into memory)
    // Returns empty vector on failure (wrong key, db not open, etc.)
    std::vector<Credential> load_all(const std::vector<uint8_t>& master_key);

    // ----------------------------
    // Transactional save (data-safety batch — ported from pw-windows @ 3f17c0a)
    // ----------------------------

    struct SaveResult {
        bool ok = false;
        int  hard_deleted = 0;
        std::string error;
    };

    // Save-time hard-delete policy. Returns the live DB rows to hard-delete: only those
    // the user explicitly removed this session (recorded, never inferred) that aren't
    // back in memory. Anything else missing from memory (failed to decrypt, failed to
    // parse, unknown schema, written by another device) is left untouched.
    std::vector<std::string> select_hard_deletes(
        const std::vector<std::string>& live_db_uuids,
        const std::unordered_set<std::string>& memory_uuids,
        const std::unordered_set<std::string>& user_removed);

    // Writes the in-memory vault to the DB in ONE transaction:
    //  - entries with no row (no uuid, or a uuid with no live/trashed row) are inserted,
    //    except uuids in `purged` (permanently deleted by the user this session)
    //  - entries with a row are updated; trashed rows back in memory are restored
    //  - live rows missing from memory are hard-deleted only if in `user_removed`
    //  - a legacy vault gets its key-check stamped once it has content
    // On ANY failure (including COMMIT) the transaction is rolled back and uuids assigned
    // by this call are cleared again, so memory matches the unchanged DB and the next
    // save retries without losing or duplicating anything.
    SaveResult save_all(std::vector<Credential>& creds,
                        const std::vector<uint8_t>& master_key,
                        const std::unordered_set<std::string>& user_removed,
                        const std::unordered_set<std::string>& purged);

    // Undo bookkeeping. `before` = memory before the undo, `after` = the restored
    // snapshot (modified in place). Drops anything in `purged` from `after` so
    // emptied-trash items can't visibly resurrect; records saved uuids the undo
    // removed in `user_removed`, and un-records any that came back.
    void reconcile_after_undo(const std::vector<Credential>& before,
                              std::vector<Credential>& after,
                              std::unordered_set<std::string>& user_removed,
                              const std::unordered_set<std::string>& purged);

    // Load deleted (trash) credentials from SQLite (decrypted for trash UI)
    std::vector<Credential> load_deleted(const std::vector<uint8_t>& master_key);

    // ----------------------------
    // CRUD operations
    // ----------------------------

    // Add new Credential (generates UUID, encrypts, stores)
    // Returns the assigned UUID, or empty string on failure
    std::string add(const Credential& c, const std::vector<uint8_t>& master_key);

    // Update existing Credential by UUID
    bool update(const std::string& uuid, const Credential& c, const std::vector<uint8_t>& master_key);

    // Encrypt a credential (for save_vault_to_disk — preserves timestamps)
    std::vector<uint8_t> encrypt_cred_public(const Credential& c, const std::vector<uint8_t>& key, const std::string& uuid);

    // Delete Credential by UUID (soft delete for sync)
    bool remove(const std::string& uuid);

    // Hard delete (after sync confirmation)
    bool remove_permanent(const std::string& uuid);

    // ----------------------------
    // Batch operations
    // ----------------------------

    // Import multiple credentials in one transaction.
    // Uses vault_db::upsert_credential under the hood: same-UUID + newer
    // updated_at_ms replaces in place; older is skipped; new UUID inserts.
    // Caller supplies UUID per cred — empty string means "always insert
    // as new" (a fresh UUID is generated). Out-params receive per-outcome
    // counts (any may be null).
    bool import_credentials(
        const std::vector<Credential>& creds,
        const std::vector<uint8_t>& master_key,
        int* out_inserted = nullptr,
        int* out_updated  = nullptr,
        int* out_skipped  = nullptr,
        std::string* out_error = nullptr
    );

    // ----------------------------
    // Utility
    // ----------------------------

    // Check if vault has any local changes pending sync
    bool has_pending_changes();

    // Get count of credentials
    int count();

}
