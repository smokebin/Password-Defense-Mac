// bridge_server.h
//
// Native-messaging bridge for the browser extension. Replaces the legacy
// localhost HTTP server (local_server.{h,cpp}) with a Unix domain socket
// consumed by a tiny stdio-shim host that the browser spawns.
//
// Phase 1 (this iteration): read-only path. BridgeServer runs in parallel
// with LocalServer; the extension can be flipped at any time to point at
// either transport. The write actions (`check`, `save`) return
// `not_implemented` for now — Phase 2 wires those into the UI-thread
// command queue.
//
// Threading contract:
//   - UI thread owns VaultState / vault_db / rendering.
//   - UI thread calls ApplySnapshot after unlock / sync / edit.
//   - UI thread calls Lock() when the vault locks.
//   - Accept thread + worker threads live inside BridgeServer.
//   - Workers serve read actions directly from the snapshot cache.
//
// Zeroize-on-lock: Lock() memzeroes every string in the cache before
// clearing it, mirroring the Windows plan (docs/native-messaging-plan.md
// §3.C).

#pragma once

#include <sys/types.h>

#include <atomic>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "credential.h"   // for the snapshot copy

// A single write command enqueued by a worker thread, waiting for the UI
// thread to execute it. UI thread drains the queue once per frame, applies
// the mutation to v.creds / vault_db, and fulfills the response promise.
struct PendingWrite
{
    std::string          action;        // "save_new" | "update" | "delete"
    std::string          payload_json;  // raw request JSON (worker already validated the frame)
    std::promise<std::string> response; // JSON response string

    PendingWrite() = default;
    PendingWrite(PendingWrite&&) noexcept = default;
    PendingWrite& operator=(PendingWrite&&) noexcept = default;
    PendingWrite(const PendingWrite&) = delete;
    PendingWrite& operator=(const PendingWrite&) = delete;
};

class BridgeServer
{
public:
    BridgeServer() = default;
    ~BridgeServer();

    // Lifecycle (call from UI thread)
    void Start();           // creates socket, spawns accept thread
    void Shutdown();        // closes socket, joins threads, zeroizes cache

    bool IsRunning() const  { return m_running.load(); }

    // pw-mac-cloud and the offline Password-Defense-Mac share one socket
    // path (the App Group container Safari can reach), so only one of them
    // serves at a time. Start() never takes a socket another app is
    // answering on; it sets this flag instead and can be retried later.
    bool ServedByOtherApp() const { return m_served_elsewhere.load(); }

    // True while the socket path still names the node this server bound.
    // False if another process replaced it (both apps launching at once).
    bool OwnsSocketPath() const;

    // Snapshot management (UI thread only).
    // Overwrites the cache with a copy of `creds` and marks the vault
    // unlocked. Call after every mutation to v.creds (unlock, edit, sync
    // pull, etc.). Takes plain fields rather than VaultState& so the
    // header stays independent of application.cpp's layout.
    void ApplySnapshot(bool unlocked,
                       const std::string& vault_path,
                       const std::vector<Credential>& creds);

    // Zeroize + clear the cache; call when the vault locks.
    void Lock();

    // Event push (any thread) — writes a newline-terminated JSON frame to
    // every connected client. Example payloads:
    //   {"event":"vault_locked"}
    //   {"event":"vault_unlocked","credCount":42}
    //   {"event":"vault_modified"}
    void PushEvent(const std::string& json_payload);

    // --- Write-queue API ---
    //
    // Workers enqueue save_new/update requests via EnqueueWrite and wait on
    // the returned future (with a timeout — see HandleRequest). The UI
    // thread drains once per frame, executes each pending write against the
    // real VaultState + vault_db, then calls .set_value() on the response
    // promise. The worker unblocks and writes the JSON back to the client.
    std::future<std::string> EnqueueWrite(std::string action,
                                          std::string payload_json);

    // UI-thread-only. Moves out the pending writes queue so the caller can
    // process it without holding the server's internal mutex. The caller
    // MUST .set_value() on every popped PendingWrite's response promise —
    // if dropped on the floor, the worker's future.wait_for() hits the
    // timeout and the extension sees a bare error.
    std::deque<PendingWrite> DrainPendingWrites();

    // Diagnostics for the Settings UI
    std::string SocketPath() const;
    size_t ConnectedClients() const;

private:
    // ------- Threads -------
    void AcceptLoop();
    void WorkerLoop(int client_fd);

    // ------- Protocol -------
    std::string HandleRequest(const std::string& request_json);

    // ------- Socket helpers -------
    static int CreateListenSocket(const std::string& path, std::string* err);
    static bool SocketIsLive(const std::string& path);
    static bool SendFrame(int fd, const std::string& json);  // appends '\n'
    static bool RecvFrame(int fd, std::string* out, size_t max_bytes);

    // ------- State -------
    std::atomic<bool>   m_running{false};
    std::atomic<bool>   m_served_elsewhere{false};
    int                 m_listen_fd = -1;
    dev_t               m_socket_dev = 0;   // identity of the node we bound,
    ino_t               m_socket_ino = 0;   // checked before unlinking it
    std::thread         m_accept_thread;

    // Snapshot cache (plaintext credentials — zeroed on Lock)
    mutable std::mutex  m_mutex;
    bool                m_unlocked = false;
    std::string         m_vault_path;
    std::vector<Credential> m_cache;

    // Connected worker threads + their fds. Separate mutex so event
    // fan-out doesn't block read handlers.
    mutable std::mutex  m_clients_mutex;
    std::vector<int>    m_clients;
    std::vector<std::thread> m_workers;

    // Pending write queue — populated by workers via EnqueueWrite, drained
    // by UI thread via DrainPendingWrites. Separate mutex so write enqueue
    // doesn't contend with cache reads.
    mutable std::mutex  m_writes_mutex;
    std::deque<PendingWrite> m_pending_writes;
};
