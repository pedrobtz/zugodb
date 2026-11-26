#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <cstdarg>

namespace zugodb {

class StringUtil {
public:
    static std::string Upper(const std::string &str);
    static std::string Lower(const std::string &str);
    static std::string Format(const char *fmt, ...);
    static std::string FormatV(const char *fmt, va_list args);
    static std::vector<std::string> Split(const std::string &str, char delimiter);
    static std::string Join(const std::vector<std::string> &parts, const std::string &separator);
    static bool StartsWith(const std::string &str, const std::string &prefix);
    static bool EndsWith(const std::string &str, const std::string &suffix);
    static bool Contains(const std::string &str, const std::string &substr);
    static std::string Replace(const std::string &str, const std::string &from, const std::string &to);
    static std::string Trim(const std::string &str);
};

// Identifiers match as DuckDB's do, ignoring the case of ASCII letters,
// quoted or not (Stage 9, design L26); the spelling a name was created
// with is kept. Other bytes compare as they are.
inline char FoldIdentifierChar(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
inline bool IdentifierEquals(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (FoldIdentifierChar(a[i]) != FoldIdentifierChar(b[i])) return false;
    }
    return true;
}
struct IdentifierHash {
    size_t operator()(const std::string &s) const {
        size_t h = 14695981039346656037ULL & static_cast<size_t>(-1);
        for (char c : s) {
            h ^= static_cast<unsigned char>(FoldIdentifierChar(c));
            h *= static_cast<size_t>(1099511628211ULL);
        }
        return h;
    }
};
struct IdentifierEqual {
    bool operator()(const std::string &a, const std::string &b) const {
        return IdentifierEquals(a, b);
    }
};

} // namespace zugodb
