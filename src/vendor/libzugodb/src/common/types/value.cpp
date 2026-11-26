#include "zugodb/common/types/value.hpp"
#include "zugodb/common/types/blob.hpp"
#include "zugodb/common/exception.hpp"
#include "zugodb/common/types/float_order.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace zugodb {

// DuckDB's text for a FLOAT or DOUBLE, its cast to VARCHAR (fmt 6's "{}"
// in string_cast.cpp): the shortest digits that read back as the value; a
// plain decimal with at least one fractional digit ("1.0", "0.001") when
// the decimal exponent is in [-4, 16), d[.ddd]e±XX otherwise ("1e+20",
// "1.5e-05"); "-0.0"; "inf", "-inf"; and "nan" or "-nan" by the sign bit
// (#42). It gave "1" for 1.0 and "100" for 100.0, and a NaN as the C
// library prints it, "nan(ind)" under MinGW's UCRT.
//
// The digits come from %.*e at increasing precision, not std::to_chars:
// GCC < 11's libstdc++ rejects that call as ambiguous, and libc++ marks
// it unavailable below macOS 13.3. %.*e rounds correctly, so the first
// precision that reads back is the shortest, and the nearest at it.
template <typename T>
static std::string ShortestFloatString(T value) {
    if (IsNaN(value)) return SignBit(value) ? "-nan" : "nan";
    bool negative = SignBit(value);
    if (value == std::numeric_limits<T>::infinity()) return "inf";
    if (value == -std::numeric_limits<T>::infinity()) return "-inf";
    if (value == static_cast<T>(0)) return negative ? "-0.0" : "0.0";
    T mag = negative ? -value : value;
    const int max_precision = sizeof(T) == 4 ? 8 : 16;
    char buf[64];
    for (int prec = 0; prec <= max_precision; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*e", prec, static_cast<double>(mag));
        if (prec == max_precision || static_cast<T>(std::strtod(buf, nullptr)) == mag) break;
    }
    // buf is d[.ddd]e[+-]X...
    std::string sci(buf);
    size_t epos = sci.find('e');
    int exp10 = std::atoi(sci.c_str() + epos + 1);
    std::string digits;
    for (size_t i = 0; i < epos; i++) {
        if (sci[i] != '.') digits += sci[i];
    }
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();

    std::string out = negative ? "-" : "";
    int n = static_cast<int>(digits.size());
    if (!(exp10 >= -4 && exp10 < 16)) {
        out += digits[0];
        if (n > 1) out += "." + digits.substr(1);
        out += exp10 < 0 ? "e-" : "e+";
        int e = exp10 < 0 ? -exp10 : exp10;
        std::snprintf(buf, sizeof(buf), "%02d", e);
        out += buf;
        return out;
    }
    int whole = exp10 + 1;  // digits before the decimal point
    if (n <= whole) {
        out += digits + std::string(static_cast<size_t>(whole - n), '0') + ".0";
    } else if (whole > 0) {
        out += digits.substr(0, static_cast<size_t>(whole)) + "." +
               digits.substr(static_cast<size_t>(whole));
    } else {
        out += "0." + std::string(static_cast<size_t>(-whole), '0') + digits;
    }
    return out;
}

// Convert days-since-1970-01-01 to (Y, M, D) using Howard Hinnant's
// civil-from-days. Used to render DATE values as ISO-8601 strings instead
// of opaque integers. In 64 bits, so any day a DATE or TIMESTAMP can hold
// converts without overflow.
static void DaysToYMD(int64_t days, int64_t &y, unsigned &m, unsigned &d) {
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yr = static_cast<int64_t>(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = yr + (m <= 2);
}

// DuckDB's text for a date: the year with at least four digits, and a
// year before 1 as its positive BC year with " (BC)" after the day.
static std::string FormatDateParts(int64_t days) {
    int64_t y; unsigned m, d;
    DaysToYMD(days, y, m, d);
    bool bc = y <= 0;
    if (bc) y = 1 - y;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u%s", static_cast<long long>(y), m, d,
                  bc ? " (BC)" : "");
    return std::string(buf);
}

// DuckDB's text for a time of day: HH:MM:SS, then the microseconds with
// their trailing zeros cut. 24:00:00 is a TIME of its own.
static std::string FormatTimeOfDay(int64_t tod) {
    int64_t secs = tod / 1000000;
    int us = static_cast<int>(tod % 1000000);
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", static_cast<int>(secs / 3600),
                          static_cast<int>((secs / 60) % 60), static_cast<int>(secs % 60));
    std::string out(buf, static_cast<size_t>(n));
    if (us != 0) {
        char frac[8];
        std::snprintf(frac, sizeof(frac), "%06d", us);
        std::string f(frac);
        while (!f.empty() && f.back() == '0') f.pop_back();
        out += "." + f;
    }
    return out;
}

// Format a TIMESTAMP value (microseconds since 1970-01-01) as DuckDB
// does. Avoids gmtime so this is reentrant and works the same on every
// platform.
static std::string FormatTimestamp(int64_t micros) {
    int64_t days = micros / 86400000000LL;
    int64_t tod = micros % 86400000000LL;
    if (tod < 0) { tod += 86400000000LL; days -= 1; }
    return FormatDateParts(days) + " " + FormatTimeOfDay(tod);
}

static std::string FormatDate(int32_t days) { return FormatDateParts(days); }

static std::string FormatTime(int64_t micros) {
    if (micros >= 0 && micros <= 86400000000LL) return FormatTimeOfDay(micros);
    int64_t tod = micros % 86400000000LL;
    if (tod < 0) tod += 86400000000LL;
    return FormatTimeOfDay(tod);
}

Value::Value() : type_(LogicalType::SQLNULL()), is_null_(true), bigint_(0) {}

Value::Value(const Value &other)
    : type_(other.type_), is_null_(other.is_null_), str_value_(other.str_value_),
      list_value_(other.list_value_) {
    std::memcpy(&boolean_, &other.boolean_, sizeof(hugeint_));
}

Value::Value(Value &&other) noexcept
    : type_(std::move(other.type_)), is_null_(other.is_null_),
      str_value_(std::move(other.str_value_)), list_value_(std::move(other.list_value_)) {
    std::memcpy(&boolean_, &other.boolean_, sizeof(hugeint_));
}

Value &Value::operator=(const Value &other) {
    if (this != &other) {
        type_ = other.type_;
        is_null_ = other.is_null_;
        std::memcpy(&boolean_, &other.boolean_, sizeof(hugeint_));
        str_value_ = other.str_value_;
        list_value_ = other.list_value_;
    }
    return *this;
}

Value &Value::operator=(Value &&other) noexcept {
    if (this != &other) {
        type_ = std::move(other.type_);
        is_null_ = other.is_null_;
        std::memcpy(&boolean_, &other.boolean_, sizeof(hugeint_));
        str_value_ = std::move(other.str_value_);
        list_value_ = std::move(other.list_value_);
    }
    return *this;
}

Value::Value(bool val) : type_(LogicalType::BOOLEAN()), is_null_(false), boolean_(val) {}
Value::Value(int8_t val) : type_(LogicalType::TINYINT()), is_null_(false), tinyint_(val) {}
Value::Value(int16_t val) : type_(LogicalType::SMALLINT()), is_null_(false), smallint_(val) {}
Value::Value(int32_t val) : type_(LogicalType::INTEGER()), is_null_(false), integer_(val) {}
Value::Value(int64_t val) : type_(LogicalType::BIGINT()), is_null_(false), bigint_(val) {}
Value::Value(uint8_t val) : type_(LogicalType::UTINYINT()), is_null_(false), utinyint_(val) {}
Value::Value(uint16_t val) : type_(LogicalType::USMALLINT()), is_null_(false), usmallint_(val) {}
Value::Value(uint32_t val) : type_(LogicalType::UINTEGER()), is_null_(false), uinteger_(val) {}
Value::Value(uint64_t val) : type_(LogicalType::UBIGINT()), is_null_(false), ubigint_(val) {}
Value::Value(float val) : type_(LogicalType::FLOAT()), is_null_(false), float_(val) {}
Value::Value(double val) : type_(LogicalType::DOUBLE()), is_null_(false), double_(val) {}
Value::Value(hugeint_t val) : type_(LogicalType::HUGEINT()), is_null_(false), hugeint_(val) {}

Value::Value(const char *val)
    : type_(LogicalType::VARCHAR()), is_null_(false), bigint_(0), str_value_(val) {}
Value::Value(const std::string &val)
    : type_(LogicalType::VARCHAR()), is_null_(false), bigint_(0), str_value_(val) {}
Value::Value(std::string &&val)
    : type_(LogicalType::VARCHAR()), is_null_(false), bigint_(0), str_value_(std::move(val)) {}

Value Value::BOOLEAN(bool val) { return Value(val); }
Value Value::TINYINT(int8_t val) { return Value(val); }
Value Value::SMALLINT(int16_t val) { return Value(val); }
Value Value::INTEGER(int32_t val) { return Value(val); }
Value Value::BIGINT(int64_t val) { return Value(val); }
Value Value::HUGEINT(hugeint_t val) { return Value(val); }
Value Value::UTINYINT(uint8_t val) { return Value(val); }
Value Value::USMALLINT(uint16_t val) { return Value(val); }
Value Value::UINTEGER(uint32_t val) { return Value(val); }
Value Value::UBIGINT(uint64_t val) { return Value(val); }
Value Value::FLOAT(float val) { return Value(val); }
Value Value::DOUBLE(double val) { return Value(val); }
Value Value::VARCHAR(const std::string &val) { return Value(val); }
Value Value::VARCHAR(const char *val) { return Value(val); }

Value Value::BLOB(const std::string &val) {
    Value result(val);
    result.type_ = LogicalType::BLOB();
    return result;
}

Value Value::DATE(int32_t days) {
    Value result(days);
    result.type_ = LogicalType::DATE();
    return result;
}

Value Value::TIMESTAMP(int64_t micros) {
    Value result(micros);
    result.type_ = LogicalType::TIMESTAMP();
    return result;
}

// ---------------------------------------------------------------------------
// DuckDB's grammar for dates, times and timestamps (design L19).
// ---------------------------------------------------------------------------

namespace {

using PR = Value::ParseResult;

bool IsSpaceChar(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
bool IsDigitChar(char c) { return c >= '0' && c <= '9'; }

// One or two digits (Date::ParseDoubleDigit).
bool ParseDoubleDigit(const char *buf, size_t len, size_t &pos, int32_t &result) {
    if (pos < len && IsDigitChar(buf[pos])) {
        result = buf[pos++] - '0';
        if (pos < len && IsDigitChar(buf[pos])) result = buf[pos++] - '0' + result * 10;
        return true;
    }
    return false;
}

bool IsLeapYear(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int32_t DaysInMonth(int64_t y, int32_t m) {
    static const int32_t kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && IsLeapYear(y) ? 29 : kDays[m - 1];
}

// DuckDB's DATE range: 5877642-06-25 (BC) to 5881580-07-10, the days
// strictly inside int32 (its infinities are the two ends).
constexpr int64_t kMinDateDays = -2147483646;
constexpr int64_t kMaxDateDays = 2147483646;
// DuckDB's TIMESTAMP range: 290309-12-22 (BC) 00:00:00 to
// 294247-01-10 04:00:54.775806.
constexpr int64_t kMinTimestampMicros = -9223372022400000000LL;
constexpr int64_t kMaxTimestampMicros = 9223372036854775806LL;
constexpr int64_t kMicrosPerDay = 86400000000LL;

int64_t DaysFromCivil(int64_t y, int32_t m, int32_t d) {
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool MatchWord(const char *buf, size_t len, size_t pos, const char *word) {
    size_t n = std::strlen(word);
    if (len - pos < n) return false;
    for (size_t i = 0; i < n; i++) {
        char c = buf[pos + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        if (c != word[i]) return false;
    }
    return true;
}

// Date::TryConvertDate. `strict`: nothing but spaces may follow; otherwise
// anything but a digit may (a timestamp's time). `pos` is left after the
// date.
PR ParseDateAt(const char *buf, size_t len, size_t &pos, int32_t &out, bool strict) {
    pos = 0;
    if (len == 0) return PR::Invalid;
    while (pos < len && IsSpaceChar(buf[pos])) pos++;
    if (pos >= len) return PR::Invalid;
    bool yearneg = false;
    if (buf[pos] == '-') {
        yearneg = true;
        pos++;
        if (pos >= len) return PR::Invalid;
    }
    if (!IsDigitChar(buf[pos])) {
        // 'infinity', '-infinity' and 'epoch' are DuckDB's special
        // values; the engine has no infinite DATE. Any other word is
        // refused too rather than guessed invalid.
        if (!yearneg && MatchWord(buf, len, pos, "epoch")) {
            size_t p = pos + 5;
            while (p < len && IsSpaceChar(buf[p])) p++;
            if (p == len) {
                pos = p;
                out = 0;
                return PR::Ok;
            }
        }
        (void)strict;
        return PR::Unsupported;
    }
    int64_t year = 0;
    while (pos < len && IsDigitChar(buf[pos])) {
        if (year >= 100000000) return PR::Invalid;
        year = (buf[pos] - '0') + year * 10;
        pos++;
    }
    if (yearneg) year = -year;
    if (pos >= len) return PR::Invalid;
    char sep = buf[pos++];
    if (sep != ' ' && sep != '-' && sep != '/' && sep != '\\') return PR::Invalid;
    int32_t month = -1, day = -1;
    if (!ParseDoubleDigit(buf, len, pos, month)) return PR::Invalid;
    if (pos >= len) return PR::Invalid;
    if (buf[pos++] != sep) return PR::Invalid;
    if (pos >= len) return PR::Invalid;
    if (!ParseDoubleDigit(buf, len, pos, day)) return PR::Invalid;
    // An optional " (BC)".
    if (len - pos >= 5 && IsSpaceChar(buf[pos]) && buf[pos + 1] == '(' &&
        (buf[pos + 2] == 'b' || buf[pos + 2] == 'B') && (buf[pos + 3] == 'c' || buf[pos + 3] == 'C') &&
        buf[pos + 4] == ')') {
        if (yearneg || year == 0) return PR::Invalid;
        year = -year + 1;
        pos += 5;
    }
    // A digit straight after the day is never a date. Other text after
    // it is a time to DuckDB's DATE cast, which the engine does not take
    // apart there: refused, not guessed.
    if (pos < len && IsDigitChar(buf[pos])) return PR::Invalid;
    if (strict) {
        while (pos < len && IsSpaceChar(buf[pos])) pos++;
        if (pos < len) return PR::Unsupported;
    }
    if (month < 1 || month > 12 || day < 1 || day > DaysInMonth(year, month)) return PR::Invalid;
    int64_t days = DaysFromCivil(year, month, day);
    if (days < kMinDateDays || days > kMaxDateDays) return PR::Invalid;
    // DuckDB's dates reach 5877642 BC and 5881580; the engine's date
    // functions (strftime, date_diff, date_trunc, ...) were written for
    // years 1 to 9999, so a date outside them is refused.
    if (year < 1 || year > 9999) return PR::Unsupported;
    out = static_cast<int32_t>(days);
    return PR::Ok;
}

// Time::TryConvertInternal: H[...]:[MM[:[SS[.f...]]]], hours up to 24
// (24:00:00 itself), fractions past microseconds cut. `pos` is left
// after the time; `strict` as for dates.
PR ParseTimeAt(const char *buf, size_t len, size_t &pos, int64_t &out, bool strict) {
    pos = 0;
    while (pos < len && IsSpaceChar(buf[pos])) pos++;
    if (pos >= len) return PR::Invalid;
    if (!IsDigitChar(buf[pos])) return IsSpaceChar(buf[pos]) ? PR::Invalid : PR::Unsupported;
    int64_t hour = 0;
    int digits = 0;
    while (pos < len && IsDigitChar(buf[pos])) {
        if (++digits > 9) return PR::Invalid;
        hour = hour * 10 + (buf[pos] - '0');
        pos++;
    }
    if (pos >= len || buf[pos] != ':') return PR::Invalid;
    pos++;
    int32_t min = 0, sec = 0;
    int64_t micros = 0;
    if (pos < len && !IsSpaceChar(buf[pos])) {
        if (!ParseDoubleDigit(buf, len, pos, min)) return PR::Invalid;
        if (pos < len && buf[pos] == ':') {
            pos++;
            if (pos < len && !IsSpaceChar(buf[pos])) {
                if (!ParseDoubleDigit(buf, len, pos, sec)) return PR::Invalid;
                if (pos < len && buf[pos] == '.') {
                    pos++;
                    int64_t mult = 100000;
                    for (; pos < len && IsDigitChar(buf[pos]); pos++, mult /= 10) {
                        if (mult > 0) micros += (buf[pos] - '0') * mult;
                    }
                }
            }
        }
    }
    if (strict) {
        size_t p = pos;
        while (p < len && IsSpaceChar(buf[p])) p++;
        // Text after a time: DuckDB may read it (a time zone, a date
        // after all); the engine does not guess.
        if (p < len) return PR::Unsupported;
        pos = p;
    }
    if (min >= 60 || sec >= 60) return PR::Invalid;
    if (hour > 24 || (hour == 24 && (min != 0 || sec != 0 || micros != 0))) return PR::Invalid;
    out = ((hour * 60 + min) * 60 + sec) * 1000000 + micros;
    return PR::Ok;
}

} // namespace

Value::ParseResult Value::ParseDate(const char *s, size_t len, int32_t &out_days) {
    size_t pos = 0;
    return ParseDateAt(s, len, pos, out_days, true);
}

Value::ParseResult Value::ParseTimestamp(const char *s, size_t len, int64_t &out_micros) {
    size_t pos = 0;
    int32_t days = 0;
    auto r = ParseDateAt(s, len, pos, days, false);
    if (r != PR::Ok) return r;
    int64_t tod = 0;
    if (pos < len) {
        if (s[pos] == ' ' || s[pos] == 'T') pos++;
        size_t time_pos = 0;
        r = ParseTimeAt(s + pos, len - pos, time_pos, tod, false);
        if (r != PR::Ok) return r;
        pos += time_pos;
        if (pos < len && s[pos] == 'Z') pos++;
        while (pos < len && IsSpaceChar(s[pos])) pos++;
        // A UTC offset or a time zone name: not taken apart.
        if (pos < len) return PR::Unsupported;
    }
    constexpr int64_t kMaxDays = kMaxTimestampMicros / kMicrosPerDay;  // 106751991
    if (days < -kMaxDays || days > kMaxDays) return PR::Invalid;
    int64_t micros = static_cast<int64_t>(days) * kMicrosPerDay;
    if (micros > kMaxTimestampMicros - tod) return PR::Invalid;
    micros += tod;
    if (micros < kMinTimestampMicros) return PR::Invalid;
    out_micros = micros;
    return PR::Ok;
}

Value::ParseResult Value::ParseTime(const char *s, size_t len, int64_t &out_micros) {
    size_t pos = 0;
    auto r = ParseTimeAt(s, len, pos, out_micros, true);
    if (r != PR::Invalid) return r;
    // DuckDB then reads the text as a timestamp and keeps its time.
    int64_t ts = 0;
    auto t = ParseTimestamp(s, len, ts);
    if (t != PR::Ok) return t;
    int64_t tod = ts % kMicrosPerDay;
    if (tod < 0) tod += kMicrosPerDay;
    out_micros = tod;
    return PR::Ok;
}

Value Value::ParseTemporal(const std::string &str, LogicalTypeId to) {
    ParseResult r;
    Value out;
    if (to == LogicalTypeId::DATE) {
        int32_t days = 0;
        r = ParseDate(str.data(), str.size(), days);
        out = Value::DATE(days);
    } else if (to == LogicalTypeId::TIME) {
        int64_t micros = 0;
        r = ParseTime(str.data(), str.size(), micros);
        out = Value::TIME(micros);
    } else {
        int64_t micros = 0;
        r = ParseTimestamp(str.data(), str.size(), micros);
        out = Value::TIMESTAMP(micros);
    }
    auto name = LogicalType(to).ToString();
    if (r == PR::Ok) return out;
    if (r == PR::Unsupported) {
        throw NotImplementedException("Cannot read '" + str + "' as " + name +
                                      ": a form or value the engine does not support");
    }
    throw ConversionException("Could not convert string '" + str + "' to " + name);
}

bool Value::TryParseTimestampMicros(const char *s, size_t len, int64_t &out_micros) {
    return ParseTimestamp(s, len, out_micros) == PR::Ok;
}

bool Value::TryParseDateStringEpochDays(const char *s, size_t len, int32_t &out_days) {
    return ParseDate(s, len, out_days) == PR::Ok;
}

Value Value::TIME(int64_t micros) {
    Value result(micros);
    result.type_ = LogicalType::TIME();
    return result;
}

Value Value::LIST(std::vector<Value> values, LogicalType child_type) {
    Value result;
    result.type_ = LogicalType::LIST(child_type);
    result.is_null_ = false;
    result.list_value_ = std::move(values);
    return result;
}

template <>
bool Value::GetValue<bool>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return boolean_;
}

// A numeric read of a Value of another numeric type converts it (#43):
// the members share a union, so GetValue<double>() of an INTEGER read a
// double's bytes (TRUNC(54::INTEGER) gave 0) and GetValue<int64_t>() of
// an INTEGER read four bytes it never wrote. Other types read the member
// they are stored in, as before.
template <>
int8_t Value::GetValue<int8_t>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return tinyint_;
}

template <>
int16_t Value::GetValue<int16_t>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    switch (type_.id()) {
    case LogicalTypeId::BOOLEAN: return boolean_;
    case LogicalTypeId::TINYINT: return tinyint_;
    case LogicalTypeId::UTINYINT: return utinyint_;
    default: return smallint_;
    }
}

template <>
int32_t Value::GetValue<int32_t>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    switch (type_.id()) {
    case LogicalTypeId::BOOLEAN: return boolean_;
    case LogicalTypeId::TINYINT: return tinyint_;
    case LogicalTypeId::SMALLINT: return smallint_;
    case LogicalTypeId::UTINYINT: return utinyint_;
    case LogicalTypeId::USMALLINT: return usmallint_;
    default: return integer_;
    }
}

template <>
int64_t Value::GetValue<int64_t>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    switch (type_.id()) {
    case LogicalTypeId::BOOLEAN: return boolean_;
    case LogicalTypeId::TINYINT: return tinyint_;
    case LogicalTypeId::SMALLINT: return smallint_;
    case LogicalTypeId::INTEGER: case LogicalTypeId::DATE: return integer_;
    case LogicalTypeId::UTINYINT: return utinyint_;
    case LogicalTypeId::USMALLINT: return usmallint_;
    case LogicalTypeId::UINTEGER: return uinteger_;
    default: return bigint_;
    }
}

template <>
uint8_t Value::GetValue<uint8_t>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return utinyint_;
}

template <>
uint16_t Value::GetValue<uint16_t>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return usmallint_;
}

template <>
uint32_t Value::GetValue<uint32_t>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return uinteger_;
}

template <>
uint64_t Value::GetValue<uint64_t>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return ubigint_;
}

template <>
float Value::GetValue<float>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    switch (type_.id()) {
    case LogicalTypeId::TINYINT: return tinyint_;
    case LogicalTypeId::SMALLINT: return smallint_;
    case LogicalTypeId::INTEGER: return static_cast<float>(integer_);
    case LogicalTypeId::BIGINT: return static_cast<float>(bigint_);
    case LogicalTypeId::UTINYINT: return utinyint_;
    case LogicalTypeId::USMALLINT: return usmallint_;
    case LogicalTypeId::UINTEGER: return static_cast<float>(uinteger_);
    case LogicalTypeId::UBIGINT: return static_cast<float>(ubigint_);
    default: return float_;
    }
}

template <>
double Value::GetValue<double>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    switch (type_.id()) {
    case LogicalTypeId::TINYINT: return tinyint_;
    case LogicalTypeId::SMALLINT: return smallint_;
    case LogicalTypeId::INTEGER: return integer_;
    case LogicalTypeId::BIGINT: return static_cast<double>(bigint_);
    case LogicalTypeId::UTINYINT: return utinyint_;
    case LogicalTypeId::USMALLINT: return usmallint_;
    case LogicalTypeId::UINTEGER: return uinteger_;
    case LogicalTypeId::UBIGINT: return static_cast<double>(ubigint_);
    case LogicalTypeId::FLOAT: return float_;
    default: return double_;
    }
}

template <>
hugeint_t Value::GetValue<hugeint_t>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return hugeint_;
}

template <>
std::string Value::GetValue<std::string>() const {
    if (is_null_) throw ConversionException("Cannot get value from NULL");
    return str_value_;
}

std::string Value::ToString() const {
    if (is_null_) return "NULL";
    switch (type_.id()) {
    case LogicalTypeId::BOOLEAN:
        return boolean_ ? "true" : "false";
    case LogicalTypeId::TINYINT:
        return std::to_string(tinyint_);
    case LogicalTypeId::SMALLINT:
        return std::to_string(smallint_);
    case LogicalTypeId::INTEGER:
        return std::to_string(integer_);
    case LogicalTypeId::DATE:
        return FormatDate(integer_);
    case LogicalTypeId::BIGINT:
        return std::to_string(bigint_);
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::TIMESTAMP_TZ:
        return FormatTimestamp(bigint_);
    case LogicalTypeId::TIME:
        return FormatTime(bigint_);
    case LogicalTypeId::UTINYINT:
        return std::to_string(utinyint_);
    case LogicalTypeId::USMALLINT:
        return std::to_string(usmallint_);
    case LogicalTypeId::UINTEGER:
        return std::to_string(uinteger_);
    case LogicalTypeId::UBIGINT:
        return std::to_string(ubigint_);
    case LogicalTypeId::FLOAT:
        // Shortest round-trip representation, matching DuckDB's output.
        // std::to_string(float) would pad to a fixed 6 decimals.
        return ShortestFloatString(float_);
    case LogicalTypeId::DOUBLE:
        return ShortestFloatString(double_);
    case LogicalTypeId::HUGEINT:
        return hugeint_.ToString();
    case LogicalTypeId::VARCHAR:
        return str_value_;
    case LogicalTypeId::BLOB:
        // DuckDB's text of a BLOB, \xHH for the bytes that are not
        // printable ASCII (#31).
        return blob::ToText(str_value_);
    default:
        return "(" + type_.ToString() + ")";
    }
}

bool Value::operator==(const Value &other) const {
    if (is_null_ && other.is_null_) return true;
    if (is_null_ != other.is_null_) return false;
    if (type_ != other.type_) return false;
    switch (type_.id()) {
    case LogicalTypeId::BOOLEAN: return boolean_ == other.boolean_;
    case LogicalTypeId::TINYINT: return tinyint_ == other.tinyint_;
    case LogicalTypeId::SMALLINT: return smallint_ == other.smallint_;
    case LogicalTypeId::INTEGER: return integer_ == other.integer_;
    case LogicalTypeId::BIGINT: return bigint_ == other.bigint_;
    case LogicalTypeId::UTINYINT: return utinyint_ == other.utinyint_;
    case LogicalTypeId::USMALLINT: return usmallint_ == other.usmallint_;
    case LogicalTypeId::UINTEGER: return uinteger_ == other.uinteger_;
    case LogicalTypeId::UBIGINT: return ubigint_ == other.ubigint_;
    // DuckDB's order: NaN equals NaN, -0.0 equals 0.0 (#42).
    case LogicalTypeId::FLOAT: return SqlEq(float_, other.float_);
    case LogicalTypeId::DOUBLE: return SqlEq(double_, other.double_);
    case LogicalTypeId::HUGEINT: return hugeint_ == other.hugeint_;
    case LogicalTypeId::VARCHAR:
    case LogicalTypeId::BLOB: return str_value_ == other.str_value_;
    // A DATE holds its days and a TIME or TIMESTAMP its microseconds as
    // an INTEGER and a BIGINT do. They compared as neither equal nor
    // less, so a sort, a UNION and DISTINCT through Values left them as
    // they came (#38).
    case LogicalTypeId::DATE: return integer_ == other.integer_;
    case LogicalTypeId::TIME:
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::TIMESTAMP_TZ: return bigint_ == other.bigint_;
    default: return false;
    }
}

bool Value::operator<(const Value &other) const {
    if (is_null_ || other.is_null_) return false;
    if (type_ != other.type_) return false;
    switch (type_.id()) {
    case LogicalTypeId::BOOLEAN: return boolean_ < other.boolean_;
    case LogicalTypeId::TINYINT: return tinyint_ < other.tinyint_;
    case LogicalTypeId::SMALLINT: return smallint_ < other.smallint_;
    case LogicalTypeId::INTEGER: return integer_ < other.integer_;
    case LogicalTypeId::BIGINT: return bigint_ < other.bigint_;
    case LogicalTypeId::UTINYINT: return utinyint_ < other.utinyint_;
    case LogicalTypeId::USMALLINT: return usmallint_ < other.usmallint_;
    case LogicalTypeId::UINTEGER: return uinteger_ < other.uinteger_;
    case LogicalTypeId::UBIGINT: return ubigint_ < other.ubigint_;
    // NaN above every other value, +inf included (#42).
    case LogicalTypeId::FLOAT: return SqlLess(float_, other.float_);
    case LogicalTypeId::DOUBLE: return SqlLess(double_, other.double_);
    case LogicalTypeId::HUGEINT: return hugeint_ < other.hugeint_;
    case LogicalTypeId::VARCHAR:
    case LogicalTypeId::BLOB: return str_value_ < other.str_value_;
    case LogicalTypeId::DATE: return integer_ < other.integer_;
    case LogicalTypeId::TIME:
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::TIMESTAMP_TZ: return bigint_ < other.bigint_;
    default: return false;
    }
}

} // namespace zugodb
