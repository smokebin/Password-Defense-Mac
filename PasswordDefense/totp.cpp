// totp.cpp
// RFC 6238 TOTP implementation with minimal SHA-1 (FIPS 180-4) and HMAC-SHA1 (RFC 2104)

#include "totp.h"
#include <cstring>
#include <ctime>
#include <algorithm>
#include <cctype>
#include <sodium.h>

namespace totp {

// ============================================================
// SHA-1 (FIPS 180-4) — minimal implementation
// ============================================================
namespace {

struct SHA1_CTX {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
};

static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_transform(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)block[i*4] << 24 | (uint32_t)block[i*4+1] << 16
              | (uint32_t)block[i*4+2] << 8 | (uint32_t)block[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);       k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else              { f = b ^ c ^ d;                   k = 0xCA62C1D6; }

        uint32_t temp = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = temp;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void sha1_init(SHA1_CTX& ctx)
{
    ctx.state[0] = 0x67452301;
    ctx.state[1] = 0xEFCDAB89;
    ctx.state[2] = 0x98BADCFE;
    ctx.state[3] = 0x10325476;
    ctx.state[4] = 0xC3D2E1F0;
    ctx.count = 0;
    std::memset(ctx.buffer, 0, 64);
}

static void sha1_update(SHA1_CTX& ctx, const uint8_t* data, size_t len)
{
    size_t idx = (size_t)(ctx.count % 64);
    ctx.count += len;

    for (size_t i = 0; i < len; i++) {
        ctx.buffer[idx++] = data[i];
        if (idx == 64) {
            sha1_transform(ctx.state, ctx.buffer);
            idx = 0;
        }
    }
}

static void sha1_final(SHA1_CTX& ctx, uint8_t digest[20])
{
    uint64_t bits = ctx.count * 8;
    size_t idx = (size_t)(ctx.count % 64);

    ctx.buffer[idx++] = 0x80;
    if (idx > 56) {
        std::memset(ctx.buffer + idx, 0, 64 - idx);
        sha1_transform(ctx.state, ctx.buffer);
        idx = 0;
    }
    std::memset(ctx.buffer + idx, 0, 56 - idx);

    // Big-endian bit length
    for (int i = 0; i < 8; i++)
        ctx.buffer[56 + i] = (uint8_t)(bits >> (56 - i * 8));

    sha1_transform(ctx.state, ctx.buffer);

    for (int i = 0; i < 5; i++) {
        digest[i*4+0] = (uint8_t)(ctx.state[i] >> 24);
        digest[i*4+1] = (uint8_t)(ctx.state[i] >> 16);
        digest[i*4+2] = (uint8_t)(ctx.state[i] >> 8);
        digest[i*4+3] = (uint8_t)(ctx.state[i]);
    }
}

// ============================================================
// HMAC-SHA1 (RFC 2104)
// ============================================================
static void hmac_sha1(const uint8_t* key, size_t key_len,
                      const uint8_t* msg, size_t msg_len,
                      uint8_t out[20])
{
    const size_t BLOCK = 64;
    uint8_t k_pad[BLOCK];
    std::memset(k_pad, 0, BLOCK);

    // If key > block size, hash it first
    if (key_len > BLOCK) {
        SHA1_CTX hctx;
        sha1_init(hctx);
        sha1_update(hctx, key, key_len);
        sha1_final(hctx, k_pad); // k_pad[0..19] = H(key)
    } else {
        std::memcpy(k_pad, key, key_len);
    }

    // Inner hash: H((K ^ ipad) || msg)
    uint8_t ipad[BLOCK], opad[BLOCK];
    for (size_t i = 0; i < BLOCK; i++) {
        ipad[i] = k_pad[i] ^ 0x36;
        opad[i] = k_pad[i] ^ 0x5C;
    }

    SHA1_CTX ctx;
    sha1_init(ctx);
    sha1_update(ctx, ipad, BLOCK);
    sha1_update(ctx, msg, msg_len);
    uint8_t inner[20];
    sha1_final(ctx, inner);

    // Outer hash: H((K ^ opad) || inner)
    sha1_init(ctx);
    sha1_update(ctx, opad, BLOCK);
    sha1_update(ctx, inner, 20);
    sha1_final(ctx, out);
}

// ============================================================
// Base32 lookup
// ============================================================
static int base32_val(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a';
    if (ch >= '2' && ch <= '7') return ch - '2' + 26;
    return -1; // invalid
}

} // anonymous namespace

// ============================================================
// SHA-1 digest (public wrapper for HIBP breach check)
// ============================================================

void sha1_digest(const uint8_t* data, size_t len, uint8_t out[20])
{
    SHA1_CTX ctx;
    sha1_init(ctx);
    sha1_update(ctx, data, len);
    sha1_final(ctx, out);
}

// ============================================================
// Public API
// ============================================================

std::vector<uint8_t> base32_decode(const std::string& input)
{
    std::vector<uint8_t> out;
    int bits = 0;
    int value = 0;

    for (char ch : input) {
        // Strip whitespace, dashes, padding
        if (ch == ' ' || ch == '-' || ch == '=' || ch == '\t' || ch == '\n' || ch == '\r')
            continue;
        int v = base32_val(ch);
        if (v < 0) continue; // skip invalid chars
        value = (value << 5) | v;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((value >> bits) & 0xFF));
        }
    }
    return out;
}

std::string generate_code(const std::vector<uint8_t>& secret, int64_t unix_sec, int period, int digits)
{
    // RFC 6238: T = floor(unix_sec / period)
    uint64_t T = (uint64_t)(unix_sec / period);

    // Encode T as big-endian 8 bytes
    uint8_t msg[8];
    for (int i = 7; i >= 0; i--) {
        msg[i] = (uint8_t)(T & 0xFF);
        T >>= 8;
    }

    // HMAC-SHA1
    uint8_t hash[20];
    hmac_sha1(secret.data(), secret.size(), msg, 8, hash);

    // Dynamic truncation (RFC 4226 section 5.4)
    int offset = hash[19] & 0x0F;
    uint32_t code = ((uint32_t)(hash[offset] & 0x7F) << 24)
                  | ((uint32_t)hash[offset+1] << 16)
                  | ((uint32_t)hash[offset+2] << 8)
                  | ((uint32_t)hash[offset+3]);

    // Modulo to get desired number of digits
    uint32_t mod = 1;
    for (int i = 0; i < digits; i++) mod *= 10;
    code %= mod;

    // Zero-pad to desired width
    std::string result = std::to_string(code);
    while ((int)result.size() < digits)
        result.insert(result.begin(), '0');

    return result;
}

std::string generate_code_now(const std::vector<uint8_t>& secret)
{
    return generate_code(secret, (int64_t)std::time(nullptr));
}

int seconds_remaining(int64_t unix_sec, int period)
{
    return period - (int)(unix_sec % period);
}

int seconds_remaining_now(int period)
{
    return seconds_remaining((int64_t)std::time(nullptr), period);
}

bool parse_otpauth_uri(const std::string& uri, std::string& out_secret)
{
    // Expected format: otpauth://totp/Label?secret=BASE32&issuer=...
    const std::string prefix = "otpauth://totp/";
    if (uri.size() < prefix.size())
        return false;

    // Case-insensitive prefix check
    std::string lower_uri = uri.substr(0, prefix.size());
    for (auto& ch : lower_uri) ch = (char)std::tolower((unsigned char)ch);
    if (lower_uri != prefix)
        return false;

    // Find "secret=" parameter (case-insensitive)
    std::string lower_full = uri;
    for (auto& ch : lower_full) ch = (char)std::tolower((unsigned char)ch);

    size_t pos = lower_full.find("secret=");
    if (pos == std::string::npos)
        return false;

    pos += 7; // skip "secret="
    size_t end = uri.find('&', pos);
    std::string secret = (end != std::string::npos) ? uri.substr(pos, end - pos) : uri.substr(pos);

    // Trim whitespace
    while (!secret.empty() && std::isspace((unsigned char)secret.back()))
        secret.pop_back();

    if (secret.empty())
        return false;

    // Convert to uppercase for consistency
    for (auto& ch : secret) ch = (char)std::toupper((unsigned char)ch);

    out_secret = secret;
    return true;
}

static std::string lower_copy(std::string s)
{
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

static std::string percent_decode(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == '%' && i + 2 < s.size() &&
            std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2]))
        {
            out.push_back((char)std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        }
        else
            out.push_back(s[i]);
    }
    return out;
}

static std::string trim_copy(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) b++;
    while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

bool parse_otpauth_info(const std::string& uri_in, OtpAuthInfo& out, std::string& out_error)
{
    out = OtpAuthInfo{};
    const std::string uri = trim_copy(uri_in);
    const std::string lower = lower_copy(uri);

    if (lower.rfind("otpauth-migration://", 0) == 0)
    {
        out_error = "This is a Google Authenticator export link, which isn't supported. Use the setup QR code or key from the website instead.";
        return false;
    }
    const std::string scheme = "otpauth://";
    if (lower.rfind(scheme, 0) != 0)
    {
        out_error = "This isn't a two-factor setup link.";
        return false;
    }

    // otpauth://<type>/<label>?<params>
    size_t typeEnd = uri.find('/', scheme.size());
    std::string type = lower.substr(scheme.size(), typeEnd == std::string::npos ? std::string::npos : typeEnd - scheme.size());
    if (type == "hotp")
    {
        out_error = "This link uses counter-based (HOTP) codes, which aren't supported. Only time-based codes work.";
        return false;
    }
    if (type != "totp" || typeEnd == std::string::npos)
    {
        out_error = "This isn't a two-factor setup link.";
        return false;
    }

    size_t q = uri.find('?', typeEnd + 1);
    std::string label = percent_decode(uri.substr(typeEnd + 1, q == std::string::npos ? std::string::npos : q - typeEnd - 1));

    std::string secret, issuerParam, digits, period, algorithm;
    if (q != std::string::npos)
    {
        size_t pos = q + 1;
        while (pos <= uri.size())
        {
            size_t amp = uri.find('&', pos);
            std::string pair = uri.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
            size_t eq = pair.find('=');
            std::string key = lower_copy(percent_decode(pair.substr(0, eq)));
            std::string val = eq == std::string::npos ? "" : percent_decode(pair.substr(eq + 1));
            if (key == "secret") secret = val;
            else if (key == "issuer") issuerParam = val;
            else if (key == "digits") digits = val;
            else if (key == "period") period = val;
            else if (key == "algorithm") algorithm = val;
            if (amp == std::string::npos) break;
            pos = amp + 1;
        }
    }

    for (char ch : secret)
        if (!std::isspace((unsigned char)ch) && ch != '=' && ch != '-')
            out.secret.push_back((char)std::toupper((unsigned char)ch));
    if (out.secret.empty())
    {
        out_error = "This link has no secret key in it.";
        return false;
    }

    // Label is "Issuer:account" or just "account"; the issuer param wins when present.
    std::string labelIssuer;
    std::string account = label;
    size_t colon = label.find(':');
    if (colon != std::string::npos)
    {
        labelIssuer = label.substr(0, colon);
        account = label.substr(colon + 1);
    }
    out.issuer = trim_copy(!issuerParam.empty() ? issuerParam : labelIssuer);
    out.account = trim_copy(account);

    if (!digits.empty() && digits != "6")
        out.unsupported.push_back(digits + "-digit codes");
    if (!period.empty() && period != "30")
        out.unsupported.push_back("a " + period + "-second period");
    if (!algorithm.empty())
    {
        std::string alg = algorithm;
        for (auto& ch : alg) ch = (char)std::toupper((unsigned char)ch);
        if (alg != "SHA1")
            out.unsupported.push_back(alg);
    }
    return true;
}

bool is_valid_secret(const std::string& base32)
{
    auto bytes = base32_decode(base32);
    return bytes.size() >= 10;
}

std::string base32_encode(const std::vector<uint8_t>& data)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string out;
    int bits = 0;
    int value = 0;
    for (uint8_t b : data) {
        value = (value << 8) | b;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out.push_back(alphabet[(value >> bits) & 0x1F]);
        }
    }
    if (bits > 0) {
        out.push_back(alphabet[(value << (5 - bits)) & 0x1F]);
    }
    return out;
}

std::string generate_random_secret()
{
    std::vector<uint8_t> buf(20);
    randombytes_buf(buf.data(), buf.size());
    return base32_encode(buf);
}

bool verify_code(const std::string& secret_b32, const std::string& code, int64_t unix_sec, int window)
{
    auto secret = base32_decode(secret_b32);
    if (secret.size() < 10) return false;

    for (int i = -window; i <= window; ++i) {
        std::string expected = generate_code(secret, unix_sec + i * 30);
        // Constant-time comparison to prevent timing side-channel
        if (expected.size() == code.size() &&
            sodium_memcmp(expected.data(), code.data(), expected.size()) == 0)
            return true;
    }
    return false;
}

bool verify_code_now(const std::string& secret_b32, const std::string& code)
{
    return verify_code(secret_b32, code, (int64_t)std::time(nullptr));
}

std::string generate_otpauth_uri(const std::string& secret_b32, const std::string& issuer, const std::string& account)
{
    // otpauth://totp/Issuer:Account?secret=BASE32&issuer=Issuer&algorithm=SHA1&digits=6&period=30
    std::string uri = "otpauth://totp/";
    uri += issuer;
    uri += ':';
    uri += account;
    uri += "?secret=";
    uri += secret_b32;
    uri += "&issuer=";
    uri += issuer;
    uri += "&algorithm=SHA1&digits=6&period=30";
    return uri;
}

} // namespace totp
