// totp_stub.mm — Stub implementations for TOTP functions
// TODO: replace with ported totp.cpp (HMAC-SHA1 + Base32)
#include "totp.h"
#include <ctime>

namespace totp {
    std::vector<uint8_t> base32_decode(const std::string&) { return {}; }
    std::string generate_code(const std::vector<uint8_t>&, int64_t, int, int) { return "000000"; }
    std::string generate_code_now(const std::vector<uint8_t>&) { return "000000"; }
    int seconds_remaining(int64_t unix_sec, int period) { return period - (int)(unix_sec % period); }
    int seconds_remaining_now(int period) { return seconds_remaining(time(nullptr), period); }
    bool parse_otpauth_uri(const std::string&, std::string&) { return false; }
    bool is_valid_secret(const std::string&) { return false; }
    std::string base32_encode(const std::vector<uint8_t>&) { return ""; }
    std::string generate_random_secret() { return ""; }
    bool verify_code(const std::string&, const std::string&, int64_t, int) { return false; }
    bool verify_code_now(const std::string&, const std::string&) { return false; }
    std::string generate_otpauth_uri(const std::string& secret, const std::string& issuer, const std::string& account) {
        return "otpauth://totp/" + issuer + ":" + account + "?secret=" + secret + "&issuer=" + issuer;
    }
    void sha1_digest(const uint8_t*, size_t, uint8_t out[20]) {
        for (int i = 0; i < 20; i++) out[i] = 0;
    }
}
