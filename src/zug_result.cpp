// Query results (design §6, D8): materialised, then fetched by slices.
//
// Two phases, so that design §4.3 holds by construction:
//
// 1. EngineSend (C++, inside try/catch): runs the query and copies every
//    column into plain buffers in an ZugResult on the heap (integers,
//    doubles, 64-bit integers, or string offsets into one byte blob, each
//    with a validity vector). Vectors are flattened first, so a constant
//    or dictionary vector reads as a flat one; a result the engine kept as
//    boxed rows is read through Value. Then the engine's QueryResult is
//    freed. A value R cannot hold (an INTEGER of -2^31, which is R's NA; a
//    string with invalid UTF-8 or an embedded NUL) is found here and
//    recorded, not raised.
// 2. zug_fetch (R API only): reads those buffers for rows [pos, pos + n)
//    and builds R vectors. Only plain data is on the stack, so an R
//    allocation that longjmps leaks nothing; the ZugResult belongs to its
//    external pointer.
//
// Errors are returned as an zug::EngineError for R to raise.

#include "zug_objects.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

namespace {

enum class Kind {
  kLogical,  // BOOLEAN
  kInt,      // TINYINT, SMALLINT, INTEGER, UTINYINT, USMALLINT
  kDouble,   // FLOAT, DOUBLE
  kInt64,    // BIGINT, UINTEGER
  kUInt64,   // UBIGINT
  kString,   // VARCHAR
  kNull,     // the type of a bare NULL: an integer NA, as in duckdb
  kDate,     // DATE: days since 1970-01-01, a Date
  kTimestamp,  // TIMESTAMP: seconds since 1970-01-01 UTC, a POSIXct
  kBlob,     // BLOB: bytes, a blob-classed list of raw vectors
  kUnsupported
};

struct Column {
  std::string name;
  std::string sql_type;
  Kind kind = Kind::kUnsupported;
  std::vector<int> i;          // kLogical, kInt; NA_INTEGER for NULL
  std::vector<double> d;  // kDouble, kDate, kTimestamp; NA_REAL for NULL
  std::vector<int64_t> i64;    // kInt64
  std::vector<uint64_t> u64;   // kUInt64
  std::vector<uint8_t> valid;  // kInt64, kUInt64, kString, kBlob
  // kString, kBlob: row r is blob[off[r], off[r + 1]).
  std::vector<uint64_t> off;
  std::string blob;
  // The first row whose value R cannot hold, and why; -1 if none.
  int64_t bad_row = -1;
  const char *bad_why = nullptr;
  // kUnsupported: why the type is not read.
  const char *unsupported_why = nullptr;
};

struct ZugResult {
  std::vector<Column> cols;
  uint64_t nrow = 0;
  uint64_t pos = 0;
  double rows_affected = 0;
  bool is_query = true;
};

Kind KindOf(zugodb::LogicalTypeId id) {
  using T = zugodb::LogicalTypeId;
  switch (id) {
  case T::BOOLEAN:
    return Kind::kLogical;
  case T::TINYINT:
  case T::SMALLINT:
  case T::INTEGER:
  case T::UTINYINT:
  case T::USMALLINT:
    return Kind::kInt;
  case T::FLOAT:
  case T::DOUBLE:
    return Kind::kDouble;
  case T::BIGINT:
  case T::UINTEGER:
    return Kind::kInt64;
  case T::UBIGINT:
    return Kind::kUInt64;
  case T::VARCHAR:
    return Kind::kString;
  case T::SQLNULL:
    return Kind::kNull;
  case T::DATE:
    return Kind::kDate;
  case T::TIMESTAMP:
    return Kind::kTimestamp;
  case T::BLOB:
    return Kind::kBlob;
  default:
    return Kind::kUnsupported;
  }
}

// Why a type that does not convert is refused (design §6).
const char *UnsupportedWhy(zugodb::LogicalTypeId id) {
  // HUGEINT, INTERVAL and UUID are refused by the engine's SQL (libzugodb
  // 0.2.0), so TIME is the one a query reaches today (#43).
  (void)id;
  return "which zugodb cannot read yet";
}

// The engine keeps a TIMESTAMP as microseconds since 1970; R, as seconds.
double MicrosToSeconds(int64_t us) {
  return static_cast<double>(us) / 1e6;
}

// RFC 3629 UTF-8: no overlongs, no surrogates, nothing past U+10FFFF.
bool ValidUtf8(const unsigned char *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    unsigned char c = s[i];
    if (c < 0x80) {
      i++;
      continue;
    }
    size_t len;
    uint32_t cp;
    if (c >= 0xC2 && c <= 0xDF) {
      len = 2;
      cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
      len = 3;
      cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
      len = 4;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + len > n) return false;
    for (size_t k = 1; k < len; k++) {
      if ((s[i + k] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (s[i + k] & 0x3F);
    }
    if ((len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
        (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))) {
      return false;
    }
    i += len;
  }
  return true;
}

void MarkBad(Column &col, uint64_t row, const char *why) {
  if (col.bad_row < 0) {
    col.bad_row = static_cast<int64_t>(row);
    col.bad_why = why;
  }
}

const char *kIntMin =
    "holds -2147483648, which is NA in an R integer";
const char *kBadUtf8 = "holds a string that is not valid UTF-8";
const char *kNul = "holds a string with an embedded NUL";

// A Value keeps its number in a union, and GetValue<T>() returns the member
// named by T without conversion (#41): each is read through the member of
// the Value's own type.
int64_t ValueInt(const zugodb::Value &v) {
  using T = zugodb::LogicalTypeId;
  switch (v.type().id()) {
  case T::BOOLEAN:
    return v.GetValue<bool>() ? 1 : 0;
  case T::TINYINT:
    return v.GetValue<int8_t>();
  case T::SMALLINT:
    return v.GetValue<int16_t>();
  case T::INTEGER:
  case T::DATE:
    return v.GetValue<int32_t>();
  case T::UTINYINT:
    return v.GetValue<uint8_t>();
  case T::USMALLINT:
    return v.GetValue<uint16_t>();
  case T::UINTEGER:
    return v.GetValue<uint32_t>();
  default:
    return v.GetValue<int64_t>();
  }
}

// The engine keeps a NaN's bits (libzugodb 0.4.0), and R reads one payload,
// 1954, as NA: every NaN comes back as one quiet NaN, R's NaN, so no stored
// payload is taken for NA (design §6).
inline double OneNaN(double x) {
  return std::isnan(x) ? std::numeric_limits<double>::quiet_NaN() : x;
}

double ValueDouble(const zugodb::Value &v) {
  using T = zugodb::LogicalTypeId;
  switch (v.type().id()) {
  case T::FLOAT:
    return OneNaN(v.GetValue<float>());
  case T::DOUBLE:
    return OneNaN(v.GetValue<double>());
  case T::UBIGINT:
    return static_cast<double>(v.GetValue<uint64_t>());
  default:
    return static_cast<double>(ValueInt(v));
  }
}

uint64_t ValueUInt(const zugodb::Value &v) {
  if (v.type().id() == zugodb::LogicalTypeId::UBIGINT) {
    return v.GetValue<uint64_t>();
  }
  return static_cast<uint64_t>(ValueInt(v));
}

// Appends one value, given as a Value (the boxed-rows path).
void AppendValue(Column &col, const zugodb::Value &v, uint64_t row) {
  const bool null = v.IsNull();
  switch (col.kind) {
  case Kind::kNull:
    col.i.push_back(NA_INTEGER);
    break;
  case Kind::kLogical:
    col.i.push_back(null ? NA_LOGICAL : (ValueInt(v) != 0 ? 1 : 0));
    break;
  case Kind::kInt: {
    if (null) {
      col.i.push_back(NA_INTEGER);
      break;
    }
    int64_t x = ValueInt(v);
    if (x == INT32_MIN) MarkBad(col, row, kIntMin);
    col.i.push_back(static_cast<int>(x));
    break;
  }
  case Kind::kDouble:
    col.d.push_back(null ? NA_REAL : ValueDouble(v));
    break;
  case Kind::kInt64:
    col.valid.push_back(!null);
    col.i64.push_back(null ? 0 : ValueInt(v));
    break;
  case Kind::kUInt64:
    col.valid.push_back(!null);
    col.u64.push_back(null ? 0 : ValueUInt(v));
    break;
  case Kind::kString: {
    col.valid.push_back(!null);
    if (!null) {
      std::string s = v.GetValue<std::string>();
      if (!ValidUtf8(reinterpret_cast<const unsigned char *>(s.data()),
                     s.size())) {
        MarkBad(col, row, kBadUtf8);
      }
      if (s.find('\0') != std::string::npos) MarkBad(col, row, kNul);
      col.blob.append(s);
    }
    col.off.push_back(col.blob.size());
    break;
  }
  case Kind::kDate:
    col.d.push_back(null ? NA_REAL : static_cast<double>(v.GetValue<int32_t>()));
    break;
  case Kind::kTimestamp:
    col.d.push_back(null ? NA_REAL : MicrosToSeconds(v.GetValue<int64_t>()));
    break;
  case Kind::kBlob:
    col.valid.push_back(!null);
    if (!null) col.blob.append(v.GetValue<std::string>());
    col.off.push_back(col.blob.size());
    break;
  case Kind::kUnsupported:
    break;
  }
}

template <class T>
void AppendInts(Column &col, const zugodb::Vector &vec, uint64_t n,
                uint64_t row0) {
  const T *data = vec.GetData<T>();
  const auto &valid = vec.GetValidity();
  for (uint64_t r = 0; r < n; r++) {
    if (!valid.RowIsValid(r)) {
      col.i.push_back(NA_INTEGER);
      continue;
    }
    int64_t x = static_cast<int64_t>(data[r]);
    if (x == INT32_MIN) MarkBad(col, row0 + r, kIntMin);
    col.i.push_back(static_cast<int>(x));
  }
}

// Appends a flat chunk vector of `n` rows, read in its physical type.
void AppendVector(Column &col, zugodb::LogicalTypeId id,
                  const zugodb::Vector &vec, uint64_t n, uint64_t row0) {
  using T = zugodb::LogicalTypeId;
  const auto &valid = vec.GetValidity();
  switch (id) {
  case T::SQLNULL:
    col.i.insert(col.i.end(), n, NA_INTEGER);
    return;
  case T::BOOLEAN: {
    const bool *data = vec.GetData<bool>();
    for (uint64_t r = 0; r < n; r++) {
      col.i.push_back(valid.RowIsValid(r) ? (data[r] ? 1 : 0) : NA_LOGICAL);
    }
    return;
  }
  case T::TINYINT:
    return AppendInts<int8_t>(col, vec, n, row0);
  case T::SMALLINT:
    return AppendInts<int16_t>(col, vec, n, row0);
  case T::INTEGER:
    return AppendInts<int32_t>(col, vec, n, row0);
  case T::UTINYINT:
    return AppendInts<uint8_t>(col, vec, n, row0);
  case T::USMALLINT:
    return AppendInts<uint16_t>(col, vec, n, row0);
  case T::FLOAT: {
    const float *data = vec.GetData<float>();
    for (uint64_t r = 0; r < n; r++) {
      col.d.push_back(valid.RowIsValid(r) ? OneNaN(data[r]) : NA_REAL);
    }
    return;
  }
  case T::DOUBLE: {
    const double *data = vec.GetData<double>();
    for (uint64_t r = 0; r < n; r++) {
      col.d.push_back(valid.RowIsValid(r) ? OneNaN(data[r]) : NA_REAL);
    }
    return;
  }
  case T::BIGINT: {
    const int64_t *data = vec.GetData<int64_t>();
    for (uint64_t r = 0; r < n; r++) {
      const bool ok = valid.RowIsValid(r);
      col.valid.push_back(ok);
      col.i64.push_back(ok ? data[r] : 0);
    }
    return;
  }
  case T::UINTEGER: {
    const uint32_t *data = vec.GetData<uint32_t>();
    for (uint64_t r = 0; r < n; r++) {
      const bool ok = valid.RowIsValid(r);
      col.valid.push_back(ok);
      col.i64.push_back(ok ? static_cast<int64_t>(data[r]) : 0);
    }
    return;
  }
  case T::UBIGINT: {
    const uint64_t *data = vec.GetData<uint64_t>();
    for (uint64_t r = 0; r < n; r++) {
      const bool ok = valid.RowIsValid(r);
      col.valid.push_back(ok);
      col.u64.push_back(ok ? data[r] : 0);
    }
    return;
  }
  case T::VARCHAR: {
    const zugodb::string_t *data = vec.GetData<zugodb::string_t>();
    for (uint64_t r = 0; r < n; r++) {
      const bool ok = valid.RowIsValid(r);
      col.valid.push_back(ok);
      if (ok) {
        const char *p = data[r].GetData();
        const size_t len = data[r].GetSize();
        if (!ValidUtf8(reinterpret_cast<const unsigned char *>(p), len)) {
          MarkBad(col, row0 + r, kBadUtf8);
        }
        if (std::memchr(p, '\0', len)) MarkBad(col, row0 + r, kNul);
        col.blob.append(p, len);
      }
      col.off.push_back(col.blob.size());
    }
    return;
  }
  case T::DATE: {
    const int32_t *data = vec.GetData<int32_t>();
    for (uint64_t r = 0; r < n; r++) {
      col.d.push_back(valid.RowIsValid(r) ? static_cast<double>(data[r])
                                          : NA_REAL);
    }
    return;
  }
  case T::TIMESTAMP: {
    const int64_t *data = vec.GetData<int64_t>();
    for (uint64_t r = 0; r < n; r++) {
      col.d.push_back(valid.RowIsValid(r) ? MicrosToSeconds(data[r]) : NA_REAL);
    }
    return;
  }
  case T::BLOB: {
    const zugodb::string_t *data = vec.GetData<zugodb::string_t>();
    for (uint64_t r = 0; r < n; r++) {
      const bool ok = valid.RowIsValid(r);
      col.valid.push_back(ok);
      if (ok) col.blob.append(data[r].GetData(), data[r].GetSize());
      col.off.push_back(col.blob.size());
    }
    return;
  }
  default:
    return;
  }
}

void EngineSend(zug::ZugConnection *c, const char *sql, ZugResult *out,
                zug::EngineError *err) {
  try {
    zugodb::QueryResult res = c->con->Query(sql);
    if (res.is_count) {
      // INSERT, UPDATE, DELETE and CREATE TABLE [AS]: the rows affected,
      // in the one row, or none for a plain CREATE TABLE.
      out->is_query = false;
      out->rows_affected = 0;
      if (res.ColumnCount() == 1 && res.RowCount() == 1) {
        const zugodb::Value &v = res.GetValue(0, 0);
        out->rows_affected = v.IsNull() ? 0 : static_cast<double>(ValueInt(v));
      }
      return;
    }
    const uint64_t ncol = res.ColumnCount();
    out->cols.resize(ncol);
    for (uint64_t k = 0; k < ncol; k++) {
      Column &col = out->cols[k];
      col.name = res.column_names[k];
      col.sql_type = res.column_types[k].ToString();
      col.kind = KindOf(res.column_types[k].id());
      if (col.kind == Kind::kUnsupported) {
        col.unsupported_why = UnsupportedWhy(res.column_types[k].id());
      }
      if (col.kind == Kind::kString || col.kind == Kind::kBlob) {
        col.off.push_back(0);
      }
    }
    if (res.rows.empty()) {
      // Typed chunks: flattened, then read in each column's physical type.
      uint64_t row0 = 0;
      for (auto &chunk : res.chunks) {
        const uint64_t n = chunk.size();
        for (uint64_t k = 0; k < ncol; k++) {
          if (out->cols[k].kind == Kind::kUnsupported) continue;
          auto &vec = chunk.GetVector(k);
          vec.Flatten(n);
          AppendVector(out->cols[k], res.column_types[k].id(), vec, n, row0);
        }
        row0 += n;
      }
      out->nrow = row0;
    } else {
      // Rows boxed as Values (PRAGMA, SHOW and other non-SELECT paths).
      const uint64_t n = res.rows.size();
      for (uint64_t r = 0; r < n; r++) {
        for (uint64_t k = 0; k < ncol; k++) {
          if (out->cols[k].kind == Kind::kUnsupported) continue;
          AppendValue(out->cols[k], res.rows[r][k], r);
        }
      }
      out->nrow = n;
    }
  } catch (const std::exception &e) {
    err->set(e.what());
  } catch (...) {
    err->set("unknown C++ exception");
  }
}

// ---------------------------------------------------------------------------
// Phase 2: R vectors from the buffers. Plain data only on the stack.

enum BigInt { kBigNumeric = 0, kBigInteger64 = 1, kBigCharacter = 2 };

// Sets `err` to a zug_type_error naming the column and the row, the
// first in [from, to) whose value R cannot hold as asked.
bool CheckSlice(const Column &col, uint64_t from, uint64_t to, int bigint,
                zug::EngineError *err) {
  char msg[512];
  if (col.kind == Kind::kUnsupported) {
    std::snprintf(msg, sizeof msg, "Column `%s` has type %s, %s.",
                  col.name.c_str(), col.sql_type.c_str(),
                  col.unsupported_why ? col.unsupported_why
                                      : "which zugodb cannot read yet");
    err->set(msg, "type");
    return false;
  }
  if (col.bad_row >= 0 && static_cast<uint64_t>(col.bad_row) >= from &&
      static_cast<uint64_t>(col.bad_row) < to) {
    std::snprintf(msg, sizeof msg, "Column `%s` (%s), row %" PRId64 ", %s.",
                  col.name.c_str(), col.sql_type.c_str(), col.bad_row + 1,
                  col.bad_why);
    err->set(msg, "type");
    return false;
  }
  for (uint64_t r = from; r < to; r++) {
    if (col.kind == Kind::kInt64 && col.valid[r]) {
      const int64_t x = col.i64[r];
      // bit64's NA is INT64_MIN; a double is exact only to 2^53.
      const char *why = nullptr;
      if (bigint == kBigNumeric && (x > 9007199254740992LL ||
                                    x < -9007199254740992LL)) {
        why = "is beyond 2^53, where a double is no longer exact; use "
              "bigint = \"integer64\" or \"character\"";
      } else if (bigint == kBigInteger64 && x == INT64_MIN) {
        why = "is -2^63, which is NA in integer64; use bigint = \"character\"";
      }
      if (why) {
        std::snprintf(msg, sizeof msg,
                      "Column `%s` (%s), row %" PRIu64 ": %" PRId64 " %s.",
                      col.name.c_str(), col.sql_type.c_str(), r + 1, x, why);
        err->set(msg, "type");
        return false;
      }
    } else if (col.kind == Kind::kUInt64 && col.valid[r]) {
      const uint64_t x = col.u64[r];
      const char *why = nullptr;
      if (bigint == kBigNumeric && x > 9007199254740992ULL) {
        why = "is beyond 2^53, where a double is no longer exact; use "
              "bigint = \"integer64\" or \"character\"";
      } else if (bigint == kBigInteger64 &&
                 x > static_cast<uint64_t>(INT64_MAX)) {
        why = "is beyond 2^63 - 1, the largest integer64; use "
              "bigint = \"character\"";
      }
      if (why) {
        std::snprintf(msg, sizeof msg,
                      "Column `%s` (%s), row %" PRIu64 ": %" PRIu64 " %s.",
                      col.name.c_str(), col.sql_type.c_str(), r + 1, x, why);
        err->set(msg, "type");
        return false;
      }
    }
  }
  return true;
}

void SetClass(SEXP x, const char *const *classes, int n) {
  SEXP cls = PROTECT(Rf_allocVector(STRSXP, n));
  for (int k = 0; k < n; k++) SET_STRING_ELT(cls, k, Rf_mkChar(classes[k]));
  Rf_setAttrib(x, R_ClassSymbol, cls);
  UNPROTECT(1);
}

void SetStringAttrib(SEXP x, const char *name, SEXP value) {
  PROTECT(value);
  Rf_setAttrib(x, Rf_install(name), value);
  UNPROTECT(1);
}

// `tz`: the tzone of a POSIXct column, a CHARSXP.
SEXP BuildColumn(const Column &col, uint64_t from, uint64_t to, int bigint,
                 SEXP tz) {
  const R_xlen_t n = static_cast<R_xlen_t>(to - from);
  SEXP out;
  switch (col.kind) {
  case Kind::kLogical:
    out = PROTECT(Rf_allocVector(LGLSXP, n));
    for (R_xlen_t r = 0; r < n; r++) LOGICAL(out)[r] = col.i[from + r];
    break;
  case Kind::kNull:
  case Kind::kInt:
    out = PROTECT(Rf_allocVector(INTSXP, n));
    for (R_xlen_t r = 0; r < n; r++) INTEGER(out)[r] = col.i[from + r];
    break;
  case Kind::kDouble:
  case Kind::kDate:
  case Kind::kTimestamp:
    out = PROTECT(Rf_allocVector(REALSXP, n));
    for (R_xlen_t r = 0; r < n; r++) REAL(out)[r] = col.d[from + r];
    if (col.kind == Kind::kDate) {
      static const char *const cls[] = {"Date"};
      SetClass(out, cls, 1);
    } else if (col.kind == Kind::kTimestamp) {
      static const char *const cls[] = {"POSIXct", "POSIXt"};
      SetClass(out, cls, 2);
      SetStringAttrib(out, "tzone", Rf_ScalarString(tz));
    }
    break;
  case Kind::kBlob: {
    out = PROTECT(Rf_allocVector(VECSXP, n));
    for (R_xlen_t r = 0; r < n; r++) {
      const uint64_t row = from + r;
      if (!col.valid[row]) continue; // NULL, as in blob::blob()
      const uint64_t a = col.off[row], b = col.off[row + 1];
      SEXP raw = Rf_allocVector(RAWSXP, static_cast<R_xlen_t>(b - a));
      SET_VECTOR_ELT(out, r, raw);
      if (b > a) std::memcpy(RAW(raw), col.blob.data() + a, b - a);
    }
    // blob::blob()'s classes, without importing blob (design §6).
    static const char *const cls[] = {"blob", "vctrs_list_of", "vctrs_vctr",
                                      "list"};
    SetClass(out, cls, 4);
    SetStringAttrib(out, "ptype", Rf_allocVector(RAWSXP, 0));
    break;
  }
  case Kind::kInt64:
  case Kind::kUInt64: {
    const bool s = col.kind == Kind::kUInt64;
    if (bigint == kBigCharacter) {
      out = PROTECT(Rf_allocVector(STRSXP, n));
      char buf[32];
      for (R_xlen_t r = 0; r < n; r++) {
        if (!col.valid[from + r]) {
          SET_STRING_ELT(out, r, NA_STRING);
          continue;
        }
        if (s) {
          std::snprintf(buf, sizeof buf, "%" PRIu64, col.u64[from + r]);
        } else {
          std::snprintf(buf, sizeof buf, "%" PRId64, col.i64[from + r]);
        }
        SET_STRING_ELT(out, r, Rf_mkCharCE(buf, CE_UTF8));
      }
    } else if (bigint == kBigInteger64) {
      out = PROTECT(Rf_allocVector(REALSXP, n));
      for (R_xlen_t r = 0; r < n; r++) {
        int64_t x = INT64_MIN; // bit64's NA
        if (col.valid[from + r]) {
          x = s ? static_cast<int64_t>(col.u64[from + r]) : col.i64[from + r];
        }
        std::memcpy(&REAL(out)[r], &x, sizeof x);
      }
      SEXP cls = PROTECT(Rf_mkString("integer64"));
      Rf_setAttrib(out, R_ClassSymbol, cls);
      UNPROTECT(1);
    } else {
      out = PROTECT(Rf_allocVector(REALSXP, n));
      for (R_xlen_t r = 0; r < n; r++) {
        if (!col.valid[from + r]) {
          REAL(out)[r] = NA_REAL;
        } else {
          REAL(out)[r] = s ? static_cast<double>(col.u64[from + r])
                           : static_cast<double>(col.i64[from + r]);
        }
      }
    }
    break;
  }
  case Kind::kString:
    out = PROTECT(Rf_allocVector(STRSXP, n));
    for (R_xlen_t r = 0; r < n; r++) {
      const uint64_t row = from + r;
      if (!col.valid[row]) {
        SET_STRING_ELT(out, r, NA_STRING);
        continue;
      }
      const uint64_t a = col.off[row], b = col.off[row + 1];
      SET_STRING_ELT(out, r,
                     Rf_mkCharLenCE(col.blob.data() + a,
                                    static_cast<int>(b - a), CE_UTF8));
    }
    break;
  default:
    out = PROTECT(Rf_allocVector(LGLSXP, 0)); // unreachable: checked first
    break;
  }
  UNPROTECT(1);
  return out;
}

ZugResult *GetResult(SEXP ptr) {
  if (TYPEOF(ptr) != EXTPTRSXP) return nullptr;
  return static_cast<ZugResult *>(R_ExternalPtrAddr(ptr));
}

void ResultFinalizer(SEXP ptr) {
  ZugResult *r = GetResult(ptr);
  if (!r) return;
  R_ClearExternalPtr(ptr);
  delete r;
}

} // namespace

extern "C" {

// zug_send(con, sql): runs `sql`, materialised, as a result pointer.
SEXP zug_send(SEXP conptr, SEXP sql) {
  zug::EngineError err;
  zug::ZugConnection *c = zug::GetConnection(conptr);
  if (!zug::ConnectionOpen(c)) {
    err.set(zug::kClosedConnection, "closed");
    return zug::ResultList(R_NilValue, err);
  }
  if (TYPEOF(sql) != STRSXP || XLENGTH(sql) != 1 ||
      STRING_ELT(sql, 0) == NA_STRING) {
    err.set("statement must be a single string", "argument");
    return zug::ResultList(R_NilValue, err);
  }
  const char *text = Rf_translateCharUTF8(STRING_ELT(sql, 0));
  SEXP ptr = PROTECT(R_MakeExternalPtr(nullptr, R_NilValue, R_NilValue));
  R_RegisterCFinalizerEx(ptr, ResultFinalizer, TRUE);
  ZugResult *r = new (std::nothrow) ZugResult();
  if (!r) {
    err.set("out of memory");
    UNPROTECT(1);
    return zug::ResultList(R_NilValue, err);
  }
  R_SetExternalPtrAddr(ptr, r);
  EngineSend(c, text, r, &err);
  if (err.failed) {
    // Free the half-built buffers now rather than at the next collection.
    R_ClearExternalPtr(ptr);
    delete r;
  }
  SEXP out = zug::ResultList(err.failed ? R_NilValue : ptr, err);
  UNPROTECT(1);
  return out;
}

// zug_result_info(res): list(names, sql_types, nrow, pos, rows_affected,
// is_query), or NULL once cleared.
SEXP zug_result_info(SEXP ptr) {
  ZugResult *r = GetResult(ptr);
  if (!r) return R_NilValue;
  const R_xlen_t ncol = static_cast<R_xlen_t>(r->cols.size());
  const char *fields[] = {"names", "sql_types", "nrow", "pos",
                          "rows_affected", "is_query", ""};
  SEXP out = PROTECT(Rf_mkNamed(VECSXP, fields));
  SEXP names = PROTECT(Rf_allocVector(STRSXP, ncol));
  SEXP types = PROTECT(Rf_allocVector(STRSXP, ncol));
  for (R_xlen_t k = 0; k < ncol; k++) {
    SET_STRING_ELT(names, k, Rf_mkCharCE(r->cols[k].name.c_str(), CE_UTF8));
    SET_STRING_ELT(types, k, Rf_mkCharCE(r->cols[k].sql_type.c_str(), CE_UTF8));
  }
  SET_VECTOR_ELT(out, 0, names);
  SET_VECTOR_ELT(out, 1, types);
  SET_VECTOR_ELT(out, 2, Rf_ScalarReal(static_cast<double>(r->nrow)));
  SET_VECTOR_ELT(out, 3, Rf_ScalarReal(static_cast<double>(r->pos)));
  SET_VECTOR_ELT(out, 4, Rf_ScalarReal(r->rows_affected));
  SET_VECTOR_ELT(out, 5, Rf_ScalarLogical(r->is_query));
  UNPROTECT(3);
  return out;
}

// zug_fetch(res, n, bigint, tz): the next `n` rows (all, if n < 0) as a
// list of columns, advancing the result. bigint: 0 numeric, 1 integer64, 2
// character; tz: a TIMESTAMP's tzone. Nothing advances if a value cannot
// be converted.
SEXP zug_fetch(SEXP ptr, SEXP n, SEXP bigint, SEXP tz) {
  zug::EngineError err;
  ZugResult *r = GetResult(ptr);
  if (!r) {
    err.set("The result is cleared.", "closed");
    return zug::ResultList(R_NilValue, err);
  }
  if (TYPEOF(n) != REALSXP || XLENGTH(n) != 1 || ISNAN(REAL(n)[0]) ||
      TYPEOF(bigint) != INTSXP || XLENGTH(bigint) != 1 ||
      INTEGER(bigint)[0] < 0 || INTEGER(bigint)[0] > 2 ||
      TYPEOF(tz) != STRSXP || XLENGTH(tz) != 1 ||
      STRING_ELT(tz, 0) == NA_STRING) {
    err.set("n must be a number, bigint 0, 1 or 2 and tz a string",
            "argument");
    return zug::ResultList(R_NilValue, err);
  }
  const uint64_t left = r->nrow - r->pos;
  const double want = REAL(n)[0];
  const uint64_t take =
      (want < 0 || want >= static_cast<double>(left)) ? left
                                                      : static_cast<uint64_t>(want);
  const uint64_t from = r->pos, to = r->pos + take;
  const int big = INTEGER(bigint)[0];
  for (const Column &col : r->cols) {
    if (!CheckSlice(col, from, to, big, &err)) {
      return zug::ResultList(R_NilValue, err);
    }
  }
  const R_xlen_t ncol = static_cast<R_xlen_t>(r->cols.size());
  SEXP out = PROTECT(Rf_allocVector(VECSXP, ncol));
  for (R_xlen_t k = 0; k < ncol; k++) {
    SET_VECTOR_ELT(out, k,
                   BuildColumn(r->cols[k], from, to, big, STRING_ELT(tz, 0)));
  }
  r->pos = to;
  SEXP res = zug::ResultList(out, err);
  UNPROTECT(1);
  return res;
}

// zug_result_clear(res): frees the buffers; later calls see it cleared.
SEXP zug_result_clear(SEXP ptr) {
  ZugResult *r = GetResult(ptr);
  if (r) {
    R_ClearExternalPtr(ptr);
    delete r;
  }
  return Rf_ScalarLogical(r != nullptr);
}

} // extern "C"
