#pragma once

// BLOB text, as DuckDB's Blob::ToString and Blob::FromString at the
// pinned tag (libzugodb Stage 8, #31): a BLOB holds bytes, and its text
// writes a byte outside printable ASCII, a backslash and the quotes as
// \xHH. Text to BLOB reads \xHH as one byte and every other ASCII
// character as itself; any other escape or byte is an error.

#include "zugodb/common/exception.hpp"

#include <string>

namespace zugodb {
namespace blob {

inline bool IsRegular(unsigned char c) {
    return c >= 32 && c <= 126 && c != '\\' && c != '\'' && c != '"';
}

inline std::string ToText(const std::string &bytes) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size());
    for (unsigned char c : bytes) {
        if (IsRegular(c)) {
            out += static_cast<char>(c);
        } else {
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

inline int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Throws ConversionException on text that is not a BLOB's.
inline std::string FromText(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\\') {
            if (i + 3 >= text.size()) {
                throw ConversionException(
                    "Invalid hex escape code encountered in string -> blob conversion: "
                    "unterminated escape code at end of blob");
            }
            int hi = HexDigit(text[i + 2]);
            int lo = HexDigit(text[i + 3]);
            if (text[i + 1] != 'x' || hi < 0 || lo < 0) {
                throw ConversionException(
                    "Invalid hex escape code encountered in string -> blob conversion: " +
                    text.substr(i, 4));
            }
            out += static_cast<char>(hi * 16 + lo);
            i += 3;
        } else if (c <= 127) {
            out += static_cast<char>(c);
        } else {
            throw ConversionException(
                "Invalid byte encountered in STRING -> BLOB conversion. All non-ascii "
                "characters must be escaped with hex codes (e.g. \\xAA)");
        }
    }
    return out;
}

} // namespace blob
} // namespace zugodb
