// bridge_install.cpp — see header.

#include "bridge_install.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace bridge_install {

namespace {

// Deterministic Chromium extension ID (derived from pwmngr-ext/manifest.json
// "key" field). Keep in sync with scripts/install-bridge-host.sh.
constexpr const char* kExtensionId   = "ambjefgkfkdeeggkajdmnpjhlefbkpak";
constexpr const char* kFirefoxId     = "addon@passworddefense.com";
constexpr const char* kHostName      = "com.passworddefense.bridge";

std::string HomeLib()
{
    const char* home = std::getenv("HOME");
    std::string base = (home && *home) ? home : "/tmp";
    base += "/Library/Application Support";
    return base;
}

struct BrowserDef
{
    const char* name;
    const char* short_key;
    const char* subpath;        // relative to ~/Library/Application Support
    bool firefox_schema;        // true → allowed_extensions; false → allowed_origins
};

const BrowserDef* AllBrowsers(size_t* count)
{
    static const BrowserDef kBrowsers[] = {
        {"Chrome",  "chrome",  "Google/Chrome",                  false},
        {"Edge",    "edge",    "Microsoft Edge",                 false},
        {"Brave",   "brave",   "BraveSoftware/Brave-Browser",    false},
        {"Firefox", "firefox", "Mozilla",                        true},
    };
    *count = sizeof(kBrowsers) / sizeof(kBrowsers[0]);
    return kBrowsers;
}

bool PathExists(const std::string& path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

bool MakeDirs(const std::string& path)
{
    // mkdir -p — iteratively create each ancestor.
    size_t pos = 1;
    while ((pos = path.find('/', pos)) != std::string::npos) {
        std::string sub = path.substr(0, pos);
        if (mkdir(sub.c_str(), 0700) != 0 && errno != EEXIST) return false;
        ++pos;
    }
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) return false;
    return true;
}

std::string BrowserRoot(const BrowserDef& b)
{
    return HomeLib() + "/" + b.subpath;
}

std::string ManifestDir(const BrowserDef& b)
{
    return BrowserRoot(b) + "/NativeMessagingHosts";
}

std::string ManifestPath(const BrowserDef& b)
{
    return ManifestDir(b) + "/" + kHostName + ".json";
}

std::string RenderManifest(const BrowserDef& b, const std::string& host_path)
{
    std::string out;
    out  = "{\n";
    out += "    \"name\": \"";        out += kHostName;    out += "\",\n";
    out += "    \"description\": \"Password Defense native-messaging bridge\",\n";
    out += "    \"path\": \"";        out += host_path;    out += "\",\n";
    out += "    \"type\": \"stdio\",\n";
    if (b.firefox_schema) {
        out += "    \"allowed_extensions\": [\""; out += kFirefoxId;   out += "\"]\n";
    } else {
        out += "    \"allowed_origins\": [\"chrome-extension://"; out += kExtensionId; out += "/\"]\n";
    }
    out += "}\n";
    return out;
}

const BrowserDef* FindBrowser(const std::string& short_key)
{
    size_t count = 0;
    auto* all = AllBrowsers(&count);
    for (size_t i = 0; i < count; ++i) {
        if (short_key == all[i].short_key) return &all[i];
    }
    return nullptr;
}

} // namespace

std::string ResolveHostBinaryPath()
{
    // 1. If we're running inside an .app bundle, prefer the sibling
    //    pw-bridge in the same Contents/MacOS directory.
    char exe_buf[4096];
    uint32_t sz = sizeof(exe_buf);
    if (_NSGetExecutablePath(exe_buf, &sz) == 0) {
        std::string exe = exe_buf;
        size_t slash = exe.find_last_of('/');
        if (slash != std::string::npos) {
            std::string candidate = exe.substr(0, slash) + "/pw-bridge";
            if (PathExists(candidate)) return candidate;
        }
    }

    // 2. Dev fallback — build/pw-bridge next to the running binary.
    //    Works when the Xcode build runs scripts/build-bridge-host.sh via
    //    a Run Script phase and places the output in build/.
    {
        std::string exe = exe_buf;
        size_t slash = exe.find_last_of('/');
        if (slash != std::string::npos) {
            // Walk up a few parents looking for a "pw-bridge" sibling.
            std::string dir = exe.substr(0, slash);
            for (int i = 0; i < 6; ++i) {
                std::string candidate = dir + "/pw-bridge";
                if (PathExists(candidate)) return candidate;
                size_t p = dir.find_last_of('/');
                if (p == std::string::npos) break;
                dir = dir.substr(0, p);
            }
        }
    }

    // 3. Hard-coded dev checkout path — matches scripts/build-bridge-host.sh
    //    default output. Lets a dev launch via Xcode and still have Settings
    //    find the host.
    {
        const char* home = std::getenv("HOME");
        if (home && *home) {
            std::string candidate = std::string(home)
                + "/Projects/pwmngrMAC/Password-Defense-Mac/build/pw-bridge";
            if (PathExists(candidate)) return candidate;
        }
    }

    return "";
}

std::vector<BrowserStatus> Scan()
{
    std::vector<BrowserStatus> out;
    size_t count = 0;
    auto* all = AllBrowsers(&count);
    for (size_t i = 0; i < count; ++i) {
        BrowserStatus s;
        s.name              = all[i].name;
        s.short_key         = all[i].short_key;
        s.browser_installed = PathExists(BrowserRoot(all[i]));
        s.host_registered   = PathExists(ManifestPath(all[i]));
        out.push_back(std::move(s));
    }
    return out;
}

bool Install(const std::string& short_key, bool force, std::string* err_out)
{
    const BrowserDef* b = FindBrowser(short_key);
    if (!b) {
        if (err_out) *err_out = "unknown browser: " + short_key;
        return false;
    }

    if (!force && !PathExists(BrowserRoot(*b))) {
        if (err_out) *err_out = std::string(b->name) + " is not installed";
        return false;
    }

    std::string host = ResolveHostBinaryPath();
    if (host.empty()) {
        if (err_out) *err_out = "pw-bridge binary not found — build it first";
        return false;
    }

    const std::string dir  = ManifestDir(*b);
    const std::string path = ManifestPath(*b);

    if (!MakeDirs(dir)) {
        if (err_out) *err_out = "failed to create " + dir + ": " + std::strerror(errno);
        return false;
    }

    std::ofstream f(path);
    if (!f) {
        if (err_out) *err_out = "failed to open " + path + " for write";
        return false;
    }
    f << RenderManifest(*b, host);
    f.close();
    ::chmod(path.c_str(), 0644);
    return true;
}

bool Uninstall(const std::string& short_key)
{
    const BrowserDef* b = FindBrowser(short_key);
    if (!b) return false;
    const std::string path = ManifestPath(*b);
    if (PathExists(path)) ::unlink(path.c_str());
    return true;
}

int InstallAll()
{
    int installed = 0;
    size_t count = 0;
    auto* all = AllBrowsers(&count);
    for (size_t i = 0; i < count; ++i) {
        if (!PathExists(BrowserRoot(all[i]))) continue;
        if (Install(all[i].short_key, /*force=*/false, nullptr)) ++installed;
    }
    return installed;
}

int UninstallAll()
{
    int removed = 0;
    size_t count = 0;
    auto* all = AllBrowsers(&count);
    for (size_t i = 0; i < count; ++i) {
        if (PathExists(ManifestPath(all[i]))) {
            Uninstall(all[i].short_key);
            ++removed;
        }
    }
    return removed;
}

} // namespace bridge_install
