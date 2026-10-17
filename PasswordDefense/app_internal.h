// app_internal.h
// Shared declarations for application translation units.
// Included by application.cpp (core), app_import_export.cpp, app_sharing.cpp.
#pragma once

#include "credential.h"
#include <string>
#include <vector>
#include <cstdint>

// Current desktop build version. Bump this on every release — it's what the
// app reports to GET /api/version so the backend can tell it whether a newer
// build exists and whether this one is still supported.
// Keep it dotted-numeric (matches the backend's version comparison).
#ifndef PWMNGR_APP_VERSION
#define PWMNGR_APP_VERSION "1.0"
#endif

// ============================================================
// Import/Export helpers (defined in app_import_export.cpp)
// ============================================================

std::string serialize_creds_json(const std::vector<Credential>& creds);
std::vector<Credential> deserialize_creds_json(const std::string& json_str);

std::string PickOpenFilePath_DB();
std::string PickSaveFilePath_DB();
std::string PickSaveFilePath_PWM();
std::string PickOpenFilePath_PWM();
std::string PickSaveFilePath_CSV();
std::string PickSaveFilePath_KDBX();
std::string PickOpenFilePath_CSV();
std::string PickOpenFilePath_Backup();

enum class CsvFormat { OwnApp, Chrome, Bitwarden, LastPass, OnePassword, KeePass, Generic, Unknown };
const char* CsvFormatName(CsvFormat f);

struct CsvImportResult {
    std::vector<Credential> creds;
    CsvFormat format = CsvFormat::Unknown;
    int skipped = 0;
    std::string error;
};

CsvImportResult parse_csv_import(const std::string& file_content);

// ============================================================
// Sharing helpers (defined in app_sharing.cpp)
// ============================================================

std::string Anonbase64_encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> base64_decode(const std::string& b64);
std::string base64_url_encode(const std::vector<uint8_t>& data);
std::string serialize_credential_for_share(const Credential& c, const std::string& pw);
