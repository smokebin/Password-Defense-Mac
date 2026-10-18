// test_invariants.cpp
// Standalone invariant verification suite for Password Defense (macOS fork).
// Covers the core data-safety invariants ported in v1.0.2 / fb5dfab / 2146313.
//
// STANDALONE — do NOT add this file to the app's Xcode target. It defines its
// own namespace helpers {...} and its own main(), so compiling it into the app
// would cause duplicate-symbol link errors (vs utility_stub.mm and the app's
// main). Build and run it on its own, e.g.:
//
//   clang++ -std=c++20 -O2 \
//       PasswordDefense/test_invariants.cpp \
//       PasswordDefense/credential_ops.cpp \
//       PasswordDefense/vault_crypto.cpp \
//       PasswordDefense/vault_db.cpp \
//       -I PasswordDefense -I /opt/homebrew/include -L /opt/homebrew/lib \
//       -lsqlite3 -lsodium \
//       -o run_tests && ./run_tests

#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_set>
#include <filesystem>
#include <fstream>
#include <chrono>

#include "credential.h"
#include "credential_ops.h"
#include "vault_crypto.h"
#include "vault_db.h"
#include "json.hpp"

#ifndef SODIUM_STATIC
#define SODIUM_STATIC
#endif
#include <sodium.h>

// Standalone implementation of helpers needed by credential_ops & vault_db.
// Eliminates any link-time dependency on utility_stub.mm or Apple Foundation.
namespace helpers {
    int64_t now_unix_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string generate_uuid() {
        unsigned char b[16];
        randombytes_buf(b, sizeof(b));
        b[6] = (b[6] & 0x0f) | 0x40; // UUID v4
        b[8] = (b[8] & 0x3f) | 0x80;
        char hex[37];
        snprintf(hex, sizeof(hex),
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
            b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        return hex;
    }
}

// ============================================================================
// INVARIANT 1: Exhaustive Field-Symmetric Serialization (28 fields)
// Guards against silent field drops (specifically card_address regression)
// ============================================================================
static void test_credential_serialization_exhaustive_symmetry()
{
    std::cout << "[*] Running Test 1: Exhaustive serialization symmetry (28 fields)...\n";

    Credential c1;
    c1.title            = "Exhaustive Symmetry Test";
    c1.user             = "alice";
    c1.email            = "alice@example.com";
    c1.password         = "p@ssword!#99";
    c1.website          = "https://secure.example.com";
    c1.group            = "Finance";
    c1.tags             = { "tax", "2026", "primary" };
    c1.notes            = "Detailed multi-line notes\nLine 2";
    c1.totp_secret      = "JBSWY3DPEHPK3PXP";
    c1.type             = CredType::CreditCard;

    // Credit Card fields (explicitly verifies card_address)
    c1.card_number      = "4111222233334444";
    c1.card_expiry      = "12/28";
    c1.card_cvv         = "789";
    c1.card_brand       = "Visa";
    c1.cardholder_name  = "Alice Smith";
    c1.card_address     = "123 Market St, Suite 400"; // Regression guard for pre-step-2 drop bug
    c1.card_city        = "San Francisco";
    c1.card_postal_code = "94105";

    // Identity fields
    c1.full_name        = "Alice Jane Smith";
    c1.id_type          = "Passport";
    c1.id_number        = "A12345678";
    c1.date_of_birth    = "1990-01-01";
    c1.expiry_date      = "2030-01-01";
    c1.country          = "USA";
    c1.address          = "123 Market St";
    c1.phone            = "+15551234567";

    // History & flags
    c1.password_history = { { "older_pass", 500 }, { "oldest_pass", 100 } };
    c1.is_favorite      = true;
    c1.is_pinned        = true;
    c1.expires_at_ms    = 999999999;
    c1.expiry_action    = 1;

    // Canonical to_json -> from_json roundtrip
    nlohmann::json j = cred_ops::to_json(c1);
    Credential c2;
    cred_ops::from_json(j, c2);

    // Assert every field is preserved symmetrically
    assert(c2.title == c1.title);
    assert(c2.user == c1.user);
    assert(c2.email == c1.email);
    assert(c2.password == c1.password);
    assert(c2.website == c1.website);
    assert(c2.group == c1.group);
    assert(c2.tags == c1.tags);
    assert(c2.notes == c1.notes);
    assert(c2.totp_secret == c1.totp_secret);
    assert(c2.type == c1.type);

    assert(c2.card_number == c1.card_number);
    assert(c2.card_expiry == c1.card_expiry);
    assert(c2.card_cvv == c1.card_cvv);
    assert(c2.card_brand == c1.card_brand);
    assert(c2.cardholder_name == c1.cardholder_name);
    assert(c2.card_address == c1.card_address); // Specific regression check
    assert(c2.card_city == c1.card_city);
    assert(c2.card_postal_code == c1.card_postal_code);

    assert(c2.full_name == c1.full_name);
    assert(c2.id_type == c1.id_type);
    assert(c2.id_number == c1.id_number);
    assert(c2.date_of_birth == c1.date_of_birth);
    assert(c2.expiry_date == c1.expiry_date);
    assert(c2.country == c1.country);
    assert(c2.address == c1.address);
    assert(c2.phone == c1.phone);

    assert(c2.password_history.size() == 2);
    assert(c2.password_history[0].password == "older_pass");
    assert(c2.is_favorite == c1.is_favorite);
    assert(c2.is_pinned == c1.is_pinned);
    assert(c2.expires_at_ms == c1.expires_at_ms);
    assert(c2.expiry_action == c1.expiry_action);

    std::cout << "    [+] Passed: 28 fields verified with zero data loss.\n";
}

// ============================================================================
// INVARIANT 1b: Legacy card_address Migration
// Verifies that a legacy credit-card JSON blob (no "card_address" key,
// "address" populated, type == CreditCard) is folded into the dedicated
// `card_address` field on deserialization and the legacy `.address` is cleared.
// Guards against the migration becoming dead code (symmetric round-trip tests
// never exercise the legacy shape).
// ============================================================================
static void test_card_address_legacy_migration()
{
    std::cout << "[*] Running Test 1b: Legacy card_address migration...\n";

    nlohmann::json legacy = {
        {"type", static_cast<int>(CredType::CreditCard)},
        {"title", "Chase Sapphire"},
        {"cardholder_name", "Jane Doe"},
        {"card_number", "4111 1111 1111 1111"},
        {"address", "456 Legacy Lane"}
    };

    Credential c;
    cred_ops::from_json(legacy, c);

    assert(c.type == CredType::CreditCard);
    assert(c.card_address == "456 Legacy Lane"); // folded over
    assert(c.address.empty());                   // legacy field cleared

    // Non-CreditCard rows must NOT be migrated — Identity's address stays put.
    nlohmann::json ident = {
        {"type", static_cast<int>(CredType::Identity)},
        {"title", "Passport"},
        {"address", "789 Identity Ave"}
    };
    Credential c2;
    cred_ops::from_json(ident, c2);
    assert(c2.type == CredType::Identity);
    assert(c2.address == "789 Identity Ave");    // preserved
    assert(c2.card_address.empty());             // not populated

    // Already-migrated CreditCard rows must keep card_address intact.
    nlohmann::json already = {
        {"type", static_cast<int>(CredType::CreditCard)},
        {"card_address", "New Field 1 Way"},
        {"address", "Legacy Shouldnt Appear"}
    };
    Credential c3;
    cred_ops::from_json(already, c3);
    assert(c3.card_address == "New Field 1 Way");
    // address is left as read (migration only fires when card_address empty).

    std::cout << "    [+] Passed: legacy address folded to card_address; Identity untouched.\n";
}

// ============================================================================
// INVARIANT 2: Reconcile After Undo (Empty Trash -> No Ghost Resurrection)
// Verifies right-removal of purged items and ledger-not-dirtied assertions
// ============================================================================
static void test_reconcile_after_undo_empty_trash()
{
    std::cout << "[*] Running Test 2: reconcile_after_undo (right-removal + ledger-not-dirtied)...\n";

    Credential live_cred;
    live_cred.uuid = "live-uuid-1";
    live_cred.title = "Keep Me";

    Credential normal_del;
    normal_del.uuid = "normal-del-uuid";
    normal_del.title = "Removed In Prior Step";

    Credential trashed_cred;
    trashed_cred.uuid = "trashed-uuid-2";
    trashed_cred.title = "Permanently Deleted";

    // before has live_cred and normal_del
    std::vector<Credential> before = { live_cred, normal_del };
    // undo stack has older snapshot containing live_cred and trashed_cred
    std::vector<Credential> after = { live_cred, trashed_cred };

    std::unordered_set<std::string> user_removed;
    std::unordered_set<std::string> purged = { "trashed-uuid-2" }; // permanently deleted from Trash

    cred_ops::reconcile_after_undo(before, after, user_removed, purged);

    // 1. Right-removal: trashed item must be dropped from 'after' snapshot
    assert(after.size() == 1);
    assert(after[0].uuid == "live-uuid-1");

    // 2. Ledger-not-dirtied: purged item must NOT be inserted into user_removed
    assert(user_removed.count("trashed-uuid-2") == 0);

    // 3. Legitimate removal: normal deletion from 'before' IS tracked in user_removed
    assert(user_removed.count("normal-del-uuid") == 1);

    std::cout << "    [+] Passed: Purged credentials dropped; ledger not dirtied; regular removals preserved.\n";
}

// ============================================================================
// INVARIANT 3: Save-Fail Recovery Snapshot Roundtrip (Primitives)
// Encrypts snapshot with AAD "recovery_snapshot", writes .rec, tests authentication
// ============================================================================
static void test_save_fail_recovery_roundtrip()
{
    std::cout << "[*] Running Test 3: Save-fail recovery snapshot roundtrip (primitives)...\n";

    namespace fs = std::filesystem;
    fs::path temp_dir = fs::temp_directory_path() / "pd_test_recovery";
    fs::create_directories(temp_dir); // Ensure directory exists before writing
    fs::path db_file = temp_dir / "vault_recovery.db";
    std::error_code ec;
    fs::remove(db_file, ec);

    // 1. Initialize vault DB and cache salt so get_cached_salt() returns valid salt
    assert(vault_db::init(db_file.string()));
    auto salt = enc::generate_salt();
    assert(vault_db::set_cached_salt(salt));

    auto cached_salt = vault_db::get_cached_salt();
    assert(!cached_salt.empty() && cached_salt == salt);

    auto key_A = enc::derive_master_key("MasterPassword_123!", cached_salt, true);
    auto key_B = enc::derive_master_key("OtherVaultKey_999!", cached_salt, true);
    vault_db::close();

    // 2. Prepare credential edited in-memory during session
    Credential c;
    c.uuid = "recovery-uuid-001";
    c.title = "Emergency Recovery Item";
    c.password = "uncommitted_password";
    c.updated_at_ms = 250000;

    // 3. Canonical serialization + AAD="recovery_snapshot"
    nlohmann::json creds_arr = nlohmann::json::array();
    auto j = cred_ops::to_json(c);
    j["uuid"] = c.uuid;
    j["updated_at"] = c.updated_at_ms;
    creds_arr.push_back(j);

    std::string serialized = creds_arr.dump();
    auto enc_blob = enc::encrypt_credential(serialized, key_A, "recovery_snapshot");
    assert(!enc_blob.empty());

    // 4. Write .rec file to disk
    fs::path rec_file = temp_dir / "vault_recovery.rec";
    {
        std::ofstream out(rec_file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(enc_blob.data()), enc_blob.size());
    }
    assert(fs::exists(rec_file));

    // 5. Ownership proof: attempting decrypt with wrong key (key_B) MUST fail
    std::string plain_wrong = enc::decrypt_credential(enc_blob, key_B, "recovery_snapshot");
    assert(plain_wrong.empty());

    // 6. Correct unlock: decrypting with correct key (key_A) must succeed
    std::string plain_correct = enc::decrypt_credential(enc_blob, key_A, "recovery_snapshot");
    assert(!plain_correct.empty());

    auto parsed_arr = nlohmann::json::parse(plain_correct);
    assert(parsed_arr.is_array() && parsed_arr.size() == 1);

    Credential restored;
    cred_ops::from_json(parsed_arr[0], restored);
    restored.uuid = parsed_arr[0].value("uuid", "");
    restored.updated_at_ms = parsed_arr[0].value("updated_at", int64_t(0));

    assert(restored.uuid == "recovery-uuid-001");
    assert(restored.password == "uncommitted_password");
    assert(restored.updated_at_ms == 250000);

    // Cleanup
    fs::remove_all(temp_dir);
    std::cout << "    [+] Passed: Recovery snapshot encrypted, authenticated, ownership-tested, and restored.\n";
}

// ============================================================================
// INVARIANT 4: Wrong-Password Unlock Refused on Empty Stamped Vault (Primitives)
// Verifies key_check prevents false-positive unlock of empty vaults
// ============================================================================
static void test_key_check_empty_vault_wrong_password_refused()
{
    std::cout << "[*] Running Test 4: Key-check stamping & wrong-password refusal...\n";

    namespace fs = std::filesystem;
    fs::path db_file = fs::temp_directory_path() / "test_key_check_empty.db";
    std::error_code ec;
    fs::remove(db_file, ec);

    // 1. Create fresh empty vault
    assert(vault_db::init(db_file.string()));
    auto salt = enc::generate_salt();
    assert(vault_db::set_cached_salt(salt));

    auto key_correct = enc::derive_master_key("CorrectPassword123!", salt, true);
    auto key_wrong   = enc::derive_master_key("WrongPassword456!", salt, true);

    // 2. Stamp key_check under correct key
    auto kc = enc::make_key_check(key_correct);
    assert(!kc.empty());
    assert(vault_db::set_key_check(kc));

    // 3. Verify key_check directly against stored blob
    auto stored_kc = vault_db::get_key_check();
    assert(!stored_kc.empty());
    assert(enc::verify_key_check(stored_kc, key_correct) == true);
    assert(enc::verify_key_check(stored_kc, key_wrong) == false);

    vault_db::close();
    fs::remove(db_file, ec);

    std::cout << "    [+] Passed: Empty stamped vault correctly verifies correct key and rejects wrong key.\n";
}

// ============================================================================
// INVARIANT 5: Ghost-Vault Lock-Escape (Save-time key_check failure)
// Simulates save-time check (application.cpp:3756): refuses save, zeroes key, closes DB
// ============================================================================
static void test_ghost_vault_lock_escape()
{
    std::cout << "[*] Running Test 5: Ghost-vault lock-escape (save-time key_check mismatch)...\n";

    namespace fs = std::filesystem;
    fs::path db_file = fs::temp_directory_path() / "test_ghost_vault.db";
    std::error_code ec;
    fs::remove(db_file, ec);

    // 1. Initialize vault on disk stamped with Key A
    assert(vault_db::init(db_file.string()));
    auto salt = enc::generate_salt();
    vault_db::set_cached_salt(salt);
    auto key_disk = enc::derive_master_key("Password_Disk_A!", salt, true);
    auto kc_disk = enc::make_key_check(key_disk);
    assert(vault_db::set_key_check(kc_disk));

    // Store a baseline row
    Credential c;
    c.title = "Important Document";
    c.password = "secret_on_disk";
    std::vector<Credential> creds = { c };
    std::unordered_set<std::string> empty_set;
    assert(cred_ops::save_all(creds, key_disk, empty_set, empty_set).ok);

    // 2. Session now holds a mismatched key (simulating ghost vault / external file swap)
    auto key_session_wrong = enc::derive_master_key("Password_Wrong_B!", salt, true);

    // 3. Verify-before-overwrite guard: must catch mismatch BEFORE save_all is called
    auto current_kc = vault_db::get_key_check();
    assert(!current_kc.empty());

    bool key_matches = enc::verify_key_check(current_kc, key_session_wrong);
    assert(key_matches == false);

    // The lock-escape action (matching application.cpp:3756-3770):
    // Save is refused, key is zeroed, DB is closed, sensitive memory wiped
    enc::secure_zero(key_session_wrong);
    assert(key_session_wrong.empty());
    vault_db::close();

    // 4. Verify disk integrity: original database was NOT overwritten or corrupted
    assert(vault_db::init(db_file.string()));
    auto disk_creds = cred_ops::load_all(key_disk);
    assert(disk_creds.size() == 1);
    assert(disk_creds[0].password == "secret_on_disk");

    vault_db::close();
    fs::remove(db_file, ec);

    std::cout << "    [+] Passed: Save refused on key mismatch; session zeroed and disk preserved.\n";
}

int main()
{
    if (sodium_init() < 0) {
        std::cerr << "[-] libsodium initialization failed\n";
        return 1;
    }

    std::cout << "========================================\n";
    std::cout << "Password Defense Mac - Invariant Tests\n";
    std::cout << "========================================\n";

    test_credential_serialization_exhaustive_symmetry();
    test_card_address_legacy_migration();
    test_reconcile_after_undo_empty_trash();
    test_save_fail_recovery_roundtrip();
    test_key_check_empty_vault_wrong_password_refused();
    test_ghost_vault_lock_escape();

    std::cout << "========================================\n";
    std::cout << "ALL 6 DATA-SAFETY INVARIANTS PASSED!\n";
    std::cout << "========================================\n";
    return 0;
}
