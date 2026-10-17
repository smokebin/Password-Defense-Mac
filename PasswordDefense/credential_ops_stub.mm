// credential_ops_stub.mm — Stub implementations for credential operations
// TODO: replace with ported credential_ops.cpp + vault_db + vault_crypto
#include "credential_ops.h"

namespace cred_ops {
    std::vector<Credential> load_all(const std::vector<uint8_t>&) { return {}; }
    std::vector<Credential> load_deleted(const std::vector<uint8_t>&) { return {}; }
    std::string add(const Credential&, const std::vector<uint8_t>&) { return ""; }
    bool update(const std::string&, const Credential&, const std::vector<uint8_t>&) { return false; }
    bool remove(const std::string&) { return false; }
    bool remove_permanent(const std::string&) { return false; }
    bool import_credentials(const std::vector<Credential>&, const std::vector<uint8_t>&) { return false; }
    bool has_pending_changes() { return false; }
    int count() { return 0; }
}
