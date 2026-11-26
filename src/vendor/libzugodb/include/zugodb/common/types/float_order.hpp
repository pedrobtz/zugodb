#pragma once

// DuckDB's order of FLOAT and DOUBLE values (libzugodb #42, design L30):
// NaN equals NaN, whatever its sign or payload, and is greater than every
// other value, +inf included; -0.0 equals 0.0. IEEE's operators give
// neither (NaN is unordered, and a comparator built on them is not a
// strict weak order, so std::sort over it is undefined behaviour). Every
// comparison, sort, hash and key of a floating-point value goes through
// these. For any other type they are the built-in operators, so a template
// over the column type can call them unconditionally.
//
// NaN is told by its bits, not by std::isnan or `x != x`: the R package
// compiles this code with the user's flags, and -ffast-math or
// -ffinite-math-only folds both to false.

#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace zugodb {

inline bool IsNaN(double d) {
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof bits);
    return (bits & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL;
}

inline bool IsNaN(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    return (bits & 0x7fffffffu) > 0x7f800000u;
}

// The sign bit, of a NaN too (std::signbit, which -ffast-math keeps, but
// spelled out beside IsNaN).
inline bool SignBit(double d) {
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof bits);
    return (bits >> 63) != 0;
}

inline bool SignBit(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    return (bits >> 31) != 0;
}

// a = b in DuckDB's order.
template <class T>
inline bool SqlEq(T a, T b) {
    if constexpr (std::is_floating_point_v<T>) {
        bool na = IsNaN(a), nb = IsNaN(b);
        if (na || nb) return na && nb;
    }
    return a == b;
}

// a < b in DuckDB's order: a strict weak order for every T.
template <class T>
inline bool SqlLess(T a, T b) {
    if constexpr (std::is_floating_point_v<T>) {
        if (IsNaN(a)) return false;
        if (IsNaN(b)) return true;
    }
    return a < b;
}

template <class T> inline bool SqlGreater(T a, T b) { return SqlLess(b, a); }
template <class T> inline bool SqlLessEq(T a, T b) { return !SqlLess(b, a); }
template <class T> inline bool SqlGreaterEq(T a, T b) { return !SqlLess(a, b); }

// One bit pattern per class of equal values, for a hash or a group key:
// every NaN is the positive quiet NaN and -0.0 is 0.0. The value a group
// shows keeps its own bits; only its key is canonical.
inline double CanonicalKey(double d) {
    if (IsNaN(d)) {
        uint64_t bits = 0x7ff8000000000000ULL;
        std::memcpy(&d, &bits, sizeof d);
        return d;
    }
    return d == 0.0 ? 0.0 : d;
}

// Whether a finite double rounds past FLOAT's range: |d| at or above
// FLT_MAX plus half its last unit, which rounds to an infinity. A cast of
// such a double to float is undefined behaviour in C++, so callers test
// first; DuckDB's cast of the DOUBLE is an error, of text an infinity.
inline bool FloatOverflows(double d) {
    if (IsNaN(d)) return false;
    double m = d < 0 ? -d : d;
    return m >= 3.4028235677973366e38 && m != std::numeric_limits<double>::infinity();
}

// The float a double rounds to, an infinity past FLOAT's range.
inline float ToFloatSaturating(double d) {
    if (FloatOverflows(d)) {
        return d < 0 ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
    }
    return static_cast<float>(d);
}

// MIN and MAX keep the last of equal values, as DuckDB's do (min over
// 0.0 then -0.0 is -0.0; over 'nan' then '-nan' is '-nan'): `x` replaces
// the state unless the state is strictly better.
template <class T> inline bool MinReplaces(T state, T x) { return !SqlLess(state, x); }
template <class T> inline bool MaxReplaces(T state, T x) { return !SqlLess(x, state); }

} // namespace zugodb
