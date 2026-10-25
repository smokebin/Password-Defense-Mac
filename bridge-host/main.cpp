// pw-bridge: Native-messaging shim between the browser (stdio) and the
// Password Defense GUI app's Unix domain socket.
//
// Protocol:
//   stdio side:  Chrome Native Messaging — each frame is a little-endian
//                uint32 length prefix followed by that many bytes of JSON.
//   socket side: newline-delimited JSON (one frame per line).
//
// Lifetime:
//   The browser spawns one copy of pw-bridge per connectNative() call and
//   keeps it running until the port disconnects. On startup pw-bridge opens
//   the socket; from then on it just shuttles frames in both directions.
//   On socket close OR stdin close, it exits.
//
// No business logic lives here. The GUI app owns the vault; this binary is
// pure plumbing. ~150 lines.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr size_t kMaxFrameBytes = 1 * 1024 * 1024;  // 1 MiB — matches BridgeServer

std::string DefaultSocketPath()
{
    // App Group container — shared with the Safari extension. See
    // bridge_server.cpp DefaultSocketPath for the full reasoning.
    const char* home = std::getenv("HOME");
    std::string base = (home && *home) ? home : "/tmp";
    base += "/Library/Group Containers/group.com.StackedSoftware.PasswordDefense/bridge.sock";
    return base;
}

// Read exactly `want` bytes from fd into buf. Returns false on EOF or error.
bool ReadExact(int fd, void* buf, size_t want)
{
    char* p = static_cast<char*>(buf);
    while (want > 0) {
        ssize_t n = ::read(fd, p, want);
        if (n == 0) return false;              // EOF
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        want -= (size_t)n;
    }
    return true;
}

bool WriteExact(int fd, const void* buf, size_t len)
{
    const char* p = static_cast<const char*>(buf);
    while (len > 0) {
        ssize_t n = ::write(fd, p, len);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

// --- Chrome stdio framing ---

bool ReadStdioFrame(std::string* out)
{
    uint32_t len_le = 0;
    if (!ReadExact(STDIN_FILENO, &len_le, sizeof(len_le))) return false;
    uint32_t len = len_le;  // host is little-endian on x86_64 + arm64; Chrome spec is LE
    if (len == 0 || len > kMaxFrameBytes) return false;
    out->resize(len);
    return ReadExact(STDIN_FILENO, &(*out)[0], len);
}

bool WriteStdioFrame(const std::string& payload)
{
    uint32_t len = (uint32_t)payload.size();
    if (!WriteExact(STDOUT_FILENO, &len, sizeof(len))) return false;
    if (!WriteExact(STDOUT_FILENO, payload.data(), payload.size())) return false;
    // Chrome reads stdout line-buffered through a pipe; force the kernel
    // to hand the bytes over now rather than batching for stdio buffering.
    (void)::fsync(STDOUT_FILENO);
    return true;
}

// --- Socket framing (newline-delim) ---

bool WriteSocketFrame(int fd, const std::string& payload)
{
    std::string buf = payload;
    buf.push_back('\n');
    return WriteExact(fd, buf.data(), buf.size());
}

bool ReadSocketFrame(int fd, std::string* out)
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
            if (out->size() > kMaxFrameBytes) return false;
        }
    }
}

int ConnectSocket(const std::string& path)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() + 1 > sizeof(addr.sun_path)) {
        ::close(fd);
        return -1;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

int main(int /*argc*/, char** /*argv*/)
{
    const std::string sock_path = DefaultSocketPath();
    int sock = ConnectSocket(sock_path);
    if (sock < 0) {
        // Report back to the extension through the native-messaging channel so
        // it can show "Desktop App Offline" rather than a silent dead port.
        const std::string err = R"({"ok":false,"error":"desktop_app_offline"})";
        WriteStdioFrame(err);
        return 1;
    }

    std::atomic<bool> quit{false};

    // Thread A: stdin (browser) → socket (desktop)
    std::thread browser_to_desktop([&] {
        std::string frame;
        while (!quit.load() && ReadStdioFrame(&frame)) {
            if (!WriteSocketFrame(sock, frame)) break;
        }
        quit.store(true);
        ::shutdown(sock, SHUT_RDWR);
    });

    // Thread B: socket (desktop) → stdout (browser)
    std::thread desktop_to_browser([&] {
        std::string frame;
        while (!quit.load() && ReadSocketFrame(sock, &frame)) {
            if (!WriteStdioFrame(frame)) break;
        }
        quit.store(true);
        // Closing stdout lets the browser see the port disconnect.
        ::close(STDOUT_FILENO);
    });

    browser_to_desktop.join();
    desktop_to_browser.join();
    ::close(sock);
    return 0;
}
