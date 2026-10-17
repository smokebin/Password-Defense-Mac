// local_server_stub.mm — Stub LocalServer for the offline macOS build.
// The real local_server.cpp runs a localhost HTTP server (cpp-httplib + OpenSSL)
// that the browser extension talks to. The offline build removes all networking,
// so this stub is a no-op — every method returns default/empty, so UI code that
// gates on IsRunning() etc. naturally skips its cloud-dependent branches.
//
// NOTE: static LocalServer g_local_server in application.cpp will be destroyed
// at process exit, so the destructor needs a definition here too (the header
// declares ~LocalServer() noexcept; without this symbol the link fails).
#include "local_server.h"
#include "credential.h"

LocalServer::~LocalServer() noexcept {}

void LocalServer::Start(int) {}
void LocalServer::Stop() {}
bool LocalServer::IsRunning() const { return false; }
int  LocalServer::GetPort() const { return 0; }
void LocalServer::UpdateCredentials(const std::vector<Credential>&) {}
void LocalServer::SetMasterKey(const std::vector<uint8_t>&) {}
void LocalServer::SetSalt(const std::vector<uint8_t>&) {}
void LocalServer::SetActiveVaultPath(const std::string&) {}
LocalServer::VaultSwitchInfo LocalServer::ConsumeVaultSwitch() { return {}; }
std::string LocalServer::GeneratePairingCode() { return ""; }
void LocalServer::RevokePairing(const std::string&) {}
std::vector<PairingInfo> LocalServer::GetPairings() const { return {}; }
