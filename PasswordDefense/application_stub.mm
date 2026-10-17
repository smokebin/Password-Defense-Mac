// application_stub.mm — Minimal stubs for extern functions declared in ui_internal.h
// TODO: replace with ported application.cpp
#include <vector>
#include <cstdint>
#include "credential.h"

// Stub: returns empty master key (no vault operations will work yet)
std::vector<uint8_t> Get2FAMasterKey()
{
    return {};
}

// Stub: no-op reload
void ReloadVaultCredentials()
{
}

// Stub: returns empty credential list
static std::vector<Credential> s_empty_creds;
const std::vector<Credential>& GetActiveVaultCreds()
{
    return s_empty_creds;
}
