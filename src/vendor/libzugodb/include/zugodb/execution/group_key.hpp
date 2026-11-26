#pragma once

// The key of a row for GROUP BY, DISTINCT and PARTITION BY (libzugodb
// Stage 10). Each column adds one part: a tag byte, NULL or present, then
// for a present value its bytes, a text's length first. No two different
// rows give the same key. The keys this replaces did: text joined with '|'
// made ('a|', 'b') and ('a', '|b') one group, "NULL|" made a NULL and the
// text 'NULL' one, and std::to_string(double) kept six decimals.

#include "zugodb/common/types/float_order.hpp"
#include "zugodb/common/types/value.hpp"
#include "zugodb/common/types/vector.hpp"

#include <cstdint>
#include <cstring>
#include <string>

namespace zugodb {

inline void AppendKeyNull(std::string &key) { key.push_back('\0'); }

inline void AppendKeyText(std::string &key, const char *data, uint32_t len) {
    key.push_back('\1');
    key.append(reinterpret_cast<const char *>(&len), sizeof len);
    key.append(data, len);
}

template <class T>
inline void AppendKeyRaw(std::string &key, T v) {
    key.push_back('\1');
    key.append(reinterpret_cast<const char *>(&v), sizeof v);
}

// -0.0 and 0.0 are one group, and so is every NaN, whatever its sign or
// payload, as DuckDB's (#42).
inline void AppendKeyDouble(std::string &key, double d) {
    AppendKeyRaw(key, CanonicalKey(d));
}

inline void AppendKeyValue(std::string &key, const Value &v) {
    if (v.IsNull()) {
        AppendKeyNull(key);
        return;
    }
    switch (v.type().id()) {
    case LogicalTypeId::DOUBLE:
        AppendKeyDouble(key, v.GetValue<double>());
        return;
    case LogicalTypeId::FLOAT:
        AppendKeyDouble(key, static_cast<double>(v.GetValue<float>()));
        return;
    default: {
        // Within one column every value has the same type, and its text is
        // exact for every other type a table holds.
        auto s = v.ToString();
        AppendKeyText(key, s.data(), static_cast<uint32_t>(s.size()));
        return;
    }
    }
}

inline void AppendKeyVector(std::string &key, const Vector &vec, idx_t row) {
    if (!vec.GetValidity().RowIsValid(row)) {
        AppendKeyNull(key);
        return;
    }
    switch (vec.GetType().id()) {
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::DATE:
        AppendKeyRaw(key, vec.GetData<int32_t>()[row]);
        return;
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::TIMESTAMP_TZ:
    case LogicalTypeId::TIME:
        AppendKeyRaw(key, vec.GetData<int64_t>()[row]);
        return;
    case LogicalTypeId::DOUBLE:
        AppendKeyDouble(key, vec.GetData<double>()[row]);
        return;
    case LogicalTypeId::VARCHAR: {
        auto &s = vec.GetData<string_t>()[row];
        AppendKeyText(key, s.GetData(), s.GetSize());
        return;
    }
    default:
        AppendKeyValue(key, vec.GetValue(row));
        return;
    }
}

} // namespace zugodb
