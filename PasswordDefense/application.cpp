
// application.cpp (Password Manager - ShellState + VaultState wired)

#include <sys/mman.h>
#include <algorithm>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "GUI.h"        // your ImGui/D3D bootstrap
#include "UI.h"         // Shell + Accordion
#include "credential.h"      // Credential struct
#include "vault_db.h"
#include "credential_ops.h"
#include "twofa_ops.h"
#include "vault_crypto.h"
#include <sodium.h>
#include "json.hpp"
#include "pwm_file.h"
#include "kdbx_export.h"
#include "utility_stub.h"
#include "totp.h"
#include "auth_crypto.h"  // Option C auth hash
#include "favicon.h"
#include "u8path.h"       // helpers::U8Path / PathToU8 for recovery-dir I/O
#include "sync_manager.h"
#include "bridge_server.h" // Native-messaging Unix-socket bridge for the browser extension
#include "save.h"          // Config persistence
#include "app_internal.h"     // Shared declarations for split TUs

#ifdef __APPLE__
#include <limits.h>
#include <unistd.h>
#include <chrono>
#include <mach-o/dyld.h>
#ifndef MAX_PATH
#define MAX_PATH PATH_MAX
#endif
#endif

// Native-messaging bridge for the browser extension. A user-only Unix
// socket in the App Group container (shared with pw-mac-cloud and the
// Safari extension) — no network listener, so it fits the offline build.
// Externally linked so the Settings UI (ui_controls.cpp) can read status.
BridgeServer g_bridge;

// Defined in GUI.mm — checks [NSApp isActive]
extern bool render_app_is_active();

// serialize_creds_json / deserialize_creds_json moved to app_import_export.cpp

// Pin a buffer in physical RAM (prevent paging to disk)
static void secure_lock(void* ptr, size_t len)
{
    if (ptr && len > 0)
        mlock(ptr, len);
}

// Unpin a buffer (allow paging again) — call before zeroing + freeing
static void secure_unlock(void* ptr, size_t len)
{
    if (ptr && len > 0)
        munlock(ptr, len);
}

// Lock a std::vector<uint8_t> in RAM (call after key derivation)
static void lock_key(std::vector<uint8_t>& key)
{
    if (!key.empty())
        secure_lock(key.data(), key.size());
}

// Unlock + zero + clear a key
static void unlock_and_zero_key(std::vector<uint8_t>& key)
{
    if (!key.empty())
    {
        secure_unlock(key.data(), key.size());
        sodium_memzero(key.data(), key.size());
    }
    key.clear();
}

// Lock a std::string in RAM (call after password storage)
static void lock_string(std::string& s)
{
    if (!s.empty())
        secure_lock(s.data(), s.size());
}

// Unlock + zero + clear a string
static void unlock_and_zero_string(std::string& s)
{
    if (!s.empty())
    {
        secure_unlock(s.data(), s.size());
        sodium_memzero(s.data(), s.size());
    }
    s.clear();
}

// Securely zero all sensitive fields of a Credential before destruction
static void secure_clear_credential(Credential& c)
{
    sodium_memzero(c.password.data(), c.password.size());
    sodium_memzero(c.user.data(), c.user.size());
    sodium_memzero(c.email.data(), c.email.size());
    sodium_memzero(c.notes.data(), c.notes.size());
    sodium_memzero(c.totp_secret.data(), c.totp_secret.size());
    sodium_memzero(c.card_number.data(), c.card_number.size());
    sodium_memzero(c.card_cvv.data(), c.card_cvv.size());
    sodium_memzero(c.card_expiry.data(), c.card_expiry.size());
    sodium_memzero(c.cardholder_name.data(), c.cardholder_name.size());
    sodium_memzero(c.id_number.data(), c.id_number.size());
    sodium_memzero(c.phone.data(), c.phone.size());
    sodium_memzero(c.address.data(), c.address.size());
    sodium_memzero(c.date_of_birth.data(), c.date_of_birth.size());
    for (auto& h : c.password_history)
        sodium_memzero(h.password.data(), h.password.size());
}

// Securely zero all credentials in a vector, then clear it
static void secure_clear_credentials(std::vector<Credential>& creds)
{
    for (auto& c : creds)
        secure_clear_credential(c);
    creds.clear();
}

// ============================================================
// Duplicate-credential signature
// Used by import-merge to reuse the uuid of an existing credential with the
// same site+email+username, so re-imports update in place via newer-wins
// instead of minting fresh UUIDs and creating duplicates.
// ============================================================

static std::string dedup_lower(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (unsigned char c : s) out += (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return out;
}

// Normalize a URL/host for matching: lowercase, strip scheme + "www.", trim
// trailing slash and anything after the first '/'. "https://www.Gmail.com/login" → "gmail.com".
static std::string dedup_norm_site(const std::string& url) {
    std::string s = dedup_lower(url);
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t')) --b;
    s = s.substr(a, b - a);
    if (s.rfind("https://", 0) == 0) s.erase(0, 8);
    else if (s.rfind("http://", 0) == 0) s.erase(0, 7);
    if (s.rfind("www.", 0) == 0) s.erase(0, 4);
    size_t slash = s.find('/');
    if (slash != std::string::npos) s.erase(slash);
    return s;
}

// Content signature: normalized site + email + username. Returns "" when not
// matchable (not a password, no site, or no login). BOTH email AND username
// (not OR) so two distinct accounts that share an email stay separate — a
// false match here in the merge path would silently collapse two real
// accounts into one (data loss), not just create a dupe.
static std::string cred_signature(const Credential& c)
{
    if (c.type != CredType::Password) return "";
    const std::string site = dedup_norm_site(c.website);
    if (site.empty()) return "";
    const std::string email = dedup_lower(c.email);
    const std::string user  = dedup_lower(c.user);
    if (email.empty() && user.empty()) return "";
    return site + "|" + email + "|" + user;
}

// Securely zero all undo stack strings, then clear the stack
static void secure_clear_undo_stack(std::vector<std::string>& stack)
{
    for (auto& s : stack)
        sodium_memzero(s.data(), s.size());
    stack.clear();
}


struct VaultState
{
    bool        unlocked = false;
    bool        tofa_pending = false;  // 2FA verification pending after password success
    bool        add_pin = false;
    bool        add_fav = false;
    bool        add_open = false;
    bool        status_is_error = false;
    bool        dirty = false;    // Real dirty system (cached)

    bool add_show_password = false;

    // Edit form state
    bool        edit_open = false;
    bool        edit_pin = false;
    bool        edit_fav = false;
    bool        edit_show_password = false;
    int         edit_id = -1;  // ID of Credential being edited

    uint32_t saved_hash = 0;
    uint32_t current_hash = 0;
    uint32_t last_groups_hash = 0;  // Per-vault group rebuild tracking

    std::string vault_path;              // .db file path
    std::vector<uint8_t> master_key;     // Derived key (kept in memory while unlocked)
    std::string session_password;        // Password (kept for restore operations)
    std::string add_password_buf{};
    std::string edit_password_buf{};
    std::string status_msg;

    std::vector<std::string> undo_stack;
    std::vector<Credential> creds;
    std::unordered_map<std::string, Credential> saved_snapshot; // keyed by uuid

    // Save-time deletion ledger. Populated by UI actions; drained by cred_ops::save_all.
    // user_removed_uuids = uuids the user explicitly soft-deleted (moved to trash) this
    //   session but hasn't persisted yet. Save-time hard-delete looks at THIS, never at
    //   "memory - db" inference, so a row that failed to decrypt isn't silently nuked.
    // purged_uuids = uuids the user permanently deleted from trash this session. Save
    //   skips re-inserting them even if they come back via undo.
    std::unordered_set<std::string> user_removed_uuids;
    std::unordered_set<std::string> purged_uuids;

    Credential  add_buf{};
    Credential  edit_buf{};

    helpers::GenOptions add_gen_opt; // optional (if you want persistent toggles)
    helpers::GenOptions edit_gen_opt;

    void init_defaults_once()
    {
        static bool inited = false;
        if (inited) return;
        inited = true;

        add_gen_opt.length = 30;
        add_gen_opt.use_upper = true;
        add_gen_opt.use_lower = true;
        add_gen_opt.use_digit = true;
        add_gen_opt.use_symbol = true;
        add_gen_opt.avoid_ambiguous = false;

        edit_gen_opt.length = 30;
        edit_gen_opt.use_upper = true;
        edit_gen_opt.use_lower = true;
        edit_gen_opt.use_digit = true;
        edit_gen_opt.use_symbol = true;
        edit_gen_opt.avoid_ambiguous = false;
    }

    void set_status(const std::string& msg, bool is_err)
    {
        status_msg = msg;
        status_is_error = is_err;
    }

    void clear_status()
    {
        status_msg.clear();
        status_is_error = false;
    }

    bool is_dirty() const
    {
        return unlocked && dirty;
    }

    void push_undo()
    {
        // Store snapshot BEFORE change
        undo_stack.push_back(serialize_creds_json(creds));
        // cap (optional) — securely zero evicted entry
        if (undo_stack.size() > 128)
        {
            sodium_memzero(undo_stack.front().data(), undo_stack.front().size());
            undo_stack.erase(undo_stack.begin());
        }
    }

    void apply_snapshot_json(const std::string& json)
    {
        creds = deserialize_creds_json(json);
    }

    void clear_sensitive()
    {
        // Unlock from physical RAM, zero, and free
        unlock_and_zero_key(master_key);
        unlock_and_zero_string(session_password);

        // Zero saved snapshot Credential data before clearing
        for (auto& [uuid, c] : saved_snapshot)
            secure_clear_credential(c);
        saved_snapshot.clear();

        // Pending-save ledger — belongs to the unlocked session, drop on lock.
        user_removed_uuids.clear();
        purged_uuids.clear();

        unlocked = false;
        tofa_pending = false;
    }

    void reset_add_form()
    {
        add_buf = Credential{};
        add_password_buf.clear();
        add_pin = false;
        add_fav = false;
        add_show_password = false;
    }

    void reset_edit_form()
    {
        edit_buf = Credential{};
        edit_password_buf.clear();
        edit_pin = false;
        edit_fav = false;
        edit_show_password = false;
        edit_id = -1;
    }

    void begin_edit(const Credential& c, const std::string& password)
    {
        edit_buf = c;
        edit_password_buf = password;
        edit_pin = c.is_pinned;
        edit_fav = c.is_favorite;
        edit_show_password = false;
        edit_id = c.id;
        edit_open = true;
    }
};

// ============================================================
// Credential Modal: Unified Add/Edit dialog
// ============================================================
struct CredentialModal
{
    enum class Mode { Closed, Add, Edit };
    Mode mode = Mode::Closed;

    // Shared form state
    Credential buf{};
    std::string password_buf{};
    std::string totp_secret_buf{};
    std::string totp_warning{};   // otpauth:// paste problem or non-standard-settings warning
    bool show_password = false;
    bool is_pinned = false;
    bool is_favorite = false;
    bool is_timed = false;
    int  duration_idx = 3;        // default: 24 hours
    int  expiry_action_idx = 0;   // default: auto-trash
    helpers::GenOptions gen_opt{ 30, true, true, true, true, false };
    std::string tag_input_buf;

    CredType selected_type = CredType::Password;

    // Edit-specific
    int edit_id = -1;

    bool IsOpen() const { return mode != Mode::Closed; }
    bool IsAdd()  const { return mode == Mode::Add; }
    bool IsEdit() const { return mode == Mode::Edit; }

    void OpenAdd()
    {
        mode = Mode::Add;
        buf = Credential{};
        password_buf.clear();
        totp_secret_buf.clear();
        totp_warning.clear();
        show_password = false;
        is_pinned = false;
        is_favorite = false;
        is_timed = false;
        duration_idx = 3;
        expiry_action_idx = 0;
        selected_type = CredType::Password;
        edit_id = -1;
        tag_input_buf.clear();
        // Keep gen_opt settings persistent
    }

    void OpenEdit(const Credential& c, const std::string& pw)
    {
        mode = Mode::Edit;
        buf = c;
        // Pre-format masked fields for display
        buf.card_number   = ui::FormatWithPattern(ui::StripNonDigits(c.card_number),   "#### #### #### ####");
        buf.card_expiry   = ui::FormatWithPattern(ui::StripNonDigits(c.card_expiry),   "## / ##");
        buf.date_of_birth = ui::FormatWithPattern(ui::StripNonDigits(c.date_of_birth), "## / ## / ####");
        buf.expiry_date   = ui::FormatWithPattern(ui::StripNonDigits(c.expiry_date),   "## / ## / ####");
        buf.phone         = ui::FormatWithPattern(ui::StripNonDigits(c.phone),         "(###) ###-####");
        password_buf = pw;
        totp_secret_buf = c.totp_secret;
        totp_warning.clear();
        show_password = false;
        is_pinned = c.is_pinned;
        is_favorite = c.is_favorite;
        is_timed = (c.expires_at_ms > 0);
        duration_idx = 3;  // default to 24h when editing
        expiry_action_idx = c.expiry_action;
        selected_type = c.type;
        edit_id = c.id;
    }

    void Close()
    {
        mode = Mode::Closed;
    }

    void Reset()
    {
        mode = Mode::Closed;
        buf = Credential{};
        password_buf.clear();
        totp_secret_buf.clear();
        totp_warning.clear();
        show_password = false;
        is_pinned = false;
        is_favorite = false;
        is_timed = false;
        duration_idx = 3;
        expiry_action_idx = 0;
        selected_type = CredType::Password;
        edit_id = -1;
    }
};

static CredentialModal g_cred_modal;

// ============================================================
// Sync Auth Modal: Login/Register dialog for cloud sync
// ============================================================
struct SyncAuthModal
{
    enum class Mode { Closed, Login, Register };
    Mode mode = Mode::Closed;

    // Form fields
    std::string server_url;
    std::string email;
    std::string username;           // Register only
    std::string password;
    std::string password_confirm;   // Register only
    bool show_password = false;

    // State
    std::string error_msg;
    bool loading = false;

    bool IsOpen() const { return mode != Mode::Closed; }
    bool Islogin() const { return mode == Mode::Login; }
    bool Isregister_user() const { return mode == Mode::Register; }

    void Openlogin(const std::string& default_url = "")
    {
        mode = Mode::Login;
        server_url = default_url;
        email.clear();
        username.clear();
        password.clear();
        password_confirm.clear();
        error_msg.clear();
        show_password = false;
        loading = false;
    }

    void Openregister_user(const std::string& default_url = "")
    {
        mode = Mode::Register;
        server_url = default_url;
        email.clear();
        username.clear();
        password.clear();
        password_confirm.clear();
        error_msg.clear();
        show_password = false;
        loading = false;
    }

    void Close()
    {
        mode = Mode::Closed;
        // Clear sensitive data
        password.clear();
        password_confirm.clear();
        error_msg.clear();
    }
};

static SyncAuthModal g_sync_modal;
static std::unique_ptr<SyncManager> g_sync_mgr;

// ============================================================
// Update notification (no auto-updater — builds are downloaded by hand).
// On first frame we fire a one-shot, unauthenticated /api/version check on a
// background thread; the result is published via the `done` atomic (the strings
// are written before done flips, so no mutex is needed — the acquire/release
// pair on `done` safely publishes them to the UI thread).
// ============================================================
struct UpdateCheckState {
    std::atomic<bool> done{false};       // background check finished
    std::atomic<bool> available{false};  // a newer build exists
    std::atomic<bool> required{false};   // below min_supported → forced
    std::string latest;                  // written by worker before done=true
    std::string download_url;
    std::string notes;
    bool dismissed = false;              // main-thread only; user closed the banner
};
static UpdateCheckState g_update;

// ============================================================
// Anonymous Share Modal
// ============================================================
struct AnonShareModal {
    enum class Step { Config, Creating, Done, Error };
    Step step = Step::Config;
    Credential cred{};
    std::string password{};
    int expiry_idx = 1;      // 0=1h, 1=24h, 2=7d, 3=30d, 4=Never
    int views_idx = 0;       // 0=1, 1=5, 2=10, 3=Unlimited
    std::string share_url{};
    std::string error_msg{};
    std::atomic<bool> creating{false};

    void Reset() {
        step = Step::Config;
        secure_clear_credential(cred);
        cred = Credential{};
        sodium_memzero(password.data(), password.size());
        password.clear();
        expiry_idx = 1;
        views_idx = 0;
        // URL fragment contains the encryption key — zero before clearing
        sodium_memzero(share_url.data(), share_url.size());
        share_url.clear();
        error_msg.clear();
        creating = false;
    }
};
static AnonShareModal g_anon_share;

// Anonbase64_encode, base64_decode, base64_url_encode,
// serialize_credential_for_share moved to app_sharing.cpp

static void DoCreateAnonShare() {
    static const int expiry_seconds[] = { 3600, 86400, 604800, 2592000, 0 };
    static const int max_views_vals[] = { 1, 5, 10, 0 };

    try {
        // 1. Generate random 32-byte key
        std::vector<uint8_t> key(32);
        randombytes_buf(key.data(), key.size());

        // 2. Encrypt Credential JSON with legacy (no AAD) encryption
        std::string json = serialize_credential_for_share(g_anon_share.cred, g_anon_share.password);
        std::vector<uint8_t> blob = enc::encrypt_credential_legacy(json, key);

        // 3. Base64 encode for upload
        std::string encrypted_data = Anonbase64_encode(blob);

        // 4. Build request payload
        nlohmann::json payload;
        payload["encrypted_data"] = encrypted_data;
        payload["max_views"] = max_views_vals[g_anon_share.views_idx];

        int exp_sec = expiry_seconds[g_anon_share.expiry_idx];
        if (exp_sec > 0)
            payload["expires_in"] = exp_sec;
        else
            payload["expires_in"] = nullptr;

        // 5. Upload
        auto resp = g_sync_mgr->make_auth_request("POST", "/api/pm/share/anon", payload.dump());
        if (!resp.success || resp.status_code != 201) {
            auto j = nlohmann::json::parse(resp.body, nullptr, false);
            std::string msg = "Upload failed";
            if (j.is_object() && j.contains("message"))
                msg = j["message"].get<std::string>();
            g_anon_share.error_msg = msg;
            g_anon_share.step = AnonShareModal::Step::Error;
            g_anon_share.creating = false;
            return;
        }

        // 6. Parse response token
        auto j = nlohmann::json::parse(resp.body);
        std::string token = j["token"].get<std::string>();

        // 7. Store token for share status tracking
        if (!g_anon_share.cred.uuid.empty()) {
            vault_db::set_sync_state("share_token:" + g_anon_share.cred.uuid, token);
            vault_db::set_sync_state("share_created:" + g_anon_share.cred.uuid,
                std::to_string(helpers::now_unix_ms()));
        }

        // 8. Construct URL
        g_anon_share.share_url = "https://passworddefense.net/s/" + token + "#" + base64_url_encode(key);

        // Clear key from memory
        sodium_memzero(key.data(), key.size());

        g_anon_share.step = AnonShareModal::Step::Done;
        g_anon_share.creating = false;
    }
    catch (const std::exception& e) {
        g_anon_share.error_msg = std::string("Error: ") + e.what();
        g_anon_share.step = AnonShareModal::Step::Error;
        g_anon_share.creating = false;
    }
    catch (...) {
        g_anon_share.error_msg = "Unknown error creating share link";
        g_anon_share.step = AnonShareModal::Step::Error;
        g_anon_share.creating = false;
    }
}

struct AutoSaveController
{
    bool enabled = true;

    double last_change_time = 0.0;
    double last_interaction_time = 0.0;
    double last_save_time = 0.0;

    bool suspended = false;
};

static AutoSaveController g_autosave;
static bool g_backups_dirty = true;
static VaultState g_vault;
static ui::ShellState g_shell;

// Forward declarations
static std::string new_db_path();
static std::vector<std::string> list_vaults_next_to_exe();

// Grouped vault discovery for the offline picker: scans Beside-app, OneDrive, and
// mounted USB volumes. Returns a flat path list plus section headers so the picker
// can render each location with its own label.
struct DiscoveredVaults
{
    std::vector<std::string> paths;                        // flat
    std::vector<std::pair<std::string, int>> sections;     // {label, start_idx}
};
static DiscoveredVaults discover_vaults_grouped();

struct VaultTab
{
    std::string label;   // filename-ish label for the tab
    VaultState  vault;   // path, unlocked, creds, status
    uint32_t    tab_id = 0;  // unique ID for this tab (never changes, even if path is empty)
};

static std::vector<VaultTab> g_tabs;
static int g_active_tab = 0;
static uint32_t g_next_tab_id = 1;  // counter for unique tab IDs

static VaultTab& ActiveTab()
{
    if (g_tabs.empty())
    {
        g_tabs.push_back({});
        g_tabs[0].label = "Vault";
        g_tabs[0].vault.vault_path = new_db_path();
        g_tabs[0].tab_id = g_next_tab_id++;
        g_active_tab = 0;
    }
    g_active_tab = ImClamp(g_active_tab, 0, (int)g_tabs.size() - 1);
    return g_tabs[g_active_tab];
}

static VaultState& ActiveVault() { return ActiveTab().vault; }

// Exposed for UI layer to access master key during 2FA setup
std::vector<uint8_t> Get2FAMasterKey()
{
    return ActiveVault().master_key;
}

// Exposed for UI layer to access Credential list (Security Center modal)
const std::vector<Credential>& GetActiveVaultCreds()
{
    return ActiveVault().creds;
}

// Record a trash-permanent-delete so cred_ops::save_all can skip re-inserting
// if the cred comes back via undo. Also wipes the uuid from user_removed_uuids
// so the next save doesn't try to hard-delete something that's already gone.
void NotePermanentlyDeleted(const std::string& uuid)
{
    if (uuid.empty()) return;
    VaultState& v = ActiveVault();
    v.purged_uuids.insert(uuid);
    v.user_removed_uuids.erase(uuid);
}

// Flush pending edits to disk before a direct DB mutation. The trash-restore
// path calls vault_db::restore_credential then reloads memory from disk; any
// in-flight memory edits would be silently lost (and soft-deletes resurrected)
// without this flush. Short-circuits if nothing is pending.
static bool save_vault_to_disk(VaultState& v);  // forward decl for FlushVaultBeforeDbOp
bool FlushVaultBeforeDbOp()
{
    VaultState& v = ActiveVault();
    if (!v.unlocked) return false;
    if (!v.dirty && v.user_removed_uuids.empty()) return true;
    return save_vault_to_disk(v);
}

static uint32_t GetVaultKey(const VaultTab& tab)
{
    // Use vault path if available, otherwise use stable tab_id
    if (!tab.vault.vault_path.empty())
        return helpers::fnv1a_32(tab.vault.vault_path.c_str());
    return tab.tab_id;
}

static uint32_t GetActiveVaultKey()
{
    return GetVaultKey(ActiveTab());
}

// ============================================================
// HIBP Pwned Passwords breach check (k-anonymity, background thread)
// ============================================================
static void CheckBreachedPasswords(ui::ShellState& shell, const std::vector<Credential>& creds)
{
    // Network & Privacy: respect the user's opt-out of the online breach check.
    if (!shell.online_breach_check) {
        shell.sec_breach_error = "Online Breach Check is disabled in Settings → Network & Privacy.";
        shell.sec_breach_checking.store(false, std::memory_order_release);
        return;
    }

    // Collect unique passwords -> Credential IDs
    std::unordered_map<std::string, std::vector<int>> pw_to_ids;
    for (const auto& c : creds) {
        if (!c.is_deleted() && c.type == CredType::Password && !c.password.empty())
            pw_to_ids[c.password].push_back(c.id);
    }

    shell.sec_breach_checking = true;
    shell.sec_breach_checked  = 0;
    shell.sec_breach_total    = (int)pw_to_ids.size();
    shell.sec_breach_error.clear();

    std::thread([&shell, pw_map = std::move(pw_to_ids)]() {
        std::unordered_set<int> exposed_ids;
        int exposed_count = 0;
        int checked = 0;
        std::string error;  // accumulate locally, write once at end

        for (const auto& [pw, ids] : pw_map) {
            // SHA-1 hash the password
            uint8_t digest[20];
            totp::sha1_digest((const uint8_t*)pw.data(), pw.size(), digest);

            // Convert to uppercase hex
            char hex[41];
            for (int i = 0; i < 20; i++)
                snprintf(hex + i * 2, 3, "%02X", digest[i]);
            hex[40] = '\0';

            std::string prefix(hex, 5);
            std::string suffix(hex + 5);

            // Query HIBP k-anonymity API. On Mac this routes through a dedicated
            // NSURLSession helper (breach_http_mac.mm) because the sync-layer
            // win_http_request is a no-op in the offline build. Only the 5-char
            // SHA-1 prefix leaves the device — k-anonymity preserved.
            extern HttpResponse hibp_range_fetch(const std::string&);
            auto resp = hibp_range_fetch(prefix);

            if (resp.success && resp.status_code == 200) {
                // Parse: each line is "SUFFIX:COUNT\r\n"
                size_t pos = 0;
                while (pos < resp.body.size()) {
                    size_t eol = resp.body.find('\n', pos);
                    if (eol == std::string::npos) eol = resp.body.size();
                    std::string line = resp.body.substr(pos, eol - pos);
                    if (!line.empty() && line.back() == '\r') line.pop_back();

                    size_t colon = line.find(':');
                    if (colon != std::string::npos) {
                        std::string line_suffix = line.substr(0, colon);
                        if (line_suffix == suffix) {
                            for (int id : ids) exposed_ids.insert(id);
                            exposed_count += (int)ids.size();
                            break;
                        }
                    }
                    pos = eol + 1;
                }
            } else {
                error = resp.error.empty()
                    ? "HIBP API returned " + std::to_string(resp.status_code)
                    : resp.error;
                break;
            }

            checked++;
            shell.sec_breach_checked = checked;

            // Rate limit: 1.5s between requests (HIBP free tier)
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }

        // Write to staging fields (only touched by background thread).
        // The UI thread will move these to live fields when it sees sec_breach_done.
        shell.sec_exposed_ids_staging   = std::move(exposed_ids);
        shell.sec_exposed_count_staging = exposed_count;
        shell.sec_breach_error_staging  = std::move(error);
        shell.sec_breach_done.store(true, std::memory_order_release);
        shell.sec_breach_checking.store(false, std::memory_order_release);
    }).detach();
}

static void PushScreen(ui::ShellState& shell, ui::Screen next)
{
    if (shell.active_screen == next)
        return;
    shell.screen_stack.push_back(shell.active_screen);
    shell.active_screen = next;
}

static void PopScreen(ui::ShellState& shell)
{
    if (shell.screen_stack.empty())
        return;
    shell.active_screen = shell.screen_stack.back();
    shell.screen_stack.pop_back();
}

static void ResetScreen(ui::ShellState& shell, ui::Screen next)
{
    shell.screen_stack.clear();
    shell.active_screen = next;
}


// Generate path for new vault database next to exe
static std::string new_db_path()
{
#ifdef _WIN32
    char exe_path_c[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path_c, MAX_PATH);
    std::filesystem::path exe_parent_path = std::filesystem::path(exe_path_c).parent_path();
#else
    // On macOS, store vaults in ~/Library/Application Support/PasswordDefense/
    std::filesystem::path exe_parent_path = std::filesystem::path(cfg::exe_path);
#endif

    std::string prefix = "vault-";
    int count = 0;

    // Create directory if it doesn't exist
    std::filesystem::create_directories(exe_parent_path);

    for (const auto& entry : std::filesystem::directory_iterator(exe_parent_path))
    {
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().string();
        if (name.rfind(prefix, 0) == 0 && entry.path().extension() == ".db")
            ++count;
    }

    return (exe_parent_path / ("vault-" + std::to_string(count) + ".db")).string();
}

// List all .db vault files next to the exe
static std::vector<std::string> list_vaults_next_to_exe()
{
    std::vector<std::string> vaults;

#ifdef _WIN32
    char exe_path_c[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path_c, MAX_PATH);
    std::filesystem::path exe_parent_path = std::filesystem::path(exe_path_c).parent_path();
#else
    // On macOS, look for vaults in ~/Library/Application Support/PasswordDefense/
    std::filesystem::path exe_parent_path = std::filesystem::path(cfg::exe_path);
#endif

    try
    {
        std::filesystem::create_directories(exe_parent_path);
        for (const auto& entry : std::filesystem::directory_iterator(exe_parent_path))
        {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() == ".db")
            {
                vaults.push_back(entry.path().string());
            }
        }
    }
    catch (...)
    {
        // Directory iteration failed - return empty
    }

    // Sort alphabetically by filename
    std::sort(vaults.begin(), vaults.end());

    return vaults;
}

static DiscoveredVaults discover_vaults_grouped()
{
    namespace fs = std::filesystem;
    DiscoveredVaults out;

    auto add_section = [&](const char* label, const std::vector<std::string>& dirs) {
        std::vector<std::string> paths;
        for (const auto& dir : dirs) {
            try {
                if (!fs::exists(dir) || !fs::is_directory(dir)) continue;
                for (auto& e : fs::directory_iterator(dir)) {
                    if (!e.is_regular_file()) continue;
                    if (e.path().extension() == ".db")
                        paths.push_back(e.path().string());
                }
            } catch (...) {}
        }
        if (paths.empty()) return;
        std::sort(paths.begin(), paths.end());
        out.sections.push_back({ label, (int)out.paths.size() });
        for (auto& p : paths) out.paths.push_back(std::move(p));
    };

    // Beside app — the default location (~/Library/Application Support/PasswordDefense/)
    add_section("Beside app", { cfg::exe_path });

    // Cloud drives
    const char* home_c = getenv("HOME");
    std::string home = home_c ? home_c : "";
    if (!home.empty()) {
        // iCloud Drive — canonical macOS path
        add_section("iCloud Drive", { home + "/Library/Mobile Documents/com~apple~CloudDocs" });

        // OneDrive — modern CloudStorage path + legacy ~/OneDrive
        std::vector<std::string> onedrive_dirs = { home + "/OneDrive" };
        try {
            fs::path cs = home + "/Library/CloudStorage";
            if (fs::exists(cs) && fs::is_directory(cs)) {
                for (auto& e : fs::directory_iterator(cs)) {
                    if (!e.is_directory()) continue;
                    std::string n = e.path().filename().string();
                    if (n.rfind("OneDrive", 0) == 0)
                        onedrive_dirs.push_back(e.path().string());
                }
            }
        } catch (...) {}
        add_section("OneDrive", onedrive_dirs);
    }

    // USB drive — scan /Volumes/* excluding the system root (symlinked to /)
    std::vector<std::string> usb_dirs;
    try {
        for (auto& e : fs::directory_iterator("/Volumes")) {
            if (!e.is_directory()) continue;
            try {
                if (fs::canonical(e.path()) == "/") continue;
            } catch (...) {}
            usb_dirs.push_back(e.path().string());
        }
    } catch (...) {}
    add_section("USB drive", usb_dirs);

    return out;
}

static std::string BasenameNoExt(const std::string& path)
{
    size_t slash = path.find_last_of("\\/");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);

    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    if (name.empty()) name = "Vault";
    return name;
}

static void CreatePreOpBackup(const VaultState& v, const char* tag)
{
    if (v.vault_path.empty()) return;

    std::string dir = helpers::GetDefaultBackupDir();

    // Ensure backup directory exists
    helpers::create_directories(dir);

    std::string base = BasenameNoExt(v.vault_path);
    std::string ts = helpers::now_iso8601_local();
    helpers::SanitizeFilename(ts);

    std::string dst =
        dir + "/" + base + "_PRE-" + tag + "_" + ts + ".lbdb";

    helpers::CopyFileAtomic(v.vault_path, dst);
    helpers::EnforceBackupRetention(dir, 20);
    g_backups_dirty = true;
}

static void SyncShellTabsFromVaultTabs()
{
    g_shell.db_labels.clear();
    g_shell.db_labels.reserve(g_tabs.size());

    for (auto& t : g_tabs)
        g_shell.db_labels.push_back(t.label.empty() ? "Vault" : t.label);

    g_shell.active_db = ImClamp(g_active_tab, 0, (int)g_shell.db_labels.size() - 1);
}

static void ApplyShellActiveTab()
{
    g_active_tab = ImClamp(g_shell.active_db, 0, (int)g_tabs.size() - 1);
}

static int FindTabByPath(const std::string& path)
{
    for (int i = 0; i < (int)g_tabs.size(); ++i)
        if (g_tabs[i].vault.vault_path == path)
            return i;
    return -1;
}

static void HandleOpenDB()
{
    std::string path = PickOpenFilePath_DB();
    if (path.empty()) return;

    int existing = FindTabByPath(path);
    if (existing != -1)
    {
        g_active_tab = existing;
        return;
    }

    VaultTab t{};
    t.vault.vault_path = path;
    t.vault.unlocked = false;
    t.label = BasenameNoExt(path);
    t.tab_id = g_next_tab_id++;

    g_tabs.push_back(std::move(t));
    g_active_tab = (int)g_tabs.size() - 1;

    // No need to clear UI state for brand new vault (no state exists yet)
}

static void HandleNewDB()
{
    std::string path = PickSaveFilePath_DB();
    if (path.empty()) return;

    int existing = FindTabByPath(path);
    if (existing != -1)
    {
        g_active_tab = existing;
        return;
    }

    VaultTab t{};
    t.vault.vault_path = path;
    t.vault.unlocked = false;
    t.label = BasenameNoExt(path);
    t.tab_id = g_next_tab_id++;

    g_tabs.push_back(std::move(t));
    g_active_tab = (int)g_tabs.size() - 1;

    // No need to clear UI state for brand new vault (no state exists yet)
}

static uint32_t HashGroupsOnly(const std::vector<Credential>& creds)
{
    uint32_t h = 2166136261u;
    for (auto& c : creds)
    {
        for (char ch : c.group) { h ^= (uint8_t)ch; h *= 16777619u; }
        h ^= 0x9E; h *= 16777619u;
    }
    return h ? h : 1u;
}

static uint32_t HashCredsNow(const std::vector<Credential>& creds)
{
    // Serialize ONLY when we explicitly decide to refresh dirty state.
    // (not every frame)
    std::string json = serialize_creds_json(creds);
    return helpers::fnv1a_32(json.c_str());
}

static void VaultRecomputeDirty(VaultState& v)
{
    v.current_hash = HashCredsNow(v.creds);
    v.dirty = (v.current_hash != v.saved_hash);
}

static void VaultMarkSaved(VaultState& v)
{
    v.saved_hash = HashCredsNow(v.creds);
    v.current_hash = v.saved_hash;
    v.dirty = false;

    // Rebuild saved snapshot for change highlighting
    v.saved_snapshot.clear();
    for (const auto& c : v.creds)
    {
        if (!c.uuid.empty())
            v.saved_snapshot[c.uuid] = c;
    }
}

// Forward decls — the real definitions sit below (rebuild_groups is at ~1281,
// rebuild_tags at ~1317). PerformBridgeWrite calls them to refresh the sidebar
// after an extension-driven save, same as the HasExternalChange handler does.
static void rebuild_groups(ui::ShellState& s, const std::vector<Credential>& creds);
static void rebuild_tags(ui::ShellState& s, const std::vector<Credential>& creds);

// Publish the current vault state to the native-messaging bridge and
// (optionally) emit the vault_unlocked event. Runs unconditionally — the
// bridge is always on; its socket has user-only perms and no network reach.
static void PublishBridgeSnapshot(const VaultState& v, bool emit_event)
{
    g_bridge.ApplySnapshot(v.unlocked, v.vault_path, v.creds);
    if (emit_event && v.unlocked)
    {
        std::string ev = R"({"event":"vault_unlocked","credCount":)";
        ev += std::to_string(v.creds.size());
        ev += "}";
        g_bridge.PushEvent(ev);
    }
}

// Execute one pending bridge write (save_new / update / delete) on the UI
// thread. Same code as pw-mac-cloud's PerformBridgeWrite.
//
// Returns the JSON response the worker thread will ship back to the browser.
static std::string PerformBridgeWrite(VaultState& v, const PendingWrite& w)
{
    nlohmann::json body;
    try { body = nlohmann::json::parse(w.payload_json); }
    catch (...) { return R"({"ok":false,"error":"invalid_json"})"; }

    if (!v.unlocked || v.master_key.empty()) {
        return R"({"ok":false,"error":"vault_locked"})";
    }

    // Treat a JSON null like an absent field: the extension historically sent
    // existing_id:null, and json::value(..., "") throws on null.
    auto str = [&](const char* key) -> std::string {
        auto it = body.find(key);
        return (it != body.end() && it->is_string()) ? it->get<std::string>() : std::string();
    };
    auto has = [&](const char* key) {
        auto it = body.find(key);
        return it != body.end() && it->is_string();
    };

    const std::string hostname    = str("hostname");
    const std::string title       = str("title");
    const std::string username    = str("username");
    const std::string password    = str("password");
    const std::string existing_id = str("existing_id");

    bool success = false;
    std::string uuid;

    if (w.action == "delete")
    {
        // Same as the UI delete: soft-delete in the DB (Trash; sync pushes the
        // tombstone), record in the save_all ledger, drop from memory.
        const std::string id = existing_id.empty() ? str("id") : existing_id;
        if (id.empty()) {
            return R"({"ok":false,"error":"missing_existing_id"})";
        }
        auto it = std::find_if(v.creds.begin(), v.creds.end(), [&](const Credential& c) {
            return c.uuid == id && !c.is_deleted();
        });
        if (it == v.creds.end()) {
            return R"({"ok":false,"error":"not_found"})";
        }
        CreatePreOpBackup(v, "BRIDGEDELETE");
        success = cred_ops::remove(id);
        if (success) {
            v.user_removed_uuids.insert(id);
            v.creds.erase(it);
            uuid = id;
        }
    }
    else if (hostname.empty() || username.empty() || password.empty())
    {
        return R"({"ok":false,"error":"missing_fields"})";
    }
    else if (w.action == "save_new")
    {
        Credential c;
        c.title       = title.empty() ? hostname : title;
        c.user        = username;
        c.password    = password;
        c.website     = hostname;
        c.notes       = str("notes");
        c.totp_secret = str("totp_secret");
        c.type        = CredType::Password;
        uuid = cred_ops::add(c, v.master_key);
        success = !uuid.empty();
        if (success) {
            c.uuid = uuid;
            c.created_at_ms = helpers::now_unix_ms();
            c.updated_at_ms = c.created_at_ms;
            v.creds.push_back(c);
        }
    }
    else if (w.action == "update")
    {
        if (existing_id.empty()) {
            return R"({"ok":false,"error":"missing_existing_id"})";
        }
        for (auto& c : v.creds) {
            if (c.uuid != existing_id || c.is_deleted()) continue;
            c.password = password;
            if (!title.empty())    c.title = title;
            if (!username.empty()) c.user  = username;
            // The save prompt sends the bare hostname; keep the stored URL
            // (scheme/path) unless the host actually changed.
            auto host_of = [](std::string u) {
                auto p = u.find("://");
                if (p != std::string::npos) u = u.substr(p + 3);
                u = u.substr(0, u.find_first_of(":/"));
                std::transform(u.begin(), u.end(), u.begin(),
                               [](unsigned char ch) { return (char)std::tolower(ch); });
                if (u.rfind("www.", 0) == 0) u.erase(0, 4);
                return u;
            };
            if (host_of(hostname) != host_of(c.website)) c.website = hostname;
            // Only the popup editor sends these; the save prompt omits them,
            // and absent must not mean "clear".
            if (has("notes"))       c.notes       = str("notes");
            if (has("totp_secret")) c.totp_secret = str("totp_secret");
            c.updated_at_ms = helpers::now_unix_ms();
            c.is_dirty      = true;
            success = cred_ops::update(c.uuid, c, v.master_key);
            uuid = c.uuid;
            break;
        }
        if (uuid.empty()) {
            return R"({"ok":false,"error":"not_found"})";
        }
    }
    else
    {
        return R"({"ok":false,"error":"unknown_write"})";
    }

    if (success) {
        // Snapshot + modified event so other extension instances see the
        // change immediately. Match the behavior of VaultMarkChanged.
        g_bridge.ApplySnapshot(v.unlocked, v.vault_path, v.creds);
        g_bridge.PushEvent(R"({"event":"vault_modified"})");

        // Rebuild UI-side group/tag caches so the credential appears in the
        // Group/Tag sidebar on the very next frame.
        rebuild_groups(g_shell, v.creds);
        rebuild_tags(g_shell, v.creds);
        ui::ForgetVaultRowState(GetActiveVaultKey());
    }

    // ok must reflect the write: the extension treats ok:true as saved.
    nlohmann::json resp;
    resp["ok"]      = success;
    resp["success"] = success;
    if (!success) resp["error"] = "write_failed";
    if (!uuid.empty()) resp["uuid"] = uuid;
    return resp.dump();
}

// Thin shim retained so every pre-existing unlock site keeps compiling
// without a rename sweep. All it does is republish the vault to the
// native-messaging bridge + emit vault_unlocked.
static void PublishVaultOnUnlock(VaultState& v)
{
    PublishBridgeSnapshot(v, /*emit_event=*/true);
}

// The bridge socket is shared with the other Password Defense app (cloud /
// offline): whichever starts first serves the extension. Retry every few
// seconds so this app takes over when the other quits, and step aside if
// our socket node was replaced (both launched at once). After a (re)start
// the cache is empty, so republish the active vault.
static void TickBridge()
{
    static double s_next_check = 0.0;
    const double now = ImGui::GetTime();
    if (now < s_next_check) return;
    s_next_check = now + 3.0;

    if (g_bridge.IsRunning()) {
        if (g_bridge.OwnsSocketPath()) return;
        g_bridge.Shutdown();   // won't unlink — the node isn't ours any more
    }
    g_bridge.Start();
    if (g_bridge.IsRunning())
        PublishBridgeSnapshot(ActiveVault(), /*emit_event=*/false);
}

static void VaultMarkChanged(VaultState& v, const char* preTagForBackup)
{
    // If we are currently clean, take the one-time "first edit" safety backup.
    if (!v.dirty && v.unlocked && !v.vault_path.empty())
        CreatePreOpBackup(v, preTagForBackup ? preTagForBackup : "EDIT");

    VaultRecomputeDirty(v);

    // Republish to the bridge + emit vault_modified so connected
    // extensions refresh their caches on the next frame.
    g_bridge.ApplySnapshot(v.unlocked, v.vault_path, v.creds);
    g_bridge.PushEvent(R"({"event":"vault_modified"})");

    // Your autosave debounce uses this
    g_autosave.last_change_time = ImGui::GetTime();
}

static uint64_t iso_to_sortkey_yyyymmddhhmmss(const std::string& iso)
{
    if (iso.size() < 19) return 0;

    auto digit = [](char c) { return (c >= '0' && c <= '9') ? (c - '0') : -1; };
    auto rd2 = [&](int i)->int { int a = digit(iso[i]); int b = digit(iso[i + 1]); return (a < 0 || b < 0) ? -1 : a * 10 + b; };
    auto rd4 = [&](int i)->int { int a = digit(iso[i]); int b = digit(iso[i + 1]); int c = digit(iso[i + 2]); int d = digit(iso[i + 3]); return (a < 0 || b < 0 || c < 0 || d < 0) ? -1 : a * 1000 + b * 100 + c * 10 + d; };

    int Y = rd4(0), M = rd2(5), D = rd2(8), h = rd2(11), m = rd2(14), s = rd2(17);
    if (Y < 0 || M < 1 || M > 12 || D < 1 || D > 31 || h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59)
        return 0;

    return (uint64_t)Y * 10000000000ull +
        (uint64_t)M * 100000000ull +
        (uint64_t)D * 1000000ull +
        (uint64_t)h * 10000ull +
        (uint64_t)m * 100ull +
        (uint64_t)s;
}

static uint32_t iso_to_monthkey_yyyymm(const std::string& iso)
{
    if (iso.size() < 7) return 0;
    int y = std::atoi(iso.substr(0, 4).c_str());
    int m = std::atoi(iso.substr(5, 2).c_str());
    if (y <= 0 || m < 1 || m > 12) return 0;
    return (uint32_t)(y * 100 + m);
}

// Convert Unix ms to YYYYMM month key
static uint32_t unix_ms_to_monthkey(int64_t ms)
{
    if (ms == 0) return 0;
    std::time_t sec = static_cast<std::time_t>(ms / 1000);
    std::tm tm_local;
#ifdef _WIN32
    localtime_s(&tm_local, &sec);
#else
    localtime_r(&sec, &tm_local);
#endif
    int y = tm_local.tm_year + 1900;
    int m = tm_local.tm_mon + 1;
    if (y <= 0 || m < 1 || m > 12) return 0;
    return static_cast<uint32_t>(y * 100 + m);
}

static const char* month_name(int m)
{
    static const char* k[] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    if (m < 1 || m > 12) return "";
    return k[m - 1];
}

static std::string month_label_from_key(uint32_t yyyymm)
{
    int y = (int)(yyyymm / 100);
    int m = (int)(yyyymm % 100);
    if (y <= 0 || m < 1 || m > 12) return "Unknown date";
    return std::string(month_name(m)) + " " + std::to_string(y);
}

static inline bool str_contains_ci(const std::string& hay, const char* needle)
{
    if (!needle || !needle[0]) return true;
    if (hay.empty()) return false;

    // ImStristr is in imgui_internal.h (included by UI.h)
    return ImStristr(hay.c_str(), nullptr, needle, nullptr) != nullptr;
}

static bool matches_search(const Credential& c, const char* needle, int search_filter)
{
    if (!needle || !needle[0]) return true;
    switch (search_filter)
    {
    case 0: // Title: also match card_brand (cards), id_type (identity)
        return str_contains_ci(c.title, needle)
            || str_contains_ci(c.card_brand, needle)
            || str_contains_ci(c.id_type, needle);
    case 1: return str_contains_ci(c.email, needle);
    case 2: // Username: also match cardholder_name (cards), full_name (identity)
        return str_contains_ci(c.user, needle)
            || str_contains_ci(c.cardholder_name, needle)
            || str_contains_ci(c.full_name, needle);
    default: return str_contains_ci(c.title, needle);
    }
}

static void rebuild_groups(ui::ShellState& s, const std::vector<Credential>& creds)
{
    std::unordered_set<std::string> uniq;
    uniq.reserve(creds.size());

    for (const auto& c : creds)
        if (!c.group.empty())
            uniq.insert(c.group);

    std::vector<std::string> out;
    out.reserve(7 + uniq.size());
    out.push_back("Filter");
    out.push_back("@Passwords");
    out.push_back("@Cards");
    out.push_back("@Identity");
    out.push_back("@Notes");
    out.push_back("---");  // separator sentinel

    // stable-ish order
    std::vector<std::string> tmp;
    tmp.reserve(uniq.size());
    for (const auto& g : uniq) tmp.push_back(g);
    std::sort(tmp.begin(), tmp.end());

    for (auto& g : tmp) out.push_back(g);

    s.groups = std::move(out);

    // Prune selected_groups to only contain groups that still exist
    std::set<std::string> pruned;
    for (const auto& g : s.selected_groups)
        if (std::find(s.groups.begin(), s.groups.end(), g) != s.groups.end())
            pruned.insert(g);
    s.selected_groups = std::move(pruned);
}

static void rebuild_tags(ui::ShellState& s, const std::vector<Credential>& creds)
{
    std::unordered_set<std::string> uniq;
    for (const auto& c : creds)
        for (const auto& t : c.tags)
            if (!t.empty()) uniq.insert(t);

    std::vector<std::string> out(uniq.begin(), uniq.end());
    std::sort(out.begin(), out.end());
    s.all_tags = std::move(out);

    // Prune selected_tags to only contain tags that still exist
    std::set<std::string> pruned;
    for (const auto& t : s.selected_tags)
        if (std::find(s.all_tags.begin(), s.all_tags.end(), t) != s.all_tags.end())
            pruned.insert(t);
    s.selected_tags = std::move(pruned);
}

// Called from UI trash modal after restoring a Credential
void ReloadVaultCredentials()
{
    VaultState& v = ActiveVault();
    if (v.master_key.empty()) return;
    v.creds = cred_ops::load_all(v.master_key);

    // Rebuild saved snapshot so change highlighting reflects the DB state
    v.saved_snapshot.clear();
    for (const auto& c : v.creds)
        if (!c.uuid.empty())
            v.saved_snapshot[c.uuid] = c;

    rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);
    rebuild_tags(g_shell, v.creds);
    ui::ForgetVaultRowState(GetActiveVaultKey());
}

// Tag editor widget: pills + autocomplete input
static void render_tag_editor(float width)
{
    auto& tags = g_cred_modal.buf.tags;
    auto& input = g_cred_modal.tag_input_buf;
    const float pillH = 20.0f;
    const float pillPad = 4.0f;
    const float pillGap = 4.0f;
    const float pillRounding = 10.0f;
    bool dark = g_shell.dark_theme;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Render existing tag pills
    float startX = ImGui::GetCursorScreenPos().x;
    float curX = startX;
    float curY = ImGui::GetCursorScreenPos().y;
    float maxX = startX + width;
    int removeIdx = -1;

    for (int i = 0; i < (int)tags.size(); i++)
    {
        const auto& t = tags[i];
        ImVec2 tSz = ImGui::CalcTextSize(t.c_str());
        ImVec2 xSz = ImGui::CalcTextSize(ICON_MDI_CLOSE);
        float pillW = pillPad * 2 + tSz.x + 4.0f + xSz.x + 2.0f;

        // Wrap to next line if needed
        if (curX + pillW > maxX && curX > startX)
        {
            curX = startX;
            curY += pillH + 2.0f;
        }

        ImVec2 pMin(curX, curY);
        ImVec2 pMax(curX + pillW, curY + pillH);

        // Pill background
        ImU32 pillBg = dark ? IM_COL32(255, 255, 255, 20) : IM_COL32(0, 0, 0, 15);
        dl->AddRectFilled(pMin, pMax, pillBg, pillRounding);

        // Tag text
        float textY = curY + (pillH - tSz.y) * 0.5f;
        dl->AddText(ImVec2(curX + pillPad, textY), ImGui::GetColorU32(ImGuiCol_Text), t.c_str());

        // X button
        float xX = curX + pillPad + tSz.x + 4.0f;
        dl->AddText(ImVec2(xX, textY), ImGui::GetColorU32(ImGuiCol_TextDisabled), ICON_MDI_CLOSE);

        // Invisible button for the whole pill to remove
        ImGui::SetCursorScreenPos(pMin);
        char btnId[32];
        snprintf(btnId, sizeof(btnId), "##tagpill_%d", i);
        ImGui::InvisibleButton(btnId, ImVec2(pillW, pillH));
        if (ImGui::IsItemClicked()) removeIdx = i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove tag");

        curX += pillW + pillGap;
    }

    if (removeIdx >= 0)
        tags.erase(tags.begin() + removeIdx);

    // Move cursor to after pills
    if (!tags.empty())
    {
        curY += pillH + 4.0f;
        ImGui::SetCursorScreenPos(ImVec2(startX, curY));
    }

    // Tag input — type-to-filter + chevron-dropdown of available tags.
    // Mirrors Windows render_tag_editor: SearchableCombo over `availTags`
    // (all_tags minus already-applied), with Enter or pick → push to tags.
    std::vector<std::string> availTags;
    for (const auto& t : g_shell.all_tags)
    {
        bool already = false;
        for (const auto& existing : tags)
            if (existing == t) { already = true; break; }
        if (!already)
            availTags.push_back(t);
    }

    ImGui::SetNextItemWidth(width);
    bool picked = ui::SearchableCombo("Add tag...##modal_tags", input, availTags, &g_shell.sb_tag_counts);
    bool addTag = picked ||
                  (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Enter));

    if (addTag && !input.empty())
    {
        std::string trimmed = input;
        while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && trimmed.back() == ' ')  trimmed.pop_back();
        if (!trimmed.empty())
        {
            bool exists = false;
            for (const auto& t : tags)
                if (t == trimmed) { exists = true; break; }
            if (!exists)
                tags.push_back(trimmed);
        }
        input.clear();
    }
}

// Group input — SearchableCombo over user-defined groups (drops system rows).
static void render_group_input(float width)
{
    auto& group = g_cred_modal.buf.group;

    std::vector<std::string> userGroups;
    for (const auto& g : g_shell.groups)
    {
        if (g.empty() || g[0] == '@' || g == "---" || g == "Filter" || g == "All") continue;
        userGroups.push_back(g);
    }

    ImGui::SetNextItemWidth(width);
    ui::SearchableCombo("Group##modal_group", group, userGroups, &g_shell.sb_group_counts);
}

// Group + Tags as one top-aligned row. Each column is wrapped in a group so
// SameLine spans the whole field: SearchableCombo nudges the cursor internally
// for its chevron hit-area, and without the group wrap SameLine keyed off that
// chevron item, leaving the tag field misaligned with the group field.
static void render_group_tags_row(float halfW, float colGap)
{
    // The Dummy()s settle SearchableCombo's trailing SetCursorScreenPos (used to
    // restore layout after its chevron hit-area). Without an item afterwards,
    // EndGroup's boundary check asserts that a SetCursorPos extended the group.
    ImGui::BeginGroup();
    render_group_input(halfW);
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::EndGroup();

    ImGui::SameLine(0, colGap);

    ImGui::BeginGroup();
    render_tag_editor(halfW);
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::EndGroup();
}

// ============================================================
// Find Duplicates tool — Settings → Find Duplicates
//
// Sync identifies credentials by UUID. If the same site/login was added
// independently on two devices before linking, each has a unique UUID so
// sync can't merge them; both surface as separate rows after pull. This
// tool surfaces those collisions, lets the user choose which to keep per
// group, and soft-deletes the rest (recoverable from Trash).
//
// Placed here (after VaultState, g_shell, ActiveVault, GetActiveVaultKey
// are declared) because render_dedup_modal touches all of those.
// ============================================================

struct DedupGroup {
    std::string       signature;     // Display label, e.g. "gmail.com / alice@example.com"
    std::vector<int>  cred_ids;      // Credential.id values in this group
    std::vector<char> keep;          // Parallel to cred_ids: 1 = keep, 0 = delete
};
static std::vector<DedupGroup> g_dedup_groups;

static const Credential* find_cred_by_id(const std::vector<Credential>& creds, int id)
{
    for (const Credential& c : creds) if (c.id == id) return &c;
    return nullptr;
}

static std::string dedup_fmt_when(int64_t ms)
{
    if (ms == 0) return "—";
    time_t t = (time_t)(ms / 1000);
    struct tm lt;
    localtime_r(&t, &lt);  // POSIX (macOS); Windows uses localtime_s
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &lt);
    return std::string(buf);
}

static std::vector<DedupGroup> find_duplicate_groups(const std::vector<Credential>& creds)
{
    // Bucket by signature; only keep buckets with >= 2 members.
    std::unordered_map<std::string, std::vector<int>> by_sig;
    std::unordered_map<std::string, std::string>      display;

    for (const Credential& c : creds) {
        if (c.is_deleted()) continue;
        const std::string sig = cred_signature(c);
        if (sig.empty()) continue;
        by_sig[sig].push_back(c.id);
        if (display.find(sig) == display.end()) {
            const std::string site  = dedup_norm_site(c.website);
            const std::string email = dedup_lower(c.email);
            const std::string user  = dedup_lower(c.user);
            const std::string who = email.empty() ? user
                                  : (user.empty() ? email : email + "  ·  " + user);
            display[sig] = site + "  /  " + who;
        }
    }

    std::vector<DedupGroup> out;
    out.reserve(by_sig.size());
    for (auto& kv : by_sig) {
        if (kv.second.size() < 2) continue;
        DedupGroup g;
        g.signature = display[kv.first];
        g.cred_ids  = std::move(kv.second);

        // Default to keeping the newest (by updated_at_ms, fallback created_at_ms);
        // others are pre-marked for deletion. User can override per-entry — this
        // is a default, not a forced choice. Multi-keep allowed so a group that
        // legitimately holds two distinct accounts can be spared in full.
        int best_local = 0;
        int64_t best_ts = -1;
        for (int li = 0; li < (int)g.cred_ids.size(); ++li) {
            const int cid = g.cred_ids[li];
            for (const Credential& c : creds) {
                if (c.id != cid) continue;
                int64_t ts = c.updated_at_ms ? c.updated_at_ms : c.created_at_ms;
                if (ts > best_ts) { best_ts = ts; best_local = li; }
                break;
            }
        }
        g.keep.assign(g.cred_ids.size(), 0);
        g.keep[best_local] = 1;
        out.push_back(std::move(g));
    }

    std::sort(out.begin(), out.end(),
        [](const DedupGroup& a, const DedupGroup& b){ return a.signature < b.signature; });
    return out;
}

static void render_dedup_modal()
{
    if (!g_shell.dedup_modal_open) return;

    // Which entries have their password revealed (keyed by uuid). Masked by
    // default; cleared when the modal closes so reveals don't persist.
    static std::unordered_set<std::string> s_dedup_revealed;

    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 150));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));

    const float modalW = 620.0f;
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(modalW, 0), ImVec2(modalW, 620));

    bool open = true;
    bool begin_ok = ImGui::BeginPopupModal("Find Duplicates###dedup_modal", &open,
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);

    if (!begin_ok) {
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(1);
        if (!open) g_shell.dedup_modal_open = false;
        return;
    }

    auto close_modal = [&]() {
        g_shell.dedup_modal_open = false;
        g_dedup_groups.clear();
        s_dedup_revealed.clear();
        ImGui::CloseCurrentPopup();
    };

    ImGui::PushFont(render::FontLarge);
    ImGui::TextUnformatted("Find Duplicates");
    ImGui::PopFont();
    ImGui::TextDisabled("Same site + login, different UUIDs — usually the same entry made on two devices.");
    ImGui::TextDisabled("Tap KEEP on the entries to keep; everything left unkept moves to Trash.");
    ImGui::Dummy(ImVec2(0, 10));

    VaultState& v = ActiveVault();

    if (g_dedup_groups.empty()) {
        ImGui::TextUnformatted("No duplicate credentials found.");
        ImGui::Dummy(ImVec2(0, 14));
        const float btnW = 110.0f, btnH = 32.0f;
        const float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - btnW);
        if (ui::StyledButton("##dedup_close", "Close", ImVec2(btnW, btnH)))
            close_modal();
        ImGui::EndPopup();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(1);
        return;
    }

    ImGui::BeginChild("##dedup_groups", ImVec2(0, 380), true);

    int total_to_delete = 0;
    for (int gi = 0; gi < (int)g_dedup_groups.size(); ++gi) {
        DedupGroup& g = g_dedup_groups[gi];
        int kept = 0;
        for (char k : g.keep) kept += k ? 1 : 0;
        total_to_delete += (int)g.cred_ids.size() - kept;

        ImGui::PushID(gi);

        // Per-group verdict: site/email/username already match by signature, so
        // only password and notes can distinguish — surface which (if any) differ.
        bool pw_differs = false, notes_differ = false;
        {
            const Credential* base = nullptr;
            for (int li = 0; li < (int)g.cred_ids.size(); ++li) {
                const Credential* cc = find_cred_by_id(v.creds, g.cred_ids[li]);
                if (!cc) continue;
                if (!base) { base = cc; continue; }
                if (cc->password != base->password) pw_differs = true;
                if (cc->notes    != base->notes)    notes_differ = true;
            }
        }

        ImGui::PushFont(render::FontLarge);
        ImGui::TextColored(ImVec4(1, 0.61f, 0.18f, 1), "%s", g.signature.c_str());
        ImGui::PopFont();
        ImGui::SameLine(0, 10);
        if (!pw_differs && !notes_differ) {
            ImGui::TextColored(ImVec4(0.29f, 0.87f, 0.50f, 1), "identical — keep either");
        } else {
            std::string what = pw_differs ? "password" : "";
            if (notes_differ) { if (!what.empty()) what += " & "; what += "notes"; }
            ImGui::TextColored(ImVec4(0.98f, 0.75f, 0.14f, 1), "%s differ — compare", what.c_str());
        }

        for (int li = 0; li < (int)g.cred_ids.size(); ++li) {
            const Credential* c = find_cred_by_id(v.creds, g.cred_ids[li]);
            if (!c) continue;

            ImGui::PushID(li);
            const bool selected = g.keep[li] != 0;

            // KEEP pill: filled accent when kept, ghost outline otherwise.
            // InvisibleButton + DrawList gives exact control of shape, hit
            // area, and kept/hover/text states.
            {
                const float pill_w   = 58.0f;
                const float pill_h   = 22.0f;
                const float rounding = pill_h * 0.5f;
                const ImU32 accent   = IM_COL32(255, 152, 31, 255);

                const ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##keep", ImVec2(pill_w, pill_h));
                const bool hovered = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked(0)) g.keep[li] = g.keep[li] ? 0 : 1;
                if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 p1(pos.x + pill_w, pos.y + pill_h);

                if (selected) {
                    dl->AddRectFilled(pos, p1, accent, rounding);
                } else {
                    const ImU32 border = hovered
                        ? IM_COL32(255, 255, 255, 110)
                        : IM_COL32(255, 255, 255, 50);
                    dl->AddRect(pos, p1, border, rounding, 0, 1.0f);
                }

                const char* glyph = selected ? "KEEP" : "keep";
                const ImU32 text_col = selected
                    ? IM_COL32(255, 255, 255, 255)
                    : (hovered ? IM_COL32(220, 220, 220, 220) : IM_COL32(160, 160, 160, 200));
                const ImVec2 ts = ImGui::CalcTextSize(glyph);
                dl->AddText(
                    ImVec2(pos.x + (pill_w - ts.x) * 0.5f,
                           pos.y + (pill_h - ts.y) * 0.5f),
                    text_col, glyph);
            }

            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();

            std::string label = c->title.empty() ? "(no title)" : c->title;
            label += "   ";
            label += "updated " + dedup_fmt_when(c->updated_at_ms ? c->updated_at_ms : c->created_at_ms);
            if (!c->totp_secret.empty()) label += "   " ICON_MDI_TWO_FACTOR_AUTHENTICATION " TOTP";
            if (!c->notes.empty())       label += "   " ICON_MDI_NOTE_TEXT " notes";
            if (c->is_favorite)          label += "   " ICON_MDI_STAR;
            if (selected) ImGui::TextUnformatted(label.c_str());
            else          ImGui::TextDisabled("%s", label.c_str());

            // Password detail line — masked by default with show/hide. Lets the
            // user compare the field that actually distinguishes duplicates
            // without leaving the modal.
            ImGui::Indent(28.0f);
            ImGui::TextDisabled("pw");
            ImGui::SameLine(0, 6);
            if (c->password.empty()) {
                ImGui::TextDisabled("none");
            } else {
                const bool revealed = s_dedup_revealed.count(c->uuid) != 0;
                if (pw_differs) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(251, 191, 36, 255));
                ImGui::TextUnformatted(revealed ? c->password.c_str() : "********");
                if (pw_differs) ImGui::PopStyleColor();
                ImGui::SameLine(0, 8);
                if (ImGui::SmallButton(revealed ? "hide##pw" : "show##pw")) {
                    if (revealed) s_dedup_revealed.erase(c->uuid);
                    else          s_dedup_revealed.insert(c->uuid);
                }
            }
            if (!c->notes.empty()) {
                ImGui::SameLine(0, 10);
                ImGui::TextDisabled(ICON_MDI_NOTE_TEXT " notes");
            }
            ImGui::Unindent(28.0f);
            ImGui::PopID();
        }

        ImGui::Dummy(ImVec2(0, 6));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, 8));

    const float btnW = 180.0f, btnH = 32.0f;
    const float cancelW = 100.0f;
    const float gap = 8.0f;
    const float total = cancelW + gap + btnW;
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - total);

    if (ui::StyledButton("##dedup_cancel", "Cancel", ImVec2(cancelW, btnH)))
        close_modal();
    ImGui::SameLine(0, gap);

    char apply_label[64];
    snprintf(apply_label, sizeof(apply_label), "Delete %d duplicate%s",
             total_to_delete, total_to_delete == 1 ? "" : "s");

    ImGui::BeginDisabled(total_to_delete == 0);
    if (ui::StyledButtonLight("##dedup_apply", apply_label, ImVec2(btnW, btnH))) {
        // Collect uuids to delete: every entry in every group NOT marked keep.
        std::vector<std::string> to_delete;
        for (const DedupGroup& g : g_dedup_groups) {
            for (int li = 0; li < (int)g.cred_ids.size(); ++li) {
                if (li < (int)g.keep.size() && g.keep[li]) continue;
                const Credential* c = find_cred_by_id(v.creds, g.cred_ids[li]);
                if (c && !c->uuid.empty()) to_delete.push_back(c->uuid);
            }
        }

        if (!to_delete.empty()) {
            for (const std::string& uuid : to_delete) {
                cred_ops::remove(uuid);
                v.user_removed_uuids.insert(uuid);  // ledger for save_all
            }

            // Reload from DB so the in-memory list reflects the soft-deletes.
            v.creds = cred_ops::load_all(v.master_key);
            rebuild_groups(g_shell, v.creds);
            rebuild_tags(g_shell, v.creds);
            ui::ForgetVaultRowState(GetActiveVaultKey());
            VaultMarkChanged(v, "DEDUP");

            char toast[96];
            snprintf(toast, sizeof(toast), "Moved %d duplicate%s to Trash",
                     (int)to_delete.size(), to_delete.size() == 1 ? "" : "s");
            ui::ShowToast(toast, ui::ToastType::Success);
        }
        close_modal();
    }
    ImGui::EndDisabled();

    ImGui::EndPopup();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(1);
}

// ============================================================
// Credential Modal Rendering
// ============================================================
static void render_credential_modal(VaultState& v)
{
    if (!g_cred_modal.IsOpen()) return;

    // Theme-aware modal colors
    const bool dark = g_shell.dark_theme;
    const ImU32 popupBg = dark
        ? IM_COL32(36, 35, 39, 255)
        : IM_COL32(250, 250, 252, 255);
    const ImU32 dimBg = IM_COL32(0, 0, 0, 140);  // Same for both themes

    // Styling
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, popupBg);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, dimBg);

    // Fixed width, auto height — wider to accommodate two-column layout
    const float modalW = 560.0f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(modalW, 0), ImVec2(modalW, FLT_MAX));

    // Re-center on every frame so the modal follows window resizes
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    bool modal_open = true;
    if (ImGui::BeginPopupModal("Add/Edit###cred_modal", &modal_open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove))
    {

        // Keyboard shortcuts
        bool escape_pressed = ImGui::IsKeyPressed(ImGuiKey_Escape);
        bool submit_shortcut = ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_Enter);

        // Form fields
        const float fieldW = ImGui::GetContentRegionAvail().x;

        // Header with close button
        ImGui::PushFont(render::FontLarge);
        ImGui::TextUnformatted(g_cred_modal.IsAdd() ? "Add Credential" : "Edit Credential");
        ImGui::PopFont();
        {
            //float closeSize = ImGui::GetFrameHeight();
            //ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - closeSize);
            //if (ui::StyledButton("##modal_cancel", ICON_MDI_CLOSE, ImVec2(closeSize, closeSize)) || escape_pressed)
            //{
            //    g_cred_modal.Close();
            //    ImGui::CloseCurrentPopup();
            //}

            // Close text
            ImVec2 closeSz = ImGui::CalcTextSize(ICON_MDI_CLOSE);
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - closeSz.x);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(ICON_MDI_CLOSE);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0) || escape_pressed)
            {
                g_cred_modal.Close();
                ImGui::CloseCurrentPopup();
            }

            //ImGui::Dummy(ImVec2(0, 6));
        }
        ImGui::Dummy(ImVec2(0, 8));

        // Type selector (Add mode: interactive buttons, Edit mode: read-only label)
        {
            const CredType ct = g_cred_modal.selected_type;
            if (g_cred_modal.IsAdd())
            {
                struct TypeBtn { CredType t; const char* icon; const char* label; };
                TypeBtn btns[] = {
                    { CredType::Password,   ICON_MDI_KEY,         "Password"    },
                    { CredType::CreditCard, ICON_MDI_CREDIT_CARD, "Card"        },
                    { CredType::Identity,   ICON_MDI_CARD_ACCOUNT_DETAILS, "Identity" },
                    { CredType::SecureNote, ICON_MDI_NOTE_TEXT,   "Note"        },
                };

                const int numTabs = 4;
                const float tabH = 30.0f;
                const float pillR = 7.0f;
                const float cardR = 5.0f;
                const float pillPad = 2.0f;
                bool dark = g_shell.dark_theme;

                // Pill background
                ImU32 pillBg = dark ? IM_COL32(40, 40, 44, 255) : IM_COL32(232, 232, 236, 255);
                ImU32 cardBg = dark ? IM_COL32(60, 60, 66, 255) : IM_COL32(255, 255, 255, 255);
                ImU32 cardShadow = dark ? IM_COL32(0, 0, 0, 70) : IM_COL32(0, 0, 0, 35);

                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 pillPos = ImGui::GetCursorScreenPos();
                float pillW = fieldW;

                // Draw pill background
                dl->AddRectFilled(pillPos, ImVec2(pillPos.x + pillW, pillPos.y + tabH), pillBg, pillR);

                // Segment width
                float segW = pillW / numTabs;

                // Find active index
                int activeIdx = 0;
                for (int i = 0; i < numTabs; i++)
                    if (ct == btns[i].t) { activeIdx = i; break; }

                // Draw active card (raised, with shadow)
                {
                    float cardX = pillPos.x + activeIdx * segW + pillPad;
                    float cardY = pillPos.y + pillPad;
                    float cardW = segW - pillPad * 2.0f;
                    float cardH = tabH - pillPad * 2.0f;
                    ImVec2 cMin(cardX, cardY);
                    ImVec2 cMax(cardX + cardW, cardY + cardH);

                    // Shadow
                    dl->AddRectFilled(
                        ImVec2(cMin.x + 0.5f, cMin.y + 1.0f),
                        ImVec2(cMax.x + 0.5f, cMax.y + 1.0f),
                        cardShadow, cardR);
                    // Card fill
                    dl->AddRectFilled(cMin, cMax, cardBg, cardR);
                }

                // Hit test + text for each segment
                for (int i = 0; i < numTabs; i++)
                {
                    bool active = (ct == btns[i].t);
                    float segX = pillPos.x + i * segW;

                    char bid[32]; snprintf(bid, sizeof(bid), "##type_%d", i);
                    ImGui::SetCursorScreenPos(ImVec2(segX, pillPos.y));
                    ImGui::PushID(bid);
                    ImGui::InvisibleButton("##tb", ImVec2(segW, tabH));
                    bool hovered = ImGui::IsItemHovered();
                    if (ImGui::IsItemClicked())
                        g_cred_modal.selected_type = btns[i].t;
                    ImGui::PopID();

                    // Hover highlight on inactive
                    if (hovered && !active)
                    {
                        ImU32 hovCol = dark ? IM_COL32(255, 255, 255, 10) : IM_COL32(0, 0, 0, 8);
                        dl->AddRectFilled(
                            ImVec2(segX + pillPad, pillPos.y + pillPad),
                            ImVec2(segX + segW - pillPad, pillPos.y + tabH - pillPad),
                            hovCol, cardR);
                    }

                    // Text
                    char lbl[48]; snprintf(lbl, sizeof(lbl), "%s %s", btns[i].icon, btns[i].label);
                    ImVec2 ts = ImGui::CalcTextSize(lbl);
                    ImVec2 tp(segX + (segW - ts.x) * 0.5f, pillPos.y + (tabH - ts.y) * 0.5f);
                    ImU32 tcol = active
                        ? ImGui::GetColorU32(ImGuiCol_Text)
                        : ImGui::GetColorU32(ImGuiCol_TextDisabled);
                    dl->AddText(tp, tcol, lbl);
                }

                ImGui::SetCursorScreenPos(ImVec2(pillPos.x, pillPos.y + tabH));
                ImGui::Dummy(ImVec2(0, 4));
            }
            else
            {
                ImGui::TextDisabled("%s", CredTypeLabel(ct));
                ImGui::Dummy(ImVec2(0, 2));
            }
        }

        const CredType ct = g_cred_modal.selected_type;

        // Two-column layout helpers
        const float colGap = 12.0f;
        const float halfW = (fieldW - colGap) * 0.5f;

        // Title (all types) — full width
        ImGui::SetNextItemWidth(fieldW);
        ui::InputTextString("Title##modal_title", &g_cred_modal.buf.title);
        ImGui::Spacing();

        // Type-specific fields — two columns where possible
        if (ct == CredType::Password)
        {
            // Row 1: Email | User
            ImGui::SetNextItemWidth(halfW);
            ui::InputTextString("Email##modal_email", &g_cred_modal.buf.email);
            ImGui::SameLine(0, colGap);
            ImGui::SetNextItemWidth(halfW);
            ui::InputTextString("User##modal_user", &g_cred_modal.buf.user);
            ImGui::Spacing();

            // Row 2: Password (full width — has icon buttons)
            ui::PasswordFieldRow("##modal_pw", g_cred_modal.password_buf,
                g_cred_modal.show_password, g_cred_modal.gen_opt, fieldW);
            ImGui::Spacing();

            // Row 3: Website (full width)
            ImGui::SetNextItemWidth(fieldW);
            ui::InputTextString("Website##modal_website", &g_cred_modal.buf.website);
            ImGui::Spacing();

            // Row 4: Group | Tags
            render_group_tags_row(halfW, colGap);
            ImGui::Spacing();
        }
        else if (ct == CredType::CreditCard)
        {
            // Layout now mirrors Windows add/edit modal exactly:
            //   Row 1: Cardholder | Brand
            //   Row 2: Card Number | Street Address  (card_address, NOT the
            //          Identity-type `address` field)
            //   Row 3: Expiry | CVV
            //   Row 4: City | Postal Code
            //   Row 5: Group | Tags
            // Previously had an extra Website row + was wrongly binding the
            // Identity `address` field to the card section's Address input.

            // Row 1: Cardholder | Brand
            ImGui::SetNextItemWidth(halfW);
            ui::InputTextString("Cardholder Name##modal_cardholder", &g_cred_modal.buf.cardholder_name);
            ImGui::SameLine(0, colGap);
            ImGui::SetNextItemWidth(halfW);
            ui::InputTextString("Brand##modal_brand", &g_cred_modal.buf.card_brand);
            ImGui::Spacing();

            // Row 2: Card Number | Street Address
            ImGui::SetNextItemWidth(halfW);
            ui::InputTextFormatted("Card Number##modal_cardnum", &g_cred_modal.buf.card_number, "#### #### #### ####");
            ImGui::SameLine(0, colGap);
            ImGui::SetNextItemWidth(halfW);
            ui::InputTextString("Street Address##modal_card_addr", &g_cred_modal.buf.card_address);
            ImGui::Spacing();

            // Row 3: Expiry | CVV
            ImGui::SetNextItemWidth(halfW);
            ui::InputTextFormatted("Expiry (MM/YY)##modal_expiry", &g_cred_modal.buf.card_expiry, "## / ##");
            ImGui::SameLine(0, colGap);
            ImGui::SetNextItemWidth(halfW);
            ui::InputTextString("CVV##modal_cvv", &g_cred_modal.buf.card_cvv);
            ImGui::Spacing();

            // Row 4: City | Postal Code
            ImGui::SetNextItemWidth(halfW);
            ui::InputTextString("City##modal_card_city", &g_cred_modal.buf.card_city);
            ImGui::SameLine(0, colGap);
            ImGui::SetNextItemWidth(halfW);
            ui::InputTextString("Postal Code##modal_card_postal", &g_cred_modal.buf.card_postal_code);
            ImGui::Spacing();

            // Row 5: Group | Tags
            render_group_tags_row(halfW, colGap);
            ImGui::Spacing();
        }
        else if (ct == CredType::Identity)
        {
            // Row 1: Full Name | Country
            ImGui::SetNextItemWidth(halfW);
            ui::InputTextString("Full Name##modal_fullname", &g_cred_modal.buf.full_name);
            ImGui::SameLine(0, colGap);
            ImGui::SetNextItemWidth(halfW);
            ui::InputTextString("Country##modal_country", &g_cred_modal.buf.country);
            ImGui::Spacing();

            // Row 2: ID Type | ID Number
            ImGui::SetNextItemWidth(halfW);
            ui::InputTextString("ID Type##modal_idtype", &g_cred_modal.buf.id_type);
            ImGui::SameLine(0, colGap);
            ImGui::SetNextItemWidth(halfW);
            ui::InputTextString("ID Number##modal_idnum", &g_cred_modal.buf.id_number);
            ImGui::Spacing();

            // Row 3: Date of Birth | Expiry
            ImGui::SetNextItemWidth(halfW);
            ui::InputTextFormatted("Date of Birth##modal_dob", &g_cred_modal.buf.date_of_birth, "## / ## / ####");
            ImGui::SameLine(0, colGap);
            ImGui::SetNextItemWidth(halfW);
            ui::InputTextFormatted("Expiry##modal_idexpiry", &g_cred_modal.buf.expiry_date, "## / ## / ####");
            ImGui::Spacing();

            // Row 4: Address | Phone
            ImGui::SetNextItemWidth(halfW);
            ui::InputTextString("Address##modal_address", &g_cred_modal.buf.address);
            ImGui::SameLine(0, colGap);
            ImGui::SetNextItemWidth(halfW);
            ui::InputTextFormatted("Phone##modal_phone", &g_cred_modal.buf.phone, "(###) ###-####");
            ImGui::Spacing();

            // Row 5: Group | Tags
            render_group_tags_row(halfW, colGap);
            ImGui::Spacing();
        }
        else
        {
            // SecureNote: Group | Tags
            render_group_tags_row(halfW, colGap);
            ImGui::Spacing();
        }

        // Notes (all types, full width — larger for SecureNote)
        ImGui::TextDisabled("Notes");
        float notesH = (ct == CredType::SecureNote) ? 180.0f : 60.0f;
        ui::InputTextMultilineString("##modal_notes", &g_cred_modal.buf.notes,
            ImVec2(fieldW, notesH), 0);

        ImGui::Spacing();

        // TOTP Secret field (Password type only)
        if (ct == CredType::Password)
        {
            //ImGui::TextDisabled(ICON_MDI_CLOCK " TOTP Secret");
            ImGui::SetNextItemWidth(fieldW - 110.0f);
            const bool totpEdited = ui::InputTextString(ICON_MDI_CLOCK " TOTP Secret", &g_cred_modal.totp_secret_buf);

            // Pasted otpauth:// link: keep the secret, fill an empty title/username/email
            // from the link, and warn when the site uses settings the vault can't store
            // (matches Windows, web and the iOS QR scanner).
            {
                std::string lowerBuf = g_cred_modal.totp_secret_buf;
                for (auto& ch : lowerBuf) ch = (char)std::tolower((unsigned char)ch);
                if (lowerBuf.find("otpauth://") != std::string::npos ||
                    lowerBuf.find("otpauth-migration://") != std::string::npos)
                {
                    totp::OtpAuthInfo info;
                    std::string err;
                    if (totp::parse_otpauth_info(g_cred_modal.totp_secret_buf, info, err))
                    {
                        Credential& b = g_cred_modal.buf;
                        g_cred_modal.totp_secret_buf = info.secret;
                        if (b.title.empty() && !info.issuer.empty())
                            b.title = info.issuer;
                        if (b.user.empty() && b.email.empty() && !info.account.empty())
                        {
                            if (info.account.find('@') != std::string::npos) b.email = info.account;
                            else b.user = info.account;
                        }
                        g_cred_modal.totp_warning.clear();
                        if (!info.unsupported.empty())
                        {
                            std::string list;
                            for (size_t i = 0; i < info.unsupported.size(); i++)
                                list += (i ? ", " : "") + info.unsupported[i];
                            g_cred_modal.totp_warning = "Secret saved, but this site uses " + list +
                                ", which Password Defense doesn't support yet, so the code shown may not match. "
                                "Keep this account in your current authenticator app.";
                        }
                    }
                    else
                    {
                        g_cred_modal.totp_secret_buf.clear();
                        g_cred_modal.totp_warning = err;
                    }
                }
                else if (totpEdited)
                    g_cred_modal.totp_warning.clear();  // typing a key by hand drops an old warning
            }

            // Live preview of current code
            if (!g_cred_modal.totp_secret_buf.empty())
            {
                auto bytes = totp::base32_decode(g_cred_modal.totp_secret_buf);
                if (bytes.size() >= 10) {
                    std::string code = totp::generate_code_now(bytes);
                    int secs = totp::seconds_remaining_now();
                    ImGui::SameLine();
                    ImGui::TextColored(colors::Green, "%s (%ds)", code.c_str(), secs);
                } else {
                    ImGui::SameLine();
                    ImGui::TextColored(colors::Red, "Invalid");
                }
            }

            if (!g_cred_modal.totp_warning.empty())
            {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + fieldW);
                ImGui::TextColored(ImVec4(0.96f, 0.69f, 0.20f, 1.0f), ICON_MDI_ALERT " %s",
                    g_cred_modal.totp_warning.c_str());
                ImGui::PopTextWrapPos();
            }
        }

        // Password age indicator (edit mode, Password type, when flagged as aging)
        if (g_cred_modal.IsEdit() && ct == CredType::Password &&
            g_shell.sec_aging_ids.count(g_cred_modal.edit_id))
        {
            int64_t pw_set_at = g_cred_modal.buf.created_at_ms;
            if (!g_cred_modal.buf.password_history.empty() &&
                g_cred_modal.buf.password_history[0].changed_at_ms > 0)
                pw_set_at = g_cred_modal.buf.password_history[0].changed_at_ms;

            if (pw_set_at > 0) {
                int64_t age_days = (helpers::now_unix_ms() - pw_set_at) / (24LL * 3600 * 1000);
                char age_label[128];
                snprintf(age_label, sizeof(age_label),
                    ICON_MDI_CLOCK_ALERT " Password unchanged for %lld day%s",
                    (long long)age_days, age_days == 1 ? "" : "s");
                ImGui::Dummy(ImVec2(0, 2));
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(39, 174, 157, 255));
                ImGui::TextUnformatted(age_label);
                ImGui::PopStyleColor();
            }
        }

        ImGui::Dummy(ImVec2(0, 6));

        // Flat checkboxes (CheckboxBg) — Windows-parity: label-after-box,
        // visible inactive background, orange active overlay with checkmark.
        ui::CheckboxBg("Pinned", &g_cred_modal.is_pinned);
        ImGui::SameLine(0, 16);
        ui::CheckboxBg("Favorite", &g_cred_modal.is_favorite);
        ImGui::SameLine(0, 16);
        ui::CheckboxBg("Timed", &g_cred_modal.is_timed);

        if (g_cred_modal.is_timed)
        {
            static const char* timer_duration_labels[] = { "30 min", "1 hour", "6 hours", "24 hours", "7 days", "30 days", "90 days" };
            static const char* expiry_action_labels[]  = { "Auto-trash", "Flag only" };

            ImGui::Dummy(ImVec2(0, 2));
            ImGui::TextDisabled("Expires after");
            ImGui::SameLine(0, 8);
            ui::AnimatedComboDot("##timer_dur",
                timer_duration_labels[g_cred_modal.duration_idx],
                timer_duration_labels, IM_ARRAYSIZE(timer_duration_labels),
                &g_cred_modal.duration_idx, 100.0f, 28.0f);
            ImGui::SameLine(0, 16);
            ImGui::TextDisabled("then");
            ImGui::SameLine(0, 8);
            ui::AnimatedComboDot("##timer_act",
                expiry_action_labels[g_cred_modal.expiry_action_idx],
                expiry_action_labels, IM_ARRAYSIZE(expiry_action_labels),
                &g_cred_modal.expiry_action_idx, 110.0f, 28.0f);
        }

        // Validation (per type)
        bool can_submit = false;
        switch (ct)
        {
        case CredType::Password:
            can_submit = (!g_cred_modal.buf.title.empty() ||
                          !g_cred_modal.buf.email.empty() ||
                          !g_cred_modal.buf.user.empty()) &&
                         !g_cred_modal.password_buf.empty();
            break;
        case CredType::CreditCard:
            can_submit = !g_cred_modal.buf.title.empty() &&
                         !g_cred_modal.buf.card_number.empty();
            break;
        case CredType::Identity:
            can_submit = !g_cred_modal.buf.title.empty() &&
                         !g_cred_modal.buf.id_number.empty();
            break;
        case CredType::SecureNote:
            can_submit = !g_cred_modal.buf.title.empty();
            break;
        }

        // ---- Duplicate / reuse detection (non-blocking, informational) ----
        // Reuses dedup_norm_site for URL normalization. Uses a local trim+lower
        // helper for title/email/user so leading/trailing whitespace doesn't
        // hide an obvious duplicate (matches the Windows trim_lower behavior).
        struct Collisions {
            bool reused = false; int reuse_count = 0;
            bool dup_title = false; bool dup_account = false;
            bool any() const { return reused || dup_title || dup_account; }
        } coll;
        {
            auto trim_lower = [](const std::string& in) {
                size_t b = in.find_first_not_of(" \t\r\n");
                if (b == std::string::npos) return std::string();
                size_t e = in.find_last_not_of(" \t\r\n");
                std::string s = in.substr(b, e - b + 1);
                for (char& ch : s) if (ch >= 'A' && ch <= 'Z') ch = char(ch + 32);
                return s;
            };

            const std::string my_title = trim_lower(g_cred_modal.buf.title);
            const std::string& my_pw   = g_cred_modal.password_buf;
            const std::string my_site  = dedup_norm_site(g_cred_modal.buf.website);
            const std::string my_email = trim_lower(g_cred_modal.buf.email);
            const std::string my_user  = trim_lower(g_cred_modal.buf.user);
            const bool is_pw_type = (ct == CredType::Password);

            for (const auto& c : v.creds) {
                if (c.is_deleted()) continue;
                if (g_cred_modal.IsEdit() && c.id == g_cred_modal.edit_id) continue; // skip self

                if (!my_title.empty() && trim_lower(c.title) == my_title)
                    coll.dup_title = true;

                if (is_pw_type && c.type == CredType::Password) {
                    if (!my_pw.empty() && c.password == my_pw) {
                        coll.reused = true; coll.reuse_count++;
                    }
                    // Duplicate account: same login (email OR user) on the same
                    // service, matched by website OR title so it works whether or
                    // not a URL was entered.
                    const bool same_email = !my_email.empty() && trim_lower(c.email) == my_email;
                    const bool same_user  = !my_user.empty()  && trim_lower(c.user)  == my_user;
                    const bool same_site  = !my_site.empty()  && dedup_norm_site(c.website) == my_site;
                    const bool same_title = !my_title.empty() && trim_lower(c.title)  == my_title;
                    if ((same_email || same_user) && (same_site || same_title))
                        coll.dup_account = true;
                }
            }
        }

        // Submit button - same line as checkboxes, right flushed
        const float btnW = 80.0f;
        const float btnH = 32.0f;

        // Collision warning icon, just left of the submit button (hover for details)
        if (coll.any())
        {
            const float iconW = ImGui::CalcTextSize(ICON_MDI_ALERT).x;
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - btnW - 10.0f - iconW);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (btnH - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.20f, 1.0f), ICON_MDI_ALERT);
            if (ImGui::IsItemHovered())
            {
                std::string tip;
                if (coll.reused) {
                    char line[80];
                    snprintf(line, sizeof(line), "This password is reused by %d other %s",
                             coll.reuse_count, coll.reuse_count == 1 ? "entry" : "entries");
                    tip += line;
                }
                if (coll.dup_title) {
                    if (!tip.empty()) tip += "\n";
                    tip += "Another entry already uses this title";
                }
                if (coll.dup_account) {
                    if (!tip.empty()) tip += "\n";
                    tip += "A duplicate account with this username/email already exists";
                }
                ui::SetTooltipPadded("%s", tip.c_str());
            }
        }

        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - btnW);

        // Why-disabled hint. The validation table (title/user/password/card_number/
        // id_number per type) matches Windows parity but the button alone gives no
        // reason — surface it on hover so users don't have to guess which field
        // they left blank.
        const char* disabled_reason = nullptr;
        if (!can_submit)
        {
            switch (ct)
            {
            case CredType::Password:
                if (g_cred_modal.password_buf.empty())
                    disabled_reason = "Password is required.";
                else
                    disabled_reason = "Add a title, email, or username so you can find this later.";
                break;
            case CredType::CreditCard:
                if (g_cred_modal.buf.title.empty() && g_cred_modal.buf.card_number.empty())
                    disabled_reason = "Title and card number are required.";
                else if (g_cred_modal.buf.title.empty())
                    disabled_reason = "Title is required.";
                else
                    disabled_reason = "Card number is required.";
                break;
            case CredType::Identity:
                if (g_cred_modal.buf.title.empty() && g_cred_modal.buf.id_number.empty())
                    disabled_reason = "Title and ID number are required.";
                else if (g_cred_modal.buf.title.empty())
                    disabled_reason = "Title is required.";
                else
                    disabled_reason = "ID number is required.";
                break;
            case CredType::SecureNote:
                disabled_reason = "Title is required.";
                break;
            }
        }

        ImGui::BeginDisabled(!can_submit);
        const char* submitLabel = g_cred_modal.IsAdd() ? ICON_MDI_PLUS " Add" : ICON_MDI_CONTENT_SAVE " Save";
        bool submit_clicked = ui::StyledButtonLight("##modal_submit", submitLabel, ImVec2(btnW, btnH));

        // Hover over the disabled button to see the missing-field reason.
        // ImGuiHoveredFlags_AllowWhenDisabled lets BeginDisabled regions still
        // register as hovered; without it the tooltip never fires.
        if (!can_submit && disabled_reason &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ui::SetTooltipPadded("%s", disabled_reason);
        }

        if ((submit_clicked || submit_shortcut) && can_submit)
        {
            v.push_undo();

            if (g_cred_modal.IsAdd())
            {
                Credential c = g_cred_modal.buf;
                // Strip formatting from masked fields
                c.card_number   = ui::StripNonDigits(c.card_number);
                c.card_expiry   = ui::StripNonDigits(c.card_expiry);
                c.date_of_birth = ui::StripNonDigits(c.date_of_birth);
                c.expiry_date   = ui::StripNonDigits(c.expiry_date);
                c.phone         = ui::StripNonDigits(c.phone);
                c.type = g_cred_modal.selected_type;
                c.password = g_cred_modal.password_buf;
                c.totp_secret = g_cred_modal.totp_secret_buf;
                c.is_pinned = g_cred_modal.is_pinned;
                c.is_favorite = g_cred_modal.is_favorite;

                // Per-Credential timer
                {
                    static const int64_t timer_dur_ms[] = { 1800000LL, 3600000LL, 21600000LL, 86400000LL, 604800000LL, 2592000000LL, 7776000000LL };
                    if (g_cred_modal.is_timed) {
                        int64_t now = helpers::now_unix_ms();
                        c.expires_at_ms = now + timer_dur_ms[g_cred_modal.duration_idx];
                        c.expiry_action = g_cred_modal.expiry_action_idx;
                    } else {
                        c.expires_at_ms = 0;
                        c.expiry_action = 0;
                    }
                }

                // Assign next available ID
                int max_id = 0;
                for (const auto& cr : v.creds)
                    if (cr.id > max_id) max_id = cr.id;
                c.id = max_id + 1;

                const int64_t now_ms = helpers::now_unix_ms();
                if (c.created_at_ms == 0)
                    c.created_at_ms = now_ms;
                c.updated_at_ms = now_ms;

                v.creds.push_back(c);
                v.set_status("Added.", false);
                ui::ShowToast("Credential added", ui::ToastType::Success);
            }
            else
            {
                for (auto& c : v.creds)
                {
                    if (c.id == g_cred_modal.edit_id)
                    {
                        // Record password history if password actually changed
                        if (!c.password.empty() && c.password != g_cred_modal.password_buf) {
                            Credential::PasswordHistoryEntry entry;
                            entry.password = c.password;
                            entry.changed_at_ms = helpers::now_unix_ms();
                            c.password_history.insert(c.password_history.begin(), std::move(entry));
                            if (c.password_history.size() > 5)
                                c.password_history.resize(5);
                        }

                        c.title = g_cred_modal.buf.title;
                        c.email = g_cred_modal.buf.email;
                        c.user = g_cred_modal.buf.user;
                        c.password = g_cred_modal.password_buf;
                        c.website = g_cred_modal.buf.website;
                        c.group = g_cred_modal.buf.group;
                        c.tags = g_cred_modal.buf.tags;
                        c.notes = g_cred_modal.buf.notes;
                        c.totp_secret = g_cred_modal.totp_secret_buf;
                        c.is_pinned = g_cred_modal.is_pinned;
                        c.is_favorite = g_cred_modal.is_favorite;
                        // Per-Credential timer
                        {
                            static const int64_t timer_dur_ms[] = { 1800000LL, 3600000LL, 21600000LL, 86400000LL, 604800000LL, 2592000000LL, 7776000000LL };
                            if (g_cred_modal.is_timed) {
                                c.expires_at_ms = helpers::now_unix_ms() + timer_dur_ms[g_cred_modal.duration_idx];
                                c.expiry_action = g_cred_modal.expiry_action_idx;
                            } else {
                                c.expires_at_ms = 0;
                                c.expiry_action = 0;
                            }
                        }
                        // Type is immutable after creation (c.type unchanged)
                        c.card_number     = ui::StripNonDigits(g_cred_modal.buf.card_number);
                        c.card_expiry     = ui::StripNonDigits(g_cred_modal.buf.card_expiry);
                        c.card_cvv        = g_cred_modal.buf.card_cvv;
                        c.card_brand      = g_cred_modal.buf.card_brand;
                        c.cardholder_name = g_cred_modal.buf.cardholder_name;
                        c.card_address     = g_cred_modal.buf.card_address;
                        c.card_city        = g_cred_modal.buf.card_city;
                        c.card_postal_code = g_cred_modal.buf.card_postal_code;
                        c.full_name     = g_cred_modal.buf.full_name;
                        c.id_type       = g_cred_modal.buf.id_type;
                        c.id_number     = g_cred_modal.buf.id_number;
                        c.date_of_birth = ui::StripNonDigits(g_cred_modal.buf.date_of_birth);
                        c.expiry_date   = ui::StripNonDigits(g_cred_modal.buf.expiry_date);
                        c.country       = g_cred_modal.buf.country;
                        c.address       = g_cred_modal.buf.address;
                        c.phone         = ui::StripNonDigits(g_cred_modal.buf.phone);
                        c.updated_at_ms = helpers::now_unix_ms();
                        break;
                    }
                }
                v.set_status("Saved.", false);
            }

            VaultMarkChanged(v, "EDIT");
            g_autosave.last_change_time = ImGui::GetTime();
            rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);

            g_cred_modal.Close();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();

        if (ImGui::IsItemHovered() && can_submit)
            ImGui::SetTooltip("Ctrl+Enter");

        ImGui::EndPopup();
    }

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    if (!modal_open)
        g_cred_modal.Close();
}

// ============================================================
// Sync Auth Modal Rendering
// ============================================================
static void init_sync_manager()
{
    if (!g_sync_mgr) {
        std::string url = cfg::get_sync_server_url();
        if (url.empty()) url = "https://api.passworddefense.net";
        g_sync_mgr = std::make_unique<SyncManager>(url);

        // Set up status callback to update shell state
        g_sync_mgr->set_status_callback([](SyncStatus status, const std::string& msg) {
            g_shell.sync_status = static_cast<int>(status);
            g_shell.sync_status_msg = msg;
        });

        // Set up sync complete callback
        g_sync_mgr->set_sync_complete_callback([](const SyncResult& result) {
            if (result.success) {
                g_shell.last_sync_ms = helpers::now_unix_ms();

                // Reload credentials if we pulled any changes from server
                if (result.pulled > 0) {
                    VaultState& v = ActiveVault();
                    if (v.unlocked && !v.master_key.empty()) {
                        v.creds = cred_ops::load_all(v.master_key);
                        rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);
                        ui::ForgetVaultRowState(GetActiveVaultKey());
                        VaultMarkSaved(v);
                    }
                }
            }
        });

        // Restore login state from config
        g_shell.sync_server_url = url;

        // Try to restore session from stored tokens
        if (g_sync_mgr->load_tokens_from_db()) {
            g_shell.sync_logged_in = g_sync_mgr->is_logged_in();
            g_shell.sync_username = g_sync_mgr->get_username();
        }
    }
}

static void propagate_sync_tokens()
{
    if (!g_sync_mgr || !g_sync_mgr->is_logged_in()) return;
    if (!vault_db::is_open()) return;
    std::string existing = vault_db::get_sync_state("sync_access_token");
    if (existing.empty())
        g_sync_mgr->save_tokens_to_db();
}

// Fire the version check exactly once, on a detached thread so a slow or
// unreachable server never stalls startup. Safe to call every frame — the
// compare_exchange gate makes all calls after the first a no-op.
static void kick_off_update_check()
{
    static std::atomic<bool> s_started{false};
    bool expected = false;
    if (!s_started.compare_exchange_strong(expected, true))
        return;

    init_sync_manager();              // main thread: ensures g_sync_mgr exists
    SyncManager* mgr = g_sync_mgr.get();
    if (!mgr) { g_update.done.store(true, std::memory_order_release); return; }

    std::thread([mgr]() {
        auto info = mgr->check_app_version(PWMNGR_APP_VERSION);
        if (info.ok) {
            g_update.latest = info.latest;             // written before
            g_update.download_url = info.download_url; // the release store
            g_update.notes = info.notes;               // below publishes them
            g_update.available.store(info.update_available, std::memory_order_relaxed);
            g_update.required.store(info.update_required, std::memory_order_relaxed);
        }
        g_update.done.store(true, std::memory_order_release);
    }).detach();
}

// Thin banner pinned to the top of the viewport. Dismissible when an update is
// merely available; persistent (no Dismiss) when this build is below the
// supported floor. We deliberately do NOT hard-block the app — locking a user
// out of their own offline vault would be worse than running a stale build.
static void render_update_banner()
{
    if (!g_update.done.load(std::memory_order_acquire))
        return;

    const bool required = g_update.required.load(std::memory_order_relaxed);
    const bool available = g_update.available.load(std::memory_order_relaxed);
    if (!required && !available) return;
    if (!required && g_update.dismissed) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 10.0f),
        ImGuiCond_Always, ImVec2(0.5f, 0.0f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, required ? ImVec4(0.52f, 0.13f, 0.13f, 0.97f)
                                                      : ImVec4(0.12f, 0.30f, 0.48f, 0.97f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 10.0f));
    if (ImGui::Begin("##update_banner", nullptr, flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::Text("%s — version %s",
                    required ? "Update required" : "Update available",
                    g_update.latest.c_str());
        if (!g_update.notes.empty()) {
            ImGui::SameLine(0, 12);
            ImGui::TextDisabled("%s", g_update.notes.c_str());
        }

        if (!g_update.download_url.empty()) {
            ImGui::SameLine(0, 14);
            if (ImGui::SmallButton("Download")) {
                // Mac uses raw `system("open ...")` for outbound URLs
                // (same pattern as ui_views.cpp:468 / :486).
                std::string cmd = "open \"" + g_update.download_url + "\"";
                system(cmd.c_str());
            }
        }
        if (!required) {
            ImGui::SameLine(0, 6);
            if (ImGui::SmallButton("Dismiss")) {
                g_update.dismissed = true;
            }
        }
        ImGui::PopStyleColor(); // Text
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(); // WindowBg
}

static void render_sync_modal()
{
#ifdef PD_OFFLINE_BUILD
    // Offline build: cloud auth modal is unreachable (the trigger that opens
    // it is also guarded out). Early-return keeps the function present for
    // link-compatibility without rendering anything.
    return;
#else
    if (!g_sync_modal.IsOpen()) return;

    init_sync_manager();

    // Theme-aware modal colors
    const ImU32 popupBg = cfg::is_dark_theme()
        ? IM_COL32(36, 35, 39, 255)
        : IM_COL32(250, 250, 252, 255);
    const ImU32 dimBg = IM_COL32(0, 0, 0, 140);

    // Styling
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, popupBg);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, dimBg);

    const float modalW = 380.0f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(modalW, 0), ImVec2(modalW, FLT_MAX));

    const char* modal_title = g_sync_modal.Islogin()
        ? "Sign In###sync_modal"
        : "Create Account###sync_modal";

    bool modal_open = true;
    if (ImGui::BeginPopupModal(modal_title, &modal_open,
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove))
    {
        // Keyboard shortcuts
        bool escape_pressed = ImGui::IsKeyPressed(ImGuiKey_Escape);
        bool submit_shortcut = ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_Enter);

        // Header
        ImGui::PushFont(render::FontLarge);
        ImGui::TextUnformatted(g_sync_modal.Islogin() ? "Sign In to Sync" : "Create Account");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 12));

        const float fieldW = ImGui::GetContentRegionAvail().x;

        // Server URL is set but not shown in UI
        // (uses default from cfg::get_sync_server_url() or fallback)

        // Email
        ImGui::TextDisabled("Email");
        ImGui::SetNextItemWidth(fieldW);
        ui::InputTextString("##sync_email", &g_sync_modal.email);

        // Username (register only)
        if (g_sync_modal.Isregister_user()) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextDisabled("Username");
            ImGui::SetNextItemWidth(fieldW);
            ui::InputTextString("##sync_username", &g_sync_modal.username);
        }

        ImGui::Dummy(ImVec2(0, 4));

        // Password
        ImGui::TextDisabled("Password");
        ImGui::SetNextItemWidth(fieldW);
        if (g_sync_modal.show_password)
            ui::InputTextString("##sync_password", &g_sync_modal.password);
        else
            ui::InputTextPasswordReveal("##sync_password", &g_sync_modal.password);

        // Confirm password (register only)
        if (g_sync_modal.Isregister_user()) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextDisabled("Confirm Password");
            ImGui::SetNextItemWidth(fieldW);
            if (g_sync_modal.show_password)
                ui::InputTextString("##sync_password_confirm", &g_sync_modal.password_confirm);
            else
                ui::InputTextPasswordReveal("##sync_password_confirm", &g_sync_modal.password_confirm);
        }

        ImGui::Checkbox2("Show password", &g_sync_modal.show_password);

        // Error message
        if (!g_sync_modal.error_msg.empty()) {
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(174, 41, 41, 255));
            ImGui::TextWrapped("%s", g_sync_modal.error_msg.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Dummy(ImVec2(0, 12));

        // Validation
        bool can_submit = !g_sync_modal.server_url.empty() &&
                          !g_sync_modal.email.empty() &&
                          !g_sync_modal.password.empty();

        if (g_sync_modal.Isregister_user()) {
            can_submit = can_submit &&
                         !g_sync_modal.username.empty() &&
                         g_sync_modal.password == g_sync_modal.password_confirm;
        }

        // Buttons
        const float btnW = 100.0f;
        const float btnH = 32.0f;
        float totalBtnW = btnW * 2 + 8.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + fieldW - totalBtnW);

        if (ui::StyledButton("##sync_cancel", "Cancel", ImVec2(btnW, btnH)) || escape_pressed) {
            g_sync_modal.Close();
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine(0, 8.0f);

        ImGui::BeginDisabled(!can_submit || g_sync_modal.loading);
        const char* submit_label = g_sync_modal.Islogin() ? "Sign In" : "Register";
        bool submit_clicked = ui::StyledButton("##sync_submit", submit_label, ImVec2(btnW, btnH));

        if ((submit_clicked || submit_shortcut) && can_submit && !g_sync_modal.loading) {
            g_sync_modal.loading = true;
            g_sync_modal.error_msg.clear();

            // Update server URL
            g_sync_mgr->set_base_url(g_sync_modal.server_url);
            cfg::set_sync_server_url(g_sync_modal.server_url);
            g_shell.sync_server_url = g_sync_modal.server_url;

            // Compute auth hash (Option C: sha256(password + email))
            std::string auth_hash = auth::compute_auth_hash(
                g_sync_modal.password, g_sync_modal.email);

            AuthResult result;
            if (g_sync_modal.Islogin()) {
                result = g_sync_mgr->login(g_sync_modal.email, auth_hash);
            } else {
                result = g_sync_mgr->register_user(
                    g_sync_modal.username, g_sync_modal.email, auth_hash);
            }

            g_sync_modal.loading = false;

            if (result.success) {
                // Login returns tokens, Register does not (user needs to login after)
                if (g_sync_modal.Islogin()) {
                    g_shell.sync_logged_in = true;
                    g_shell.sync_username = result.username.empty() ? g_sync_modal.email : result.username;
                }

                // Handle encryption salt synchronization
                if (vault_db::is_open()) {
                    VaultState& v = ActiveVault();
                    auto local_salt = vault_db::get_cached_salt();

                    if (result.has_server_salt && !result.encryption_salt.empty()) {
                        // Server has salt - check if different from local
                        if (local_salt != result.encryption_salt) {
                            if (v.unlocked && !v.session_password.empty() && !v.master_key.empty()) {
                                auto new_key = enc::derive_master_key(v.session_password, result.encryption_salt);
                                if (!new_key.empty()) {
                                    auto old_key = std::move(v.master_key);
                                    v.master_key = std::move(new_key);
                                    vault_db::set_cached_salt(result.encryption_salt);

                                    // Re-encrypt all credentials with new key
                                    bool re_encrypt_ok = true;
                                    for (auto& c : v.creds) {
                                        if (!cred_ops::update(c.uuid, c, v.master_key)) {
                                            re_encrypt_ok = false;
                                            break;
                                        }
                                        c.is_dirty = true;
                                    }

                                    if (re_encrypt_ok) {
                                        enc::secure_zero(old_key);
                                        if (vault_db::has_recovery_key())
                                            vault_db::delete_recovery_blob();
                                    } else {
                                        // Rollback
                                        v.master_key = std::move(old_key);
                                        vault_db::set_cached_salt(local_salt);
                                    }
                                }
                            } else {
                                // Vault locked or no password - just save salt for next unlock
                                vault_db::set_cached_salt(result.encryption_salt);
                            }
                        }
                    } else if (!local_salt.empty()) {
                        // Server has no salt but we have local salt - upload it
                        g_sync_mgr->upload_salt(local_salt);
                    }
                }

                // Save mode before closing (Close() clears it)
                g_sync_modal.Close();
                ImGui::CloseCurrentPopup();
            } else {
                g_sync_modal.error_msg = result.message;
            }
        }
        ImGui::EndDisabled();

        ImGui::EndPopup();
    }

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    if (!modal_open)
        g_sync_modal.Close();
#endif  // PD_OFFLINE_BUILD
}

static uint32_t compute_changed_fields(const Credential& cur, const Credential* saved)
{
    if (!saved)
        return ui::FCF_IsNew;

    uint32_t f = 0;
    if (cur.title       != saved->title)       f |= ui::FCF_Title;
    if (cur.user        != saved->user)        f |= ui::FCF_User;
    if (cur.email       != saved->email)       f |= ui::FCF_Email;
    if (cur.password    != saved->password)    f |= ui::FCF_Password;
    if (cur.website     != saved->website)     f |= ui::FCF_Website;
    if (cur.group       != saved->group)       f |= ui::FCF_Group;
    if (cur.notes       != saved->notes)       f |= ui::FCF_Notes;
    if (cur.totp_secret != saved->totp_secret) f |= ui::FCF_TotpSecret;
    if (cur.card_number != saved->card_number) f |= ui::FCF_CardNumber;
    if (cur.card_expiry != saved->card_expiry) f |= ui::FCF_CardExpiry;
    if (cur.card_cvv    != saved->card_cvv)    f |= ui::FCF_CardCvv;
    if (cur.card_brand  != saved->card_brand)  f |= ui::FCF_CardBrand;
    if (cur.cardholder_name != saved->cardholder_name) f |= ui::FCF_Cardholder;
    if (cur.card_address     != saved->card_address)     f |= ui::FCF_CardAddress;
    if (cur.card_city        != saved->card_city)        f |= ui::FCF_CardCity;
    if (cur.card_postal_code != saved->card_postal_code) f |= ui::FCF_CardPostalCode;
    if (cur.full_name   != saved->full_name)   f |= ui::FCF_FullName;
    if (cur.id_type     != saved->id_type)     f |= ui::FCF_IdType;
    if (cur.id_number   != saved->id_number)   f |= ui::FCF_IdNumber;
    if (cur.date_of_birth != saved->date_of_birth) f |= ui::FCF_DateOfBirth;
    if (cur.expiry_date != saved->expiry_date) f |= ui::FCF_ExpiryDate;
    if (cur.country     != saved->country)     f |= ui::FCF_Country;
    if (cur.address     != saved->address)     f |= ui::FCF_Address;
    if (cur.phone       != saved->phone)       f |= ui::FCF_Phone;
    if (cur.is_favorite != saved->is_favorite) f |= ui::FCF_IsFavorite;
    if (cur.is_pinned   != saved->is_pinned)   f |= ui::FCF_IsPinned;
    if (cur.expires_at_ms != saved->expires_at_ms) f |= ui::FCF_ExpiresAt;
    return f;
}

// ============================================================
// Recently-used tracking
// ============================================================
static void load_recent_items(ui::ShellState& s)
{
    s.recent_items.clear();
    try {
        auto arr = nlohmann::json::parse(cfg::get_recent_items_json());
        for (const auto& e : arr) {
            ui::ShellState::RecentEntry re;
            re.uuid = e.value("uuid", "");
            re.accessed_at_ms = e.value("ts", (int64_t)0);
            if (!re.uuid.empty())
                s.recent_items.push_back(std::move(re));
        }
    } catch (...) {}
}

static void persist_recent_items(const ui::ShellState& s)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : s.recent_items)
        arr.push_back({{"uuid", e.uuid}, {"ts", e.accessed_at_ms}});
    cfg::set_recent_items_json(arr.dump());
}

static void touch_recent(ui::ShellState& s, const std::string& uuid)
{
    if (uuid.empty()) return;
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::erase_if(s.recent_items, [&](const ui::ShellState::RecentEntry& e) {
        return e.uuid == uuid;
    });
    s.recent_items.insert(s.recent_items.begin(), {uuid, now});
    if (s.recent_items.size() > 25)
        s.recent_items.resize(25);
    persist_recent_items(s);
}

static std::vector<ui::AccordionItem> build_accordion_items(
    const std::vector<Credential>& creds,
    const std::unordered_map<std::string, Credential>& saved_snapshot,
    const char* search,
    int search_filter,
    int sort_mode,
    const std::string& pill_tab_0,
    const std::string& pill_tab_1,
    const std::set<std::string>& groups,
    const std::set<std::string>& tag_filters,
    ui::OrderKey order_key,
    ui::OrderDir order_dir,
    ui::GroupMode group_mode,
    const std::vector<ui::ShellState::RecentEntry>& recent_items = {})
{
    // 1) filter pointers
    std::vector<const Credential*> list;
    list.reserve(creds.size());

    // Split selected groups into type filters (@-prefixed) and group filters
    std::set<CredType> type_filters;
    std::set<std::string> group_filters;
    for (const auto& g : groups) {
        if (g == "@Passwords")      type_filters.insert(CredType::Password);
        else if (g == "@Cards")    type_filters.insert(CredType::CreditCard);
        else if (g == "@Identity") type_filters.insert(CredType::Identity);
        else if (g == "@Notes")    type_filters.insert(CredType::SecureNote);
        else if (g != "---")           group_filters.insert(g);
    }

    // Determine active pill tab filter
    std::string active_pill;
    if (sort_mode == 0) active_pill = pill_tab_0;
    else if (sort_mode == 1) active_pill = pill_tab_1;
    else if (sort_mode == 3) active_pill = "Recent";

    // Build recent UUID set for O(1) lookup
    std::unordered_set<std::string> recent_uuids;
    if (active_pill == "Recent") {
        for (const auto& e : recent_items)
            recent_uuids.insert(e.uuid);
    }

    for (const auto& c : creds)
    {
        if (!type_filters.empty() && type_filters.find(c.type) == type_filters.end()) continue;
        if (!group_filters.empty() && group_filters.find(c.group) == group_filters.end()) continue;

        // Tag filtering (OR logic: credential must have ANY selected tag)
        if (!tag_filters.empty()) {
            bool has_any = false;
            for (const auto& t : c.tags)
                if (tag_filters.count(t)) { has_any = true; break; }
            if (!has_any) continue;
        }

        // Pill tab filtering
        if (!active_pill.empty())
        {
            if (active_pill == "Pinned") { if (!c.is_pinned) continue; }
            else if (active_pill == "Favorites") { if (!c.is_favorite) continue; }
            else if (active_pill == "Recent") { if (recent_uuids.find(c.uuid) == recent_uuids.end()) continue; }
            else { if (c.group != active_pill) continue; }
        }

        if (!matches_search(c, search, search_filter)) continue;

        list.push_back(&c);
    }

    // Use Unix ms timestamps directly for sorting (already numeric)
    auto created_key = [](const Credential* c) -> uint64_t {
        return c ? static_cast<uint64_t>(c->created_at_ms) : 0;
        };
    auto updated_key = [&](const Credential* c) -> uint64_t {
        if (!c) return 0;
        if (c->updated_at_ms != 0)
            return static_cast<uint64_t>(c->updated_at_ms);
        return created_key(c);
        };

    auto compare_items = [&](const Credential* a, const Credential* b) -> bool
        {
            if (order_key == ui::OrderKey::Title)
            {
                if (sort_mode == 2)
                {
                    if (a->is_pinned != b->is_pinned)      return a->is_pinned > b->is_pinned;
                    if (a->is_favorite != b->is_favorite) return a->is_favorite > b->is_favorite;
                }

                // Case-insensitive title compare. Raw `<` on std::string is a
                // byte compare (uppercase < lowercase in ASCII), so 'Yahoo'
                // would land before 'bofa' / 'cross' / etc.
                int cmp = strcasecmp(a->title.c_str(), b->title.c_str());
                if (cmp != 0)
                {
                    if (order_dir == ui::OrderDir::Asc)  return cmp < 0;
                    else                                 return cmp > 0;
                }

                const uint64_t ka = created_key(a);
                const uint64_t kb = created_key(b);
                if (ka != kb) return ka > kb;
                return a->id < b->id;
            }

            const uint64_t ka = (order_key == ui::OrderKey::Created) ? created_key(a) : updated_key(a);
            const uint64_t kb = (order_key == ui::OrderKey::Created) ? created_key(b) : updated_key(b);

            if (ka != kb)
            {
                if (order_dir == ui::OrderDir::Asc)  return ka < kb;
                else                                 return ka > kb;
            }

            // Same case-insensitive title tiebreak for date sorts.
            int cmp_tb = strcasecmp(a->title.c_str(), b->title.c_str());
            if (cmp_tb != 0) return cmp_tb < 0;
            return a->id < b->id;
        };

    // Build uuid->position map for recent sorting
    std::unordered_map<std::string, int> recent_pos;
    if (active_pill == "Recent") {
        for (int i = 0; i < (int)recent_items.size(); i++)
            recent_pos[recent_items[i].uuid] = i;
    }

    auto sort_bucket = [&](std::vector<const Credential*>& bucket)
        {
            if (active_pill == "Recent" && !recent_pos.empty()) {
                std::sort(bucket.begin(), bucket.end(), [&](const Credential* a, const Credential* b) {
                    auto ia = recent_pos.find(a->uuid);
                    auto ib = recent_pos.find(b->uuid);
                    int pa = (ia != recent_pos.end()) ? ia->second : 9999;
                    int pb = (ib != recent_pos.end()) ? ib->second : 9999;
                    return pa < pb;
                });
            } else {
                std::sort(bucket.begin(), bucket.end(), compare_items);
            }
        };

    // 2) build UI items (+ optional headers)
    std::vector<ui::AccordionItem> out;
    out.reserve(list.size() + 32);

    const bool showCount = ui::GetShowGroupCount();
    auto push_header = [&](const std::string& label, int count = 0)
        {
            ui::AccordionItem hdr{};
            hdr.is_header = true;
            hdr.id = 0;
            if (showCount && count > 0)
                hdr.header_label = label + "  (" + std::to_string(count) + ")";
            else
                hdr.header_label = label;
            out.push_back(std::move(hdr));
        };

    auto push_item = [&](const Credential* c)
        {
            if (!c) return;
            ui::AccordionItem it{};
            it.id = c->id;
            it.uuid = c->uuid;
            it.title = c->title;
            it.user = c->user;
            it.email = c->email;
            it.website = c->website;
            it.group = c->group;
            it.notes = c->notes;
            it.totp_secret = c->totp_secret;
            it.type = c->type;
            it.card_number = c->card_number;
            it.card_expiry = c->card_expiry;
            it.card_cvv = c->card_cvv;
            it.card_brand = c->card_brand;
            it.cardholder_name = c->cardholder_name;
            it.card_address = c->card_address;
            it.card_city = c->card_city;
            it.card_postal_code = c->card_postal_code;
            it.full_name = c->full_name;
            it.id_type = c->id_type;
            it.id_number = c->id_number;
            it.date_of_birth = c->date_of_birth;
            it.expiry_date = c->expiry_date;
            it.country = c->country;
            it.address = c->address;
            it.phone = c->phone;
            it.is_pinned = c->is_pinned;
            it.is_favorite = c->is_favorite;
            it.expires_at_ms = c->expires_at_ms;
            it.expiry_action = c->expiry_action;
            it.created_at_ms = c->created_at_ms;
            it.updated_at_ms = c->updated_at_ms;
            it.password_history.reserve(c->password_history.size());
            for (const auto& h : c->password_history) {
                it.password_history.push_back({h.password, h.changed_at_ms});
            }

            // Compute change flags vs saved snapshot
            const Credential* saved_ptr = nullptr;
            if (!c->uuid.empty()) {
                auto sit = saved_snapshot.find(c->uuid);
                if (sit != saved_snapshot.end())
                    saved_ptr = &sit->second;
            }
            it.changed_fields = compute_changed_fields(*c, saved_ptr);
            it.is_changed = (it.changed_fields != 0);

            out.push_back(std::move(it));
        };

    if (group_mode == ui::GroupMode::PinnedFavorites)
    {
        std::vector<const Credential*> pinned;
        std::vector<const Credential*> favorites;
        std::vector<const Credential*> rest;
        pinned.reserve(list.size());
        favorites.reserve(list.size());
        rest.reserve(list.size());

        for (const Credential* c : list)
        {
            if (!c) continue;
            if (c->is_pinned) pinned.push_back(c);
            else if (c->is_favorite) favorites.push_back(c);
            else rest.push_back(c);
        }

        if (!pinned.empty())
        {
            sort_bucket(pinned);
            push_header("Pinned", (int)pinned.size());
            for (const Credential* c : pinned) push_item(c);
        }

        if (!favorites.empty())
        {
            sort_bucket(favorites);
            push_header("Favorites", (int)favorites.size());
            for (const Credential* c : favorites) push_item(c);
        }

        if (!rest.empty())
        {
            sort_bucket(rest);
            push_header("All", (int)rest.size());
            for (const Credential* c : rest) push_item(c);
        }
    }
    else if (group_mode == ui::GroupMode::Group)
    {
        // Pinned & favorites float to top in their own sections
        std::vector<const Credential*> pinned;
        std::vector<const Credential*> favorites;
        std::unordered_map<std::string, std::vector<const Credential*>> buckets;
        buckets.reserve(list.size());

        for (const Credential* c : list)
        {
            if (!c) continue;
            if (c->is_pinned)        pinned.push_back(c);
            else if (c->is_favorite) favorites.push_back(c);
            else {
                std::string key = c->group.empty() ? "Ungrouped" : c->group;
                buckets[key].push_back(c);
            }
        }

        if (!pinned.empty())
        {
            sort_bucket(pinned);
            push_header("Pinned", (int)pinned.size());
            for (const Credential* c : pinned) push_item(c);
        }

        if (!favorites.empty())
        {
            sort_bucket(favorites);
            push_header("Favorites", (int)favorites.size());
            for (const Credential* c : favorites) push_item(c);
        }

        std::vector<std::string> keys;
        keys.reserve(buckets.size());
        for (const auto& kv : buckets) keys.push_back(kv.first);

        std::sort(keys.begin(), keys.end(), [](const std::string& a, const std::string& b)
            {
                if (a == "Ungrouped") return false;
                if (b == "Ungrouped") return true;
                return a < b;
            });

        for (const auto& key : keys)
        {
            auto& bucket = buckets[key];
            sort_bucket(bucket);
            push_header(key, (int)bucket.size());
            for (const Credential* c : bucket) push_item(c);
        }
    }
    else if (group_mode == ui::GroupMode::Alphabetical)
    {
        std::vector<const Credential*> pinned;
        std::vector<const Credential*> favorites;
        std::map<char, std::vector<const Credential*>> buckets;
        std::vector<const Credential*> other;

        for (const Credential* c : list)
        {
            if (!c) continue;
            if (c->is_pinned)        pinned.push_back(c);
            else if (c->is_favorite) favorites.push_back(c);
            else if (!c->title.empty() && std::isalpha((unsigned char)c->title[0]))
                buckets[(char)std::toupper((unsigned char)c->title[0])].push_back(c);
            else
                other.push_back(c);
        }

        if (!pinned.empty())
        {
            sort_bucket(pinned);
            push_header("Pinned", (int)pinned.size());
            for (const Credential* c : pinned) push_item(c);
        }

        if (!favorites.empty())
        {
            sort_bucket(favorites);
            push_header("Favorites", (int)favorites.size());
            for (const Credential* c : favorites) push_item(c);
        }

        for (auto& kv : buckets)
        {
            sort_bucket(kv.second);
            char hdr[2] = { kv.first, '\0' };
            push_header(hdr, (int)kv.second.size());
            for (const Credential* c : kv.second) push_item(c);
        }

        if (!other.empty())
        {
            sort_bucket(other);
            push_header("#", (int)other.size());
            for (const Credential* c : other) push_item(c);
        }
    }
    else if (group_mode == ui::GroupMode::MonthCreated || group_mode == ui::GroupMode::MonthUpdated)
    {
        std::vector<const Credential*> pinned;
        std::vector<const Credential*> favorites;
        std::unordered_map<uint32_t, std::vector<const Credential*>> buckets;
        buckets.reserve(list.size());

        for (const Credential* c : list)
        {
            if (!c) continue;
            if (c->is_pinned)        pinned.push_back(c);
            else if (c->is_favorite) favorites.push_back(c);
            else {
                int64_t src_ms = (group_mode == ui::GroupMode::MonthCreated) ? c->created_at_ms : c->updated_at_ms;
                uint32_t mk = unix_ms_to_monthkey(src_ms);
                buckets[mk].push_back(c);
            }
        }

        if (!pinned.empty())
        {
            sort_bucket(pinned);
            push_header("Pinned", (int)pinned.size());
            for (const Credential* c : pinned) push_item(c);
        }

        if (!favorites.empty())
        {
            sort_bucket(favorites);
            push_header("Favorites", (int)favorites.size());
            for (const Credential* c : favorites) push_item(c);
        }

        std::vector<uint32_t> keys;
        keys.reserve(buckets.size());
        for (const auto& kv : buckets) keys.push_back(kv.first);

        const bool desc = (order_dir == ui::OrderDir::Desc);
        std::sort(keys.begin(), keys.end(), [&](uint32_t a, uint32_t b)
            {
                if (a == 0) return false;
                if (b == 0) return true;
                return desc ? (a > b) : (a < b);
            });

        for (uint32_t key : keys)
        {
            auto& bucket = buckets[key];
            sort_bucket(bucket);
            push_header((key == 0) ? "Unknown date" : month_label_from_key(key), (int)bucket.size());
            for (const Credential* c : bucket) push_item(c);
        }
    }
    else
    {
        sort_bucket(list);
        for (const Credential* c : list) push_item(c);
    }

    return out;
}

static const char* GetPasswordForRow(int id)
{
    auto& v = ActiveVault();
    for (auto& c : v.creds)
        if (c.id == id)
            return (c.type == CredType::Password) ? c.password.c_str() : "";
    return "";
}

// Forward declaration for auto-lock timer (defined with TickAutoLock)
namespace {
    double g_last_activity_time = 0.0;
    ImVec2 g_last_mouse_pos = ImVec2(0, 0);
}

// Scan the recovery dir on unlock. If any .rec decrypts under this vault's master
// key (ownership proof — unrelated vaults' .rec files have different keys and will
// fail the AEAD check), merge newer-than-local entries into v.creds, re-save, then
// delete the .rec. "Newer than local" is the stale-recovery guard — a .rec left
// behind from a stale session can't clobber a legit edit the user made since.
// Defined later in the file (next to save_vault_to_disk) because it needs the
// save path to commit the restored state.
static bool restore_recovery_snapshot_if_newer(VaultState& v);

static bool load_vault_from_disk(const std::string& path, const std::string& password, VaultState& v)
{
    v.clear_status();

    if (path.empty() || password.empty())
    {
        v.set_status("Path + password required.", true);
        return false;
    }

    // Close any previously open vault
    vault_db::close();

    // Open the SQLite database
    if (!vault_db::init(path))
    {
        v.set_status("Failed to open vault database.", true);
        return false;
    }

    // Integrity check — warn but allow access (user may want to salvage data)
    if (!vault_db::quick_integrity_check())
        v.set_status("Warning: database may be corrupted. Consider restoring from backup.", true);

    // Get cached salt
    std::vector<uint8_t> salt = vault_db::get_cached_salt();
    if (salt.empty())
    {
        vault_db::close();
        v.set_status("Vault has no encryption salt (corrupt or wrong format).", true);
        return false;
    }

    // Check stored KDF level (defaults to moderate for legacy vaults)
    bool high_sec = (vault_db::get_sync_state("kdf_level") == "sensitive");

    // Derive master key from password + salt
    std::vector<uint8_t> master_key = enc::derive_master_key(password, salt, high_sec);
    if (master_key.empty())
    {
        vault_db::close();
        v.set_status("Failed to derive encryption key.", true);
        return false;
    }

    // Unlock-time key-check: if the vault has been stamped (any vault that's been
    // saved by this build or newer), verifying the stamp is a definitive wrong-key
    // test. Covers the gap in the "did anything decrypt?" check below, which lets
    // wrong-password unlock succeed when the vault happens to be empty.
    const std::vector<uint8_t> kc_blob = vault_db::get_key_check();
    const bool had_key_check = !kc_blob.empty();
    if (had_key_check && !enc::verify_key_check(kc_blob, master_key))
    {
        enc::secure_zero(master_key);
        vault_db::close();
        v.set_status("Wrong password.", true);
        return false;
    }

    // Load all credentials
    std::vector<Credential> creds = cred_ops::load_all(master_key);

    // If we have credentials in DB but couldn't decrypt any, password is likely wrong.
    // Secondary defense for legacy vaults without a key_check stamp (verified above).
    if (creds.empty() && vault_db::count_credentials() > 0)
    {
        enc::secure_zero(master_key);
        vault_db::close();
        v.set_status("Failed to decrypt credentials (wrong password?).", true);
        return false;
    }

    // Legacy vault (created before key_check stamping): stamp one now, but only
    // once the password is proven by decrypting real data. Mirrors pw-windows
    // load_vault_from_disk. This also closes the trash-only wrong-password hole:
    // a vault whose only rows are trashed has no live creds and count_credentials()
    // == 0, so the check above can't catch a wrong password — load_deleted does.
    // A legacy vault with no live AND no trashed entries can't be verified here; it
    // gets stamped on its first save with content (cred_ops::save_all).
    if (!had_key_check)
    {
        bool verified = !creds.empty();
        if (!verified && !vault_db::get_deleted_credentials().empty())
        {
            if (cred_ops::load_deleted(master_key).empty())
            {
                enc::secure_zero(master_key);
                vault_db::close();
                v.set_status("Wrong password.", true);
                return false;
            }
            verified = true;
        }
        if (verified)
            vault_db::set_key_check(enc::make_key_check(master_key));
    }

    v.vault_path = path;
    v.master_key = std::move(master_key);
    lock_key(v.master_key);            // pin in physical RAM
    v.session_password = password;     // Keep for restore operations
    lock_string(v.session_password);   // pin in physical RAM
    v.creds = std::move(creds);

    // Auto-purge old trash items
    {
        int days = cfg::get_trash_retention_days();
        if (days > 0) {
            int64_t cutoff = helpers::now_unix_ms() - (int64_t)days * 86400000LL;
            vault_db::purge_old_tombstones(cutoff);
        }
    }

    // Check if 2FA is enabled — if so, defer full unlock
    if (twofa_ops::is_enabled())
    {
        v.tofa_pending = true;
        v.set_status("Enter your two-factor authentication code.", false);
        return false;
    }

    VaultMarkSaved(v);

    secure_clear_undo_stack(v.undo_stack);
    v.unlocked = true;

    // Set up re-prompt security
    ui::SetRepromptMasterPassword(password);
    ui::ResetRepromptLockout();

    g_autosave.last_change_time = ImGui::GetTime();
    g_autosave.last_save_time = ImGui::GetTime();
    g_last_activity_time = ImGui::GetTime(); // Reset auto-lock timer

    PublishVaultOnUnlock(v);
    propagate_sync_tokens();

    // Replay any .rec snapshot left behind by a prior save-fail. Decrypts under
    // this vault's master key, so unrelated vaults' snapshots are ignored.
    restore_recovery_snapshot_if_newer(v);

    v.set_status("Vault unlocked.", false);
    return true;
}

static void complete_2fa_unlock(const std::string& code, VaultState& v)
{
    if (!v.tofa_pending) return;

    // Try TOTP first
    bool ok = twofa_ops::verify_totp(code, v.master_key);

    // If TOTP fails, try recovery code
    if (!ok)
        ok = twofa_ops::verify_recovery(code, v.master_key);

    if (!ok)
    {
        v.set_status("Invalid code. Try again.", true);
        return;
    }

    // Success — complete the unlock
    VaultMarkSaved(v);

    secure_clear_undo_stack(v.undo_stack);
    v.unlocked = true;
    v.tofa_pending = false;

    ui::SetRepromptMasterPassword(v.session_password);
    ui::ResetRepromptLockout();

    g_autosave.last_change_time = ImGui::GetTime();
    g_autosave.last_save_time = ImGui::GetTime();
    g_last_activity_time = ImGui::GetTime();

    PublishVaultOnUnlock(v);

    // Mirror load_vault_from_disk — a .rec left behind by a prior save-fail should
    // be replayed after 2FA succeeds too.
    restore_recovery_snapshot_if_newer(v);

    v.set_status("Vault unlocked.", false);
}

static bool create_new_vault_on_disk(const std::string& path, const std::string& password, VaultState& v)
{
    v.clear_status();

    if (path.empty() || password.empty())
    {
        v.set_status("Path + password required.", true);
        return false;
    }

    // Close any previously open vault
    vault_db::close();

    // Create new SQLite database
    if (!vault_db::init(path))
    {
        v.set_status("Failed to create vault database.", true);
        return false;
    }

    // Check if server has encryption salt (for cross-device sync)
    std::vector<uint8_t> salt;
    if (g_sync_mgr && g_shell.sync_logged_in) {
        if (g_sync_mgr->fetch_server_salt(salt) && salt.size() == enc::SALT_SIZE) {
            // Use server's salt for cross-device compatibility
        } else {
            salt.clear();  // Will generate new below
        }
    }

    // Generate new salt if not from server
    if (salt.empty()) {
        salt = enc::generate_salt();
    }

    if (!vault_db::set_cached_salt(salt))
    {
        vault_db::close();
        v.set_status("Failed to store encryption salt.", true);
        return false;
    }

    // Derive master key from password + salt
    bool high_sec = cfg::get_high_security_kdf();
    std::vector<uint8_t> master_key = enc::derive_master_key(password, salt, high_sec);
    if (master_key.empty())
    {
        vault_db::close();
        v.set_status("Failed to derive encryption key.", true);
        return false;
    }

    // Store KDF level in vault for future reference
    vault_db::set_sync_state("kdf_level", high_sec ? "sensitive" : "moderate");

    // Stamp key_check at creation (parity with pw-windows create_new_vault_on_disk).
    // Without this a brand-new empty vault has no stamp until the first credential is
    // saved, so the first unlock would accept ANY password — a typo'd creation
    // password then silently commits the vault to that typo on first save. Stamp now,
    // under the just-derived key, so every vault is verifiable from birth.
    if (!vault_db::set_key_check(enc::make_key_check(master_key)))
    {
        enc::secure_zero(master_key);
        vault_db::close();
        v.set_status("Failed to store password key-check.", true);
        return false;
    }

    v.vault_path = path;
    v.master_key = std::move(master_key);
    lock_key(v.master_key);            // pin in physical RAM
    v.session_password = password;     // Keep for restore operations
    lock_string(v.session_password);   // pin in physical RAM
    v.creds.clear();

    VaultMarkSaved(v);

    secure_clear_undo_stack(v.undo_stack);
    v.unlocked = true;

    // Set up re-prompt security
    ui::SetRepromptMasterPassword(password);
    ui::ResetRepromptLockout();

    g_autosave.last_change_time = ImGui::GetTime();
    g_autosave.last_save_time = ImGui::GetTime();
    g_last_activity_time = ImGui::GetTime(); // Reset auto-lock timer

    // Generate recovery key
    {
        // 32 random bytes
        std::vector<uint8_t> recovery_raw(32);
        randombytes_buf(recovery_raw.data(), 32);

        // Derive encryption key from recovery bytes via BLAKE2b
        std::vector<uint8_t> rec_enc_key(32);
        crypto_generichash(rec_enc_key.data(), 32,
                           recovery_raw.data(), 32, nullptr, 0);

        // Encrypt master key (hex-encoded) with recovery encryption key
        std::string mk_hex = enc::bytes_to_hex(v.master_key);
        auto blob = enc::encrypt_credential(mk_hex, rec_enc_key, "vault-recovery");
        enc::secure_zero(mk_hex);
        enc::secure_zero(rec_enc_key);

        // Store encrypted blob
        vault_db::set_recovery_blob(blob, helpers::now_unix_ms());

        // Format recovery key for display: xxxx-xxxx-... (16 groups of 4 hex)
        std::string hex = enc::bytes_to_hex(recovery_raw);
        enc::secure_zero(recovery_raw);
        std::string display;
        for (size_t i = 0; i < hex.size(); i++) {
            if (i > 0 && i % 4 == 0) display += '-';
            display += hex[i];
        }
        enc::secure_zero(hex);

        // Signal UI to show modal
        g_shell.recovery_key_display = std::move(display);
        g_shell.recovery_key_modal_open = true;
    }

    v.set_status("New vault created.", false);
    return true;
}

// Autolock-save-failed telemetry per vault_path. When write_recovery_snapshot_file
// stashes a .rec, we remember both the fail-flag and the filename so a future pass
// (step 5: restore_recovery_snapshot_if_newer) can clean up after a successful
// post-restart save. Vault_path keyed so multi-tab usage keeps them separate.
static std::unordered_map<std::string, bool>        g_autolock_save_failed_by_vault;
static std::unordered_map<std::string, std::string> g_autolock_recovery_file_by_vault;

// Synchronous recovery-snapshot write. Called ONLY on save failure, from the main
// thread — intentionally not detached: a backgrounded writer would race vault_db::close()
// on the shared g_db handle (fixed-bug invariant #1 from the data-safety batch).
//
// Serializes the current in-memory creds via cred_ops::to_json (invariant #2: same
// field list as the DB blob — new fields propagate automatically), wraps with uuid +
// timestamps that the snapshot needs on restore, encrypts under the master key with
// AAD "recovery_snapshot" (dedicated so the blob can't be swapped with any other),
// and writes to the per-user recovery directory.
static bool write_recovery_snapshot_file(
    const std::vector<Credential>& creds,
    const std::unordered_set<std::string>& user_removed_uuids,
    const std::vector<uint8_t>& master_key,
    const std::string& v_key)
{
    try {
        std::string rec_dir_u8 = cfg::get_recovery_directory_path();
        if (rec_dir_u8.empty()) return false;
        std::filesystem::path rec_dir = helpers::U8Path(rec_dir_u8);
        std::error_code ec;
        std::filesystem::create_directories(rec_dir, ec);

        int64_t now_ms = helpers::now_unix_ms();
        std::string filename = "vault_recovery_" + std::to_string(now_ms) + ".rec";
        std::filesystem::path rec_file = rec_dir / filename;

        nlohmann::json j;
        nlohmann::json creds_arr = nlohmann::json::array();
        for (const auto& c : creds) {
            nlohmann::json cj = cred_ops::to_json(c);
            cj["uuid"]       = c.uuid;
            cj["created_at"] = c.created_at_ms;
            cj["updated_at"] = c.updated_at_ms;
            cj["deleted_at"] = c.deleted_at_ms;
            creds_arr.push_back(std::move(cj));
        }
        j["creds"] = creds_arr;
        nlohmann::json removed_arr = nlohmann::json::array();
        for (const auto& u : user_removed_uuids) removed_arr.push_back(u);
        j["user_removed_uuids"] = removed_arr;

        std::string serialized = j.dump();
        auto enc_blob = enc::encrypt_credential(serialized, master_key, "recovery_snapshot");
        sodium_memzero(&serialized[0], serialized.size());

        if (!enc_blob.empty()) {
            const std::string rec_file_u8 = helpers::PathToU8(rec_file);
            if (helpers::bytes_to_file(rec_file_u8.c_str(), enc_blob)) {
                g_autolock_save_failed_by_vault[v_key] = true;
                g_autolock_recovery_file_by_vault[v_key] = rec_file_u8;
                return true;
            }
        }
    }
    catch (...) {}
    return false;
}

static bool save_vault_to_disk(VaultState& v)
{
    v.clear_status();

    if (!v.unlocked)
    {
        v.set_status("Vault is locked.", true);
        return false;
    }

    if (v.vault_path.empty() || v.master_key.empty())
    {
        v.set_status("Missing vault path or encryption key.", true);
        return false;
    }

    // Ensure the database is open
    if (!vault_db::is_open())
    {
        if (!vault_db::init(v.vault_path))
        {
            v.set_status("Failed to open vault database.", true);
            return false;
        }
    }

    // Verify-before-overwrite + ghost-vault escape (step 3). key_check decrypt failure
    // means the key doesn't match this vault file — lock the session instead of saving.
    {
        auto kc_blob = vault_db::get_key_check();
        if (!kc_blob.empty() && !enc::verify_key_check(kc_blob, v.master_key))
        {
            ui::ShowToast("Save refused — wrong key, vault locked", ui::ToastType::Error);

            vault_db::close();
            v.clear_sensitive();
            secure_clear_credentials(v.creds);
            secure_clear_undo_stack(v.undo_stack);
            g_bridge.Lock();
            g_bridge.PushEvent(R"({"event":"vault_locked"})");
            ui::SetRepromptMasterPassword("");
            ui::ResetRepromptLockout();
            ResetScreen(g_shell, ui::Screen::Locked);
            return false;
        }
    }

    // Transactional save: inserts, updates, undo-restores, explicit hard-deletes only.
    // On any failure (including COMMIT) it rolls back and un-assigns new uuids, so
    // memory still matches the unchanged DB and the next save retries cleanly.
    const auto snap_removed = v.user_removed_uuids;
    const cred_ops::SaveResult r =
        cred_ops::save_all(v.creds, v.master_key, snap_removed, v.purged_uuids);

    if (!r.ok)
    {
        // Snapshot-on-fail: stash the in-memory vault to disk SYNCHRONOUSLY so a
        // crash/kill right after this doesn't lose the session's edits. See the
        // write_recovery_snapshot_file doc for why not a background thread.
        const std::string v_key = v.vault_path.empty() ? "default" : v.vault_path;
        write_recovery_snapshot_file(v.creds, v.user_removed_uuids, v.master_key, v_key);

        v.set_status(r.error, true);
        ui::ShowToast("Save failed", ui::ToastType::Error);
        return false;
    }

    // Committed: erase only the snapshotted removals so concurrent deletions
    // (user removed another cred while this save was in-flight) are preserved.
    for (const auto& u : snap_removed)
        v.user_removed_uuids.erase(u);

    // purged uuids are session-local; nothing further to persist. Clear once committed
    // so re-adding a formerly-purged cred (same session) can insert again.
    v.purged_uuids.clear();

    VaultMarkSaved(v);
    v.set_status("Saved.", false);
    ui::ShowToast("Vault saved", ui::ToastType::Success);
    return true;
}

static bool restore_recovery_snapshot_if_newer(VaultState& v)
{
    if (!v.unlocked || v.master_key.empty()) return false;

    std::string rec_dir_u8 = cfg::get_recovery_directory_path();
    if (rec_dir_u8.empty()) return false;
    std::filesystem::path rec_dir = helpers::U8Path(rec_dir_u8);
    std::error_code ec;
    if (!std::filesystem::exists(rec_dir, ec)) return false;

    bool restored_any = false;
    std::vector<std::filesystem::path> rec_files_to_remove;

    for (const auto& entry : std::filesystem::directory_iterator(rec_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".rec") continue;

        auto enc_blob = helpers::file_to_vec(helpers::PathToU8(entry.path()).c_str());
        if (enc_blob.empty()) continue;

        std::string plain = enc::decrypt_credential(enc_blob, v.master_key, "recovery_snapshot");
        if (plain.empty()) continue; // Not this vault's key — ownership proof

        try {
            auto j = nlohmann::json::parse(plain);
            nlohmann::json creds_arr;
            if (j.is_array()) {
                creds_arr = j;
            } else if (j.is_object() && j.contains("creds") && j["creds"].is_array()) {
                creds_arr = j["creds"];
            } else {
                continue;
            }

            for (const auto& it : creds_arr) {
                std::string u = it.value("uuid", "");
                if (u.empty()) continue;

                int64_t rec_updated = it.value("updated_at", int64_t(0));

                Credential* existing = nullptr;
                for (auto& c_item : v.creds) {
                    if (c_item.uuid == u) {
                        existing = &c_item;
                        break;
                    }
                }

                if (existing != nullptr) {
                    // Stale-recovery guard: only overwrite when the snapshot is strictly
                    // newer than the local row; a leftover .rec can't clobber a legit edit.
                    if (rec_updated > existing->updated_at_ms) {
                        cred_ops::from_json(it, *existing);
                        existing->uuid = u;
                        existing->updated_at_ms = rec_updated;
                        existing->deleted_at_ms = it.value("deleted_at", existing->deleted_at_ms);
                        // created_at_ms: keep the row's original creation time
                        restored_any = true;
                    }
                } else {
                    Credential c;
                    cred_ops::from_json(it, c);
                    c.uuid = u;
                    c.created_at_ms = it.value("created_at", helpers::now_unix_ms());
                    c.updated_at_ms = rec_updated;
                    c.deleted_at_ms = it.value("deleted_at", int64_t(0));  // trashed stays trashed
                    v.creds.push_back(std::move(c));
                    restored_any = true;
                }
            }

            if (j.is_object() && j.contains("user_removed_uuids") && j["user_removed_uuids"].is_array()) {
                for (const auto& u_val : j["user_removed_uuids"]) {
                    if (u_val.is_string())
                        v.user_removed_uuids.insert(u_val.get<std::string>());
                }
            }

            rec_files_to_remove.push_back(entry.path());
        }
        catch (...) {}
    }

    const std::string v_key = v.vault_path.empty() ? "default" : v.vault_path;

    if (restored_any) {
        VaultMarkChanged(v, "RECOVERY");
        // Delete the .rec ONLY after the restored state is committed — if the save
        // fails, we keep the snapshot and surface a toast; the next unlock will try again.
        if (save_vault_to_disk(v)) {
            for (const auto& p : rec_files_to_remove)
                std::filesystem::remove(p, ec);
            g_autolock_save_failed_by_vault[v_key] = false;
            ui::ShowToast("Recovered unsaved changes from previous session", ui::ToastType::Success);
        }
    } else if (!rec_files_to_remove.empty()) {
        // Snapshots existed but were strictly older than local — just clean them up.
        for (const auto& p : rec_files_to_remove)
            std::filesystem::remove(p, ec);
        g_autolock_save_failed_by_vault[v_key] = false;
    }

    return restored_any;
}

static void TickAutosave()
{
    VaultState& v = ActiveVault();
    if (!g_shell.autosave_enabled) return;
    if (g_autosave.suspended) return;
    if (!v.unlocked) return;
    if (!v.is_dirty()) return;

    const double now = ImGui::GetTime();

    // Debounce (~1.2s)
    if (now - g_autosave.last_change_time >= 1.2)
    {
        // Preserve user status messages if they exist (especially errors)
        const bool had_status = !v.status_msg.empty();
        const bool was_error = v.status_is_error;
        const std::string saved_status = v.status_msg;

        save_vault_to_disk(v);
        g_autosave.last_save_time = now;

        // Restore user status if it was non-empty or an error
        if (had_status && (was_error || !saved_status.empty()))
        {
            v.status_msg = saved_status;
            v.status_is_error = was_error;
        }
    }
}

// ============================================================
// Auto-lock: lock vault after inactivity
// ============================================================
static void TickAutoLock()
{
    VaultState& v = ActiveVault();
    if (!v.unlocked) return;

    const int timeout = cfg::get_auto_lock_timeout();
    if (timeout == 0) return; // "Never" - disabled

    const double now = ImGui::GetTime();
    const ImGuiIO& io = ImGui::GetIO();

    // Detect user activity: mouse movement, mouse clicks, or key presses
    bool has_activity = false;

    // Check mouse movement (with small threshold to avoid jitter)
    ImVec2 mouse_delta = ImVec2(io.MousePos.x - g_last_mouse_pos.x, io.MousePos.y - g_last_mouse_pos.y);
    if (fabsf(mouse_delta.x) > 2.0f || fabsf(mouse_delta.y) > 2.0f)
        has_activity = true;
    g_last_mouse_pos = io.MousePos;

    // Check mouse clicks
    for (int i = 0; i < IM_ARRAYSIZE(io.MouseDown); ++i)
        if (io.MouseDown[i]) has_activity = true;

    // Check keyboard input via input queue
    if (io.InputQueueCharacters.Size > 0)
        has_activity = true;

    // Check any key press
    for (int i = ImGuiKey_NamedKey_BEGIN; i < ImGuiKey_NamedKey_END; ++i)
        if (ImGui::IsKeyDown((ImGuiKey)i)) has_activity = true;

    // Reset timer on activity
    if (has_activity)
    {
        g_last_activity_time = now;
        return;
    }

    // Initialize timer if not set
    if (g_last_activity_time == 0.0)
    {
        g_last_activity_time = now;
        return;
    }

    // Check if timeout exceeded
    if (now - g_last_activity_time >= (double)timeout)
    {
        // Lock the vault
        g_shell.footer_close_anyway = true;
        g_last_activity_time = now; // Reset to avoid repeated triggers
        ui::ShowToast("Vault locked due to inactivity", ui::ToastType::Info);
    }
}

// ============================================================
// Credential expiry: auto-trash or flag expired timed credentials
// ============================================================
static void TickCredentialExpiry()
{
    VaultState& v = ActiveVault();
    if (!v.unlocked) return;

    // Throttle: check once per second
    static double s_last_expiry_check = 0.0;
    const double now_sec = ImGui::GetTime();
    if (now_sec - s_last_expiry_check < 1.0) return;
    s_last_expiry_check = now_sec;

    const int64_t now_ms = helpers::now_unix_ms();
    bool changed = false;

    for (auto& c : v.creds) {
        if (c.expires_at_ms <= 0) continue;           // no timer or already flagged
        if (c.is_deleted()) continue;                  // already in trash
        if (now_ms < c.expires_at_ms) continue;        // not yet expired

        if (!changed) v.push_undo();                   // one undo snapshot per tick
        changed = true;

        if (c.expiry_action == 0) {
            // Auto-trash
            c.deleted_at_ms = now_ms;
            c.updated_at_ms = now_ms;
            char toast[256];
            snprintf(toast, sizeof(toast), "Timed credential trashed: %s", DisplayTitle(c.title));
            ui::ShowToast(toast, ui::ToastType::Info);
        } else {
            // Flag only: negate expires_at_ms to mark as expired
            c.expires_at_ms = -c.expires_at_ms;
            c.updated_at_ms = now_ms;
            char toast[256];
            snprintf(toast, sizeof(toast), "Credential expired: %s", DisplayTitle(c.title));
            ui::ShowToast(toast, ui::ToastType::Error);
        }
    }

    if (changed) {
        VaultMarkChanged(v, "EXPIRY");
        g_autosave.last_change_time = ImGui::GetTime();
    }
}

static void do_undo(VaultState& v)
{
    v.clear_status();

    if (!v.unlocked)
    {
        v.set_status("Vault is locked.", true);
        return;
    }

    if (v.undo_stack.empty())
    {
        v.set_status("Nothing to undo.", true);
        return;
    }

    const std::string snap = v.undo_stack.back();
    v.undo_stack.pop_back();

    // Snapshot "before" so reconcile can tell what the undo removed vs restored
    const std::vector<Credential> before = v.creds;

    v.apply_snapshot_json(snap);

    // Drop any uuid the user purged this session (so Empty-Trash-then-Undo doesn't
    // visibly resurrect ghosts the next save would silently drop), and keep the
    // user_removed ledger in sync with what the restored memory actually contains.
    cred_ops::reconcile_after_undo(before, v.creds, v.user_removed_uuids, v.purged_uuids);

    // Important: reset per-row UI state for THIS vault after big changes
    ui::ForgetVaultRowState(GetActiveVaultKey());

    g_autosave.last_change_time = ImGui::GetTime();

    VaultRecomputeDirty(v);

    v.set_status("Undo applied.", false);
}

static void render_locked_screen()
{
    VaultState& v = ActiveVault();

    // ---- 2FA pending screen ----
    if (v.tofa_pending)
    {
        static std::string s_tofa_code;

        render::BeginCenteredColumn("##tofa_col", 400.0f);
        ImGui::Dummy(ImVec2(0, 80));
        render::BeginSoftContainer(20.0f, 10.0f);

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::PushFont(render::FontLarge);
        ImGui::TextUnformatted("Vault Two-Factor Authentication");
        ImGui::PopFont();

        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextWrapped("Enter the 6-digit code from your authenticator app, or a recovery code.");
        ImGui::Dummy(ImVec2(0, 8));

        ImGui::SetNextItemWidth(-1);
        bool enter = ui::InputTextString(
            "##tofa_input", &s_tofa_code,
            ImGuiInputTextFlags_EnterReturnsTrue
        );

        ImGui::Dummy(ImVec2(0, 6));

        bool canVerify = !s_tofa_code.empty();
        float btnW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

        if (!canVerify) ImGui::BeginDisabled();
        bool verify = ui::StyledButton("##tofa_verify", "Verify", ImVec2(btnW, 34.0f));
        if (!canVerify) ImGui::EndDisabled();

        ImGui::SameLine();

        if (ui::StyledButton("##tofa_cancel", "Cancel", ImVec2(btnW, 34.0f)))
        {
            s_tofa_code.clear();
            vault_db::close();
            v.clear_sensitive();
            secure_clear_credentials(v.creds);
            secure_clear_undo_stack(v.undo_stack);
            g_bridge.Lock();
            g_bridge.PushEvent(R"({"event":"vault_locked"})");
            v.vault_path.clear();
        }

        if ((enter && canVerify) || verify)
        {
            complete_2fa_unlock(s_tofa_code, v);
            if (v.unlocked)
            {
                s_tofa_code.clear();
                rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);
                ui::ForgetVaultRowState(GetActiveVaultKey());
            }
        }

        // Status
        if (!v.status_msg.empty())
        {
            ImGui::Dummy(ImVec2(0, 4));
            if (v.status_is_error) ImGui::TextColored(colors::Red, "%s", v.status_msg.c_str());
            else                   ImGui::TextColored(colors::Green, "%s", v.status_msg.c_str());
        }

        ImGui::Dummy(ImVec2(0, 8));
        render::EndSoftContainer();
        render::EndCenteredColumn();
        return;
    }

    // per-tab buffers
    static std::vector<std::string> s_pw;
    static std::vector<std::string> s_cached_vaults;
    static std::vector<std::pair<std::string, int>> s_sections;  // {label, start_idx into s_cached_vaults}
    static int s_selected_vault_idx = -1;  // >=0 = local vault, <0 = cloud vault (-(idx+1))
    static std::string s_search_filter;

    auto refresh_vault_discovery = []() {
        auto d = discover_vaults_grouped();
        s_cached_vaults = std::move(d.paths);
        s_sections      = std::move(d.sections);
    };

    // Cloud vault cache
    static std::vector<SyncManager::CloudVaultInfo> s_cloud_vaults;
    static std::unordered_map<std::string, std::string> s_slug_to_path; // slug -> local .db path
    static bool s_cloud_vaults_fetched = false;

    if ((int)s_pw.size() < (int)g_tabs.size())
        s_pw.resize(g_tabs.size());

    const int ti = ImClamp(g_active_tab, 0, (int)g_tabs.size() - 1);

    // Narrow, centered "picker" column
    render::BeginCenteredColumn("##locked_col", 450.0f);

    // Vertical centering offset (push content down a bit)
    ImGui::Dummy(ImVec2(0, 50));

    // Soft container: lifted surface with subtle shadow
    render::BeginSoftContainer(20.0f, 10.0f);

    // Save-fail recovery banner. Set by write_recovery_snapshot_file when an
    // autosave/lock-time save fails and a .rec snapshot was stashed. Tells the
    // user the data isn't lost — restore_recovery_snapshot_if_newer will replay
    // the snapshot on unlock. Cleared once the restore commits.
    {
        const std::string cur_v_key = v.vault_path.empty() ? "default" : v.vault_path;
        auto it = g_autolock_save_failed_by_vault.find(cur_v_key);
        if (it != g_autolock_save_failed_by_vault.end() && it->second) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.2f, 1.0f));  // orange warning
            ImGui::TextWrapped("Auto-save failed last session. An encrypted recovery snapshot was created and will be restored when you unlock.");
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 6));
        }
    }

    // Cache vault list
    if (s_cached_vaults.empty())
        refresh_vault_discovery();

    // Fetch cloud vaults (once per refresh cycle)
    if (!s_cloud_vaults_fetched && g_sync_mgr && g_shell.sync_logged_in) {
        auto result = g_sync_mgr->list_cloud_vaults();
        if (result.success)
            s_cloud_vaults = std::move(result.vaults);
        else
            s_cloud_vaults.clear();

        // Build slug-to-path map by scanning local .db files
        s_slug_to_path.clear();
        for (const auto& db_path : s_cached_vaults) {
            std::string slug = vault_db::read_vault_slug_from_file(db_path);
            if (!slug.empty())
                s_slug_to_path[slug] = db_path;
        }

        s_cloud_vaults_fetched = true;
    }

    // Auto-select last opened vault (one-shot on first render)
    {
        static bool s_auto_open_done = false;
        if (!s_auto_open_done)
        {
            s_auto_open_done = true;
            if (g_shell.auto_open_vault
                && cfg::_path != "NONE" && !cfg::_path.empty()
                && helpers::file_exists(cfg::_path))
            {
                v.vault_path = cfg::_path;
                for (int i = 0; i < (int)s_cached_vaults.size(); i++)
                {
                    if (s_cached_vaults[i] == cfg::_path)
                    {
                        s_selected_vault_idx = i;
                        break;
                    }
                }
            }
        }
    }

    // ===========================================================
    // HEADER: Title on its own line, then Search + icons row
    // ===========================================================
    ImGui::Dummy(ImVec2(0, 8));
    {
        char headerBuf[128];
        snprintf(headerBuf, sizeof(headerBuf), "Vaults (%d)", (int)s_cached_vaults.size());
        ImGui::PushFont(render::FontRegular);
        ImGui::TextUnformatted(headerBuf);
        ImGui::PopFont();

        // Search + Open-From-Folder + Refresh + Add row
        const float icon = 34.0f; // match input field height
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float reservedRight = (icon * 3.0f) + (gap * 3.0f);

        float avail = ImGui::GetContentRegionAvail().x;
        float searchW = ImMax(140.0f, avail - reservedRight);

        ImGui::SetNextItemWidth(searchW);
        ui::InputTextString("Search in this folder", &s_search_filter);
        ImGui::SameLine();

        // Align buttons with the field box (skip floating label area)
        float labelAreaH = ImGui::GetFontSize() * 0.82f + 2.0f;
        float btnY = ImGui::GetCursorPosY() + labelAreaH;
        ImGui::SetCursorPosY(btnY);

        // Refresh list
        if (ui::IconButtonSquare("refresh_list", ICON_MDI_REFRESH, icon, true))
        {
            refresh_vault_discovery();
            s_selected_vault_idx = -1;
            s_cloud_vaults_fetched = false;
            s_slug_to_path.clear();
        }
        if (ImGui::IsItemHovered()) { ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6)); ImGui::BeginTooltip(); ImGui::TextUnformatted("Refresh vault list"); ImGui::EndTooltip(); ImGui::PopStyleVar(); }

        ImGui::SameLine();
        ImGui::SetCursorPosY(btnY);

        // Open vault from any folder (native file picker)
        if (ui::IconButtonSquare("open_from_folder", ICON_MDI_FOLDER_OPEN, icon, true))
        {
            std::string picked = PickOpenFilePath_DB();
            if (!picked.empty())
            {
                v.vault_path = picked;

                int found = -1;
                for (int i = 0; i < (int)s_cached_vaults.size(); i++)
                {
                    if (s_cached_vaults[i] == picked) { found = i; break; }
                }
                if (found < 0)
                {
                    // Append to a trailing "Other" section so the picked file shows up
                    found = (int)s_cached_vaults.size();
                    s_cached_vaults.push_back(picked);
                    if (s_sections.empty() || s_sections.back().first != "Other")
                        s_sections.push_back({ "Other", found });
                }
                s_selected_vault_idx = found;
                s_search_filter.clear();
            }
        }
        if (ImGui::IsItemHovered()) { ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6)); ImGui::BeginTooltip(); ImGui::TextUnformatted("Open vault from folder"); ImGui::EndTooltip(); ImGui::PopStyleVar(); }

        ImGui::SameLine();
        ImGui::SetCursorPosY(btnY);

        // Add vault (create new — cloud-aware when logged in)
        if (ui::IconButtonSquare("add_tolist", ICON_MDI_FILE_PLUS, icon, true))
        {
            if (s_pw[ti].empty())
            {
                v.set_status("Enter a password to create a new vault.", true);
            }
            else if (g_sync_mgr && g_shell.sync_logged_in)
            {
                // Cloud vault creation
                auto salt = enc::generate_salt();
                std::string salt_b64 = Anonbase64_encode(salt);
                std::string slug = "vault-" + std::to_string(helpers::now_unix_ms());
                std::string name = slug; // default name = slug

                auto cr = g_sync_mgr->create_cloud_vault(slug, name, salt_b64);
                if (cr.success)
                {
                    v.vault_path = new_db_path();

                    // Close any previously open vault and create the .db
                    vault_db::close();
                    if (vault_db::init(v.vault_path))
                    {
                        vault_db::set_vault_slug(cr.created_slug);
                        vault_db::set_cached_salt(salt);

                        bool high_sec = cfg::get_high_security_kdf();
                        auto master_key = enc::derive_master_key(s_pw[ti], salt, high_sec);
                        if (!master_key.empty())
                        {
                            vault_db::set_sync_state("kdf_level", high_sec ? "sensitive" : "moderate");
                            v.master_key = std::move(master_key);
                            lock_key(v.master_key);
                            v.session_password = s_pw[ti];
                            lock_string(v.session_password);
                            v.creds.clear();
                            VaultMarkSaved(v);
                            secure_clear_undo_stack(v.undo_stack);
                            v.unlocked = true;

                            ui::SetRepromptMasterPassword(s_pw[ti]);
                            ui::ResetRepromptLockout();
                            g_autosave.last_change_time = ImGui::GetTime();
                            g_autosave.last_save_time = ImGui::GetTime();
                            g_last_activity_time = ImGui::GetTime();

                            g_sync_mgr->save_tokens_to_db();
                            PublishVaultOnUnlock(v);

                            sodium_memzero(s_pw[ti].data(), s_pw[ti].size());
                            s_pw[ti].clear();

                            rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);
                            ui::ForgetVaultRowState(GetActiveVaultKey());

                            v.set_status("Cloud vault created.", false);
                        }
                        else
                        {
                            vault_db::close();
                            v.set_status("Failed to derive encryption key.", true);
                        }
                    }
                    else
                    {
                        v.set_status("Failed to create vault database.", true);
                    }
                }
                else
                {
                    v.set_status(cr.message.empty() ? "Failed to create cloud vault." : cr.message.c_str(), true);
                }

                refresh_vault_discovery();
                s_selected_vault_idx = -1;
                s_cloud_vaults_fetched = false;
                s_slug_to_path.clear();
            }
            else
            {
                // Local-only vault creation (existing flow)
                v.vault_path = new_db_path();
                create_new_vault_on_disk(v.vault_path, s_pw[ti], v);

                if (v.unlocked)
                {
                    sodium_memzero(s_pw[ti].data(), s_pw[ti].size());
                    s_pw[ti].clear();

                    rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);
                    ui::ForgetVaultRowState(GetActiveVaultKey());
                }

                refresh_vault_discovery();
                s_selected_vault_idx = -1;
                s_cloud_vaults_fetched = false;
                s_slug_to_path.clear();
            }
        }
        if (ImGui::IsItemHovered()) { ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6)); ImGui::BeginTooltip(); ImGui::TextUnformatted("Create new vault"); ImGui::EndTooltip(); ImGui::PopStyleVar(); }
    }

    ImGui::Dummy(ImVec2(0, 4)); // tighter than 6/8

    // ===========================================================
    // FILTER VAULTS (case-insensitive)
    // ===========================================================
    std::vector<int> filtered_indices;
    filtered_indices.reserve(s_cached_vaults.size());

    for (int i = 0; i < (int)s_cached_vaults.size(); i++)
    {
        std::string filename = helpers::Basename(s_cached_vaults[i]);

        if (s_search_filter.empty() ||
            ImStristr(filename.c_str(), nullptr, s_search_filter.c_str(), nullptr))
        {
            filtered_indices.push_back(i);
        }
    }

    // ===========================================================
    // COMPACT HEIGHT RULES
    // ===========================================================
    const float footerH = 68.0f;     // taller to fit outlined input + label area
    const float statusH = (!v.status_msg.empty()) ? 20.0f : 0.0f;

    // List should NOT expand forever — cap it so screen feels "picker"
    float listAvail = ImGui::GetContentRegionAvail().y - footerH - statusH - 8.0f;
    float listH = ImClamp(listAvail, 160.0f, 215.0f); // <-- compact cap

    // Smaller rows = denser list
    const float rowH = 32.0f;

    // ===========================================================
    // VAULT LIST (compact)
    // ===========================================================
    ImGui::BeginChild("##vault_list", ImVec2(-1, listH));

    if (filtered_indices.empty())
    {
        float h = ImGui::GetContentRegionAvail().y;
        ImGui::Dummy(ImVec2(0, h * 0.35f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));

        const char* emptyMsg = s_cached_vaults.empty()
            ? "No vaults found in this folder."
            : "No vaults match search.";

        float msgW = ImGui::CalcTextSize(emptyMsg).x;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - msgW) * 0.5f);
        ImGui::TextUnformatted(emptyMsg);

        if (s_cached_vaults.empty())
        {
            ImGui::Dummy(ImVec2(0, 4));
            const char* hint = "Enter a password and click " ICON_MDI_FILE_PLUS " to create one";
            float hintW = ImGui::CalcTextSize(hint).x;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - hintW) * 0.5f);
            ImGui::PushFont(render::FontSmall);
            ImGui::TextUnformatted(hint);
            ImGui::PopFont();
        }

        ImGui::PopStyleColor();
    }
    else
    {
        // Render each section with a left-aligned dim header, iterating section bounds
        // and skipping headers whose rows are all filtered out.
        std::unordered_set<int> filter_set(filtered_indices.begin(), filtered_indices.end());

        auto render_vault_row = [&](int vaultIdx) {
            const std::string& vaultPath = s_cached_vaults[vaultIdx];
            std::string filename = helpers::Basename(vaultPath);
            bool is_selected = (s_selected_vault_idx == vaultIdx);

            if (ImGui::Selectable2(filename.c_str(), is_selected, 0, ImVec2(0, rowH)))
            {
                s_selected_vault_idx = vaultIdx;
                v.vault_path = vaultPath;
            }

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary))
                ui::SetTooltipPadded("%s", vaultPath.c_str());

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
            {
                s_selected_vault_idx = vaultIdx;
                v.vault_path = vaultPath;

                if (!s_pw[ti].empty())
                {
                    load_vault_from_disk(v.vault_path, s_pw[ti], v);
                    if (v.unlocked)
                    {
                        sodium_memzero(s_pw[ti].data(), s_pw[ti].size());
                        s_pw[ti].clear();

                        cfg::_path = v.vault_path;
                        cfg::update_db_path();

                        rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);
                        ui::ForgetVaultRowState(GetActiveVaultKey());
                    }
                }
                else
                {
                    v.set_status("Enter password to unlock.", true);
                }
            }
        };

        // Fallback: if no sections (shouldn't happen when list is non-empty) render flat
        if (s_sections.empty())
        {
            for (int vaultIdx : filtered_indices) render_vault_row(vaultIdx);
        }
        else
        {
            bool first_section_rendered = false;
            for (size_t si = 0; si < s_sections.size(); si++)
            {
                int start = s_sections[si].second;
                int end   = (si + 1 < s_sections.size()) ? s_sections[si + 1].second : (int)s_cached_vaults.size();

                // Collect visible rows for this section
                std::vector<int> rows;
                for (int idx = start; idx < end; idx++)
                    if (filter_set.count(idx)) rows.push_back(idx);
                if (rows.empty()) continue;

                if (first_section_rendered) ImGui::Dummy(ImVec2(0, 4));
                first_section_rendered = true;

                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
                ImGui::TextUnformatted(s_sections[si].first.c_str());
                ImGui::PopStyleColor();

                for (int vaultIdx : rows) render_vault_row(vaultIdx);
            }
        }
    }

    // Cloud vaults section (only when logged in and have cloud vaults)
    if (!s_cloud_vaults.empty())
    {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(ICON_MDI_CLOUD " Cloud Vaults");
        ImGui::PopStyleColor();
        ImGui::Separator();

        for (int ci = 0; ci < (int)s_cloud_vaults.size(); ci++)
        {
            const auto& cv = s_cloud_vaults[ci];

            // Filter by search
            if (!s_search_filter.empty() &&
                !ImStristr(cv.vault_name.c_str(), nullptr, s_search_filter.c_str(), nullptr))
                continue;

            // Encode cloud index as negative: -(ci+1)
            int cloud_sel_idx = -(ci + 1);
            bool is_selected = (s_selected_vault_idx == cloud_sel_idx);

            // Check if this cloud vault has a local .db
            auto it = s_slug_to_path.find(cv.vault_slug);
            bool has_local = (it != s_slug_to_path.end());

            std::string label = ICON_MDI_CLOUD " " + cv.vault_name + "##cloud_" + std::to_string(ci);

            if (ImGui::Selectable2(label.c_str(), is_selected, 0, ImVec2(0, rowH)))
            {
                s_selected_vault_idx = cloud_sel_idx;
                if (has_local) {
                    v.vault_path = it->second;
                } else {
                    // Will be created on unlock (Part 7)
                    v.vault_path.clear();
                }
            }

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary)) {
                if (has_local)
                    ImGui::SetTooltip("%s\n(%s)", cv.vault_name.c_str(), it->second.c_str());
                else
                    ImGui::SetTooltip("%s (cloud only — will create local file on unlock)", cv.vault_name.c_str());
            }

            // Double-click unlock for cloud vaults with local .db
            if (has_local && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
            {
                s_selected_vault_idx = cloud_sel_idx;
                v.vault_path = it->second;

                if (!s_pw[ti].empty())
                {
                    load_vault_from_disk(v.vault_path, s_pw[ti], v);
                    if (v.unlocked)
                    {
                        sodium_memzero(s_pw[ti].data(), s_pw[ti].size());
                        s_pw[ti].clear();

                        cfg::_path = v.vault_path;
                        cfg::update_db_path();

                        rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);
                        ui::ForgetVaultRowState(GetActiveVaultKey());
                    }
                }
                else
                {
                    v.set_status("Enter password to unlock.", true);
                }
            }
        }
    }

    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, 6)); // tight spacing between list and footer

    // ===========================================================
    // FOOTER: Password + Unlock (compact + attached)
    // ===========================================================
    {
        bool hasVault = !v.vault_path.empty();
        bool hasCloudVault = (s_selected_vault_idx < 0); // cloud vault selected (may not have local .db yet)
        bool hasPw = !s_pw[ti].empty();
        bool canUnlock = (hasVault || hasCloudVault) && hasPw;

        static bool s_show_pw = false;

        const float icon = 34.0f; // match input field height
        const float gap = ImGui::GetStyle().ItemSpacing.x;

        // Keep input tight inside column (reserve space for eye + unlock icons)
        float avail = ImGui::GetContentRegionAvail().x;
        float pwW = ImMax(140.0f, avail - (icon * 2.0f) - (gap * 2.0f));

        ImGui::SetNextItemWidth(pwW);
        bool enterPressed;
        if (s_show_pw)
        {
            enterPressed = ui::InputTextString(
                "Vault Password##vault_pw",
                &s_pw[ti],
                ImGuiInputTextFlags_EnterReturnsTrue
            );
        }
        else
        {
            enterPressed = ui::InputTextPasswordReveal(
                "Vault Password##vault_pw",
                &s_pw[ti],
                ImGuiInputTextFlags_EnterReturnsTrue
            );
        }

        ImGui::SameLine();

        // Align buttons with the field box (skip floating label area)
        float labelAreaH = ImGui::GetFontSize() * 0.82f + 2.0f;
        float btnY = ImGui::GetCursorPosY() + labelAreaH;
        ImGui::SetCursorPosY(btnY);

        if (ui::IconButtonSquare("show_pw", s_show_pw ? ICON_MDI_EYE_OFF : ICON_MDI_EYE, icon, true))
            s_show_pw = !s_show_pw;

        ImGui::SameLine();
        ImGui::SetCursorPosY(btnY);

        if (!canUnlock) ImGui::BeginDisabled();
        bool clickUnlock = ui::IconButtonSquare("unlock_vault", ICON_MDI_LOCK_OPEN, icon, true);
        if (!canUnlock) ImGui::EndDisabled();

        if ((enterPressed && canUnlock) || clickUnlock)
        {
            if (hasCloudVault && !hasVault)
            {
                // First-open of cloud vault with no local .db
                int ci = -(s_selected_vault_idx + 1);
                if (ci >= 0 && ci < (int)s_cloud_vaults.size())
                {
                    const auto& cv = s_cloud_vaults[ci];

                    // Decode the per-vault salt
                    auto vault_salt = base64_decode(cv.encryption_salt_b64);
                    if (vault_salt.size() != enc::SALT_SIZE) {
                        v.set_status("Cloud vault has invalid encryption salt.", true);
                    }
                    else
                    {
                        v.vault_path = new_db_path();
                        vault_db::close();

                        if (vault_db::init(v.vault_path))
                        {
                            vault_db::set_vault_slug(cv.vault_slug);
                            vault_db::set_cached_salt(vault_salt);

                            bool high_sec = cfg::get_high_security_kdf();
                            auto master_key = enc::derive_master_key(s_pw[ti], vault_salt, high_sec);
                            if (!master_key.empty())
                            {
                                vault_db::set_sync_state("kdf_level", high_sec ? "sensitive" : "moderate");
                                v.master_key = std::move(master_key);
                                lock_key(v.master_key);
                                v.session_password = s_pw[ti];
                                lock_string(v.session_password);
                                v.creds.clear();
                                VaultMarkSaved(v);
                                secure_clear_undo_stack(v.undo_stack);
                                v.unlocked = true;

                                ui::SetRepromptMasterPassword(s_pw[ti]);
                                ui::ResetRepromptLockout();
                                g_autosave.last_change_time = ImGui::GetTime();
                                g_autosave.last_save_time = ImGui::GetTime();
                                g_last_activity_time = ImGui::GetTime();

                                g_sync_mgr->save_tokens_to_db();
                                PublishVaultOnUnlock(v);

                                // Pull existing credentials from server
                                g_sync_mgr->start_background_sync();

                                sodium_memzero(s_pw[ti].data(), s_pw[ti].size());
                                s_pw[ti].clear();

                                cfg::_path = v.vault_path;
                                cfg::update_db_path();

                                rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);
                                ui::ForgetVaultRowState(GetActiveVaultKey());

                                // Refresh lists to pick up new local .db
                                refresh_vault_discovery();
                                s_cloud_vaults_fetched = false;
                                s_slug_to_path.clear();

                                v.set_status("Cloud vault opened. Syncing...", false);
                            }
                            else
                            {
                                vault_db::close();
                                v.set_status("Failed to derive encryption key.", true);
                            }
                        }
                        else
                        {
                            v.set_status("Failed to create vault database.", true);
                        }
                    }
                }
            }
            else if (!v.vault_path.empty())
            {
                load_vault_from_disk(v.vault_path, s_pw[ti], v);
                if (v.unlocked)
                {
                    // Clear password from input buffer (no longer needed)
                    sodium_memzero(s_pw[ti].data(), s_pw[ti].size());
                    s_pw[ti].clear();

                    // Remember last opened vault
                    cfg::_path = v.vault_path;
                    cfg::update_db_path();

                    rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);
                    ui::ForgetVaultRowState(GetActiveVaultKey());
                }
            }
            else
            {
                v.set_status("Please select a vault first.", true);
            }
        }

        if (!hasVault && !hasCloudVault && ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary))
            ImGui::SetTooltip("Select a vault first.");
        if ((hasVault || hasCloudVault) && !hasPw && ImGui::IsItemHovered(ImGuiHoveredFlags_Stationary))
            ImGui::SetTooltip("Enter your password.");
    }

    // ===========================================================
    // Recovery key unlock option
    // ===========================================================
    {
        static bool s_show_recovery = false;
        static std::string s_recovery_input;

        // Check if vault has a recovery key
        bool vault_has_recovery = false;
        if (!v.vault_path.empty()) {
            if (vault_db::is_open())
                vault_has_recovery = vault_db::has_recovery_key();
            else if (vault_db::exists(v.vault_path) && vault_db::init(v.vault_path)) {
                vault_has_recovery = vault_db::has_recovery_key();
                vault_db::close();
            }
        }

        if (vault_has_recovery)
        {
            ImGui::Dummy(ImVec2(0, 4));

            {
                const char* linkLabel = s_show_recovery ? "Use password instead" : "Use Recovery Key";
                const char* chevron = " " ICON_MDI_CHEVRON_DOWN;
                ImVec2 textSz = ImGui::CalcTextSize(linkLabel);
                ImVec2 chevSz = ImGui::CalcTextSize(chevron);
                ImVec2 totalSz(textSz.x + chevSz.x, textSz.y);

                ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##recovery_link", totalSz);
                bool clicked = ImGui::IsItemClicked();
                bool hovered = ImGui::IsItemHovered();

                ImU32 col = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddText(pos, col, linkLabel);
                dl->AddText(ImVec2(pos.x + textSz.x, pos.y), col, chevron);

                if (hovered)
                {
                    float y = pos.y + textSz.y + 1.0f;
                    dl->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + textSz.x, y), col, 1.0f);
                }

                if (clicked)
                {
                    s_show_recovery = !s_show_recovery;
                    s_recovery_input.clear();
                }
            }

            if (s_show_recovery)
            {
                ImGui::Dummy(ImVec2(0, 4));
                float btnW = 75.0f;
                float gap = ImGui::GetStyle().ItemSpacing.x;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btnW - gap);
                bool enterRec = ui::InputTextString(
                    "Recovery Key##rec_input", &s_recovery_input,
                    ImGuiInputTextFlags_EnterReturnsTrue);

                ImGui::SameLine();
                // Align button with the field box (skip floating label area)
                float labelAreaH = ImGui::GetFontSize() * 0.82f + 2.0f;
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + labelAreaH);

                bool canRecover = !s_recovery_input.empty() && !v.vault_path.empty();
                if (!canRecover) ImGui::BeginDisabled();
                bool clickRecover = ui::StyledButton("##recover_btn", "Recover", ImVec2(btnW, 34.0f));
                if (!canRecover) ImGui::EndDisabled();

                if ((enterRec && canRecover) || clickRecover)
                {
                    // Parse input: strip dashes/spaces, hex-decode
                    std::string clean;
                    for (char ch : s_recovery_input)
                        if (ch != '-' && ch != ' ') clean += ch;

                    std::vector<uint8_t> recovery_raw = enc::hex_to_bytes(clean);
                    enc::secure_zero(clean);

                    if (recovery_raw.size() == 32)
                    {
                        // Open DB if needed
                        if (!vault_db::is_open())
                            vault_db::init(v.vault_path);

                        // Derive encryption key
                        std::vector<uint8_t> rec_enc_key(32);
                        crypto_generichash(rec_enc_key.data(), 32,
                                           recovery_raw.data(), 32, nullptr, 0);
                        enc::secure_zero(recovery_raw);

                        // Decrypt recovery blob
                        auto blob = vault_db::get_recovery_blob();
                        std::string mk_hex = enc::decrypt_credential(blob, rec_enc_key, "vault-recovery");
                        enc::secure_zero(rec_enc_key);

                        if (!mk_hex.empty())
                        {
                            std::vector<uint8_t> master_key = enc::hex_to_bytes(mk_hex);
                            enc::secure_zero(mk_hex);

                            // Load credentials with recovered master key
                            auto creds = cred_ops::load_all(master_key);

                            v.master_key = std::move(master_key);
                            lock_key(v.master_key);            // pin in physical RAM
                            v.session_password.clear(); // no password known
                            v.creds = std::move(creds);
                            v.tofa_pending = false; // recovery bypasses 2FA

                            // Auto-purge trash
                            int days = cfg::get_trash_retention_days();
                            if (days > 0) {
                                int64_t cutoff = helpers::now_unix_ms() - (int64_t)days * 86400000LL;
                                vault_db::purge_old_tombstones(cutoff);
                            }

                            VaultMarkSaved(v);
                            secure_clear_undo_stack(v.undo_stack);
                            v.unlocked = true;

                            ui::SetRepromptMasterPassword("");
                            ui::ResetRepromptLockout();

                            g_autosave.last_change_time = ImGui::GetTime();
                            g_autosave.last_save_time = ImGui::GetTime();
                            g_last_activity_time = ImGui::GetTime();

                            rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);
                            ui::ForgetVaultRowState(GetActiveVaultKey());

                            s_recovery_input.clear();
                            s_show_recovery = false;
                            v.set_status("Vault recovered.", false);
                            ui::ShowToast("Vault unlocked via recovery key", ui::ToastType::Success);
                        }
                        else
                        {
                            v.set_status("Invalid recovery key.", true);
                        }
                    }
                    else
                    {
                        enc::secure_zero(recovery_raw);
                        v.set_status("Invalid recovery key format.", true);
                    }
                }
            }
        }
    }

    // ===========================================================
    // Status message (tight)
    // ===========================================================
    if (!v.status_msg.empty())
    {
        ImGui::Dummy(ImVec2(0, 4));
        if (v.status_is_error) ImGui::TextColored(colors::Red, "%s", v.status_msg.c_str());
        else                   ImGui::TextColored(colors::Green, "%s", v.status_msg.c_str());
    }

    ImGui::Dummy(ImVec2(0, 8));

    // Close soft container
    render::EndSoftContainer();

    render::EndCenteredColumn();
}

// render_login_screen() removed - app now defaults to locked screen

// application.cpp
static void render_unlocked_screen()
{
    // Ensure at least one tab
    ActiveTab();

    // Sync shell top tabs from vault tabs (labels + active index)
    SyncShellTabsFromVaultTabs();

    // --- Center window once ---
#ifdef _WIN32
    RECT screen_rect;
    GetWindowRect(GetDesktopWindow(), &screen_rect);
    const float x = float(screen_rect.right - WINDOW_WIDTH) * 0.5f;
    const float y = float(screen_rect.bottom - WINDOW_HEIGHT) * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Once);
#else
    // On macOS, let ImGui handle window positioning
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Once);
#endif
    ImGui::SetNextWindowSize(ImVec2(WINDOW_WIDTH, WINDOW_HEIGHT), ImGuiCond_Always);

    // ============================================================
    // Begin Shell
    // ============================================================
    ui::BeginShell(g_shell, "Password Manager");

    // Store previous active tab BEFORE syncing
    const int prev_active_tab = g_active_tab;

    // If user switched top DB tab in the shell, update active tab index
    ApplyShellActiveTab();

    // If we switched DB, reset close confirm state IMMEDIATELY
    if (g_active_tab != prev_active_tab)
    {
        g_shell.footer_close_confirming = false;
        g_shell.footer_close_anyway = false;
        g_shell.footer_close_cancel = false;

        propagate_sync_tokens();
    }

    // Now bind the correct active vault after tab switching
    VaultState& v = ActiveVault();
    const uint32_t activeVaultKey = GetActiveVaultKey();

    // Security center stats (compute on Settings and Unlocked screens)
    if ((g_shell.active_screen == ui::Screen::Settings ||
         g_shell.active_screen == ui::Screen::Unlocked) && v.unlocked)
    {
        // Reused: password frequency map
        std::unordered_map<std::string, int> pw_freq;
        for (const auto& c : v.creds) {
            if (!c.is_deleted() && c.type == CredType::Password && !c.password.empty())
                pw_freq[c.password]++;
        }
        int reused = 0;
        g_shell.sec_reused_ids.clear();
        for (const auto& c : v.creds) {
            if (!c.is_deleted() && c.type == CredType::Password && !c.password.empty() && pw_freq[c.password] > 1) {
                reused++;
                g_shell.sec_reused_ids.insert(c.id);
            }
        }

        // Weak: score <= 1
        int weak = 0;
        g_shell.sec_weak_ids.clear();
        for (const auto& c : v.creds) {
            if (!c.is_deleted() && c.type == CredType::Password && !c.password.empty())
                if (helpers::analyze_password(c.password).score <= 1) {
                    weak++;
                    g_shell.sec_weak_ids.insert(c.id);
                }
        }

        g_shell.sec_reused_count  = reused;
        g_shell.sec_weak_count    = weak;

        // Aging: password not changed in more than N days
        int aging = 0;
        g_shell.sec_aging_ids.clear();
        {
            int64_t now_ms = helpers::now_unix_ms();
            int64_t max_age_ms = (int64_t)g_shell.password_max_age_days * 24 * 3600 * 1000LL;
            for (const auto& c : v.creds) {
                if (!c.is_deleted() && c.type == CredType::Password && !c.password.empty()) {
                    int64_t pw_set_at = c.created_at_ms;
                    if (!c.password_history.empty() && c.password_history[0].changed_at_ms > 0)
                        pw_set_at = c.password_history[0].changed_at_ms;

                    if (pw_set_at > 0 && (now_ms - pw_set_at) > max_age_ms) {
                        aging++;
                        g_shell.sec_aging_ids.insert(c.id);
                    }
                }
            }
        }
        g_shell.sec_aging_count = aging;

        // Consume breach check results from background thread (acquire pairs with release in thread)
        if (g_shell.sec_breach_done.load(std::memory_order_acquire)) {
            g_shell.sec_exposed_ids   = std::move(g_shell.sec_exposed_ids_staging);
            g_shell.sec_exposed_count = g_shell.sec_exposed_count_staging;
            g_shell.sec_breach_error  = std::move(g_shell.sec_breach_error_staging);
            g_shell.sec_breach_done.store(false, std::memory_order_relaxed);
        }

        // Consume share status results from background thread
        if (g_shell.share_status_done.load(std::memory_order_acquire)) {
            const auto& uuid = g_shell.share_status_staging_uuid;
            if (!uuid.empty()) {
                if (g_shell.share_status_staging.valid) {
                    auto& cached = g_shell.share_status_cache[uuid];
                    cached.status = g_shell.share_status_staging;
                    cached.fetched_at_ms = helpers::now_unix_ms();
                    cached.token = g_shell.share_status_staging_token;
                } else {
                    // 404 -- share was deleted/expired server-side, clean up
                    vault_db::set_sync_state("share_token:" + uuid, "");
                    vault_db::set_sync_state("share_created:" + uuid, "");
                    g_shell.share_status_cache.erase(uuid);
                }
            }
            g_shell.share_status_done.store(false, std::memory_order_relaxed);
        }

        // Share status fetch: triggered by UI when a credential with a share token is viewed
        if (!g_shell.share_status_request_queue.empty() && !g_shell.share_status_fetching.load()) {
            std::string req_uuid = g_shell.share_status_request_queue.front();
            g_shell.share_status_request_queue.erase(g_shell.share_status_request_queue.begin());

            std::string token = vault_db::get_sync_state("share_token:" + req_uuid);
            if (!token.empty()) {
                // Check cache freshness (60s)
                bool need_fetch = true;
                auto it = g_shell.share_status_cache.find(req_uuid);
                if (it != g_shell.share_status_cache.end()) {
                    int64_t age_ms = helpers::now_unix_ms() - it->second.fetched_at_ms;
                    if (age_ms < 60000) need_fetch = false;
                }

                if (need_fetch && g_sync_mgr) {
                    g_shell.share_status_fetching.store(true);
                    std::thread([req_uuid, token]() {
                        auto result = g_sync_mgr->get_share_status(token);
                        ui::ShellState::ShareStatusInfo info;
                        info.valid           = result.valid;
                        info.view_count      = result.view_count;
                        info.max_views       = result.max_views;
                        info.expired         = result.expired;
                        info.views_exhausted = result.views_exhausted;
                        info.expires_at_ms   = result.expires_at_ms;
                        info.created_at_ms   = result.created_at_ms;

                        g_shell.share_status_staging_uuid  = req_uuid;
                        g_shell.share_status_staging       = info;
                        g_shell.share_status_staging_token = token;
                        g_shell.share_status_done.store(true, std::memory_order_release);
                        g_shell.share_status_fetching.store(false, std::memory_order_release);
                    }).detach();
                }
            }
        }

        // Breach check: triggered by user "Check Now" button
        if (g_shell.sec_breach_trigger && !g_shell.sec_breach_checking) {
            g_shell.sec_breach_trigger = false;
            CheckBreachedPasswords(g_shell, v.creds);
        }
    }

    // Screen transition logic:
    // - Lock/unlock transitions based on vault state
    if (!v.unlocked &&
        g_shell.active_screen != ui::Screen::Locked)
    {
        ResetScreen(g_shell, ui::Screen::Locked);
    }
    else if (v.unlocked && g_shell.active_screen == ui::Screen::Locked)
    {
        ResetScreen(g_shell, ui::Screen::Unlocked);
    }

    // ============================================================
    // FIXED HEADER: DrawListControlsRow (not scrollable)
    // ============================================================
    if (g_shell.active_screen != ui::Screen::Locked &&
        g_shell.active_screen != ui::Screen::Settings)
    {
        ui::BeginShellHeader();
        ui::DrawListControlsRow(g_shell, activeVaultKey);
        ui::EndShellHeader();
    }

    // ============================================================
    // SCROLLABLE LIST: Add panel + accordion list
    // (Skip if settings page is open)
    // ============================================================

    const bool render_scroll = (g_shell.active_screen != ui::Screen::Settings);
    if (render_scroll)
        ui::BeginShellScroll();

    // ============================================================
    // Footer status + dirty
    // ============================================================
    g_shell.dirty = v.is_dirty();
    g_shell.can_undo = !v.undo_stack.empty();

    g_shell.footer_status_text = v.status_msg.empty()
        ? (v.unlocked ? "Vault unlocked." : "Locked — enter password.")
        : v.status_msg;

    g_shell.footer_status_is_error = v.status_is_error;

    auto stamp_saved_status = [&]()
        {
            // Optional: show local time without seconds if you want
            // simplest: reuse now_iso8601_local() and trim
            std::string t = helpers::now_iso8601_local();
            // "YYYY-MM-DDTHH:MM:SS-08:00" -> "YYYY-MM-DD HH:MM"
            if (t.size() >= 16) {
                t[10] = ' ';
                t = t.substr(0, 16);
            }
            v.set_status("Saved • " + t, false);
        };

    // ============================================================
    // Ctrl+S saves active vault (BLOCKED in read-only)
    // ============================================================
    if (!g_shell.read_only && ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_S, false))
    {
        save_vault_to_disk(v);
        g_autosave.last_save_time = ImGui::GetTime();
        stamp_saved_status();
    }

    // ============================================================
    // Footer backups list for active vault
    // ============================================================
    static std::string s_last_backup_vault;

    auto rebuild_footer_backups = [&]()
        {
            g_shell.footer_backups.clear();
            g_shell.footer_backup_paths.clear();
            g_shell.footer_backup_meta.clear();

            const auto paths = helpers::ListBackupsForVault(v.vault_path);
            g_shell.footer_backup_paths = paths;

            const int n = (int)g_shell.footer_backup_paths.size();
            g_shell.footer_backups.reserve(n);
            g_shell.footer_backup_meta.resize(n);

            std::string vaultBase = helpers::Basename(v.vault_path);
            if (auto dot = vaultBase.find_last_of('.'); dot != std::string::npos)
                vaultBase = vaultBase.substr(0, dot);

            const std::string stripPrefix = vaultBase + "_";

            for (int i = 0; i < n; ++i)
            {
                const std::string fn = helpers::Basename(g_shell.footer_backup_paths[i]);

                std::string label = fn;
                if (label.rfind(stripPrefix, 0) == 0)
                    label = label.substr(stripPrefix.size());

                g_shell.footer_backups.push_back(label);

                uint64_t bytes = 0;
                std::string t;
                if (!helpers::GetFileInfo(g_shell.footer_backup_paths[i], bytes, t))
                {
                    bytes = 0;
                    t.clear();
                }

                g_shell.footer_backup_meta[i].sizeBytes = bytes;
                g_shell.footer_backup_meta[i].localTime = t;
            }

            if (n <= 0) g_shell.footer_backup_index = 0;
            else
            {
                if (g_shell.footer_backup_index < 0 || g_shell.footer_backup_index >= n)
                    g_shell.footer_backup_index = 0;

                if (g_shell.backup_select_newest_on_refresh)
                    g_shell.footer_backup_index = 0;
            }
        };

    auto update_last_backup_label = [&]()
        {
            g_shell.last_backup_label.clear();
            if (g_shell.footer_backup_paths.empty() || g_shell.footer_backup_meta.empty())
                return;

            // newest is index 0 (your ListBackupsForVault sorts newest first)
            const auto& m = g_shell.footer_backup_meta[0];
            if (m.localTime.empty())
                return;

            g_shell.last_backup_label = "Last backup: " + m.localTime + " \u2022 " + helpers::FormatBytes(m.sizeBytes);
        };

    if (g_shell.footer_refresh_backups_clicked || g_backups_dirty || s_last_backup_vault != v.vault_path)
    {
        rebuild_footer_backups();
        update_last_backup_label();

        s_last_backup_vault = v.vault_path;
        g_shell.footer_refresh_backups_clicked = false;
        g_backups_dirty = false;
    }

    // ============================================================
    // Apply read-only requested by UI popup
    // ============================================================
    if (g_shell.footer_set_read_only)
    {
        g_shell.footer_set_read_only = false;
        g_shell.read_only = g_shell.footer_set_read_only_value;

        g_shell.footer_status_text = g_shell.read_only ? "Read-only enabled" : "Read-only disabled";
        g_shell.footer_status_is_error = false;

        // Optional: close any open edit/add states when enabling read-only
        // if (g_shell.read_only) v.add_open = false;
    }

    // ============================================================
    // Create backup now
    // ============================================================
    if (g_shell.footer_create_backup_clicked)
    {
        g_shell.footer_create_backup_clicked = false;

        auto res = helpers::CreateBackupNow(v.vault_path, (size_t)g_shell.backup_keep_count);
        if (res.ok)
        {
            g_shell.backup_select_newest_on_refresh = true;

            v.set_status("Backup created.", false);
            g_backups_dirty = true;
        }
        else
        {
            v.set_status(res.err.empty() ? "Backup failed." : res.err.c_str(), true);
        }
    }

    // ============================================================
    // Security lockout - force vault lock on too many failed re-prompts
    // ============================================================
    if (ui::IsRepromptLockedOut() && v.unlocked)
    {
        g_shell.footer_close_anyway = true; // Trigger vault lock
    }

    // ============================================================
    // Browse for backup file
    // ============================================================
    if (g_shell.footer_browse_backup_clicked)
    {
        g_shell.footer_browse_backup_clicked = false;
#ifdef _WIN32
        char file[MAX_PATH] = { 0 };
        OPENFILENAMEA ofn{};
        ofn.lStructSize  = sizeof(ofn);
        ofn.hwndOwner    = nullptr;
        ofn.lpstrFile    = file;
        ofn.nMaxFile     = MAX_PATH;
        ofn.lpstrFilter  = "Vault Backup (*.lbdb)\0*.lbdb\0All Files\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameA(&ofn))
            g_shell.footer_browse_backup_path = std::string(file);
#else
        std::string picked = PickOpenFilePath_Backup();
        if (!picked.empty())
            g_shell.footer_browse_backup_path = picked;
#endif
    }

    // ============================================================
    // Block restore/save intents in read-only (belt + suspenders)
    // ============================================================
    if (g_shell.footer_restore_clicked && g_shell.read_only)
    {
        g_shell.footer_restore_clicked = false;
        v.set_status("Blocked: Read-only mode.", true);
    }

    if (g_shell.footer_save_clicked && g_shell.read_only)
    {
        g_shell.footer_save_clicked = false;
        v.set_status("Blocked: Read-only mode.", true);
    }

    // ============================================================
    // If read-only, nuke mutating intents so nothing slips through
    // ============================================================
    if (g_shell.read_only)
    {
        g_shell.add_clicked = false;
        g_shell.undo_clicked = false;
        g_shell.bulk_delete_clicked = false;
        g_shell.bulk_pin_clicked = false;
        g_shell.bulk_unpin_clicked = false;
        g_shell.bulk_fav_clicked = false;
        g_shell.bulk_unfav_clicked = false;
        g_shell.bulk_set_group_clicked = false;
    }

    // ============================================================
    // Groups rebuild (per-vault tracking)
    // ============================================================
    const uint32_t gh = HashGroupsOnly(v.creds);
    if (gh != v.last_groups_hash)
    {
        rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);
        v.last_groups_hash = gh;
    }

    // ============================================================
    // Add / Undo
    // ============================================================
    if (g_shell.add_clicked && !g_shell.read_only && g_shell.active_screen == ui::Screen::Unlocked)
    {
        g_cred_modal.OpenAdd();
        ImGui::OpenPopup("Add/Edit###cred_modal");
    }

    if (g_shell.undo_clicked)
        do_undo(v);

    // ============================================================
    // Sync intents
    // ============================================================
    // Cloud sign-in trigger — hidden in offline builds (no server to sign into).
#ifndef PD_OFFLINE_BUILD
    if (g_shell.sync_login_clicked) {
        init_sync_manager();
        g_sync_modal.Openlogin(g_shell.sync_server_url);
        ImGui::OpenPopup("Sign In###sync_modal");
        g_shell.sync_login_clicked = false;
    }
#endif

    // Find Duplicates is local (scans in-memory creds) — keep in both targets.
    if (g_shell.dedup_scan_clicked) {
        g_shell.dedup_scan_clicked = false;
        g_dedup_groups = find_duplicate_groups(ActiveVault().creds);
        g_shell.dedup_modal_open = true;
        ImGui::OpenPopup("Find Duplicates###dedup_modal");
    }

#ifndef PD_OFFLINE_BUILD
    if (g_shell.sync_now_clicked) {
        // Update sync status message (visible on Settings screen)
        g_shell.sync_status = 1;  // Connecting
        g_shell.sync_status_msg = "Starting sync...";

        init_sync_manager();
        if (!g_sync_mgr) {
            g_shell.sync_status = 6;  // Error
            g_shell.sync_status_msg = "Sync manager not initialized.";
        } else if (!g_shell.sync_logged_in) {
            g_shell.sync_status = 6;
            g_shell.sync_status_msg = "Not logged in.";
        } else if (!vault_db::is_open()) {
            g_shell.sync_status = 6;
            g_shell.sync_status_msg = "Vault database not open.";
        } else {
            g_shell.sync_status = 1;  // Connecting
            g_shell.sync_status_msg = "Checking salt...";

            // Debug: get raw hex string first
            std::string raw_hex = vault_db::get_sync_state("encryption_salt");
            auto local_salt = vault_db::get_cached_salt();

            std::string vault_slug = vault_db::get_vault_slug();
            bool can_sync = false;

            if (!vault_slug.empty()) {
                // Named vault: salt was set at creation. Skip user-level salt endpoint.
                if (local_salt.size() == enc::SALT_SIZE) {
                    can_sync = true;
                } else {
                    g_shell.sync_status = 6;
                    g_shell.sync_status_msg = "Named vault missing salt.";
                }
            } else {
            // Default vault: existing FetchServerSalt flow
            std::vector<uint8_t> server_salt;
            bool server_has_salt = g_sync_mgr->fetch_server_salt(server_salt);

            // Update status with detailed salt info
            char salt_msg[256];
            snprintf(salt_msg, sizeof(salt_msg), "Hex len: %zu, Bytes: %zu, Server: %s",
                raw_hex.size(), local_salt.size(), server_has_salt ? "yes" : "no");
            g_shell.sync_status_msg = salt_msg;

            if (server_has_salt && !server_salt.empty()) {
                // Server has salt - use it if different from local
                if (local_salt != server_salt) {
                    if (v.unlocked && !v.session_password.empty() && !v.master_key.empty()) {
                        auto new_key = enc::derive_master_key(v.session_password, server_salt);
                        if (!new_key.empty()) {
                            auto old_key = std::move(v.master_key);
                            v.master_key = std::move(new_key);
                            vault_db::set_cached_salt(server_salt);

                            // Re-encrypt all credentials with new key
                            bool re_encrypt_ok = true;
                            for (auto& c : v.creds) {
                                if (!cred_ops::update(c.uuid, c, v.master_key)) {
                                    re_encrypt_ok = false;
                                    break;
                                }
                                c.is_dirty = true;
                            }

                            if (re_encrypt_ok) {
                                enc::secure_zero(old_key);

                                // Re-encrypt recovery blob if present
                                if (vault_db::has_recovery_key()) {
                                    auto rec_blob = vault_db::get_recovery_blob();
                                    // Recovery blob was encrypted with old master key hex
                                    // We can't re-encrypt it without the recovery key itself
                                    // so we must delete it — user loses recovery until next vault creation
                                    vault_db::delete_recovery_blob();
                                }

                                can_sync = true;
                            } else {
                                // Rollback
                                v.master_key = std::move(old_key);
                                vault_db::set_cached_salt(local_salt);
                                g_shell.sync_status = 6;
                                g_shell.sync_status_msg = "Salt migration failed: re-encryption error.";
                            }
                        } else {
                            g_shell.sync_status = 6;
                            g_shell.sync_status_msg = "Failed to derive key.";
                        }
                    } else {
                        // Vault locked or no password — just adopt server salt
                        // (credentials will fail to decrypt until re-created)
                        vault_db::set_cached_salt(server_salt);
                        can_sync = true;
                    }
                } else {
                    can_sync = true;
                }
            } else if (!local_salt.empty() && local_salt.size() == enc::SALT_SIZE) {
                // Valid 32-byte salt - upload it
                char size_msg[64];
                snprintf(size_msg, sizeof(size_msg), "Uploading salt (%zu bytes)...", local_salt.size());
                g_shell.sync_status_msg = size_msg;

                std::string upload_error;
                if (g_sync_mgr->upload_salt_with_error(local_salt, upload_error)) {
                    can_sync = true;
                } else {
                    g_shell.sync_status = 6;
                    char err_msg[128];
                    snprintf(err_msg, sizeof(err_msg), "Salt upload failed (%zu bytes): %s",
                        local_salt.size(), upload_error.c_str());
                    g_shell.sync_status_msg = err_msg;
                }
            } else if (!local_salt.empty() && local_salt.size() != enc::SALT_SIZE) {
                // Corrupted salt (wrong size) - attempt migration if vault is unlocked
                if (v.unlocked && !v.session_password.empty() && !v.master_key.empty()) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Fixing corrupted salt (%zu->%zu bytes)...", local_salt.size(), enc::SALT_SIZE);
                    g_shell.sync_status_msg = msg;

                    // Generate proper salt
                    auto new_salt = enc::generate_salt();
                    auto new_key = enc::derive_master_key(v.session_password, new_salt);

                    if (!new_key.empty()) {
                        vault_db::set_cached_salt(new_salt);
                        auto old_key = std::move(v.master_key);
                        v.master_key = std::move(new_key);

                        bool re_encrypt_ok = true;
                        for (auto& c : v.creds) {
                            if (!cred_ops::update(c.uuid, c, v.master_key)) {
                                re_encrypt_ok = false;
                                break;
                            }
                            c.is_dirty = true;
                        }

                        if (re_encrypt_ok) {
                            enc::secure_zero(old_key);
                            local_salt = new_salt;
                            g_shell.sync_status_msg = "Salt fixed, uploading...";

                            std::string upload_error;
                            if (g_sync_mgr->upload_salt_with_error(local_salt, upload_error)) {
                                can_sync = true;
                            } else {
                                g_shell.sync_status = 6;
                                char err_msg[128];
                                snprintf(err_msg, sizeof(err_msg), "Salt upload failed: %s", upload_error.c_str());
                                g_shell.sync_status_msg = err_msg;
                            }
                        } else {
                            v.master_key = std::move(old_key);
                            g_shell.sync_status = 6;
                            g_shell.sync_status_msg = "Migration failed: re-encryption error.";
                        }
                    } else {
                        g_shell.sync_status = 6;
                        g_shell.sync_status_msg = "Migration failed: key derivation error.";
                    }
                } else {
                    g_shell.sync_status = 6;
                    char err_msg[128];
                    snprintf(err_msg, sizeof(err_msg), "Corrupted salt (%zu bytes). Unlock vault to fix.", local_salt.size());
                    g_shell.sync_status_msg = err_msg;
                }
            } else {
                // No local salt found - attempt recovery if vault is unlocked
                if (v.unlocked && !v.session_password.empty() && !v.master_key.empty()) {
                    g_shell.sync_status_msg = "Migrating vault for sync...";

                    // Generate new salt
                    auto new_salt = enc::generate_salt();
                    auto new_key = enc::derive_master_key(v.session_password, new_salt);

                    if (!new_key.empty()) {
                        // Store new salt first
                        vault_db::set_cached_salt(new_salt);
                        // Update in-memory key
                        auto old_key = std::move(v.master_key);
                        v.master_key = std::move(new_key);

                        // Re-encrypt all credentials with new key
                        bool re_encrypt_ok = true;
                        for (auto& c : v.creds) {
                            // c.password already has plaintext from load_all()
                            // Just update in DB with new key
                            if (!cred_ops::update(c.uuid, c, v.master_key)) {
                                re_encrypt_ok = false;
                                break;
                            }
                            c.is_dirty = true;  // Mark for sync
                        }

                        if (re_encrypt_ok) {
                            enc::secure_zero(old_key);
                            local_salt = new_salt;
                            g_shell.sync_status_msg = "Migration complete, uploading salt...";

                            // Now upload the new salt
                            std::string upload_error;
                            if (g_sync_mgr->upload_salt_with_error(local_salt, upload_error)) {
                                can_sync = true;
                            } else {
                                g_shell.sync_status = 6;
                                char err_msg[128];
                                snprintf(err_msg, sizeof(err_msg), "Salt upload failed: %s", upload_error.c_str());
                                g_shell.sync_status_msg = err_msg;
                            }
                        } else {
                            // Re-encryption failed - restore old key
                            v.master_key = std::move(old_key);
                            g_shell.sync_status = 6;
                            g_shell.sync_status_msg = "Migration failed: re-encryption error.";
                        }
                    } else {
                        g_shell.sync_status = 6;
                        g_shell.sync_status_msg = "Migration failed: key derivation error.";
                    }
                } else {
                    g_shell.sync_status = 6;
                    char err_msg[256];
                    snprintf(err_msg, sizeof(err_msg), "No salt (hex=%zu). Unlock vault first.", raw_hex.size());
                    g_shell.sync_status_msg = err_msg;
                }
            }
            } // end default vault salt flow

            if (can_sync) {
                g_shell.sync_status = 3;  // Uploading
                g_shell.sync_status_msg = "Syncing...";
                g_sync_mgr->start_background_sync();
            }
        }
        g_shell.sync_now_clicked = false;
    }

    if (g_shell.sync_logout_clicked) {
        if (g_sync_mgr) {
            g_sync_mgr->logout();
        }
        g_shell.sync_logged_in = false;
        g_shell.sync_username.clear();
        g_shell.sync_status = 0;  // Idle
        g_shell.sync_status_msg.clear();
        g_shell.sync_logout_clicked = false;
    }
#endif  // PD_OFFLINE_BUILD — end of Sync intents (sync_now, logout, etc.)

    // ============================================================
    // CSV Export
    // ============================================================
    if (g_shell.export_csv_clicked)
    {
        g_shell.export_csv_clicked = false;
        std::string path = PickSaveFilePath_CSV();
        if (!path.empty())
        {
            // Build CSV: quote every field, escape internal quotes by doubling
            auto csv_quote = [](const std::string& s) -> std::string {
                std::string out = "\"";
                for (char c : s) {
                    if (c == '"') out += "\"\"";
                    else out += c;
                }
                out += '"';
                return out;
            };

            std::string csv;
            csv += "Type,Title,Username,Password,Email,URL,Group,Notes,Favorite,Pinned,"
                   "CardholderName,CardNumber,CardExpiry,CardCVV,CardBrand,"
                   "FullName,IDType,IDNumber,DateOfBirth,ExpiryDate,Country,Address,Phone,"
                   "Created,Modified,ExpiresAt,CardCity,CardPostalCode,CardAddress\n";

            int count = 0;
            for (const auto& c : v.creds) {
                if (c.is_deleted()) continue;
                csv += csv_quote(CredTypeLabel(c.type)) + ','
                     + csv_quote(c.title) + ','
                     + csv_quote(c.user) + ','
                     + csv_quote(c.password) + ','
                     + csv_quote(c.email) + ','
                     + csv_quote(c.website) + ','
                     + csv_quote(c.group) + ','
                     + csv_quote(c.notes) + ','
                     + (c.is_favorite ? "True" : "False") + ','
                     + (c.is_pinned ? "True" : "False") + ','
                     + csv_quote(c.cardholder_name) + ','
                     + csv_quote(c.card_number) + ','
                     + csv_quote(c.card_expiry) + ','
                     + csv_quote(c.card_cvv) + ','
                     + csv_quote(c.card_brand) + ','
                     + csv_quote(c.full_name) + ','
                     + csv_quote(c.id_type) + ','
                     + csv_quote(c.id_number) + ','
                     + csv_quote(c.date_of_birth) + ','
                     + csv_quote(c.expiry_date) + ','
                     + csv_quote(c.country) + ','
                     + csv_quote(c.address) + ','
                     + csv_quote(c.phone) + ','
                     + csv_quote(helpers::unix_ms_to_iso8601(c.created_at_ms)) + ','
                     + csv_quote(helpers::unix_ms_to_iso8601(c.updated_at_ms)) + ','
                     + (c.expires_at_ms > 0 ? csv_quote(helpers::unix_ms_to_iso8601(c.expires_at_ms)) : "") + ','
                     + csv_quote(c.card_city) + ','
                     + csv_quote(c.card_postal_code) + ','
                     + csv_quote(c.card_address) + '\n';
                count++;
            }

            helpers::str_to_file(path, csv);
            sodium_memzero(csv.data(), csv.size());

            char msg[128];
            snprintf(msg, sizeof(msg), "Exported %d credentials to CSV", count);
            ui::ShowToast(msg, ui::ToastType::Success);
        }
    }

    // ============================================================
    // PWM Export / Import
    // ============================================================
    {
        static bool    s_pwm_export_modal = false;
        static char    s_pwm_export_pw1[256] = {};
        static char    s_pwm_export_pw2[256] = {};
        static std::string s_pwm_export_error;

        static bool    s_pwm_import_modal = false;
        static char    s_pwm_import_pw[256] = {};
        static std::string s_pwm_import_path;
        static std::string s_pwm_import_error;

        // --- Export intent ---
        if (g_shell.export_pwm_clicked)
        {
            g_shell.export_pwm_clicked = false;
            s_pwm_export_modal = true;
            sodium_memzero(s_pwm_export_pw1, sizeof(s_pwm_export_pw1));
            sodium_memzero(s_pwm_export_pw2, sizeof(s_pwm_export_pw2));
            s_pwm_export_error.clear();
            ImGui::OpenPopup("Export PWM###pwm_export_modal");
        }

        // --- Export modal ---
        if (s_pwm_export_modal)
        {
            ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

            ImU32 dimBg = IM_COL32(0, 0, 0, 150);
            ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, dimBg);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));   // breathing room — matches the 2FA modals

            const float modalW = 380.0f;
            ImGui::SetNextWindowSizeConstraints(ImVec2(modalW, 0), ImVec2(modalW, FLT_MAX));

            bool modal_open = true;
            if (ImGui::BeginPopupModal("Export PWM###pwm_export_modal", &modal_open,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove))
            {
                bool escape_pressed = ImGui::IsKeyPressed(ImGuiKey_Escape);
                bool submit_shortcut = ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_Enter);

                ImGui::PushFont(render::FontLarge);
                ImGui::TextUnformatted(ICON_MDI_LOCK "  Export Encrypted Backup");
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0, 12));

                const float fieldW = ImGui::GetContentRegionAvail().x;

                ImGui::TextDisabled("Export Password");
                ImGui::SetNextItemWidth(fieldW);
                ImGui::InputText("##pwm_export_pw1", s_pwm_export_pw1, sizeof(s_pwm_export_pw1), ImGuiInputTextFlags_Password);

                ImGui::Dummy(ImVec2(0, 4));

                ImGui::TextDisabled("Confirm Password");
                ImGui::SetNextItemWidth(fieldW);
                ImGui::InputText("##pwm_export_pw2", s_pwm_export_pw2, sizeof(s_pwm_export_pw2), ImGuiInputTextFlags_Password);

                // Error message
                if (!s_pwm_export_error.empty()) {
                    ImGui::Dummy(ImVec2(0, 8));
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(174, 41, 41, 255));
                    ImGui::TextWrapped("%s", s_pwm_export_error.c_str());
                    ImGui::PopStyleColor();
                }

                ImGui::Dummy(ImVec2(0, 12));

                // Buttons
                const float btnW = 100.0f;
                const float btnH = 32.0f;
                float totalBtnW = btnW * 2 + 8.0f;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + fieldW - totalBtnW);

                if (ui::StyledButton("##pwm_exp_cancel", "Cancel", ImVec2(btnW, btnH)) || escape_pressed)
                {
                    sodium_memzero(s_pwm_export_pw1, sizeof(s_pwm_export_pw1));
                    sodium_memzero(s_pwm_export_pw2, sizeof(s_pwm_export_pw2));
                    s_pwm_export_error.clear();
                    s_pwm_export_modal = false;
                    ImGui::CloseCurrentPopup();
                }

                ImGui::SameLine(0, 8.0f);

                bool can_submit = s_pwm_export_pw1[0] != '\0';
                bool pw_match = (strcmp(s_pwm_export_pw1, s_pwm_export_pw2) == 0);

                if (!can_submit) ImGui::BeginDisabled();
                bool do_export = ui::StyledButton("##pwm_exp_go", "Export", ImVec2(btnW, btnH)) || (submit_shortcut && can_submit);
                if (!can_submit) ImGui::EndDisabled();

                if (do_export && can_submit)
                {
                    if (!pw_match) {
                        s_pwm_export_error = "Passwords do not match.";
                    } else {
                        std::string path = PickSaveFilePath_PWM();
                        if (!path.empty())
                        {
                            auto result = pwm_file::export_pwm(v.creds, std::string(s_pwm_export_pw1), path.c_str());
                            sodium_memzero(s_pwm_export_pw1, sizeof(s_pwm_export_pw1));
                            sodium_memzero(s_pwm_export_pw2, sizeof(s_pwm_export_pw2));

                            if (result.ok) {
                                char msg[128];
                                snprintf(msg, sizeof(msg), "Exported %d credentials", result.count);
                                ui::ShowToast(msg, ui::ToastType::Success);
                                s_pwm_export_error.clear();
                                s_pwm_export_modal = false;
                                ImGui::CloseCurrentPopup();
                            } else {
                                s_pwm_export_error = "Export failed: " + result.error;
                            }
                        }
                    }
                }

                ImGui::EndPopup();
            }

            if (!modal_open)
            {
                sodium_memzero(s_pwm_export_pw1, sizeof(s_pwm_export_pw1));
                sodium_memzero(s_pwm_export_pw2, sizeof(s_pwm_export_pw2));
                s_pwm_export_error.clear();
                s_pwm_export_modal = false;
            }

            ImGui::PopStyleVar();   // WindowPadding
            ImGui::PopStyleColor();
        }

        // --- Import intent ---
        if (g_shell.import_pwm_clicked)
        {
            g_shell.import_pwm_clicked = false;
            s_pwm_import_path = PickOpenFilePath_PWM();
            if (!s_pwm_import_path.empty())
            {
                s_pwm_import_modal = true;
                sodium_memzero(s_pwm_import_pw, sizeof(s_pwm_import_pw));
                s_pwm_import_error.clear();
                ImGui::OpenPopup("Import PWM###pwm_import_modal");
            }
        }

        // --- Import modal ---
        if (s_pwm_import_modal)
        {
            ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

            ImU32 dimBg = IM_COL32(0, 0, 0, 150);
            ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, dimBg);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));   // breathing room

            const float modalW = 380.0f;
            ImGui::SetNextWindowSizeConstraints(ImVec2(modalW, 0), ImVec2(modalW, FLT_MAX));

            bool modal_open = true;
            if (ImGui::BeginPopupModal("Import PWM###pwm_import_modal", &modal_open,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove))
            {
                bool escape_pressed = ImGui::IsKeyPressed(ImGuiKey_Escape);
                bool submit_shortcut = ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_Enter);

                ImGui::PushFont(render::FontLarge);
                ImGui::TextUnformatted(ICON_MDI_FILE_IMPORT "  Import Encrypted Backup");
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0, 12));

                const float fieldW = ImGui::GetContentRegionAvail().x;

                ImGui::TextDisabled("Export Password");
                ImGui::SetNextItemWidth(fieldW);
                ImGui::InputText("##pwm_import_pw", s_pwm_import_pw, sizeof(s_pwm_import_pw), ImGuiInputTextFlags_Password);

                // Error message
                if (!s_pwm_import_error.empty()) {
                    ImGui::Dummy(ImVec2(0, 8));
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(174, 41, 41, 255));
                    ImGui::TextWrapped("%s", s_pwm_import_error.c_str());
                    ImGui::PopStyleColor();
                }

                ImGui::Dummy(ImVec2(0, 12));

                // Buttons
                const float btnW = 100.0f;
                const float btnH = 32.0f;
                float totalBtnW = btnW * 2 + 8.0f;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + fieldW - totalBtnW);

                if (ui::StyledButton("##pwm_imp_cancel", "Cancel", ImVec2(btnW, btnH)) || escape_pressed)
                {
                    sodium_memzero(s_pwm_import_pw, sizeof(s_pwm_import_pw));
                    s_pwm_import_error.clear();
                    s_pwm_import_path.clear();
                    s_pwm_import_modal = false;
                    ImGui::CloseCurrentPopup();
                }

                ImGui::SameLine(0, 8.0f);

                bool can_submit = s_pwm_import_pw[0] != '\0';

                if (!can_submit) ImGui::BeginDisabled();
                bool do_import = ui::StyledButton("##pwm_imp_go", "Import", ImVec2(btnW, btnH)) || (submit_shortcut && can_submit);
                if (!can_submit) ImGui::EndDisabled();

                if (do_import && can_submit)
                {
                    auto result = pwm_file::import_pwm(std::string(s_pwm_import_pw), s_pwm_import_path);
                    sodium_memzero(s_pwm_import_pw, sizeof(s_pwm_import_pw));

                    if (result.ok)
                    {
                        // Content-keyed merge: reuse the UUID of an existing
                        // credential with the same site+email+username so
                        // re-imports update in place (via upsert_credential's
                        // newer-wins) instead of minting fresh UUIDs and
                        // creating duplicates. Unmatched rows still get a
                        // fresh UUID (cleared below).
                        std::unordered_map<std::string, std::string> sig_to_uuid;
                        for (const auto& ex : v.creds) {
                            if (ex.is_deleted()) continue;
                            const std::string sig = cred_signature(ex);
                            if (!sig.empty()) sig_to_uuid.emplace(sig, ex.uuid);
                        }
                        int merged = 0;
                        for (auto& c : result.creds) {
                            const std::string sig = cred_signature(c);
                            auto it = sig.empty() ? sig_to_uuid.end() : sig_to_uuid.find(sig);
                            if (it != sig_to_uuid.end()) { c.uuid = it->second; ++merged; }
                            else                         { c.uuid.clear(); }
                        }

                        if (cred_ops::import_credentials(result.creds, v.master_key))
                        {
                            v.creds = cred_ops::load_all(v.master_key);
                            rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);
                            ui::ForgetVaultRowState(GetActiveVaultKey());
                            VaultMarkChanged(v, "IMPORT");

                            char msg[160];
                            if (merged > 0)
                                snprintf(msg, sizeof(msg), "Imported %d credentials (%d merged into existing)", result.count, merged);
                            else
                                snprintf(msg, sizeof(msg), "Imported %d credentials", result.count);
                            ui::ShowToast(msg, ui::ToastType::Success);

                            s_pwm_import_error.clear();
                            s_pwm_import_path.clear();
                            s_pwm_import_modal = false;
                            ImGui::CloseCurrentPopup();
                        }
                        else
                        {
                            s_pwm_import_error = "Database error during import";
                        }
                    }
                    else
                    {
                        s_pwm_import_error = result.error;
                    }
                }

                ImGui::EndPopup();
            }

            if (!modal_open)
            {
                sodium_memzero(s_pwm_import_pw, sizeof(s_pwm_import_pw));
                s_pwm_import_error.clear();
                s_pwm_import_path.clear();
                s_pwm_import_modal = false;
            }

            ImGui::PopStyleVar();   // WindowPadding
            ImGui::PopStyleColor();
        }
    }

    // ============================================================
    // KDBX Export
    // ============================================================
    {
        static bool    s_kdbx_export_modal = false;
        static char    s_kdbx_export_pw1[256] = {};
        static char    s_kdbx_export_pw2[256] = {};
        static std::string s_kdbx_export_error;

        // --- Export intent ---
        if (g_shell.export_kdbx_clicked)
        {
            g_shell.export_kdbx_clicked = false;
            s_kdbx_export_modal = true;
            sodium_memzero(s_kdbx_export_pw1, sizeof(s_kdbx_export_pw1));
            sodium_memzero(s_kdbx_export_pw2, sizeof(s_kdbx_export_pw2));
            s_kdbx_export_error.clear();
            ImGui::OpenPopup("Export KDBX###kdbx_export_modal");
        }

        // --- Export modal ---
        if (s_kdbx_export_modal)
        {
            ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

            ImU32 dimBg = IM_COL32(0, 0, 0, 150);
            ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, dimBg);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));   // breathing room

            const float modalW = 380.0f;
            ImGui::SetNextWindowSizeConstraints(ImVec2(modalW, 0), ImVec2(modalW, FLT_MAX));

            bool modal_open = true;
            if (ImGui::BeginPopupModal("Export KDBX###kdbx_export_modal", &modal_open,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove))
            {
                bool escape_pressed = ImGui::IsKeyPressed(ImGuiKey_Escape);
                bool submit_shortcut = ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_Enter);

                ImGui::PushFont(render::FontLarge);
                ImGui::TextUnformatted(ICON_MDI_FILE_EXPORT "  Export KeePass Database");
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0, 12));

                const float fieldW = ImGui::GetContentRegionAvail().x;

                ImGui::TextDisabled("Export Password");
                ImGui::SetNextItemWidth(fieldW);
                ImGui::InputText("##kdbx_export_pw1", s_kdbx_export_pw1, sizeof(s_kdbx_export_pw1), ImGuiInputTextFlags_Password);

                ImGui::Dummy(ImVec2(0, 4));

                ImGui::TextDisabled("Confirm Password");
                ImGui::SetNextItemWidth(fieldW);
                ImGui::InputText("##kdbx_export_pw2", s_kdbx_export_pw2, sizeof(s_kdbx_export_pw2), ImGuiInputTextFlags_Password);

                // Error message
                if (!s_kdbx_export_error.empty()) {
                    ImGui::Dummy(ImVec2(0, 8));
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(174, 41, 41, 255));
                    ImGui::TextWrapped("%s", s_kdbx_export_error.c_str());
                    ImGui::PopStyleColor();
                }

                ImGui::Dummy(ImVec2(0, 12));

                // Buttons
                const float btnW = 100.0f;
                const float btnH = 32.0f;
                float totalBtnW = btnW * 2 + 8.0f;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + fieldW - totalBtnW);

                if (ui::StyledButton("##kdbx_exp_cancel", "Cancel", ImVec2(btnW, btnH)) || escape_pressed)
                {
                    sodium_memzero(s_kdbx_export_pw1, sizeof(s_kdbx_export_pw1));
                    sodium_memzero(s_kdbx_export_pw2, sizeof(s_kdbx_export_pw2));
                    s_kdbx_export_error.clear();
                    s_kdbx_export_modal = false;
                    ImGui::CloseCurrentPopup();
                }

                ImGui::SameLine(0, 8.0f);

                bool can_submit = s_kdbx_export_pw1[0] != '\0';
                bool pw_match = (strcmp(s_kdbx_export_pw1, s_kdbx_export_pw2) == 0);

                if (!can_submit) ImGui::BeginDisabled();
                bool do_export = ui::StyledButton("##kdbx_exp_go", "Export", ImVec2(btnW, btnH)) || (submit_shortcut && can_submit);
                if (!can_submit) ImGui::EndDisabled();

                if (do_export && can_submit)
                {
                    if (!pw_match) {
                        s_kdbx_export_error = "Passwords do not match.";
                    } else {
                        std::string path = PickSaveFilePath_KDBX();
                        if (!path.empty())
                        {
                            auto result = kdbx_export::export_kdbx(v.creds, std::string(s_kdbx_export_pw1), path.c_str());
                            sodium_memzero(s_kdbx_export_pw1, sizeof(s_kdbx_export_pw1));
                            sodium_memzero(s_kdbx_export_pw2, sizeof(s_kdbx_export_pw2));

                            if (result.ok) {
                                char msg[128];
                                snprintf(msg, sizeof(msg), "Exported %d credentials to KDBX", result.count);
                                ui::ShowToast(msg, ui::ToastType::Success);
                                s_kdbx_export_error.clear();
                                s_kdbx_export_modal = false;
                                ImGui::CloseCurrentPopup();
                            } else {
                                s_kdbx_export_error = "Export failed: " + result.error;
                            }
                        }
                    }
                }

                ImGui::EndPopup();
            }

            if (!modal_open)
            {
                sodium_memzero(s_kdbx_export_pw1, sizeof(s_kdbx_export_pw1));
                sodium_memzero(s_kdbx_export_pw2, sizeof(s_kdbx_export_pw2));
                s_kdbx_export_error.clear();
                s_kdbx_export_modal = false;
            }

            ImGui::PopStyleVar();   // WindowPadding
            ImGui::PopStyleColor();
        }
    }

    // ============================================================
    // CSV Import
    // ============================================================
    {
        static bool        s_csv_import_modal = false;
        static CsvImportResult s_csv_result;
        static std::string s_csv_import_error;

        // --- Import intent ---
        if (g_shell.import_csv_clicked)
        {
            g_shell.import_csv_clicked = false;
            std::string path = PickOpenFilePath_CSV();
            if (!path.empty())
            {
                std::string content = helpers::file_to_str(path);
                if (content.empty())
                {
                    s_csv_import_error = "Could not read file";
                    s_csv_result = {};
                }
                else
                {
                    s_csv_result = parse_csv_import(content);
                    sodium_memzero(content.data(), content.size());
                    s_csv_import_error = s_csv_result.error;
                }
                s_csv_import_modal = true;
                ImGui::OpenPopup("Import CSV###csv_import_modal");
            }
        }

        // --- Import modal ---
        if (s_csv_import_modal)
        {
            ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

            ImU32 dimBg = IM_COL32(0, 0, 0, 150);
            ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, dimBg);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));   // breathing room

            const float modalW = 400.0f;
            ImGui::SetNextWindowSizeConstraints(ImVec2(modalW, 0), ImVec2(modalW, FLT_MAX));

            bool modal_open = true;
            if (ImGui::BeginPopupModal("Import CSV###csv_import_modal", &modal_open,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove))
            {
                bool escape_pressed = ImGui::IsKeyPressed(ImGuiKey_Escape);
                bool submit_shortcut = ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_Enter);

                ImGui::PushFont(render::FontLarge);
                ImGui::TextUnformatted(ICON_MDI_FILE_DELIMITED "  Import CSV");
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0, 12));

                bool has_error = !s_csv_import_error.empty();
                bool has_creds = !s_csv_result.creds.empty();

                if (has_error)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(174, 41, 41, 255));
                    ImGui::TextWrapped("%s", s_csv_import_error.c_str());
                    ImGui::PopStyleColor();
                }
                else if (has_creds)
                {
                    ImGui::Text("Detected format: %s", CsvFormatName(s_csv_result.format));
                    ImGui::Dummy(ImVec2(0, 4));
                    ImGui::Text("Import %d credential%s?",
                        (int)s_csv_result.creds.size(),
                        s_csv_result.creds.size() == 1 ? "" : "s");
                    if (s_csv_result.skipped > 0)
                    {
                        ImGui::Dummy(ImVec2(0, 4));
                        ImGui::TextDisabled("(%d empty row%s skipped)",
                            s_csv_result.skipped,
                            s_csv_result.skipped == 1 ? "" : "s");
                    }
                }

                ImGui::Dummy(ImVec2(0, 12));

                // Buttons
                const float btnW = 100.0f;
                const float btnH = 32.0f;
                float totalBtnW = btnW * 2 + 8.0f;
                float fieldW = ImGui::GetContentRegionAvail().x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + fieldW - totalBtnW);

                if (ui::StyledButton("##csv_imp_cancel", "Cancel", ImVec2(btnW, btnH)) || escape_pressed)
                {
                    // Clear parsed Credential passwords
                    for (auto& c : s_csv_result.creds)
                        sodium_memzero(c.password.data(), c.password.size());
                    s_csv_result = {};
                    s_csv_import_error.clear();
                    s_csv_import_modal = false;
                    ImGui::CloseCurrentPopup();
                }

                ImGui::SameLine(0, 8.0f);

                if (!has_creds || has_error) ImGui::BeginDisabled();
                bool do_import = ui::StyledButton("##csv_imp_go", "Import", ImVec2(btnW, btnH)) || (submit_shortcut && has_creds && !has_error);
                if (!has_creds || has_error) ImGui::EndDisabled();

                if (do_import && has_creds && !has_error)
                {
                    // Content-keyed merge: reuse the UUID of an existing
                    // credential with the same site+email+username so
                    // re-imports update in place (via upsert_credential's
                    // newer-wins) instead of minting fresh UUIDs and
                    // creating duplicates. Unmatched rows still get a
                    // fresh UUID (cleared below).
                    std::unordered_map<std::string, std::string> sig_to_uuid;
                    for (const auto& ex : v.creds) {
                        if (ex.is_deleted()) continue;
                        const std::string sig = cred_signature(ex);
                        if (!sig.empty()) sig_to_uuid.emplace(sig, ex.uuid);
                    }
                    int merged = 0;
                    for (auto& c : s_csv_result.creds) {
                        const std::string sig = cred_signature(c);
                        auto it = sig.empty() ? sig_to_uuid.end() : sig_to_uuid.find(sig);
                        if (it != sig_to_uuid.end()) { c.uuid = it->second; ++merged; }
                        else                         { c.uuid.clear(); }
                    }

                    if (cred_ops::import_credentials(s_csv_result.creds, v.master_key))
                    {
                        v.creds = cred_ops::load_all(v.master_key);
                        rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);
                        ui::ForgetVaultRowState(GetActiveVaultKey());
                        VaultMarkChanged(v, "IMPORT");

                        char msg[160];
                        if (merged > 0)
                            snprintf(msg, sizeof(msg), "Imported %d from %s (%d merged into existing)",
                                (int)s_csv_result.creds.size(), CsvFormatName(s_csv_result.format), merged);
                        else
                            snprintf(msg, sizeof(msg), "Imported %d credentials from %s",
                                (int)s_csv_result.creds.size(), CsvFormatName(s_csv_result.format));
                        ui::ShowToast(msg, ui::ToastType::Success);

                        // Clear passwords
                        for (auto& c : s_csv_result.creds)
                            sodium_memzero(c.password.data(), c.password.size());

                        s_csv_result = {};
                        s_csv_import_error.clear();
                        s_csv_import_modal = false;
                        ImGui::CloseCurrentPopup();
                    }
                    else
                    {
                        s_csv_import_error = "Database error during import";
                    }
                }

                ImGui::EndPopup();
            }

            if (!modal_open)
            {
                for (auto& c : s_csv_result.creds)
                    sodium_memzero(c.password.data(), c.password.size());
                s_csv_result = {};
                s_csv_import_error.clear();
                s_csv_import_modal = false;
            }

            ImGui::PopStyleVar();   // WindowPadding
            ImGui::PopStyleColor();
        }
    }

    // ============================================================
    // Share Vault intent
    // ============================================================
    {
        // Thread-safe result buffer (bg thread writes, main thread reads once done)
        static std::atomic<bool> s_vault_share_busy{false};
        static std::atomic<bool> s_vault_share_done{false};
        static std::string s_vault_share_url;
        static std::string s_vault_share_err;

        // Poll for completed background share
        if (s_vault_share_done.load())
        {
            s_vault_share_done = false;
            g_shell.share_loading = false;
            if (!s_vault_share_url.empty())
                g_shell.share_result_url = s_vault_share_url;
            else
                g_shell.share_error = s_vault_share_err;
            s_vault_share_url.clear();
            s_vault_share_err.clear();
        }

        // Dispatch new share request
        if (g_shell.show_share_vault_modal && g_shell.share_loading && !s_vault_share_busy.load())
        {
            // Filter credentials by scope
            std::vector<Credential> to_share;
            for (const auto& c : v.creds)
            {
                if (c.is_deleted()) continue;

                if (g_shell.share_scope == 0)
                {
                    to_share.push_back(c);
                }
                else if (g_shell.share_scope == 1)
                {
                    if (c.group == g_shell.share_scope_value)
                        to_share.push_back(c);
                }
                else if (g_shell.share_scope == 2)
                {
                    if (CredTypeLabel(c.type) == g_shell.share_scope_value)
                        to_share.push_back(c);
                }
            }

            if (to_share.empty())
            {
                g_shell.share_loading = false;
                g_shell.share_error = "No credentials match the selected scope";
            }
            else
            {
                static const int expiry_seconds[] = { 3600, 86400, 604800, 2592000 };
                int exp_sec = expiry_seconds[ImClamp(g_shell.share_expiry, 0, 3)];

                static const int max_views_vals[] = { 0, 1, 5, 10 };
                int max_views = max_views_vals[ImClamp(g_shell.share_max_views, 0, 3)];

                std::string passphrase(g_shell.share_passphrase);
                std::string bundle_name = ActiveTab().label;

                s_vault_share_busy = true;
                std::thread([bundle_name, to_share, exp_sec, max_views, passphrase]() {
                    auto result = g_sync_mgr->share_vault_anon(to_share, bundle_name, exp_sec, max_views, passphrase);
                    if (result.success)
                        s_vault_share_url = result.url;
                    else
                        s_vault_share_err = result.error;
                    s_vault_share_busy = false;
                    s_vault_share_done = true;
                }).detach();
            }
        }
    }

    // Render Credential modal (Add/Edit)
    render_credential_modal(v);

    // Render Find Duplicates modal
    render_dedup_modal();

    // Render sync modal (Login/Register)
    render_sync_modal();

    // Render re-prompt modal (security)
    ui::RenderRepromptModal();

    // Render trash bin modal
    ui::RenderTrashModal(g_shell);

    // Render security center modal
    ui::RenderSecurityCenterModal(g_shell);

    // Render recovery key modal (shown once at vault creation)
    ui::RenderRecoveryKeyModal(g_shell);

    // Render settings modal
    ui::RenderSettingsPage(g_shell);

    // Render on-screen keyboard (foreground overlay — drawn above all widgets)
    ui::RenderOnScreenKeyboard();

    // If locked, render locked and bail
    if (g_shell.active_screen == ui::Screen::Locked)
    {
        render_locked_screen();
        // Keep retrying the shared bridge socket on the locked screen too, so
        // this app takes over when the cloud app quits. The unlocked path calls
        // it at the end of this function.
        TickBridge();
        if (render_scroll)
            ui::EndShellScroll();
        ui::EndShell();
        return;
    }

    ui::AccordionListResult r{};
    r.delete_id = -1;
    r.edit_commit_id = -1;
    const bool render_list = (g_shell.active_screen == ui::Screen::Unlocked);

    if (render_list)
    {
        if (v.creds.empty())
        {
            // ============================================================
            // Empty vault onboarding message
            // ============================================================
            ImVec2 avail   = ImGui::GetContentRegionAvail();
            float  startY  = ImGui::GetCursorPosY();

            const char* icon      = ICON_MDI_SHIELD_HALF_FULL;
            const char* primary   = "Your vault is empty";
            const char* secondary = "Click  " ICON_MDI_ACCOUNT_PLUS "  in the top right to add your first credential";

            // Measure each line with its font
            ImGui::PushFont(render::FontLarge);
            ImVec2 iconSz = ImGui::CalcTextSize(icon);
            ImGui::PopFont();

            ImGui::PushFont(render::FontRegular);
            ImVec2 primarySz = ImGui::CalcTextSize(primary);
            ImGui::PopFont();

            ImGui::PushFont(render::FontSmall);
            ImVec2 secondarySz = ImGui::CalcTextSize(secondary);
            ImGui::PopFont();

            float spacing = 6.0f;
            float totalH  = iconSz.y + spacing + primarySz.y + spacing + secondarySz.y;
            float yOff    = startY + (avail.y - totalH) * 0.4f; // slightly above center

            // Icon
            ImGui::SetCursorPos(ImVec2((avail.x - iconSz.x) * 0.5f, yOff));
            ImGui::PushFont(render::FontLarge);
            ImGui::TextDisabled("%s", icon);
            ImGui::PopFont();

            // Primary text
            yOff += iconSz.y + spacing;
            ImGui::SetCursorPos(ImVec2((avail.x - primarySz.x) * 0.5f, yOff));
            ImGui::PushFont(render::FontRegular);
            ImGui::TextDisabled("%s", primary);
            ImGui::PopFont();

            // Secondary text
            yOff += primarySz.y + spacing;
            ImGui::SetCursorPos(ImVec2((avail.x - secondarySz.x) * 0.5f, yOff));
            ImGui::PushFont(render::FontSmall);
            ImGui::TextDisabled("%s", secondary);
            ImGui::PopFont();
        }
        else
        {
            // ============================================================
            // Build list items
            // ============================================================
            const char* search = ui::GetSearchText();
            const int searchFilter = ui::GetSearchFilter();

            std::vector<ui::AccordionItem> items =
                build_accordion_items(
                    v.creds,
                    v.saved_snapshot,
                    search,
                    searchFilter,
                    g_shell.sort_mode,
                    g_shell.pill_tab_0,
                    g_shell.pill_tab_1,
                    g_shell.selected_groups,
                    g_shell.selected_tags,
                    g_shell.order_key,
                    g_shell.order_dir,
                    g_shell.group_mode,
                    g_shell.recent_items
                );

            // Populate sidebar badge counts from UNFILTERED credentials
            {
                int ca = 0, cp = 0, cf = 0, cPw = 0, cCd = 0, cId = 0, cNt = 0;
                std::unordered_map<std::string, int> gc;
                std::unordered_map<std::string, int> tc;
                std::unordered_map<std::string, std::vector<ui::ShellState::SidebarCredItem>> gi;
                for (const auto& c : v.creds)
                {
                    if (c.is_deleted()) continue;
                    ca++;
                    if (c.is_pinned)   cp++;
                    if (c.is_favorite) cf++;
                    switch (c.type) {
                        case CredType::Password:   cPw++; break;
                        case CredType::CreditCard: cCd++; break;
                        case CredType::Identity:   cId++; break;
                        case CredType::SecureNote: cNt++; break;
                    }
                    if (!c.group.empty())
                    {
                        gc[c.group]++;
                        gi[c.group].push_back({c.id, c.title, c.website, c.type, c.uuid});
                    }
                    for (const auto& t : c.tags)
                        if (!t.empty()) tc[t]++;
                }
                g_shell.sb_count_all       = ca;
                g_shell.sb_count_pinned    = cp;
                g_shell.sb_count_favorites = cf;
                g_shell.sb_count_passwords = cPw;
                g_shell.sb_count_cards     = cCd;
                g_shell.sb_count_identity  = cId;
                g_shell.sb_count_notes     = cNt;
                g_shell.sb_group_counts    = std::move(gc);
                g_shell.sb_group_items     = std::move(gi);
                g_shell.sb_tag_counts      = std::move(tc);
            }

            r = ui::RenderAccordionList(
                items,
                activeVaultKey,
                &GetPasswordForRow,
                g_shell.read_only,
                g_shell.view_mode
            );

            if (g_shell.select_all_clicked)
            {
                for (const auto& it : items)
                    if (!it.is_header)
                        ui::SelectRow(activeVaultKey, it.id);
                g_shell.select_all_clicked = false;
            }
        }
    }


    // ============================================================
    // HARD READ-ONLY GATE for commits coming back from UI
    // (this is the part you were missing)
    // ============================================================
    if (g_shell.read_only && render_list)
    {
        // If UI tried to commit anything, ignore it and notify.
        if (r.delete_id != -1 || r.edit_commit_id != -1 || r.edit_open_id != -1 ||
            r.toggle_pin_id != -1 || r.toggle_fav_id != -1)
        {
            v.set_status("Blocked: Read-only mode.", true);
            r.delete_id = -1;
            r.edit_commit_id = -1;
            r.edit_open_id = -1;
            r.toggle_pin_id = -1;
            r.toggle_fav_id = -1;
        }
    }

    // ============================================================
    // Security Center: feed selected Credential into edit pipeline
    // ============================================================
    if (g_shell.sec_center_edit_id != -1)
    {
        r.edit_open_id = g_shell.sec_center_edit_id;
        g_shell.sec_center_edit_id = -1;
    }

    // ============================================================
    // Handle edit request -> open modal
    // ============================================================
    if (render_list && r.edit_open_id != -1)
    {
        for (const auto& c : v.creds)
        {
            if (c.id == r.edit_open_id)
            {
                touch_recent(g_shell, c.uuid);
                g_cred_modal.OpenEdit(c, c.password);
                ImGui::OpenPopup("Add/Edit###cred_modal");
                break;
            }
        }
    }

    // ============================================================
    // Handle recent touch (three-pane selection)
    // ============================================================
    if (!g_shell.recent_touch_uuid.empty())
    {
        touch_recent(g_shell, g_shell.recent_touch_uuid);
        g_shell.recent_touch_uuid.clear();
    }

    // ============================================================
    // Handle anonymous share request — cloud-only feature.
    // ============================================================
#ifndef PD_OFFLINE_BUILD
    if (render_list && r.anon_share_id != -1)
    {
        if (g_sync_mgr && g_sync_mgr->is_logged_in())
        {
            for (const auto& c : v.creds)
            {
                if (c.id == r.anon_share_id)
                {
                    g_anon_share.Reset();
                    g_anon_share.cred = c;
                    g_anon_share.password = c.password;
                    ImGui::OpenPopup("Share Credential###anon_share_modal");
                    break;
                }
            }
        }
        else
        {
            ui::ShowToast("Sign in to share credentials", ui::ToastType::Error);
        }
    }
#endif

    // ============================================================
    // BULK INTENTS
    // ============================================================
    auto ApplyBulkToSelected = [&](auto&& fn)
        {
            std::vector<uint64_t> keys;
            ui::GetSelectedRowKeys(activeVaultKey, keys);
            if (keys.empty()) return false;

            v.push_undo();

            std::unordered_set<int> ids;
            ids.reserve(keys.size());
            for (uint64_t k : keys)
                ids.insert((int)(k & 0xFFFFFFFFu));

            for (auto& c : v.creds)
                if (ids.count(c.id))
                    fn(c);

            rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);

            VaultMarkChanged(v, "BULKEDIT");
            g_autosave.last_change_time = ImGui::GetTime();
            return true;
        };

    if (g_shell.clear_selection_clicked && render_list)
    {
        ui::ClearSelectionForVault(activeVaultKey);
        g_shell.clear_selection_clicked = false;
    }

    if (g_shell.bulk_delete_clicked && render_list)
    {
        std::vector<uint64_t> keys;
        ui::GetSelectedRowKeys(activeVaultKey, keys);

        if (!keys.empty())
        {
            std::unordered_set<int> ids;
            ids.reserve(keys.size());
            for (uint64_t k : keys)
                ids.insert((int)(k & 0xFFFFFFFFu));

            v.push_undo();
            CreatePreOpBackup(v, "BULKDELETE");

            // Soft-delete each in DB before removing from memory
            for (const auto& c : v.creds) {
                if (ids.count(c.id) > 0 && !c.uuid.empty()) {
                    cred_ops::remove(c.uuid);
                    v.user_removed_uuids.insert(c.uuid);  // ledger for save_all
                }
            }

            const size_t before = v.creds.size();
            v.creds.erase(std::remove_if(v.creds.begin(), v.creds.end(),
                [&](const Credential& c) { return ids.count(c.id) > 0; }),
                v.creds.end());

            ui::ClearSelectionForVault(activeVaultKey);
            ui::ForgetVaultRowState(activeVaultKey);

            rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);

            VaultMarkChanged(v, "BULKDELETE");
            g_autosave.last_change_time = ImGui::GetTime();

            const size_t removed = before - v.creds.size();
            v.set_status(("Moved " + std::to_string(removed) + " item(s) to trash.").c_str(), false);
            ui::ShowToast(("Moved " + std::to_string(removed) + " to trash").c_str(), ui::ToastType::Success);
        }

        g_shell.bulk_delete_clicked = false;
    }

    if (g_shell.bulk_pin_clicked && render_list)
    {
        if (ApplyBulkToSelected([](Credential& c) { c.is_pinned = true; }))
            v.set_status("Pinned selected.", false);
        g_shell.bulk_pin_clicked = false;
    }

    if (g_shell.bulk_unpin_clicked && render_list)
    {
        if (ApplyBulkToSelected([](Credential& c) { c.is_pinned = false; }))
            v.set_status("Unpinned selected.", false);
        g_shell.bulk_unpin_clicked = false;
    }

    if (g_shell.bulk_fav_clicked && render_list)
    {
        if (ApplyBulkToSelected([](Credential& c) { c.is_favorite = true; }))
            v.set_status("Favorited selected.", false);
        g_shell.bulk_fav_clicked = false;
    }

    if (g_shell.bulk_unfav_clicked && render_list)
    {
        if (ApplyBulkToSelected([](Credential& c) { c.is_favorite = false; }))
            v.set_status("Unfavorited selected.", false);
        g_shell.bulk_unfav_clicked = false;
    }

    if (g_shell.bulk_set_group_clicked && render_list)
    {
        std::string target = "Filter";
        if (g_shell.bulk_group_index >= 0 && g_shell.bulk_group_index < (int)g_shell.groups.size())
            target = g_shell.groups[g_shell.bulk_group_index];

        // Reject non-group entries (Filter, @-prefixed types, separator)
        if (target == "Filter" || (!target.empty() && target[0] == '@') || target == "---")
        {
            v.set_status("Pick a real group.", true);
        }
        else
        {
            if (ApplyBulkToSelected([&](Credential& c) { c.group = target; }))
                v.set_status("Updated group for selected.", false);
        }

        g_shell.bulk_set_group_clicked = false;
    }

    // Bulk tag add
    if (!g_shell.bulk_tag_to_add.empty() && render_list)
    {
        const std::string tag = g_shell.bulk_tag_to_add;
        if (ApplyBulkToSelected([&](Credential& c) {
            // Add tag if not already present
            bool exists = false;
            for (const auto& t : c.tags)
                if (t == tag) { exists = true; break; }
            if (!exists)
                c.tags.push_back(tag);
        }))
            v.set_status("Added tag to selected.", false);
        g_shell.bulk_tag_to_add.clear();
    }

    // Bulk tag remove
    if (!g_shell.bulk_tag_to_remove.empty() && render_list)
    {
        const std::string tag = g_shell.bulk_tag_to_remove;
        if (ApplyBulkToSelected([&](Credential& c) {
            c.tags.erase(std::remove(c.tags.begin(), c.tags.end(), tag), c.tags.end());
        }))
            v.set_status("Removed tag from selected.", false);
        g_shell.bulk_tag_to_remove.clear();
    }

    // Drag-drop: credential → group
    if (g_shell.drag_drop_cred_id >= 0 && !g_shell.drag_drop_target_group.empty())
    {
        v.push_undo();
        for (auto& c : v.creds)
        {
            if (c.id == g_shell.drag_drop_cred_id)
            {
                c.group = g_shell.drag_drop_target_group;
                c.updated_at_ms = helpers::now_unix_ms();
                cred_ops::update(c.uuid, c, v.master_key);
                break;
            }
        }
        rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);
        VaultMarkChanged(v, "DRAG_GROUP");
        g_autosave.last_change_time = ImGui::GetTime();
        g_shell.drag_drop_cred_id = -1;
        g_shell.drag_drop_target_group.clear();
    }

    // ============================================================
    // Per-row delete/edit commits (now safe due to read-only gate)
    // ============================================================
    if (render_list && r.delete_id != -1)
    {
        CreatePreOpBackup(v, "DELETE");
        v.push_undo();

        // Soft-delete in DB before removing from memory
        for (const auto& c : v.creds) {
            if (c.id == r.delete_id && !c.uuid.empty()) {
                // Remove from recent items
                std::erase_if(g_shell.recent_items, [&](const ui::ShellState::RecentEntry& e) {
                    return e.uuid == c.uuid;
                });
                persist_recent_items(g_shell);
                cred_ops::remove(c.uuid);
                v.user_removed_uuids.insert(c.uuid);  // ledger for save_all
                break;
            }
        }

        // Remove Credential from UI
        v.creds.erase(
            std::remove_if(v.creds.begin(), v.creds.end(),
                [&](const Credential& c) { return c.id == r.delete_id; }),
            v.creds.end());

        VaultMarkChanged(v, "DELETE");

        ui::ForgetRowState(activeVaultKey, r.delete_id);

        g_autosave.last_change_time = ImGui::GetTime();

        rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);
        v.set_status("Moved to trash.", false);
        ui::ShowToast("Moved to trash", ui::ToastType::Success);
    }

    if (render_list && r.toggle_pin_id != -1)
    {
        for (auto& c : v.creds)
        {
            if (c.id == r.toggle_pin_id)
            {
                v.push_undo();
                c.is_pinned = !c.is_pinned;
                c.updated_at_ms = helpers::now_unix_ms();
                VaultMarkChanged(v, "PIN");
                g_autosave.last_change_time = ImGui::GetTime();
                break;
            }
        }
    }

    if (render_list && r.toggle_fav_id != -1)
    {
        for (auto& c : v.creds)
        {
            if (c.id == r.toggle_fav_id)
            {
                v.push_undo();
                c.is_favorite = !c.is_favorite;
                c.updated_at_ms = helpers::now_unix_ms();
                VaultMarkChanged(v, "FAV");
                g_autosave.last_change_time = ImGui::GetTime();
                break;
            }
        }
    }

    if (render_list && r.edit_commit_id != -1)
    {
        v.push_undo();

        // Update Credential by ID
        for (auto& c : v.creds)
        {
            if (c.id == r.edit_commit_id)
            {
                c.title = r.edited.title;
                c.user = r.edited.user;
                c.email = r.edited.email;
                c.website = r.edited.website;
                c.group = r.edited.group;
                c.notes = r.edited.notes;
                c.is_pinned = r.edited.is_pinned;
                c.is_favorite = r.edited.is_favorite;
                c.expires_at_ms = r.edited.expires_at_ms;
                c.expiry_action = r.edited.expiry_action;
                c.password = r.edited_password;
                // Type-specific fields
                c.card_number     = r.edited.card_number;
                c.card_expiry     = r.edited.card_expiry;
                c.card_cvv        = r.edited.card_cvv;
                c.card_brand      = r.edited.card_brand;
                c.cardholder_name = r.edited.cardholder_name;
                c.card_address     = r.edited.card_address;
                c.card_city        = r.edited.card_city;
                c.card_postal_code = r.edited.card_postal_code;
                c.full_name     = r.edited.full_name;
                c.id_type       = r.edited.id_type;
                c.id_number     = r.edited.id_number;
                c.date_of_birth = r.edited.date_of_birth;
                c.expiry_date   = r.edited.expiry_date;
                c.country       = r.edited.country;
                c.address       = r.edited.address;
                c.phone         = r.edited.phone;
                c.updated_at_ms = helpers::now_unix_ms();
                break;
            }
        }

        VaultMarkChanged(v, "EDIT");
        g_autosave.last_change_time = ImGui::GetTime();

        rebuild_groups(g_shell, v.creds); rebuild_tags(g_shell, v.creds);
        v.set_status("Saved changes (in memory). Ctrl+S to write to disk.", false);
    }

    // ============================================================
    // Anonymous Share Modal
    // ============================================================
    {
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        const bool dark = g_shell.dark_theme;
        const ImU32 popupBg = dark ? IM_COL32(36, 35, 39, 255) : IM_COL32(250, 250, 252, 255);
        const ImU32 dimBg   = IM_COL32(0, 0, 0, 140);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, popupBg);
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, dimBg);

        ImGui::SetNextWindowSizeConstraints(ImVec2(420, 0), ImVec2(420, FLT_MAX));
        bool modal_open = true;
        if (ImGui::BeginPopupModal("Share Credential###anon_share_modal", &modal_open,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove))
        {
            bool escape_pressed = ImGui::IsKeyPressed(ImGuiKey_Escape);

            // Header with close button
            ImGui::TextUnformatted(ICON_MDI_SHARE_VARIANT "  Share Credential");
            {
                float closeSize = ImGui::GetFrameHeight();
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - closeSize);
                if (ui::StyledButton("##share_close", ICON_MDI_CLOSE, ImVec2(closeSize, closeSize)) || escape_pressed)
                {
                    ui::ReleaseShareQRTexture();
                    g_anon_share.Reset();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::Dummy(ImVec2(0, 8));

            static const char* expiry_labels[] = { "1 hour", "24 hours", "7 days", "30 days", "Never" };
            static const char* views_labels[] = { "1 view", "5 views", "10 views", "Unlimited" };

            switch (g_anon_share.step)
            {
            case AnonShareModal::Step::Config:
            {
                ImGui::TextWrapped("Create a shareable link for this credential. "
                    "The encryption key stays in the URL fragment and is never sent to the server.");
                ImGui::Dummy(ImVec2(0, 8));

                ImGui::TextDisabled("Credential:");
                ImGui::SameLine();
                ImGui::TextUnformatted(DisplayTitle(g_anon_share.cred.title));
                ImGui::Dummy(ImVec2(0, 8));

                ImGui::TextDisabled("Expires after");
                ui::AnimatedComboDot("##share_expiry",
                    expiry_labels[g_anon_share.expiry_idx],
                    expiry_labels, IM_ARRAYSIZE(expiry_labels),
                    &g_anon_share.expiry_idx, 120.0f, 30.0f);

                ImGui::Dummy(ImVec2(0, 4));

                ImGui::TextDisabled("Max views");
                ui::AnimatedComboDot("##share_views",
                    views_labels[g_anon_share.views_idx],
                    views_labels, IM_ARRAYSIZE(views_labels),
                    &g_anon_share.views_idx, 120.0f, 30.0f);

                ImGui::Dummy(ImVec2(0, 8));

                if (ui::StyledButton("##create_share_link", ICON_MDI_SHARE_VARIANT " Create Link", ImVec2(-1, 32), 4.0f))
                {
                    g_anon_share.step = AnonShareModal::Step::Creating;
                    g_anon_share.creating = true;
                    std::thread(DoCreateAnonShare).detach();
                }
                break;
            }
            case AnonShareModal::Step::Creating:
            {
                ImGui::TextUnformatted("Creating share link...");
                ImGui::Dummy(ImVec2(0, 4));
                float spinner_radius = 8.0f;
                ImVec2 pos = ImGui::GetCursorScreenPos();
                float t = (float)ImGui::GetTime();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
                float a_start = t * 3.0f;
                dl->PathArcTo(ImVec2(pos.x + spinner_radius + 4, pos.y + spinner_radius),
                    spinner_radius, a_start, a_start + IM_PI * 1.5f, 12);
                dl->PathStroke(col, false, 2.0f);
                ImGui::Dummy(ImVec2(spinner_radius * 2 + 8, spinner_radius * 2));
                break;
            }
            case AnonShareModal::Step::Done:
            {
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Share link created!");
                ImGui::Dummy(ImVec2(0, 8));

                ImGui::TextDisabled("Share URL:");
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 4));
                ImGui::TextWrapped("%s", g_anon_share.share_url.c_str());
                ImGui::PopStyleVar();

                ImGui::Dummy(ImVec2(0, 8));

                if (ui::StyledButton("##copy_share_url", ICON_MDI_CONTENT_COPY " Copy URL", ImVec2(-1, 30), 4.0f))
                {
                    ImGui::SetClipboardText(g_anon_share.share_url.c_str());
                    ui::ShowToast("Share URL copied!", ui::ToastType::Success);
                }

                ImGui::Dummy(ImVec2(0, 8));

                // QR Code
                ImTextureID qr_tex = ui::GetShareQRTexture();
                if (!qr_tex && !g_anon_share.share_url.empty())
                {
                    ui::CreateShareQRTexture(g_anon_share.share_url);
                    qr_tex = ui::GetShareQRTexture();
                }
                if (qr_tex)
                {
                    float qr_display_size = 200.0f;
                    float avail = ImGui::GetContentRegionAvail().x;
                    float offset = (avail - qr_display_size) * 0.5f;
                    if (offset > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
                    ImGui::Image(qr_tex, ImVec2(qr_display_size, qr_display_size));
                }
                break;
            }
            case AnonShareModal::Step::Error:
            {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error");
                ImGui::TextWrapped("%s", g_anon_share.error_msg.c_str());
                ImGui::Dummy(ImVec2(0, 8));

                if (ui::StyledButton("##retry_share", ICON_MDI_REFRESH " Try Again", ImVec2(-1, 30), 4.0f))
                {
                    g_anon_share.step = AnonShareModal::Step::Config;
                    g_anon_share.error_msg.clear();
                }
                break;
            }
            }

            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);

        // Cleanup when modal is closed
        if (!modal_open && g_anon_share.step != AnonShareModal::Step::Config)
        {
            ui::ReleaseShareQRTexture();
            g_anon_share.Reset();
        }
    }

    // ============================================================
    // Bridge write queue — drain pending save_new / update commands
    // posted by browser extension worker threads. Each pending write
    // must have its response promise fulfilled before we exit this
    // function, otherwise the worker's future.wait_for() times out.
    // ============================================================
    {
        auto writes = g_bridge.DrainPendingWrites();
        if (!writes.empty()) {
            VaultState& bv = ActiveVault();
            for (auto& w : writes) {
                std::string resp = PerformBridgeWrite(bv, w);
                w.response.set_value(std::move(resp));
            }
        }
    }

    // (vault_switch from the extension is deferred — the native-messaging
    // protocol doesn't expose it today. If needed, add a vault_switch
    // write action to BridgeServer; the UI thread already owns the full
    // VaultState so the switch logic drops in next to PerformBridgeWrite.)

    // ============================================================
    // Autosave tick + Auto-lock tick + end shell
    // ============================================================
    TickAutosave();
    TickAutoLock();
    TickBridge();
    TickCredentialExpiry();

    if (render_scroll)
        ui::EndShellScroll();
    ui::EndShell();

    // Style editor (floating window)
    ui::RenderAppStyleEditor(g_shell);

    // ============================================================
    // Back navigation
    // ============================================================
    if (g_shell.back_clicked)
    {
        g_shell.back_clicked = false;
        PopScreen(g_shell);
    }

    // ============================================================
    // Handle footer Open/New intents
     // ============================================================
    if (g_shell.footer_open_db_clicked) HandleOpenDB();
    if (g_shell.footer_new_db_clicked)  HandleNewDB();

    if (g_shell.open_db_clicked) HandleOpenDB();
    if (g_shell.new_db_clicked)  HandleNewDB();

    // ============================================================
    // Footer Save (guarded)
    // ============================================================
    if (g_shell.footer_save_clicked && !g_shell.read_only)
    {
        save_vault_to_disk(ActiveVault());
        g_autosave.last_save_time = ImGui::GetTime();
        stamp_saved_status();
    }
    g_shell.footer_save_clicked = false; // consume


    // ============================================================
    // Settings toggle
    // ============================================================
    if (g_shell.settings_clicked)
    {
        g_shell.settings_modal_open = true;
    }

    // ============================================================
    // Always on Top toggle
    // ============================================================
    if (g_shell.always_on_top_changed)
    {
        g_shell.always_on_top_changed = false;
#ifdef _WIN32
        ::SetWindowPos(render::hwnd,
            g_shell.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
        render::SetAlwaysOnTop(g_shell.always_on_top);
#endif
    }

    // ============================================================
    // System tray events
    // ============================================================
    // Sync minimize_to_tray flag
    render::SetMinimizeToTray(g_shell.minimize_to_tray);

    if (render::ConsumeTrayLock())
    {
        // Trigger vault lock (same path as close-anyway)
        g_shell.footer_close_anyway = true;
    }

    if (render::ConsumeTrayQuit())
    {
        render::RemoveTrayIcon();
#ifdef _WIN32
        ::PostQuitMessage(0);
#else
        render::RequestAppQuit();
#endif
    }

    // ============================================================
    // Go to locked screen (from settings "Create New Vault")
    // ============================================================
    if (g_shell.goto_locked_clicked)
    {
        g_shell.goto_locked_clicked = false;

        VaultState& vv = ActiveVault();

        // Close current database
        vault_db::close();

        // Clear sensitive data (zero before free)
        vv.clear_sensitive();
        secure_clear_credentials(vv.creds);
        secure_clear_undo_stack(vv.undo_stack);
        g_bridge.Lock();
        g_bridge.PushEvent(R"({"event":"vault_locked"})");
        vv.vault_path.clear();

        // Clear re-prompt security state
        ui::SetRepromptMasterPassword("");
        ui::ResetRepromptLockout();

        // Reset to locked screen
        ResetScreen(g_shell, ui::Screen::Locked);
    }

    // ============================================================
    // Close clicked / confirm flow
    // ============================================================
    if (g_shell.footer_close_clicked)
    {
        if (ActiveVault().is_dirty())
            g_shell.footer_close_confirming = true;
        else
            g_shell.footer_close_anyway = true;

        g_shell.footer_close_clicked = false; // consume
    }

    if (g_shell.footer_close_anyway)
    {
        g_shell.footer_close_anyway = false;
        g_shell.settings_modal_open = false;

        // Tell connected extensions the vault is closing.
        g_bridge.Lock();
        g_bridge.PushEvent(R"({"event":"vault_locked"})");

        VaultState& vv = ActiveVault();
        uint32_t vk = GetActiveVaultKey();

        ui::ForgetVaultRowState(vk);

        // Clear re-prompt security state
        ui::SetRepromptMasterPassword("");
        ui::ResetRepromptLockout();

        g_tabs.erase(g_tabs.begin() + g_active_tab);
        if (g_tabs.empty())
            g_tabs.push_back({ "Vault", VaultState{} });

        g_active_tab = ImClamp(g_active_tab, 0, (int)g_tabs.size() - 1);

        SyncShellTabsFromVaultTabs();
        g_shell.active_db = g_active_tab;
    }

    // ============================================================
    // Delete vault
    // ============================================================
    if (g_shell.delete_vault_clicked)
    {
        g_shell.delete_vault_clicked = false;
        g_shell.delete_vault_confirming = true;
    }

    if (g_shell.delete_vault_confirming)
    {
        ImGui::OpenPopup("Delete Vault?###delete_vault_modal");
    }

    {
        bool modal_open = g_shell.delete_vault_confirming;

        // Theme-aware modal colors (matching credential modal)
        const bool dark = g_shell.dark_theme;
        const ImU32 popupBg = dark
            ? IM_COL32(36, 35, 39, 255)
            : IM_COL32(250, 250, 252, 255);
        const ImU32 dimBg = IM_COL32(0, 0, 0, 140);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, popupBg);
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, dimBg);

        const float modalW = 420.0f;
        ImGui::SetNextWindowSizeConstraints(ImVec2(modalW, 0), ImVec2(modalW, FLT_MAX));

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

        if (ImGui::BeginPopupModal("Delete Vault?###delete_vault_modal", &modal_open,
                                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove))
        {
            // Header with close button
            ImGui::PushFont(render::FontLarge);
            ImGui::TextUnformatted("Delete Vault");
            ImGui::PopFont();

            ImVec2 closeSz = ImGui::CalcTextSize(ICON_MDI_CLOSE);
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - closeSz.x);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(ICON_MDI_CLOSE);
            ImGui::PopStyleColor();
            if ((ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                g_shell.delete_vault_confirming = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            VaultState& vv = ActiveVault();
            int cred_count = 0;
            for (const auto& c : vv.creds)
                if (!c.is_deleted()) cred_count++;

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextWrapped("This will permanently delete the vault database and all %d credentials. This cannot be undone.", cred_count);
            ImGui::PopStyleColor();
            ImGui::Spacing();

            static char s_confirm_buf[64] = "";
            ImGui::Text("Type DELETE to confirm:");
            ImGui::Spacing();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##delete_confirm", s_confirm_buf, sizeof(s_confirm_buf));
            ImGui::Spacing();
            ImGui::Spacing();

            bool can_delete = (strcmp(s_confirm_buf, "DELETE") == 0);

            if (!can_delete) ImGui::BeginDisabled();
            if (ui::StyledButton("##del_vault_go", "Delete Vault", ImVec2(-1, 36)))
            {
                g_shell.delete_vault_confirmed = true;
                g_shell.delete_vault_confirming = false;
                s_confirm_buf[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            if (!can_delete) ImGui::EndDisabled();

            ImGui::Spacing();
            if (ui::StyledButton("##del_vault_cancel", "Cancel", ImVec2(-1, 30)))
            {
                g_shell.delete_vault_confirming = false;
                s_confirm_buf[0] = '\0';
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);

        if (!modal_open)
        {
            g_shell.delete_vault_confirming = false;
        }
    }

    if (g_shell.delete_vault_confirmed)
    {
        g_shell.delete_vault_confirmed = false;

        VaultState& vv = ActiveVault();
        std::string path_to_delete = vv.vault_path;

        // Tell connected extensions the vault is going away.
        g_bridge.Lock();
        g_bridge.PushEvent(R"({"event":"vault_locked"})");

        // Clear re-prompt security state
        ui::SetRepromptMasterPassword("");
        ui::ResetRepromptLockout();

        uint32_t vk = GetActiveVaultKey();
        ui::ForgetVaultRowState(vk);

        // Close the vault from tabs
        g_tabs.erase(g_tabs.begin() + g_active_tab);
        if (g_tabs.empty())
            g_tabs.push_back({ "Vault", VaultState{} });

        g_active_tab = ImClamp(g_active_tab, 0, (int)g_tabs.size() - 1);
        SyncShellTabsFromVaultTabs();
        g_shell.active_db = g_active_tab;

        // Close the database connection
        vault_db::close();

        // Delete the .db file and companions
        if (!path_to_delete.empty())
        {
            std::remove(path_to_delete.c_str());
            std::remove((path_to_delete + "-wal").c_str());
            std::remove((path_to_delete + "-shm").c_str());
        }

        // Clear saved db_path if it was this vault
        if (cfg::_path == path_to_delete)
        {
            cfg::_path.clear();
            cfg::update_db_path();
        }

        ui::ShowToast("Vault deleted permanently", ui::ToastType::Info);
    }

    // ============================================================
    // Restore selected backup
    // ============================================================
    if (g_shell.footer_restore_clicked)
    {
        g_shell.footer_restore_clicked = false;

        VaultState& vv = ActiveVault();

        if (vv.is_dirty())
        {
            vv.set_status("Restore blocked: you have unsaved changes. Save or Undo first.", true);
            return;
        }

        const std::string& backup_path = g_shell.footer_browse_backup_path;
        if (!backup_path.empty())
        {

            CreatePreOpBackup(vv, "RESTORE");

            if (!helpers::CopyFileAtomic(backup_path, vv.vault_path))
            {
                vv.set_status("Restore failed (copy error).", true);
            }
            else
            {
                if (!load_vault_from_disk(vv.vault_path, vv.session_password, vv))
                {
                    vv.set_status("Restore applied, but reload failed (password mismatch?).", true);
                }
                else
                {
                    // Integrity check on restored vault
                    if (!vault_db::quick_integrity_check())
                        vv.set_status("Warning: restored database may be corrupted.", true);

                    ui::ForgetVaultRowState(GetActiveVaultKey());
                    rebuild_groups(g_shell, vv.creds); rebuild_tags(g_shell, vv.creds);

                    vv.set_status("Restored backup.", false);
                    g_autosave.last_change_time = ImGui::GetTime();
                    g_autosave.last_save_time = ImGui::GetTime();
                }
            }
        }
    }
}

void render::DisplayProgram()
{
    // One-time initialization
    static bool s_first_frame = true;
    if (s_first_frame)
    {
        s_first_frame = false;
        g_shell.dark_theme = cfg::is_dark_theme();

        // Load custom card colors
        {
            auto cd = cfg::get_card_bg_dark();
            g_shell.card_bg_dark = ImVec4(cd.r, cd.g, cd.b, cd.a);
            auto cl = cfg::get_card_bg_light();
            g_shell.card_bg_light = ImVec4(cl.r, cl.g, cl.b, cl.a);
        }

        // Load extended color customization
        {
            auto load_color = [](ImVec4& dst, cfg::ColorRGBA c) {
                dst = ImVec4(c.r, c.g, c.b, c.a);
            };
            load_color(g_shell.card_header_bg_dark,  cfg::get_card_header_bg_dark());
            load_color(g_shell.card_header_bg_light, cfg::get_card_header_bg_light());
            load_color(g_shell.card_body_bg_dark,    cfg::get_card_body_bg_dark());
            load_color(g_shell.card_body_bg_light,   cfg::get_card_body_bg_light());
            load_color(g_shell.soft_container_dark,   cfg::get_soft_container_dark());
            load_color(g_shell.soft_container_light,  cfg::get_soft_container_light());
            load_color(g_shell.controls_bg_dark,      cfg::get_controls_bg_dark());
            load_color(g_shell.controls_bg_light,     cfg::get_controls_bg_light());
            load_color(g_shell.window_bg_dark,        cfg::get_window_bg_dark());
            load_color(g_shell.window_bg_light,       cfg::get_window_bg_light());
        }

        // Load font scale and privacy mode
        g_shell.font_scale = cfg::get_font_scale();
        ImGui::GetIO().FontGlobalScale = g_shell.font_scale;
        g_shell.privacy_mode = cfg::get_privacy_mode();
        g_shell.blur_on_unfocus = cfg::get_blur_on_unfocus();
        g_shell.online_favicons = cfg::get_online_favicons();
        g_shell.online_breach_check = cfg::get_online_breach_check();
        favicon::SetNetworkEnabled(g_shell.online_favicons);
        load_recent_items(g_shell);

        // Load password aging threshold
        g_shell.password_max_age_days = cfg::get_password_max_age_days();

        // Drop the old localhost-extension keys from the config file.
        cfg::migrate_remove_legacy_local_server_keys();

        // Start the native-messaging bridge. Always on (no toggle): a
        // user-only Unix socket with no network reach. If the cloud app is
        // already serving the shared socket, TickBridge takes over later.
        g_bridge.Start();
    }

    ui::OskPreFrame(); // Intercept clicks over the on-screen keyboard before widgets see them
    ui::Initialize();
    ActiveTab(); // ensure tab exists

    // Fire the version-check on first frame (no-op after that). Banner appears
    // on top of everything via render_update_banner() below.
    // Offline builds don't phone home for version info — skip the check and
    // the banner render entirely.
#ifndef PD_OFFLINE_BUILD
    kick_off_update_check();
#endif

    // Always draw shell; inside it, draw locked or unlocked for ActiveVault()
    render_unlocked_screen(); // (this function now handles locked internally)

#ifndef PD_OFFLINE_BUILD
    // Update banner is drawn LAST so it floats over modals/shell content.
    render_update_banner();
#endif

    // Blur overlay when app loses focus
    if (g_shell.blur_on_unfocus && !render_app_is_active())
    {
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ds);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0.85f));
        ImGui::Begin("##unfocus_blur", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoInputs);
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
}

