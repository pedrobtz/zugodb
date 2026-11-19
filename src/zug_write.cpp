// Writing a data frame into a table (design §5, §6; roadmap Stage 5).
//
// Two phases, the reverse of zug_result.cpp's, so that design §4.3 holds:
//
// 1. zug_append (R API only): R has already made each column one of the
//    payload kinds below (R/write.R). This reads only pointers into those
//    vectors, into arrays from R_alloc, so no C++ object is live while R
//    can longjmp.
// 2. EngineAppend (C++, inside try/catch): reads the table's column types
//    from the engine, checks every value of every column against its
//    column's type, and only then builds DataChunks and appends them. A
//    value that does not fit is an error before any row is written, so a
//    failed append leaves the table as it was (there is no rollback; the
//    fork makes ROLLBACK an error, design §5).

#include "zug_objects.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// What R hands over for a column (R/write.R, payload_kind()).
enum Payload {
  kPNull = -1,     // LGLSXP of NA: a column the data frame does not give
  kPLogical = 0,   // LGLSXP
  kPInteger = 1,   // INTSXP
  kPDouble = 2,    // REALSXP
  kPInt64 = 3,     // REALSXP holding bit64::integer64's bits
  kPString = 4,    // STRSXP, every element UTF-8
  kPRaw = 5,       // VECSXP of RAWSXP or NULL
  kPDate = 6,      // REALSXP, days since 1970-01-01
  kPTimestamp = 7  // REALSXP, seconds since 1970-01-01 UTC
};

// One column as plain pointers into R's memory, valid while R/write.R holds
// the vectors.
struct InColumn {
  const char *name = nullptr;  // UTF-8, for messages
  int payload = 0;
  const int *ints = nullptr;      // kPLogical, kPInteger
  const double *reals = nullptr;  // kPDouble, kPInt64, kPDate, kPTimestamp
  const char **strs = nullptr;    // kPString, kPRaw: nullptr for NA/NULL
  const uint64_t *lens = nullptr; // kPString, kPRaw
};

struct Input {
  const char *table = nullptr;   // the name, for Connection::Append
  const char *quoted = nullptr;  // the quoted name, for SQL
  InColumn *cols = nullptr;
  int ncol = 0;
  uint64_t nrow = 0;
};

bool IsNA(const InColumn &c, uint64_t r) {
  switch (c.payload) {
  case kPNull:
    return true;
  case kPLogical:
  case kPInteger:
    return c.ints[r] == NA_INTEGER;
  case kPInt64: {
    int64_t x;
    std::memcpy(&x, &c.reals[r], sizeof x);
    return x == INT64_MIN; // bit64's NA
  }
  case kPDouble:
    return R_IsNA(c.reals[r]); // NaN is a value, NA a NULL
  case kPDate:
  case kPTimestamp:
    return std::isnan(c.reals[r]);
  default:
    return c.strs[r] == nullptr;
  }
}

int64_t Int64Of(const InColumn &c, uint64_t r) {
  int64_t x;
  std::memcpy(&x, &c.reals[r], sizeof x);
  return x;
}

const char *PayloadName(int p) {
  static const char *names[] = {"logical", "integer", "double",
                                "integer64", "character", "blob",
                                "Date", "POSIXct"};
  if (p == kPNull) return "NULL";
  return (p >= 0 && p <= 7) ? names[p] : "unknown";
}

// Whether payload `p` can go into a column of type `id` at all.
bool Compatible(int p, zugodb::LogicalTypeId id) {
  using T = zugodb::LogicalTypeId;
  if (p == kPNull) {
    return id == T::BOOLEAN || id == T::TINYINT || id == T::SMALLINT ||
           id == T::INTEGER || id == T::BIGINT || id == T::FLOAT ||
           id == T::DOUBLE || id == T::VARCHAR || id == T::BLOB ||
           id == T::DATE || id == T::TIMESTAMP;
  }
  switch (id) {
  case T::BOOLEAN:
    return p == kPLogical;
  case T::TINYINT:
  case T::SMALLINT:
  case T::INTEGER:
  case T::BIGINT:
    return p == kPInteger || p == kPDouble || p == kPInt64;
  case T::FLOAT:
  case T::DOUBLE:
    return p == kPInteger || p == kPDouble || p == kPInt64;
  case T::VARCHAR:
    return p == kPString;
  case T::BLOB:
    return p == kPRaw;
  case T::DATE:
    return p == kPDate;
  case T::TIMESTAMP:
    return p == kPTimestamp;
  default:
    return false;
  }
}

// Integer range of a column type, for integer and double payloads.
void IntRange(zugodb::LogicalTypeId id, double *lo, double *hi) {
  using T = zugodb::LogicalTypeId;
  switch (id) {
  case T::TINYINT:
    *lo = -128;
    *hi = 127;
    return;
  case T::SMALLINT:
    *lo = -32768;
    *hi = 32767;
    return;
  case T::INTEGER:
    *lo = -2147483648.0;
    *hi = 2147483647.0;
    return;
  default: // BIGINT: [-2^63, 2^63), as doubles hold them
    *lo = -9223372036854775808.0;
    *hi = 9223372036854775808.0;
    return;
  }
}

// Why value `r` of `c` does not fit a column of type `id`, or nullptr.
const char *Misfit(const InColumn &c, uint64_t r, zugodb::LogicalTypeId id) {
  using T = zugodb::LogicalTypeId;
  if (IsNA(c, r)) return nullptr;
  const bool integral = id == T::TINYINT || id == T::SMALLINT ||
                        id == T::INTEGER || id == T::BIGINT;
  if (integral) {
    double lo, hi;
    IntRange(id, &lo, &hi);
    if (c.payload == kPDouble) {
      const double x = c.reals[r];
      if (!std::isfinite(x) || x != std::trunc(x)) return "is not a whole number";
      if (x < lo || (id == T::BIGINT ? x >= hi : x > hi)) {
        return "is out of the column's range";
      }
    } else if (c.payload == kPInteger) {
      const double x = c.ints[r];
      if (x < lo || x > hi) return "is out of the column's range";
    } else if (c.payload == kPInt64 && id != T::BIGINT) {
      const double x = static_cast<double>(Int64Of(c, r));
      if (x < lo || x > hi) return "is out of the column's range";
    }
    return nullptr;
  }
  if ((id == T::FLOAT || id == T::DOUBLE) && c.payload == kPInt64) {
    const int64_t x = Int64Of(c, r);
    if (x > 9007199254740992LL || x < -9007199254740992LL) {
      return "is beyond 2^53, where a double is no longer exact";
    }
    return nullptr;
  }
  if (id == T::FLOAT && c.payload == kPDouble) {
    const double x = c.reals[r];
    if (std::isfinite(x) && std::fabs(x) > 3.4028234663852886e38) {
      return "is out of FLOAT's range";
    }
    return nullptr;
  }
  if (id == T::DATE) {
    const double x = c.reals[r];
    if (!std::isfinite(x) || x != std::trunc(x)) return "is not a whole day";
    if (x < -2147483648.0 || x > 2147483647.0) return "is out of DATE's range";
    return nullptr;
  }
  if (id == T::TIMESTAMP) {
    const double us = std::nearbyint(c.reals[r] * 1e6);
    if (!std::isfinite(us) || us < -9223372036854775808.0 ||
        us >= 9223372036854775808.0) {
      return "is out of TIMESTAMP's range";
    }
    return nullptr;
  }
  if (c.payload == kPString || c.payload == kPRaw) {
    if (c.lens[r] > 0xFFFFFFFFull) return "is longer than 4 GiB";
  }
  return nullptr;
}

double NumberOf(const InColumn &c, uint64_t r) {
  switch (c.payload) {
  case kPInteger:
    return c.ints[r];
  case kPInt64:
    return static_cast<double>(Int64Of(c, r));
  default:
    return c.reals[r];
  }
}

int64_t WholeOf(const InColumn &c, uint64_t r) {
  switch (c.payload) {
  case kPInteger:
    return c.ints[r];
  case kPInt64:
    return Int64Of(c, r);
  default:
    return static_cast<int64_t>(c.reals[r]);
  }
}

// Writes rows [from, from + n) of `c` into `vec` of type `id`. Every value
// was checked by Misfit() first.
void Fill(zugodb::Vector &vec, zugodb::LogicalTypeId id, const InColumn &c,
          uint64_t from, uint64_t n) {
  using T = zugodb::LogicalTypeId;
  auto &valid = vec.GetValidity();
  for (uint64_t k = 0; k < n; k++) {
    const uint64_t r = from + k;
    if (IsNA(c, r)) {
      valid.SetInvalid(k);
      continue;
    }
    switch (id) {
    case T::BOOLEAN:
      vec.GetData<bool>()[k] = c.ints[r] != 0;
      break;
    case T::TINYINT:
      vec.GetData<int8_t>()[k] = static_cast<int8_t>(WholeOf(c, r));
      break;
    case T::SMALLINT:
      vec.GetData<int16_t>()[k] = static_cast<int16_t>(WholeOf(c, r));
      break;
    case T::INTEGER:
      vec.GetData<int32_t>()[k] = static_cast<int32_t>(WholeOf(c, r));
      break;
    case T::BIGINT:
      vec.GetData<int64_t>()[k] = WholeOf(c, r);
      break;
    case T::FLOAT:
      vec.GetData<float>()[k] = static_cast<float>(NumberOf(c, r));
      break;
    case T::DOUBLE:
      vec.GetData<double>()[k] = NumberOf(c, r);
      break;
    case T::DATE:
      vec.GetData<int32_t>()[k] = static_cast<int32_t>(c.reals[r]);
      break;
    case T::TIMESTAMP:
      vec.GetData<int64_t>()[k] =
          static_cast<int64_t>(std::nearbyint(c.reals[r] * 1e6));
      break;
    case T::VARCHAR:
    case T::BLOB: {
      const char *p = c.strs[r];
      const uint32_t len = static_cast<uint32_t>(c.lens[r]);
      if (len <= zugodb::string_t::INLINE_LENGTH) {
        vec.GetData<zugodb::string_t>()[k] = zugodb::string_t(p, len);
      } else {
        const char *own = vec.GetStringBuffer().AddString(p, len);
        vec.GetData<zugodb::string_t>()[k] = zugodb::string_t(own, len);
      }
      break;
    }
    default:
      break; // excluded by Compatible()
    }
  }
}

// check_only: every check, and nothing appended (R/write.R checks the data
// against a probe table before it drops one it replaces).
void EngineAppend(zug::ZugConnection *c, const Input &in, bool check_only,
                  double *appended, zug::EngineError *err) {
  char msg[1024];
  try {
    std::string probe = std::string("SELECT * FROM ") + in.quoted + " LIMIT 0";
    zugodb::QueryResult shape = c->con->Query(probe);
    const auto &types = shape.column_types;
    if (static_cast<int>(types.size()) != in.ncol) {
      std::snprintf(msg, sizeof msg,
                    "The table has %d columns and the data %d.",
                    static_cast<int>(types.size()), in.ncol);
      err->set(msg, "argument");
      return;
    }
    // Every value is checked before the first row is appended.
    for (int k = 0; k < in.ncol; k++) {
      const InColumn &col = in.cols[k];
      const auto id = types[k].id();
      if (!Compatible(col.payload, id)) {
        std::snprintf(msg, sizeof msg,
                      "Column `%s` is %s, which cannot be written to a %s "
                      "column.",
                      col.name, PayloadName(col.payload),
                      types[k].ToString().c_str());
        err->set(msg, "type");
        return;
      }
      for (uint64_t r = 0; r < in.nrow; r++) {
        if (const char *why = Misfit(col, r, id)) {
          std::snprintf(msg, sizeof msg, "Column `%s`, row %" PRIu64 ": the value %s.",
                        col.name, r + 1, why);
          err->set(msg, "type");
          return;
        }
      }
    }
    if (check_only) return;
    for (uint64_t from = 0; from < in.nrow; from += zugodb::VECTOR_SIZE) {
      const uint64_t n = std::min<uint64_t>(zugodb::VECTOR_SIZE, in.nrow - from);
      zugodb::DataChunk chunk;
      chunk.Initialize(types);
      for (int k = 0; k < in.ncol; k++) {
        Fill(chunk.GetVector(k), types[k].id(), in.cols[k], from, n);
      }
      chunk.SetCardinality(n);
      c->con->Append(in.table, chunk);
      *appended += static_cast<double>(n);
    }
  } catch (const std::exception &e) {
    err->set(e.what());
  } catch (...) {
    err->set("unknown C++ exception");
  }
}

} // namespace

extern "C" {

// zug_append(con, table, quoted, cols, payloads, names, check_only): appends
// the data frame `cols` (a list of vectors made by R/write.R) to `table`;
// returns the rows appended. With check_only, checks every value against
// the table's types and appends nothing.
SEXP zug_append(SEXP conptr, SEXP table, SEXP quoted, SEXP cols, SEXP payloads,
                SEXP names, SEXP check_only) {
  zug::EngineError err;
  zug::ZugConnection *c = zug::GetConnection(conptr);
  if (!zug::ConnectionOpen(c)) {
    err.set(zug::kClosedConnection, "closed");
    return zug::ResultList(R_NilValue, err);
  }
  const R_xlen_t ncol = XLENGTH(cols);
  if (TYPEOF(table) != STRSXP || XLENGTH(table) != 1 ||
      TYPEOF(quoted) != STRSXP || XLENGTH(quoted) != 1 ||
      TYPEOF(cols) != VECSXP || TYPEOF(payloads) != INTSXP ||
      XLENGTH(payloads) != ncol || TYPEOF(names) != STRSXP ||
      XLENGTH(names) != ncol || ncol > 100000 ||
      TYPEOF(check_only) != LGLSXP || XLENGTH(check_only) != 1 ||
      LOGICAL(check_only)[0] == NA_LOGICAL) {
    err.set("zug_append: malformed arguments", "argument");
    return zug::ResultList(R_NilValue, err);
  }
  Input in;
  in.table = CHAR(STRING_ELT(table, 0));
  in.quoted = CHAR(STRING_ELT(quoted, 0));
  in.ncol = static_cast<int>(ncol);
  in.nrow = ncol ? static_cast<uint64_t>(XLENGTH(VECTOR_ELT(cols, 0))) : 0;
  in.cols = reinterpret_cast<InColumn *>(
      R_alloc(ncol ? ncol : 1, sizeof(InColumn)));
  for (R_xlen_t k = 0; k < ncol; k++) {
    SEXP x = VECTOR_ELT(cols, k);
    InColumn &col = in.cols[k];
    col = InColumn();
    col.name = CHAR(STRING_ELT(names, k));
    col.payload = INTEGER(payloads)[k];
    if (static_cast<uint64_t>(XLENGTH(x)) != in.nrow) {
      err.set("zug_append: columns of unequal length", "argument");
      return zug::ResultList(R_NilValue, err);
    }
    const R_xlen_t n = XLENGTH(x);
    switch (col.payload) {
    case kPNull:
    case kPLogical:
    case kPInteger:
      if (TYPEOF(x) != (col.payload == kPInteger ? INTSXP : LGLSXP)) break;
      col.ints = col.payload == kPInteger ? INTEGER(x) : LOGICAL(x);
      continue;
    case kPDouble:
    case kPInt64:
    case kPDate:
    case kPTimestamp:
      if (TYPEOF(x) != REALSXP) break;
      col.reals = REAL(x);
      continue;
    case kPString:
    case kPRaw: {
      const bool str = col.payload == kPString;
      if (TYPEOF(x) != (str ? STRSXP : VECSXP)) break;
      const char **strs =
          reinterpret_cast<const char **>(R_alloc(n ? n : 1, sizeof(char *)));
      uint64_t *lens =
          reinterpret_cast<uint64_t *>(R_alloc(n ? n : 1, sizeof(uint64_t)));
      bool ok = true;
      for (R_xlen_t r = 0; r < n; r++) {
        if (str) {
          SEXP s = STRING_ELT(x, r);
          strs[r] = s == NA_STRING ? nullptr : CHAR(s);
          lens[r] = s == NA_STRING ? 0 : static_cast<uint64_t>(XLENGTH(s));
        } else {
          SEXP b = VECTOR_ELT(x, r);
          if (b == R_NilValue) {
            strs[r] = nullptr;
            lens[r] = 0;
          } else if (TYPEOF(b) == RAWSXP) {
            // An empty raw vector's RAW() may not be dereferenceable; it is
            // never read, as its length is 0.
            static const char empty = 0;
            strs[r] = XLENGTH(b) ? reinterpret_cast<const char *>(RAW(b)) : &empty;
            lens[r] = static_cast<uint64_t>(XLENGTH(b));
          } else {
            ok = false;
            break;
          }
        }
      }
      if (!ok) break;
      col.strs = strs;
      col.lens = lens;
      continue;
    }
    default:
      break;
    }
    err.set("zug_append: a column does not match its payload", "argument");
    return zug::ResultList(R_NilValue, err);
  }
  double appended = 0;
  EngineAppend(c, in, LOGICAL(check_only)[0] != 0, &appended, &err);
  return zug::ResultList(err.failed ? R_NilValue : Rf_ScalarReal(appended), err);
}

} // extern "C"
