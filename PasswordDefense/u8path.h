// u8path.h — the app's single string encoding for file paths is UTF-8.
//
// Ported shim for the offline macOS fork. On Windows std::filesystem::path's
// native encoding is UTF-16, so paths held in a UTF-8 std::string must be
// converted at every narrow-API boundary (see the Windows u8path.h). On macOS
// libc++ std::filesystem::path's native encoding is already char/UTF-8, so these
// are passthroughs. They exist so cross-platform call sites ported verbatim from
// pw-windows (recovery snapshots, config/vault paths) compile unchanged here.
#pragma once

#include <filesystem>
#include <string>

namespace helpers
{
    // UTF-8 string → filesystem path (no conversion needed on macOS)
    inline std::filesystem::path U8Path(const std::string& utf8)
    {
        return std::filesystem::path(utf8);
    }

    // filesystem path → UTF-8 string (path::string() is already UTF-8 on macOS;
    // unlike Windows it never routes through the ANSI code page or throws on CJK)
    inline std::string PathToU8(const std::filesystem::path& p)
    {
        return p.string();
    }
}
