// bridge_server.cpp — see header for architecture overview.

#include "bridge_server.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <sodium.h>
#include "json.hpp"

// -----------------------------------------------------------------------------
// Helpers (file-local)
// -----------------------------------------------------------------------------
namespace {

// Minimal port of local_server.cpp's extractHostname. Lower-cases, strips
// scheme + path, drops a leading "www.". Public-suffix matching is out of
// scope for Phase 1 — same shortcut the HTTP server took.
std::string ExtractHost(const std::string& url_or_domain)
{
    auto scheme_end = url_or_domain.find("://");
    size_t start = (scheme_end != std::string::npos) ? scheme_end + 3 : 0;
    size_t end = url_or_domain.find_first_of(":/", start);
    std::string host = url_or_domain.substr(start,
        end == std::string::npos ? end : end - start);
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (host.size() > 4 && host.compare(0, 4, "www.") == 0)
        host.erase(0, 4);
    return host;
}

bool CaseInsensitiveContains(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return false;
    auto it = std::search(
        hay.begin(), hay.end(),
        needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
        });
    return it != hay.end();
}

std::string DefaultSocketPath()
{
    // Lives in the App Group container so Safari's sandboxed extension can
    // reach it. Both pw-mac-cloud and the Safari extension declare
    // "group.com.StackedSoftware.PasswordDefense" in their entitlements;
    // macOS creates / owns this dir for signed app-group members. For the
    // unsandboxed pw-mac-cloud process we just mkdir -p into it.
    const char* home = std::getenv("HOME");
    std::string base = (home && *home) ? home : "/tmp";
    base += "/Library/Group Containers/group.com.StackedSoftware.PasswordDefense/bridge.sock";
    return base;
}

constexpr size_t kMaxFrameBytes = 1 * 1024 * 1024;  // 1 MiB

// Full credential envelope sent in match / search / fill responses. The
// extension's content.js picker holds the entire cred object from match
// and expects to find cred.password on it when the user clicks an entry —
// so splitting a "list" vs "fill" shape (as Phase 1 did) silently broke
// autofill: the picker would fill the username and leave the password
// empty. Mirror local_server.cpp's pre-Phase-4 /api/creds/match shape
// instead so content.js keeps working unchanged.
//
// Hygiene note: shipping plaintext passwords on every match looks loose,
// but the socket is user-permissions + App Group — only our own signed
// pw-bridge (or Safari's sandboxed .appex) can connect, so there's no
// additional read surface beyond what `fill` already exposes.
nlohmann::json SerializeCred(const Credential& c)
{
    nlohmann::json j;
    j["uuid"]        = c.uuid;
    j["title"]       = c.title;
    j["user"]        = c.user;
    j["email"]       = c.email;
    j["password"]    = c.password;
    j["website"]     = c.website;
    j["group"]       = c.group;
    j["notes"]       = c.notes;
    j["totp_secret"] = c.totp_secret;
    j["is_favorite"] = c.is_favorite;
    j["is_pinned"]   = c.is_pinned;
    j["type"]        = static_cast<int>(c.type);
    return j;
}

// Full field set for get_cards / get_identities — matches pw-windows-cloud's
// CredentialToJson so the extension's card/identity fill works on both.
// Kept separate from SerializeCred so match/search responses don't start
// carrying card numbers and ID numbers.
nlohmann::json SerializeCredFull(const Credential& c)
{
    nlohmann::json j = SerializeCred(c);
    j["card_number"]      = c.card_number;
    j["card_expiry"]      = c.card_expiry;
    j["card_cvv"]         = c.card_cvv;
    j["card_brand"]       = c.card_brand;
    j["cardholder_name"]  = c.cardholder_name;
    j["card_address"]     = c.card_address;
    j["card_city"]        = c.card_city;
    j["card_postal_code"] = c.card_postal_code;
    j["full_name"]        = c.full_name;
    j["id_type"]          = c.id_type;
    j["id_number"]        = c.id_number;
    j["date_of_birth"]    = c.date_of_birth;
    j["expiry_date"]      = c.expiry_date;
    j["country"]          = c.country;
    j["address"]          = c.address;
    j["phone"]            = c.phone;
    return j;
}

} // namespace

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

BridgeServer::~BridgeServer()
{
    Shutdown();
}

std::string BridgeServer::SocketPath() const
{
    return DefaultSocketPath();
}

bool BridgeServer::OwnsSocketPath() const
{
    if (!m_running.load()) return false;
    struct stat st{};
    if (::stat(DefaultSocketPath().c_str(), &st) != 0) return false;
    return st.st_dev == m_socket_dev && st.st_ino == m_socket_ino;
}

bool BridgeServer::SocketIsLive(const std::string& path)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() + 1 > sizeof(addr.sun_path)) { ::close(fd); return false; }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    const bool live = ::connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0;
    ::close(fd);
    return live;
}

size_t BridgeServer::ConnectedClients() const
{
    std::lock_guard<std::mutex> lk(m_clients_mutex);
    return m_clients.size();
}

int BridgeServer::CreateListenSocket(const std::string& path, std::string* err)
{
    // Ensure the parent directory exists. Mac vault code already creates
    // this on first unlock, but Start() can race ahead of that.
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        std::string dir = path.substr(0, slash);
        if (!dir.empty() && mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
            if (err) {
                err->assign("mkdir failed: ");
                err->append(std::strerror(errno));
            }
            return -1;
        }
    }

    // Another Password Defense app may be serving this path (cloud and
    // offline share it). Only a node nobody answers on — left by a crashed
    // run — gets removed; bind() otherwise fails with EADDRINUSE.
    if (SocketIsLive(path)) {
        if (err) err->assign("in_use");
        return -1;
    }
    ::unlink(path.c_str());

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (err) {
            err->assign("socket() failed: ");
            err->append(std::strerror(errno));
        }
        return -1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() + 1 > sizeof(addr.sun_path)) {
        if (err) err->assign("socket path too long");
        ::close(fd);
        return -1;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        if (err) {
            err->assign("bind failed: ");
            err->append(std::strerror(errno));
        }
        ::close(fd);
        return -1;
    }

    // AF_UNIX node inherits umask on create — tighten explicitly.
    ::chmod(path.c_str(), 0600);

    if (::listen(fd, 8) != 0) {
        if (err) {
            err->assign("listen failed: ");
            err->append(std::strerror(errno));
        }
        ::close(fd);
        return -1;
    }

    return fd;
}

void BridgeServer::Start()
{
    if (m_running.load()) return;

    std::string err;
    const std::string path = DefaultSocketPath();
    int fd = CreateListenSocket(path, &err);
    if (fd < 0) {
        m_served_elsewhere.store(err == "in_use");
        if (err != "in_use")
            std::fprintf(stderr, "[BridgeServer] start failed: %s\n", err.c_str());
        return;
    }
    m_served_elsewhere.store(false);

    struct stat st{};
    if (::stat(path.c_str(), &st) == 0) {
        m_socket_dev = st.st_dev;
        m_socket_ino = st.st_ino;
    }

    m_listen_fd = fd;
    m_running.store(true);
    m_accept_thread = std::thread([this] { AcceptLoop(); });
}

void BridgeServer::Shutdown()
{
    // Checked before m_running drops: OwnsSocketPath() needs it set.
    const bool owns_path = OwnsSocketPath();
    if (!m_running.exchange(false)) return;

    // Closing the listen fd breaks accept() out of its blocking wait.
    if (m_listen_fd >= 0) {
        ::shutdown(m_listen_fd, SHUT_RDWR);
        ::close(m_listen_fd);
        m_listen_fd = -1;
    }

    if (m_accept_thread.joinable()) m_accept_thread.join();

    // Close every connected client — their worker threads see recv() = 0
    // and exit the WorkerLoop cleanly.
    std::vector<int> to_close;
    {
        std::lock_guard<std::mutex> lk(m_clients_mutex);
        to_close = m_clients;
        m_clients.clear();
    }
    for (int cfd : to_close) {
        ::shutdown(cfd, SHUT_RDWR);
        ::close(cfd);
    }

    for (auto& t : m_workers)
        if (t.joinable()) t.join();
    m_workers.clear();

    // Fail any writes that are still waiting on the UI thread. If we don't
    // drain + set_value here, a worker whose connection was already closed
    // is harmless, but any caller still holding a future from EnqueueWrite
    // would deadlock. Belt and suspenders.
    {
        std::lock_guard<std::mutex> lk(m_writes_mutex);
        while (!m_pending_writes.empty()) {
            m_pending_writes.front().response.set_value(
                R"({"ok":false,"error":"bridge_shutting_down"})");
            m_pending_writes.pop_front();
        }
    }

    // Never unlink a node another app bound after replacing ours.
    if (owns_path) ::unlink(DefaultSocketPath().c_str());

    Lock();  // zeroize cache on shutdown
}

// -----------------------------------------------------------------------------
// Accept / worker threads
// -----------------------------------------------------------------------------

void BridgeServer::AcceptLoop()
{
    while (m_running.load()) {
        int cfd = ::accept(m_listen_fd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (!m_running.load()) break;
            continue;
        }

        std::lock_guard<std::mutex> lk(m_clients_mutex);
        m_clients.push_back(cfd);
        m_workers.emplace_back([this, cfd] { WorkerLoop(cfd); });
    }
}

void BridgeServer::WorkerLoop(int client_fd)
{
    for (;;) {
        std::string frame;
        if (!RecvFrame(client_fd, &frame, kMaxFrameBytes)) break;
        std::string response = HandleRequest(frame);
        if (!SendFrame(client_fd, response)) break;
    }

    {
        std::lock_guard<std::mutex> lk(m_clients_mutex);
        auto it = std::find(m_clients.begin(), m_clients.end(), client_fd);
        if (it != m_clients.end()) m_clients.erase(it);
    }
    ::close(client_fd);
}

bool BridgeServer::RecvFrame(int fd, std::string* out, size_t max_bytes)
{
    out->clear();
    char buf[4096];
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n == 0) return false;              // peer closed
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') return true;
            out->push_back(buf[i]);
            if (out->size() > max_bytes) return false;
        }
    }
}

bool BridgeServer::SendFrame(int fd, const std::string& json)
{
    std::string buf = json;
    buf.push_back('\n');
    size_t sent = 0;
    while (sent < buf.size()) {
        ssize_t n = ::send(fd, buf.data() + sent, buf.size() - sent, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Snapshot cache
// -----------------------------------------------------------------------------

void BridgeServer::ApplySnapshot(bool unlocked,
                                 const std::string& vault_path,
                                 const std::vector<Credential>& creds)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    // Zeroize anything already in the cache before overwriting.
    for (auto& c : m_cache) {
        sodium_memzero((void*)c.password.data(),    c.password.capacity());
        sodium_memzero((void*)c.totp_secret.data(), c.totp_secret.capacity());
        sodium_memzero((void*)c.notes.data(),       c.notes.capacity());
        sodium_memzero((void*)c.card_number.data(), c.card_number.capacity());
        sodium_memzero((void*)c.card_cvv.data(),    c.card_cvv.capacity());
    }
    m_cache = creds;
    m_unlocked = unlocked;
    m_vault_path = vault_path;
}

void BridgeServer::Lock()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_unlocked = false;
    for (auto& c : m_cache) {
        sodium_memzero((void*)c.password.data(),    c.password.capacity());
        sodium_memzero((void*)c.totp_secret.data(), c.totp_secret.capacity());
        sodium_memzero((void*)c.notes.data(),       c.notes.capacity());
        sodium_memzero((void*)c.card_number.data(), c.card_number.capacity());
        sodium_memzero((void*)c.card_cvv.data(),    c.card_cvv.capacity());
    }
    m_cache.clear();
    m_cache.shrink_to_fit();
    m_vault_path.clear();
}

void BridgeServer::PushEvent(const std::string& json_payload)
{
    std::vector<int> clients;
    {
        std::lock_guard<std::mutex> lk(m_clients_mutex);
        clients = m_clients;
    }
    for (int fd : clients) {
        (void)SendFrame(fd, json_payload);
    }
}

// -----------------------------------------------------------------------------
// Write queue
// -----------------------------------------------------------------------------

std::future<std::string> BridgeServer::EnqueueWrite(std::string action,
                                                    std::string payload_json)
{
    PendingWrite w;
    w.action       = std::move(action);
    w.payload_json = std::move(payload_json);
    auto fut = w.response.get_future();

    // If the server is on its way down, fail fast instead of enqueuing into
    // a drain that will never run.
    if (!m_running.load()) {
        w.response.set_value(R"({"ok":false,"error":"bridge_shutting_down"})");
        return fut;
    }

    std::lock_guard<std::mutex> lk(m_writes_mutex);
    m_pending_writes.push_back(std::move(w));
    return fut;
}

std::deque<PendingWrite> BridgeServer::DrainPendingWrites()
{
    std::deque<PendingWrite> out;
    std::lock_guard<std::mutex> lk(m_writes_mutex);
    out.swap(m_pending_writes);
    return out;
}

// -----------------------------------------------------------------------------
// Request dispatch (read-only in Phase 1)
// -----------------------------------------------------------------------------

std::string BridgeServer::HandleRequest(const std::string& request_json)
{
    nlohmann::json req;
    try {
        req = nlohmann::json::parse(request_json);
    } catch (...) {
        // Can't echo _req_id — we never got far enough to extract it.
        // The extension's lib/api.js already handles missing-correlation
        // via a single-pending-request fallback, so this stays a safe
        // early return.
        return R"({"ok":false,"error":"invalid_json"})";
    }

    std::string action = req.value("action", "");
    // Windows bridge protocol: action "save" + save_action ("save_new" |
    // "update"). The extension sends this form first; map it onto ours.
    if (action == "save") {
        action = req.value("save_action", req.value("action_type", std::string()));
    }

    // _req_id correlates request → response in the Chrome/Firefox
    // connectNative streaming port where multiple requests can be in
    // flight at once. Safari (sendNativeMessage) is one-shot per call
    // and omits _req_id — in that case we just don't inject one in the
    // response, which is harmless.
    //
    // Preserve the field's original JSON type (the extension sends an
    // integer — see lib/api.js: `const reqId = _nextRequestId++`).
    // Previously we extracted this as std::string and nlohmann silently
    // returned the "" default for a non-string field, so echo never
    // actually ran. Symptom: popup showed "Desktop App Offline" even
    // though content-script autofill worked, because the popup opens
    // multiple concurrent requests (status + matches + badge) and the
    // extension's single-pending fallback couldn't disambiguate without
    // _req_id.
    const bool has_req_id = req.contains("_req_id") && !req["_req_id"].is_null();
    const nlohmann::json req_id = has_req_id ? req["_req_id"] : nlohmann::json(nullptr);

    // Single-point helper: echo _req_id into any JSON response. Used by
    // every return path so correlation is impossible to forget.
    auto respond = [&](nlohmann::json resp) -> std::string {
        if (has_req_id) resp["_req_id"] = req_id;
        return resp.dump();
    };

    // -------- Read actions (served from m_cache under m_mutex) --------
    //
    // Scoped so we drop the mutex before potentially blocking on the
    // write-queue future below.
    {
        std::lock_guard<std::mutex> lk(m_mutex);

        if (action == "status") {
            nlohmann::json resp;
            resp["ok"]          = true;
            resp["unlocked"]    = m_unlocked;
            resp["credCount"]   = m_unlocked ? (int)m_cache.size() : 0;
            resp["activeVault"] = m_vault_path;
            return respond(std::move(resp));
        }

        if (!m_unlocked) {
            return respond({{"ok", false}, {"error", "vault_locked"}});
        }

        if (action == "match") {
            std::string url = req.value("url", "");
            std::string tab_host = ExtractHost(url);
            auto arr = nlohmann::json::array();
            for (const auto& c : m_cache) {
                if (c.is_deleted()) continue;
                if (c.website.empty()) continue;
                if (ExtractHost(c.website) == tab_host) {
                    arr.push_back(SerializeCred(c));
                }
            }
            nlohmann::json resp;
            resp["ok"]          = true;
            resp["credentials"] = std::move(arr);
            return respond(std::move(resp));
        }

        if (action == "search") {
            std::string q = req.value("q", "");
            auto arr = nlohmann::json::array();
            for (const auto& c : m_cache) {
                if (c.is_deleted()) continue;
                if (CaseInsensitiveContains(c.title,   q) ||
                    CaseInsensitiveContains(c.user,    q) ||
                    CaseInsensitiveContains(c.email,   q) ||
                    CaseInsensitiveContains(c.website, q)) {
                    arr.push_back(SerializeCred(c));
                }
            }
            nlohmann::json resp;
            resp["ok"]          = true;
            resp["credentials"] = std::move(arr);
            return respond(std::move(resp));
        }

        if (action == "fill") {
            std::string uuid = req.value("uuid", "");
            for (const auto& c : m_cache) {
                if (c.uuid == uuid && !c.is_deleted()) {
                    nlohmann::json resp;
                    resp["ok"]         = true;
                    resp["credential"] = SerializeCred(c);
                    return respond(std::move(resp));
                }
            }
            return respond({{"ok", false}, {"error", "not_found"}});
        }

        if (action == "get_cards" || action == "get_identities") {
            if (!m_unlocked) {
                return respond({{"ok", false}, {"error", "vault_locked"}});
            }
            const bool cards = (action == "get_cards");
            const CredType want = cards ? CredType::CreditCard : CredType::Identity;
            auto arr = nlohmann::json::array();
            for (const auto& c : m_cache) {
                if (c.is_deleted() || c.type != want) continue;
                arr.push_back(SerializeCredFull(c));
            }
            nlohmann::json resp;
            resp["ok"] = true;
            resp[cards ? "cards" : "identities"] = std::move(arr);
            return respond(std::move(resp));
        }
        if (action == "check") {
            // Advisory lookup — tells the extension whether a save prompt
            // should be "Save new?" or "Update existing?". Reads from the
            // cache so autosave stays snappy. Mirrors local_server's
            // /api/creds/check (local_server.cpp:982-1019).
            std::string hostname = req.value("hostname", "");
            std::string username = req.value("username", "");
            std::string password = req.value("password", "");

            if (hostname.empty() || username.empty() || password.empty()) {
                return respond({{"ok", false}, {"error", "missing_fields"}});
            }

            std::string sub_host = ExtractHost(hostname);
            for (const auto& c : m_cache) {
                if (c.is_deleted() || c.website.empty()) continue;
                if (ExtractHost(c.website) != sub_host) continue;
                std::string cred_user = c.user.empty() ? c.email : c.user;
                if (cred_user != username) continue;

                nlohmann::json resp;
                resp["ok"]             = true;
                resp["action"]         = (c.password == password) ? "already_exists" : "update";
                resp["existing_id"]    = c.uuid;
                resp["existing_title"] = c.title;
                return respond(std::move(resp));
            }

            nlohmann::json resp;
            resp["ok"]             = true;
            resp["action"]         = "save_new";
            resp["existing_id"]    = nullptr;
            resp["existing_title"] = nullptr;
            return respond(std::move(resp));
        }
    } // end locked read-path scope

    // -------- Write actions (enqueue for UI thread) --------
    //
    // Writes arrive as "save_new" / "update" (either directly, or mapped
    // from the Windows-style "save" + save_action above) or "delete".
    // The UI thread's PerformBridgeWrite keys off the same strings.
    if (action == "save_new" || action == "update" || action == "delete") {
        auto fut = EnqueueWrite(action, request_json);
        auto status = fut.wait_for(std::chrono::seconds(5));
        if (status != std::future_status::ready) {
            return respond({{"ok", false}, {"error", "timeout"}});
        }
        // PerformBridgeWrite returns a serialized JSON string that doesn't
        // know about _req_id. Re-parse + inject before returning so the
        // extension's correlation table matches. Falls back gracefully if
        // the response was malformed somehow.
        std::string raw = fut.get();
        if (!has_req_id) return raw;
        try {
            auto resp = nlohmann::json::parse(raw);
            resp["_req_id"] = req_id;
            return resp.dump();
        } catch (...) {
            return raw;
        }
    }

    return respond({{"ok", false}, {"error", "unknown_action"}});
}
