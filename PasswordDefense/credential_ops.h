// credential_ops.h
// High-level Credential operations (encrypt/decrypt + database)
// Works with decrypted Credential structs

#pragma once

#include "credential.h"
#include <vector>
#include <cstdint>

namespace cred_ops {

    // ----------------------------
    // Load/Save operations
    // ----------------------------

    // Load all credentials from SQLite (decrypted into memory)
    // Returns empty vector on failure (wrong key, db not open, etc.)
    std::vector<Credential> load_all(const std::vector<uint8_t>& master_key);

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
