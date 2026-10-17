// twofa_ops_stub.mm — Stub implementations for 2FA operations
// TODO: replace with ported twofa_ops.cpp
#include "twofa_ops.h"

namespace twofa_ops {
    TwoFactorData generate_new() { return {}; }
    TwoFactorData load(const std::vector<uint8_t>&) { return {}; }
    bool save(const TwoFactorData&, const std::vector<uint8_t>&) { return false; }
    bool remove() { return false; }
    bool verify_totp(const std::string&, const std::vector<uint8_t>&) { return false; }
    bool verify_recovery(const std::string&, const std::vector<uint8_t>&) { return false; }
    bool is_enabled() { return false; }
    std::string make_recovery_code() { return "XXXX-XXXX"; }
}
