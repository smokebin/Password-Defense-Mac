// favicon.h — favicon texture loading
#pragma once
#include "imgui.h"
#include <string>

namespace favicon {
#ifdef __APPLE__
    void Init(void* mtlDevice);   // Pass id<MTLDevice> as void*
#else
    void Init();
#endif
    void Shutdown();

    // Returns ImTextureID for the website's favicon, or 0 if unavailable.
    ImTextureID Get(const std::string& website);

    // Extract root domain from a URL
    std::string ExtractDomain(const std::string& url);

    // Network & Privacy controls (offline build).
    // When disabled, Get() returns 0 without issuing an HTTP request.
    void SetNetworkEnabled(bool enabled);

    struct CacheStats {
        int file_count = 0;      // number of cached favicons (in-memory textures)
        size_t total_bytes = 0;  // approximate RAM used by those textures
    };
    CacheStats GetCacheStats();

    // Drop all cached textures and clear the "already failed" set.
    void ClearCache();
}
