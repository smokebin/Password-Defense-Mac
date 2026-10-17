// favicon_stub.mm — macOS favicon implementation (Metal textures)
// Downloads favicons via Google's favicon service, decodes with NSImage,
// creates Metal textures for ImGui rendering.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <AppKit/AppKit.h>
#include "favicon.h"
#include <unordered_map>
#include <mutex>
#include <string>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace favicon {

// ============================================================
// State
// ============================================================

static id<MTLDevice> g_device = nil;
static std::unordered_map<std::string, ImTextureID> g_cache;       // domain → texture
static std::unordered_map<std::string, bool>        g_pending;     // domain → in-flight
static std::unordered_map<std::string, bool>        g_failed;      // domain → failed (don't retry)
static std::mutex g_mutex;
static bool g_network_enabled = true;
static std::string g_cache_dir;                                    // on-disk cache (.png per domain)

static const int kIconSize = 64; // pixels (2x for Retina sharpness)

// ============================================================
// Disk cache helpers
// ============================================================

static std::string DiskCachePath(const std::string& domain)
{
    return g_cache_dir + "/" + domain + ".png";
}

static NSData* ReadDiskCache(const std::string& domain)
{
    if (g_cache_dir.empty()) return nil;
    NSString* path = [NSString stringWithUTF8String:DiskCachePath(domain).c_str()];
    return [NSData dataWithContentsOfFile:path];
}

static void WriteDiskCache(const std::string& domain, NSData* data)
{
    if (g_cache_dir.empty() || !data || data.length == 0) return;
    NSString* path = [NSString stringWithUTF8String:DiskCachePath(domain).c_str()];
    [data writeToFile:path atomically:YES];
}

// ============================================================
// Metal texture from RGBA pixels
// ============================================================

static ImTextureID CreateMetalTexture(const uint8_t* rgba, int w, int h)
{
    if (!g_device || !rgba || w <= 0 || h <= 0) return 0;

    MTLTextureDescriptor* desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                    width:(NSUInteger)w
                                   height:(NSUInteger)h
                                mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeManaged;

    id<MTLTexture> texture = [g_device newTextureWithDescriptor:desc];
    if (!texture) return 0;

    [texture replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
               mipmapLevel:0
                 withBytes:rgba
               bytesPerRow:(NSUInteger)(w * 4)];

    // Retain the texture so ARC doesn't release it — we manage its lifetime manually
    // __bridge_retained transfers ownership out of ARC (balanced by __bridge_transfer in Shutdown)
    return (ImTextureID)(intptr_t)(__bridge_retained void*)texture;
}

// ============================================================
// Decode NSImage → RGBA pixel buffer
// ============================================================

static uint8_t* DecodeImageToRGBA(NSData* data, int targetSize)
{
    if (!data || data.length == 0) return nullptr;

    NSImage* image = [[NSImage alloc] initWithData:data];
    if (!image || image.size.width < 1 || image.size.height < 1) return nullptr;

    int w = targetSize;
    int h = targetSize;

    uint8_t* pixels = (uint8_t*)calloc(w * h * 4, 1);
    if (!pixels) return nullptr;

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(pixels, w, h, 8, w * 4, cs,
        (CGBitmapInfo)(kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big));
    CGColorSpaceRelease(cs);

    if (!ctx) { free(pixels); return nullptr; }

    // Draw the image scaled to target size
    NSGraphicsContext* nsCtx = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:NO];
    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext.currentContext = nsCtx;
    [image drawInRect:NSMakeRect(0, 0, w, h)
             fromRect:NSZeroRect
            operation:NSCompositingOperationSourceOver
             fraction:1.0];
    [NSGraphicsContext restoreGraphicsState];
    CGContextRelease(ctx);

    // Un-premultiply alpha (ImGui expects straight alpha)
    for (int i = 0; i < w * h; ++i) {
        uint8_t* p = &pixels[i * 4];
        uint8_t a = p[3];
        if (a > 0 && a < 255) {
            p[0] = (uint8_t)((p[0] * 255) / a);
            p[1] = (uint8_t)((p[1] * 255) / a);
            p[2] = (uint8_t)((p[2] * 255) / a);
        }
    }

    return pixels;
}

// ============================================================
// Async download + texture creation
// ============================================================

static void FetchFaviconAsync(const std::string& domain)
{
    NSString* urlStr = [NSString stringWithFormat:
        @"https://img.logo.dev/%s?token=pk_TGWm4D_7RzaqfSyDYqc6_g&size=%d&format=png",
        domain.c_str(), kIconSize];
    NSURL* url = [NSURL URLWithString:urlStr];
    if (!url) {
        fprintf(stderr, "[favicon] bad URL for: %s\n", domain.c_str());
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.erase(domain);
        g_failed[domain] = true;
        return;
    }

    std::string domCopy = domain; // copy before block capture
    NSURLSessionDataTask* task = [[NSURLSession sharedSession]
        dataTaskWithURL:url
        completionHandler:^(NSData* data, NSURLResponse* response, NSError* error)
    {
        std::string dom = domCopy;

        if (error) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pending.erase(dom);
            g_failed[dom] = true;
            return;
        }

        if (!data || data.length == 0) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pending.erase(dom);
            g_failed[dom] = true;
            return;
        }

        if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
            NSHTTPURLResponse* http = (NSHTTPURLResponse*)response;
            if (http.statusCode != 200) {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_pending.erase(dom);
                g_failed[dom] = true;
                return;
            }
        }

        uint8_t* pixels = DecodeImageToRGBA(data, kIconSize);
        if (!pixels) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pending.erase(dom);
            g_failed[dom] = true;
            return;
        }

        // Persist raw PNG bytes to disk for next launch (safe off main thread).
        WriteDiskCache(dom, data);

        // Create Metal texture on main thread
        std::string domForMain = dom; // copy for inner block
        dispatch_async(dispatch_get_main_queue(), ^{
            ImTextureID tex = CreateMetalTexture(pixels, kIconSize, kIconSize);
            free(pixels);

            std::lock_guard<std::mutex> lock(g_mutex);
            g_pending.erase(domForMain);
            if (tex) {
                g_cache[domForMain] = tex;
            } else {
                g_failed[domForMain] = true;
            }
        });
    }];

    [task resume];
}

// ============================================================
// Public API
// ============================================================

void Init(void* mtlDevice)
{
    g_device = (__bridge id<MTLDevice>)mtlDevice;

    // Persistent on-disk cache: ~/Library/Application Support/PasswordDefense/Favicons/
    const char* home = getenv("HOME");
    if (home) {
        g_cache_dir = std::string(home) + "/Library/Application Support/PasswordDefense/Favicons";
        std::error_code ec;
        std::filesystem::create_directories(g_cache_dir, ec);
    }
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [domain, tex] : g_cache) {
        if (tex) {
            id<MTLTexture> mtlTex = (__bridge_transfer id<MTLTexture>)(void*)(intptr_t)tex;
            mtlTex = nil;
        }
    }
    g_cache.clear();
    g_pending.clear();
    g_failed.clear();
    g_device = nil;
}

ImTextureID Get(const std::string& website)
{
    if (!g_device) return 0;

    std::string domain = ExtractDomain(website);
    if (domain.empty()) return 0;

    std::lock_guard<std::mutex> lock(g_mutex);

    // Already in memory?
    auto it = g_cache.find(domain);
    if (it != g_cache.end()) return it->second;

    // Already failed this session?
    if (g_failed.count(domain)) return 0;

    // Try disk cache before issuing any network request. Reading cached icons
    // from disk is NOT a network event — survives the online_favicons toggle.
    if (NSData* diskBytes = ReadDiskCache(domain)) {
        if (diskBytes.length > 0) {
            uint8_t* pixels = DecodeImageToRGBA(diskBytes, kIconSize);
            if (pixels) {
                ImTextureID tex = CreateMetalTexture(pixels, kIconSize, kIconSize);
                free(pixels);
                if (tex) {
                    g_cache[domain] = tex;
                    return tex;
                }
            }
        }
    }

    // Network disabled → never issue a request. Disk cache above still serves.
    if (!g_network_enabled) return 0;

    // Already in flight?
    if (g_pending.count(domain)) return 0;

    // Start async fetch (will persist to disk on success)
    g_pending[domain] = true;
    FetchFaviconAsync(domain);

    return 0; // Will appear on a future frame
}

void SetNetworkEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_network_enabled = enabled;
}

CacheStats GetCacheStats()
{
    CacheStats s;
    if (g_cache_dir.empty()) return s;
    std::error_code ec;
    if (!std::filesystem::exists(g_cache_dir, ec)) return s;
    for (std::filesystem::directory_iterator it(g_cache_dir, ec), end; it != end; it.increment(ec))
    {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != ".png") continue;
        s.file_count++;
        s.total_bytes += (size_t)it->file_size(ec);
    }
    return s;
}

void ClearCache()
{
    // Delete every cached .png on disk
    if (!g_cache_dir.empty())
    {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(g_cache_dir, ec), end; it != end; it.increment(ec))
        {
            if (ec) break;
            if (it->path().extension() == ".png")
                std::filesystem::remove(it->path(), ec);
        }
    }

    // Drop in-memory textures and the "failed this session" set so re-fetches can happen.
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [domain, tex] : g_cache) {
        if (tex) {
            id<MTLTexture> mtlTex = (__bridge_transfer id<MTLTexture>)(void*)(intptr_t)tex;
            mtlTex = nil;
        }
    }
    g_cache.clear();
    g_failed.clear();
}

std::string ExtractDomain(const std::string& url)
{
    std::string s = url;
    auto pos = s.find("://");
    if (pos != std::string::npos) s = s.substr(pos + 3);
    pos = s.find('/');
    if (pos != std::string::npos) s = s.substr(0, pos);
    if (s.rfind("www.", 0) == 0) s = s.substr(4);
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

} // namespace favicon
