// auth_crypto_stub.mm
// Offline-build no-op implementation of auth_crypto.h.
//
// The real auth_crypto.cpp computes an Argon2id pre-hash of the master
// password + email, used by the cloud sync layer to prove possession to the
// server without transmitting the raw password. This offline build never
// talks to a server, so there is no authentication to pre-hash for — every
// function here returns an empty value. If any caller somehow reaches these
// (shouldn't, since the cloud UI is #ifndef'd out), the empty string either
// no-ops the surrounding sync code or produces a benign empty hash field.
//
// Why a stub instead of deleting the whole file: the real .cpp is gone, but
// the header + stub satisfy the linker for the one surviving #include in
// application.cpp. That keeps "did I guard every UI entry point perfectly?"
// decoupled from "does the offline target build?" — same decoupling pattern
// used by sync_manager_stub.mm and local_server_stub.mm.

#include "auth_crypto.h"

namespace auth {

std::string compute_auth_hash(const std::string& /*password*/,
                              const std::string& /*email*/)
{
    return "";
}

std::string bytes_to_hex(const uint8_t* /*data*/, size_t /*len*/)
{
    return "";
}

std::string bytes_to_hex(const std::vector<uint8_t>& /*bytes*/)
{
    return "";
}

} // namespace auth
