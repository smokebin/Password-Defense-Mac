// utility_stub.mm — Minimal stubs for helpers:: so UI code compiles
// TODO: replace with fully ported utility.cpp

#include "utility_stub.h"
#include "imgui.h"
#include "common_passwords_data.h"   // generated wordlist blobs (gen_wordlist.py)
#include <ctime>
#include <chrono>
#include <cstring>
#include <cctype>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <random>
#include <cmath>
#include <string_view>
#include <sys/stat.h>
#include <algorithm>

#ifdef __APPLE__
#import <AppKit/AppKit.h>
#endif

namespace helpers
{
    bool directory_exists(std::string path) { return std::filesystem::exists(path) && std::filesystem::is_directory(path); }
    bool create_directories(const std::string& path) { return std::filesystem::create_directories(path); }
    bool file_exists(std::string path) { return std::filesystem::exists(path); }
    bool file_exists2(std::string file_name) { return std::filesystem::exists(file_name); }

    std::string file_to_str(const std::string& filepath)
    {
        std::ifstream f(filepath, std::ios::binary);
        if (!f) return {};
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    void str_to_file(const std::string& filename, const std::string& content)
    {
        std::ofstream f(filename, std::ios::binary);
        if (f) f << content;
    }

    std::vector<unsigned char> file_to_vec(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return {};
        auto sz = f.tellg();
        f.seekg(0);
        std::vector<unsigned char> buf(sz);
        f.read(reinterpret_cast<char*>(buf.data()), sz);
        return buf;
    }

    bool bytes_to_file(const char* file_path, const std::vector<unsigned char>& bytes)
    {
        std::ofstream f(file_path, std::ios::binary);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return f.good();
    }

    std::vector<unsigned char> str_to_bytes(const std::string& str)
    {
        return std::vector<unsigned char>(str.begin(), str.end());
    }

    std::string bytes_to_str(const std::vector<unsigned char>& bytes)
    {
        return std::string(bytes.begin(), bytes.end());
    }

    bool is_digit(const std::string& s)
    {
        for (char c : s) if (c < '0' || c > '9') return false;
        return !s.empty();
    }

    WORD int_to_word(int val) { return (WORD)(val & 0xFFFF); }

    uint32_t fnv1a_32(const char* s)
    {
        uint32_t h = 2166136261u;
        for (; *s; ++s) { h ^= (uint8_t)*s; h *= 16777619u; }
        return h;
    }

    std::string b64_encode(const std::string& input)
    {
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int val = 0, bits = -6;
        for (unsigned char c : input) {
            val = (val << 8) + c; bits += 8;
            while (bits >= 0) { out.push_back(table[(val >> bits) & 0x3F]); bits -= 6; }
        }
        if (bits > -6) out.push_back(table[((val << 8) >> (bits + 8)) & 0x3F]);
        while (out.size() % 4) out.push_back('=');
        return out;
    }

    std::vector<unsigned char> add_padding(const std::vector<unsigned char>& data) { return data; }
    std::vector<unsigned char> remove_padding(const std::vector<unsigned char>& data) { return data; }

    std::vector<char> uint_to_char(const unsigned int* data, size_t size)
    {
        std::vector<char> out(size);
        for (size_t i = 0; i < size; i++) out[i] = (char)data[i];
        return out;
    }

    void clear_string(std::string& s)
    {
        memset(s.data(), 0, s.size());
        s.clear();
    }

    // Clipboard — macOS
    void to_clipboard(const std::string& s)
    {
#ifdef __APPLE__
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:[NSString stringWithUTF8String:s.c_str()] forType:NSPasteboardTypeString];
#endif
    }

    void clear_clipboard()
    {
#ifdef __APPLE__
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
#endif
    }

    // Time
    int64_t now_unix_ms()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    std::string now_iso8601_utc()
    {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        struct tm utc;
        gmtime_r(&t, &utc);
        char buf[64];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                 utc.tm_year+1900, utc.tm_mon+1, utc.tm_mday,
                 utc.tm_hour, utc.tm_min, utc.tm_sec, (int)ms.count());
        return buf;
    }

    std::string now_iso8601_local()
    {
        auto t = time(nullptr);
        struct tm local;
        localtime_r(&t, &local);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &local);
        return buf;
    }

    std::string unix_ms_to_iso8601(int64_t ms)
    {
        time_t sec = ms / 1000;
        struct tm utc;
        gmtime_r(&sec, &utc);
        char buf[64];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                 utc.tm_year+1900, utc.tm_mon+1, utc.tm_mday,
                 utc.tm_hour, utc.tm_min, utc.tm_sec, (int)(ms % 1000));
        return buf;
    }

    int64_t iso8601_to_unix_ms(const std::string& iso)
    {
        struct tm t = {};
        int ms = 0;
        sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d.%dZ",
               &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min, &t.tm_sec, &ms);
        t.tm_year -= 1900;
        t.tm_mon -= 1;
        time_t sec = timegm(&t);
        return (int64_t)sec * 1000 + ms;
    }

    std::string format_display_date_ms(int64_t unix_ms)
    {
        if (unix_ms <= 0) return "";
        time_t sec = unix_ms / 1000;
        struct tm local;
        localtime_r(&sec, &local);
        char buf[64];
        const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        int h = local.tm_hour % 12; if (h == 0) h = 12;
        snprintf(buf, sizeof(buf), "%s %d, %d \xC2\xB7 %d:%02d %s",
                 months[local.tm_mon], local.tm_mday, local.tm_year+1900,
                 h, local.tm_min, local.tm_hour >= 12 ? "PM" : "AM");
        return buf;
    }

    std::string iso_date_only(const std::string& iso)
    {
        return iso.size() >= 10 ? iso.substr(0, 10) : iso;
    }

    std::string format_display_date(const std::string& iso)
    {
        // Simple pass-through for now
        return iso;
    }

    // URL
    void open_website(const std::string& url_raw)
    {
#ifdef __APPLE__
        std::string url = url_raw;
        if (url.find("://") == std::string::npos) url = "https://" + url;
        [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]]];
#endif
    }

    // ============================================================
    // Password strength — common-password + dictionary + keyboard-walk
    // detection. Ported from pw-windows @ 3f17c0a (tools/utility.cpp).
    // Blobs in common_passwords_data.h are '\n'-separated, sorted by unsigned
    // byte order. Build the string_view index once and binary-search.
    // ============================================================

    static std::vector<std::string_view> _build_index(const unsigned char* blob, unsigned long len)
    {
        std::vector<std::string_view> v;
        const char* p = reinterpret_cast<const char*>(blob);
        size_t start = 0;
        for (size_t i = 0; i <= (size_t)len; ++i) {
            if (i == (size_t)len || p[i] == '\n') {
                if (i > start) v.emplace_back(p + start, i - start);
                start = i + 1;
            }
        }
        return v;
    }

    static bool _in_common_pw(std::string_view key)
    {
        static const std::vector<std::string_view> idx =
            _build_index(g_common_pw_blob, g_common_pw_blob_len);
        return std::binary_search(idx.begin(), idx.end(), key);
    }

    static bool _in_common_word(std::string_view key)
    {
        static const std::vector<std::string_view> idx =
            _build_index(g_common_word_blob, g_common_word_blob_len);
        return std::binary_search(idx.begin(), idx.end(), key);
    }

    static std::string _to_lower(const std::string& s)
    {
        std::string r = s;
        std::transform(r.begin(), r.end(), r.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });
        return r;
    }

    static std::string _reverse(const std::string& s)
    {
        return std::string(s.rbegin(), s.rend());
    }

    // "pa$$w0rd" -> "password"; '1' is ambiguous (i vs l), callers try both.
    static std::string _deleet(const std::string& s, bool oneToL)
    {
        std::string r; r.reserve(s.size());
        for (unsigned char c : s) {
            char m;
            switch (c) {
            case '@': case '4':           m = 'a'; break;
            case '8':                     m = 'b'; break;
            case '(': case '{': case '<': m = 'c'; break;
            case '3':                     m = 'e'; break;
            case '6': case '9':           m = 'g'; break;
            case '#':                     m = 'h'; break;
            case '1':                     m = oneToL ? 'l' : 'i'; break;
            case '!': case '|':           m = 'i'; break;
            case '0':                     m = 'o'; break;
            case '$': case '5':           m = 's'; break;
            case '7': case '+':           m = 't'; break;
            case '2':                     m = 'z'; break;
            default:                      m = (char)std::tolower(c); break;
            }
            r += m;
        }
        return r;
    }

    // "passworddd" -> "password"; singles/doubles left alone ("feel" stays "feel").
    static std::string _collapse_repeats(const std::string& s)
    {
        std::string r; r.reserve(s.size());
        size_t i = 0;
        while (i < s.size()) {
            char c = s[i];
            size_t j = i;
            while (j < s.size() && s[j] == c) ++j;
            r += c;
            if (j - i == 2) r += c;
            i = j;
        }
        return r;
    }

    static bool _matches_with_leet(const std::string& word, bool (*pred)(std::string_view))
    {
        if (pred(word)) return true;
        std::string a = _deleet(word, false); // 1→i
        if (a != word && pred(a)) return true;
        std::string b = _deleet(word, true);  // 1→l
        if (b != a && b != word && pred(b)) return true;
        return false;
    }

    // "maggie21" -> "maggie"
    static std::string _extract_base_word(const std::string& pw)
    {
        std::string low = _to_lower(pw);
        size_t end = low.size();
        while (end > 0 && !std::isalpha((unsigned char)low[end - 1])) --end;
        size_t start = 0;
        while (start < end && !std::isalpha((unsigned char)low[start])) ++start;
        if (end - start < 3) return "";
        return low.substr(start, end - start);
    }

    static bool _is_keyboard_walk(const std::string& s)
    {
        if (s.size() < 4) return false;
        std::string low = _to_lower(s);
        static const char* rows[] = {
            "qwertyuiop", "asdfghjkl", "zxcvbnm", "1234567890",
            "1qaz2wsx3edc4rfv5tgb6yhn7ujm8ik9ol0p", // vertical columns
            "qazwsxedcrfvtgbyhnujmikolp",           // diagonal-ish
        };
        for (const char* row : rows) {
            std::string fwd(row);
            std::string rev(fwd.rbegin(), fwd.rend());
            for (size_t i = 0; i + 4 <= low.size(); ++i) {
                std::string w = low.substr(i, 4);
                if (fwd.find(w) != std::string::npos ||
                    rev.find(w) != std::string::npos)
                    return true;
            }
        }
        return false;
    }

    static bool _is_common_password(const std::string& pw)
    {
        std::string low = _to_lower(pw);
        if (_matches_with_leet(low, &_in_common_pw)) return true;
        std::string rev = _reverse(low);
        if (rev != low && _in_common_pw(rev)) return true;
        return false;
    }

    static bool _is_dictionary_based(const std::string& pw)
    {
        std::string base = _extract_base_word(pw);
        if (base.size() < 3) return false;
        if (_matches_with_leet(base, &_in_common_word)) return true;
        if (_matches_with_leet(base, &_in_common_pw))   return true;
        std::string col = _collapse_repeats(base);
        if (col != base && col.size() >= 3 &&
            (_in_common_word(col) || _in_common_pw(col))) return true;
        return false;
    }

    // Password strength
    PwStrength analyze_password(const std::string& pw)
    {
        PwStrength r;
        if (pw.empty()) return r;
        r.shortPwd = pw.size() < 8;

        int charsetSize = 0;
        for (char c : pw) {
            if (c >= 'a' && c <= 'z') r.hasLower = true;
            else if (c >= 'A' && c <= 'Z') r.hasUpper = true;
            else if (c >= '0' && c <= '9') r.hasDigit = true;
            else r.hasSymbol = true;
        }
        if (r.hasLower) charsetSize += 26;
        if (r.hasUpper) charsetSize += 26;
        if (r.hasDigit) charsetSize += 10;
        if (r.hasSymbol) charsetSize += 33;
        if (charsetSize == 0) charsetSize = 26;
        r.entropy_bits = pw.size() * log2((double)charsetSize);

        bool allSame = true;
        for (size_t i = 1; i < pw.size(); i++) if (pw[i] != pw[0]) { allSame = false; break; }
        r.allSame = allSame;

        // Count character classes used
        int classes = (r.hasLower ? 1 : 0) + (r.hasUpper ? 1 : 0)
                    + (r.hasDigit ? 1 : 0) + (r.hasSymbol ? 1 : 0);

        // Check for sequential characters (abc, 123, etc.)
        int seqCount = 0;
        for (size_t i = 2; i < pw.size(); i++) {
            if ((pw[i] == pw[i-1] + 1 && pw[i-1] == pw[i-2] + 1) ||
                (pw[i] == pw[i-1] - 1 && pw[i-1] == pw[i-2] - 1))
                seqCount++;
        }
        bool tooSequential = seqCount > (int)pw.size() / 3;
        r.tooSequential = tooSequential;
        r.charClasses = classes;

        // Check for repeated patterns (e.g., "abcabc")
        bool repeatedPattern = false;
        for (int patLen = 1; patLen <= (int)pw.size() / 2; patLen++) {
            bool allMatch = true;
            for (size_t i = patLen; i < pw.size(); i++) {
                if (pw[i] != pw[i % patLen]) { allMatch = false; break; }
            }
            if (allMatch) { repeatedPattern = true; break; }
        }
        r.repeatedPattern = repeatedPattern;

        // Keyboard walks + known-common + dictionary-based (ported from pw-windows).
        // isDictWord gated on !isCommonPwd so a known-common password doesn't double-flag.
        r.keyboardWalk = _is_keyboard_walk(pw);
        r.isCommonPwd  = _is_common_password(pw);
        r.isDictWord   = !r.isCommonPwd && _is_dictionary_based(pw);

        // Scoring with penalties
        if (r.entropy_bits < 28 || r.allSame || r.shortPwd || repeatedPattern) r.score = 0;
        else if (r.entropy_bits < 36 || classes == 1) r.score = 1;
        else if (r.entropy_bits < 50 || classes <= 2 || tooSequential) r.score = 2;
        else if (r.entropy_bits < 70 || classes <= 2) r.score = 2;
        else if (r.entropy_bits < 80 || classes <= 3) r.score = 3;
        else r.score = 4;

        // Bonus: very long passwords with 3+ classes get a bump
        if (pw.size() >= 20 && classes >= 3 && r.score < 4) r.score = std::min(r.score + 1, 4);
        // Penalty: no symbols or no mixed case caps at max 3
        if (!r.hasSymbol && r.score > 3) r.score = 3;
        if ((!r.hasUpper || !r.hasLower) && r.score > 3) r.score = 3;

        // Semantic penalties (match pw-windows weighting):
        //   keyboardWalk -2 — "qwerty"-family
        //   isDictWord   -2 — base word is dictionary / name
        //   isCommonPwd  -3 — already appears in SecLists top-100k
        if (r.keyboardWalk) r.score = std::max(0, r.score - 2);
        if (r.isDictWord)   r.score = std::max(0, r.score - 2);
        if (r.isCommonPwd)  r.score = std::max(0, r.score - 3);

        return r;
    }

    unsigned int strength_color(int score)
    {
        switch (score) {
            case 0: return IM_COL32(200, 50, 50, 255);
            case 1: return IM_COL32(220, 120, 30, 255);
            case 2: return IM_COL32(220, 200, 30, 255);
            case 3: return IM_COL32(100, 200, 50, 255);
            case 4: return IM_COL32(50, 180, 50, 255);
            default: return IM_COL32(128, 128, 128, 255);
        }
    }

    const char* strength_label(int score)
    {
        switch (score) {
            case 0: return "Very Weak";
            case 1: return "Weak";
            case 2: return "Fair";
            case 3: return "Strong";
            case 4: return "Very Strong";
            default: return "";
        }
    }

    void DrawStrengthMeter(const std::string& pw, float width) { /* TODO */ }

    // RNG
    bool secure_rand_bytes(void* dst, size_t len)
    {
        return arc4random_buf(dst, len), true;
    }

    size_t secure_rand_index(size_t n)
    {
        return arc4random_uniform((uint32_t)n);
    }

    std::string generate_uuid()
    {
        uint8_t bytes[16];
        arc4random_buf(bytes, sizeof(bytes));
        bytes[6] = (bytes[6] & 0x0F) | 0x40;
        bytes[8] = (bytes[8] & 0x3F) | 0x80;
        char buf[40];
        snprintf(buf, sizeof(buf),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 bytes[0],bytes[1],bytes[2],bytes[3],
                 bytes[4],bytes[5], bytes[6],bytes[7],
                 bytes[8],bytes[9], bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]);
        return buf;
    }

    // Build per-class charsets (respecting avoid_ambiguous) and the combined
    // charset. Ported from Windows tools/utility.cpp:590 build_charset.
    static inline std::string build_charset(const GenOptions& opt,
        std::string& upper, std::string& lower,
        std::string& digit, std::string& symbol)
    {
        upper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        lower = "abcdefghijklmnopqrstuvwxyz";
        digit = "0123456789";
        symbol = "~!@#$%^&*()_+-={}[]|:;\"'<>,.?/";

        if (opt.avoid_ambiguous)
        {
            auto strip = [](std::string& s, const char* bad) {
                s.erase(std::remove_if(s.begin(), s.end(),
                    [&](char c) { return std::strchr(bad, c) != nullptr; }),
                    s.end());
            };
            strip(upper, "OI");
            strip(lower, "ol");
            strip(digit, "01");
        }

        std::string charset;
        if (opt.use_upper)  charset += upper;
        if (opt.use_lower)  charset += lower;
        if (opt.use_digit)  charset += digit;
        if (opt.use_symbol) charset += symbol;
        return charset;
    }

    // Password generator — Windows-parity (tools/utility.cpp:620).
    // Guarantees ≥1 character from each enabled class (so toggling "Digits"
    // doesn't produce a 30-char password with zero digits), then fills the
    // remainder from the combined pool, then Fisher-Yates shuffles so the
    // guaranteed chars aren't always at the front. Also respects
    // avoid_ambiguous (strips 0/O/o/1/l/I from their respective classes).
    std::string generate_password(const GenOptions& opt)
    {
        std::string upper, lower, digit, symbol;
        std::string all = build_charset(opt, upper, lower, digit, symbol);

        // User disabled everything → nothing to generate.
        if (all.empty() || opt.length <= 0)
            return {};

        std::string out;
        out.reserve((size_t)opt.length);

        auto push_one = [&](const std::string& set) {
            if (!set.empty())
                out.push_back(set[secure_rand_index(set.size())]);
        };

        // Ensure at least one from each enabled class.
        if (opt.use_upper)  push_one(upper);
        if (opt.use_lower)  push_one(lower);
        if (opt.use_digit)  push_one(digit);
        if (opt.use_symbol) push_one(symbol);

        while ((int)out.size() < opt.length)
            out.push_back(all[secure_rand_index(all.size())]);

        // Fisher-Yates with cryptographically secure index.
        for (size_t i = out.size(); i > 1; --i)
        {
            size_t j = secure_rand_index(i);
            std::swap(out[i - 1], out[j]);
        }

        // If length is shorter than enabled-class count, trim the overshoot.
        if ((int)out.size() > opt.length)
            out.resize((size_t)opt.length);

        return out;
    }

    double estimate_entropy_bits(const GenOptions& opt)
    {
        // Use the actual charset size (post avoid_ambiguous strip) rather
        // than hardcoded class sizes — otherwise entropy is overestimated
        // when avoid_ambiguous is on.
        std::string u, l, d, s;
        std::string all = build_charset(opt, u, l, d, s);
        if (all.empty() || opt.length <= 0) return 0.0;
        return opt.length * log2((double)all.size());
    }

    std::string GetDefaultBackupDir() {
        std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
        std::string dir = home + "/Library/Application Support/PasswordDefense/Backups";
        std::filesystem::create_directories(dir);
        return dir;
    }
    void SanitizeFilename(std::string& s) {
        for (char& c : s) if (c == '/' || c == '\\' || c == ':') c = '_';
    }
    bool CopyFileAtomic(const std::string& src, const std::string& dst) {
        try { std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing); return true; }
        catch (...) { return false; }
    }
    void EnforceBackupRetention(const std::string& dir, size_t keep_count) {
        if (keep_count == 0) return;
        std::vector<std::filesystem::directory_entry> backups;
        try {
            for (const auto& e : std::filesystem::directory_iterator(dir)) {
                if (e.is_regular_file() && e.path().extension() == ".lbdb")
                    backups.push_back(e);
            }
        } catch (...) { return; }
        if (backups.size() <= keep_count) return;
        // Sort oldest first by last write time
        std::sort(backups.begin(), backups.end(), [](const auto& a, const auto& b) {
            return std::filesystem::last_write_time(a) < std::filesystem::last_write_time(b);
        });
        size_t to_remove = backups.size() - keep_count;
        for (size_t i = 0; i < to_remove; ++i) {
            try { std::filesystem::remove(backups[i].path()); } catch (...) {}
        }
    }

    std::vector<std::string> ListBackupsForVault(const std::string& vault_path) {
        std::vector<std::string> result;
        std::string dir = GetDefaultBackupDir();
        std::string vault_base = std::filesystem::path(vault_path).stem().string();
        try {
            std::vector<std::filesystem::directory_entry> entries;
            for (const auto& e : std::filesystem::directory_iterator(dir)) {
                if (!e.is_regular_file()) continue;
                if (e.path().extension() != ".lbdb") continue;
                std::string name = e.path().filename().string();
                if (name.find(vault_base) == 0)
                    entries.push_back(e);
            }
            // Sort newest first
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                return std::filesystem::last_write_time(a) > std::filesystem::last_write_time(b);
            });
            for (const auto& e : entries)
                result.push_back(e.path().string());
        } catch (...) {}
        return result;
    }
    std::string Basename(const std::string& path) {
        auto pos = path.find_last_of("/\\");
        return pos != std::string::npos ? path.substr(pos + 1) : path;
    }
    bool GetFileInfo(const std::string& path, uint64_t& outSizeBytes, std::string& outLocalTimeStr) {
        try {
            outSizeBytes = std::filesystem::file_size(path);
            // Get file modification time via stat
            struct stat st;
            if (stat(path.c_str(), &st) == 0) {
                struct tm local;
                localtime_r(&st.st_mtime, &local);
                char buf[32];
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local);
                outLocalTimeStr = buf;
            } else {
                outLocalTimeStr = "";
            }
            return true;
        } catch (...) { return false; }
    }
    std::string FormatBytes(uint64_t bytes) {
        if (bytes < 1024) return std::to_string(bytes) + " B";
        if (bytes < 1024*1024) return std::to_string(bytes/1024) + " KB";
        return std::to_string(bytes/(1024*1024)) + " MB";
    }
    BackupCreateResult CreateBackupNow(const std::string& vault_path, size_t keep_count) {
        BackupCreateResult r;
        if (vault_path.empty()) { r.err = "No vault open"; return r; }

        std::string dir = GetDefaultBackupDir();
        create_directories(dir);

        std::string base = std::filesystem::path(vault_path).stem().string();
        std::string ts = now_iso8601_local();
        SanitizeFilename(ts);

        std::string dst = dir + "/" + base + "_MANUAL_" + ts + ".lbdb";

        if (CopyFileAtomic(vault_path, dst)) {
            r.ok = true;
            r.outPath = dst;
            EnforceBackupRetention(dir, keep_count);
        } else {
            r.err = "Failed to copy vault file";
        }
        return r;
    }
}
