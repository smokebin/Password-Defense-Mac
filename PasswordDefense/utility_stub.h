// utility_stub.h — macOS-compatible stub for utility.h
// Provides the interface that UI code depends on without Windows APIs
// TODO: replace with fully ported utility.cpp
#pragma once

#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>

// Replace Windows WORD type
typedef unsigned short WORD;

namespace helpers
{
    // Files / paths
    bool directory_exists(std::string path);
    bool create_directories(const std::string& path);
    bool file_exists(std::string path);
    bool file_exists2(std::string file_name);
    std::string file_to_str(const std::string& filepath);
    void        str_to_file(const std::string& filename, const std::string& content);
    std::vector<unsigned char> file_to_vec(const std::string& path);
    bool bytes_to_file(const char* file_path, const std::vector<unsigned char>& bytes);

    // Conversions
    std::vector<unsigned char> str_to_bytes(const std::string& str);
    std::string bytes_to_str(const std::vector<unsigned char>& bytes);
    bool        is_digit(const std::string& s);
    WORD        int_to_word(int val);
    uint32_t    fnv1a_32(const char* s);
    std::string b64_encode(const std::string& input);
    std::vector<unsigned char> add_padding(const std::vector<unsigned char>& data);
    std::vector<unsigned char> remove_padding(const std::vector<unsigned char>& data);
    std::vector<char> uint_to_char(const unsigned int* data, size_t size);
    void        clear_string(std::string& s);

    // Clipboard
    void to_clipboard(const std::string& s);
    void clear_clipboard();

    // Time
    std::string iso_date_only(const std::string& iso);
    std::string now_iso8601_local();
    std::string format_display_date(const std::string& iso);
    int64_t     now_unix_ms();
    std::string now_iso8601_utc();
    std::string unix_ms_to_iso8601(int64_t ms);
    int64_t     iso8601_to_unix_ms(const std::string& iso);
    std::string format_display_date_ms(int64_t unix_ms);

    // URL
    void open_website(const std::string& url_raw);

    // Password strength
    struct PwStrength
    {
        double entropy_bits = 0.0;
        int    score = 0;
        bool   hasLower = false;
        bool   hasUpper = false;
        bool   hasDigit = false;
        bool   hasSymbol = false;
        bool   allSame = false;
        bool   simpleSeq = false;
        bool   shortPwd = false;
        bool   tooSequential = false;
        bool   repeatedPattern = false;
        int    charClasses = 0;
    };

    PwStrength   analyze_password(const std::string& pw);
    unsigned int strength_color(int score);
    const char* strength_label(int score);
    void         DrawStrengthMeter(const std::string& pw, float width = 200.0f);

    // Secure RNG + UUID + password generator
    bool        secure_rand_bytes(void* dst, size_t len);
    size_t      secure_rand_index(size_t n);
    std::string generate_uuid();

    struct GenOptions
    {
        int  length = 30;
        bool use_upper = true;
        bool use_lower = true;
        bool use_digit = true;
        bool use_symbol = true;
        bool avoid_ambiguous = false;
    };

    std::string generate_password(const GenOptions& opt);
    double      estimate_entropy_bits(const GenOptions& opt);

    std::string GetDefaultBackupDir();
    void        SanitizeFilename(std::string& s);
    bool        CopyFileAtomic(const std::string& src, const std::string& dst);
    void        EnforceBackupRetention(const std::string& dir, size_t keep_count);
    std::vector<std::string> ListBackupsForVault(const std::string& vault_path);
    std::string              Basename(const std::string& path);
    bool GetFileInfo(const std::string& path, uint64_t& outSizeBytes, std::string& outLocalTimeStr);
    std::string FormatBytes(uint64_t bytes);

    struct BackupCreateResult
    {
        bool ok = false;
        std::string outPath;
        std::string err;
    };

    BackupCreateResult CreateBackupNow(const std::string& vault_path, size_t keep_count);
}
