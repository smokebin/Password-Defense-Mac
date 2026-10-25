// sync_manager_stub.mm — Offline-build stub for SyncManager.
//
// Compiled INSTEAD of sync_manager.cpp in the PD_OFFLINE_BUILD target, so the
// offline app links with zero cloud/networking code. Every method is a no-op
// that reports "offline": is_logged_in() is always false, status is always
// Offline, and any sync/share/cloud/version call returns a benign failure.
// Mirrors the existing favicon_stub.mm pattern.
//
// KEEP IN SYNC WITH sync_manager.h. If the header gains/changes a method and
// this file doesn't, the offline target won't link until you add the matching
// no-op here. (Private helpers — parse_url, set_status, authenticated_request,
// push_local_changes, pull_remote_changes, extract_error_message — are only
// called inside the real .cpp, so they are intentionally NOT defined here.)

#include "sync_manager.h"
#include <utility>

namespace {
constexpr const char* kOfflineMsg = "Offline build — cloud sync is disabled";
}

// --- Free-standing HTTP helper -------------------------------------------
HttpResponse win_http_request(
    const std::string& /*method*/, const std::string& /*host*/, int /*port*/,
    bool /*use_ssl*/, const std::string& /*path*/, const std::string& /*body*/,
    const std::string& /*bearer_token*/) {
    HttpResponse r;
    r.success = false;
    r.status_code = 0;
    r.error = kOfflineMsg;
    return r;
}

// --- Construction ---------------------------------------------------------
SyncManager::SyncManager(const std::string& api_base_url)
    : m_base_url(api_base_url) {
    m_status.store(SyncStatus::Offline);
}
SyncManager::~SyncManager() {}

// --- Configuration --------------------------------------------------------
void SyncManager::set_base_url(const std::string& url) { m_base_url = url; }
void SyncManager::set_timeout_seconds(int seconds) { m_timeout_sec = seconds; }

// --- Callbacks ------------------------------------------------------------
void SyncManager::set_status_callback(StatusCallback cb) { m_status_callback = std::move(cb); }
void SyncManager::set_sync_complete_callback(SyncCompleteCallback cb) { m_sync_complete_callback = std::move(cb); }

// --- Authentication -------------------------------------------------------
AuthResult SyncManager::login(const std::string&, const std::string&) {
    AuthResult r; r.success = false; r.message = kOfflineMsg; return r;
}
AuthResult SyncManager::register_user(const std::string&, const std::string&, const std::string&) {
    AuthResult r; r.success = false; r.message = kOfflineMsg; return r;
}
void SyncManager::logout() {
    m_access_token.clear();
    m_refresh_token.clear();
    m_username.clear();
}
bool SyncManager::is_logged_in() const { return false; }
std::string SyncManager::get_username() const { return {}; }

// --- Token management -----------------------------------------------------
void SyncManager::set_access_token(const std::string& token) { m_access_token = token; }
std::string SyncManager::get_access_token() const { return m_access_token; }
void SyncManager::set_refresh_token(const std::string& token) { m_refresh_token = token; }
std::string SyncManager::get_refresh_token() const { return m_refresh_token; }

void SyncManager::save_tokens_to_db() {}
bool SyncManager::load_tokens_from_db() { return false; }
void SyncManager::clear_tokens_from_db() {}

bool SyncManager::refresh_access_token() { return false; }

// --- Salt sync ------------------------------------------------------------
bool SyncManager::fetch_server_salt(std::vector<uint8_t>&) { return false; }
bool SyncManager::upload_salt(const std::vector<uint8_t>&) { return false; }
bool SyncManager::upload_salt_with_error(const std::vector<uint8_t>&, std::string& out_error) {
    out_error = kOfflineMsg;
    return false;
}

// --- Generic authenticated request (external callers, e.g. share) ---------
::HttpResponse SyncManager::make_auth_request(const std::string&, const std::string&, const std::string&) {
    ::HttpResponse r; r.success = false; r.status_code = 0; r.error = kOfflineMsg; return r;
}

// --- Share ----------------------------------------------------------------
SyncManager::ShareStatus SyncManager::get_share_status(const std::string&) { return {}; }
SyncManager::ShareResult SyncManager::share_vault_anon(
    const std::vector<Credential>&, const std::string&, int, int, const std::string&) {
    ShareResult r; r.success = false; r.error = kOfflineMsg; return r;
}

// --- Cloud multi-vault ----------------------------------------------------
SyncManager::CloudVaultResult SyncManager::list_cloud_vaults() {
    CloudVaultResult r; r.success = false; r.message = kOfflineMsg; return r;
}
SyncManager::CloudVaultResult SyncManager::create_cloud_vault(const std::string&, const std::string&, const std::string&) {
    CloudVaultResult r; r.success = false; r.message = kOfflineMsg; return r;
}
SyncManager::CloudVaultResult SyncManager::delete_cloud_vault(const std::string&) {
    CloudVaultResult r; r.success = false; r.message = kOfflineMsg; return r;
}

// --- App version / update check -------------------------------------------
SyncManager::AppVersionInfo SyncManager::check_app_version(const std::string&) {
    AppVersionInfo r; r.ok = false; return r;  // fail-open: offline shows nothing
}

// --- Synchronization ------------------------------------------------------
SyncResult SyncManager::perform_full_sync() {
    SyncResult r; r.success = false; r.message = kOfflineMsg; return r;
}
void SyncManager::start_background_sync() {}

// --- Status ---------------------------------------------------------------
SyncStatus SyncManager::get_status() const { return SyncStatus::Offline; }
std::string SyncManager::get_last_message() const { return kOfflineMsg; }
bool SyncManager::is_syncing() const { return false; }
