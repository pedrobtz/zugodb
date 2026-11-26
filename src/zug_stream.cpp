// An Arrow C stream into a table (design §7.3, Q15; roadmap Stage 7).
//
// The one adapter from a stream to the engine: the stream's batches become
// DataChunks and go in through Connection::Append, the path zug_write.cpp
// uses for a data frame. R/arrow.R drives it, and decides which table the
// rows go to (a new table, or a staging table it copies from, so that a
// load that fails part-way leaves nothing, §7.3).
//
// Ownership. zug_stream_take() moves the producer's stream into a holder
// behind an external pointer of this package's: from then on the holder
// owns the stream, its schema and the batch being read, and releases each
// exactly once, whether the load succeeds, fails, or is interrupted (an R
// error unwinds to R/arrow.R's on.exit(), which calls zug_stream_release();
// the finalizer is the last resort). The producer's own object is left
// released, as the Arrow C stream interface's "move" leaves it.
//
// Design §4.3 holds as elsewhere: the producer's callbacks and R's
// interrupt check run in the .Call frame, with no C++ object live; each
// batch is converted and appended in EngineAppendBatch(), inside
// try/catch, with every engine object scoped there. A producer whose
// callback calls R and longjmps leaves the holder to its finalizer.
//
// The pull is one batch at a time, so a scan operator (Q19) can use the
// same conversion later.

#include "zug_arrow.h"
#include "zug_objects.h"

#include <algorithm>
#include <cerrno>
#include <new>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// ---------------------------------------------------------------- holder

struct StreamHolder {
  ArrowArrayStream stream;  // moved from the producer; release NULL once done
  ArrowSchema schema;       // from get_schema(), once
  ArrowArray batch;         // the batch being read
  bool have_schema;
  long pid;
};

void ReleaseBatch(StreamHolder *h) {
  if (h->batch.release) {
    h->batch.release(&h->batch);
    h->batch.release = nullptr;
  }
}

void ReleaseHolder(StreamHolder *h) {
  ReleaseBatch(h);
  if (h->schema.release) {
    h->schema.release(&h->schema);
    h->schema.release = nullptr;
  }
  if (h->stream.release) {
    h->stream.release(&h->stream);
    h->stream.release = nullptr;
  }
}

void HolderFinalizer(SEXP ptr) {
  StreamHolder *h = static_cast<StreamHolder *>(R_ExternalPtrAddr(ptr));
  if (!h) return;
  // In a forked child the producer's threads and memory are not its own:
  // release nothing there, as the database guard does (Q13).
  if (zug::SameProcess(h->pid)) ReleaseHolder(h);
  std::free(h);
  R_ClearExternalPtr(ptr);
}

SEXP kHolderTag() { return Rf_install("zugodb_stream"); }

StreamHolder *GetHolder(SEXP ptr) {
  if (TYPEOF(ptr) != EXTPTRSXP || R_ExternalPtrTag(ptr) != kHolderTag()) {
    return nullptr;
  }
  return static_cast<StreamHolder *>(R_ExternalPtrAddr(ptr));
}

const char *kReleased =
    "The stream has been released: it was read already, or released by its "
    "producer.";

// Whether the holder can be read, or why not.
bool HolderUsable(StreamHolder *h, zug::EngineError *err) {
  if (!h) {
    err->set("zug_stream: not a stream holder", "argument");
    return false;
  }
  if (!zug::SameProcess(h->pid)) {
    err->set("The stream belongs to another process.", "closed");
    return false;
  }
  if (!h->stream.release) {
    err->set(kReleased, "closed");
    return false;
  }
  return true;
}

void ProducerError(ArrowArrayStream *s, int code, const char *what,
                   zug::EngineError *err) {
  const char *last = s->get_last_error ? s->get_last_error(s) : nullptr;
  char msg[1536];
  if (last && *last) {
    std::snprintf(msg, sizeof msg, "The stream's producer failed %s: %s", what,
                  last);
  } else {
    std::snprintf(msg, sizeof msg,
                  "The stream's producer failed %s, with error code %d.", what,
                  code);
  }
  err->set(msg, "stream");
}

bool EnsureSchema(StreamHolder *h, zug::EngineError *err) {
  if (h->have_schema) return true;
  std::memset(&h->schema, 0, sizeof h->schema);
  const int rc = h->stream.get_schema(&h->stream, &h->schema);
  if (rc != 0) {
    h->schema.release = nullptr;
    ProducerError(&h->stream, rc, "to give its schema", err);
    return false;
  }
  if (!h->schema.release) {
    err->set("The stream's producer gave a released schema.", "stream");
    return false;
  }
  h->have_schema = true;
  return true;
}

// ------------------------------------------------------------ Arrow types

enum Kind {
  kUnsupported = -1,
  kBool,
  kI8,
  kI16,
  kI32,
  kI64,
  kU8,
  kU16,
  kU32,
  kU64,
  kF32,
  kF64,
  kStr,
  kLargeStr,
  kStrView,
  kBin,
  kLargeBin,
  kBinView,
  kFixedBin,
  kDate32,
  kDate64,
  kTsS,
  kTsMs,
  kTsUs,
  kTsNs
};

const char *KindName(int k) {
  static const char *names[] = {
      "boolean",      "int8",          "int16",       "int32",
      "int64",        "uint8",         "uint16",      "uint32",
      "uint64",       "float",         "double",      "string",
      "large_string", "string_view",   "binary",      "large_binary",
      "binary_view",  "fixed_size_binary", "date32",  "date64",
      "timestamp[s]", "timestamp[ms]", "timestamp[us]", "timestamp[ns]"};
  return (k >= 0 && k <= kTsNs) ? names[k] : "unsupported";
}

// The engine type a new table gives a column of kind `k` (design §6).
const char *SqlTypeOf(int k) {
  switch (k) {
  case kBool:
    return "BOOLEAN";
  case kI8:
    return "TINYINT";
  case kI16:
  case kU8:
    return "SMALLINT";
  case kI32:
  case kU16:
    return "INTEGER";
  case kI64:
  case kU32:
  case kU64: // checked value by value: past 2^63 - 1 is an error
    return "BIGINT";
  case kF32:
    return "FLOAT";
  case kF64:
    return "DOUBLE";
  case kStr:
  case kLargeStr:
  case kStrView:
    return "VARCHAR";
  case kBin:
  case kLargeBin:
  case kBinView:
  case kFixedBin:
    return "BLOB";
  case kDate32:
  case kDate64:
    return "DATE";
  case kTsS:
  case kTsMs:
  case kTsUs:
  case kTsNs:
    return "TIMESTAMP";
  default:
    return nullptr;
  }
}

// Whether the schema's metadata names an extension type. The metadata is
// the specification's: an int32 count, then each key and value as an
// int32 length and its bytes.
bool IsExtension(const char *metadata) {
  if (!metadata) return false;
  int32_t n;
  std::memcpy(&n, metadata, 4);
  const char *p = metadata + 4;
  static const char key[] = "ARROW:extension:name";
  for (int32_t i = 0; i < n; i++) {
    int32_t klen, vlen;
    std::memcpy(&klen, p, 4);
    p += 4;
    const bool hit = klen == static_cast<int32_t>(sizeof key - 1) &&
                     std::memcmp(p, key, sizeof key - 1) == 0;
    p += klen;
    std::memcpy(&vlen, p, 4);
    p += 4 + vlen;
    if (hit) return true;
  }
  return false;
}

// The kind of a column's schema, or kUnsupported with what it is.
int KindOf(const ArrowSchema *s, int64_t *width, const char **what) {
  *width = 0;
  *what = "an Arrow type";
  const char *f = s->format;
  if (!f) {
    *what = "a column with no format";
    return kUnsupported;
  }
  if (s->dictionary) {
    *what = "dictionary-encoded";
    return kUnsupported;
  }
  if (IsExtension(s->metadata)) {
    *what = "an Arrow extension type";
    return kUnsupported;
  }
  if (f[0] && !f[1]) {
    switch (f[0]) {
    case 'b':
      return kBool;
    case 'c':
      return kI8;
    case 's':
      return kI16;
    case 'i':
      return kI32;
    case 'l':
      return kI64;
    case 'C':
      return kU8;
    case 'S':
      return kU16;
    case 'I':
      return kU32;
    case 'L':
      return kU64;
    case 'f':
      return kF32;
    case 'g':
      return kF64;
    case 'u':
      return kStr;
    case 'U':
      return kLargeStr;
    case 'z':
      return kBin;
    case 'Z':
      return kLargeBin;
    case 'e':
      *what = "a half float";
      return kUnsupported;
    case 'n':
      *what = "of the null type";
      return kUnsupported;
    default:
      return kUnsupported;
    }
  }
  if (!std::strcmp(f, "vu")) return kStrView;
  if (!std::strcmp(f, "vz")) return kBinView;
  if (!std::strcmp(f, "tdD")) return kDate32;
  if (!std::strcmp(f, "tdm")) return kDate64;
  if (!std::strncmp(f, "ts", 2) && f[2] && f[3] == ':') {
    switch (f[2]) {
    case 's':
      return kTsS;
    case 'm':
      return kTsMs;
    case 'u':
      return kTsUs;
    case 'n':
      return kTsNs;
    default:
      return kUnsupported;
    }
  }
  if (!std::strncmp(f, "w:", 2)) {
    char *end = nullptr;
    const long long w = std::strtoll(f + 2, &end, 10);
    if (end && !*end && w >= 0 && w <= 0x7FFFFFFF) {
      *width = w;
      return kFixedBin;
    }
    return kUnsupported;
  }
  if (f[0] == 'd' && f[1] == ':') *what = "a decimal";
  else if (f[0] == 't' && f[1] == 't') *what = "a time of day";
  else if (f[0] == 't' && f[1] == 'D') *what = "a duration";
  else if (f[0] == 't' && f[1] == 'i') *what = "an interval";
  else if (!std::strcmp(f, "+l") || !std::strcmp(f, "+L") ||
           !std::strncmp(f, "+w:", 3) || !std::strcmp(f, "+vl") ||
           !std::strcmp(f, "+vL"))
    *what = "a list";
  else if (!std::strcmp(f, "+s")) *what = "a struct";
  else if (!std::strcmp(f, "+m")) *what = "a map";
  else if (!std::strncmp(f, "+u", 2)) *what = "a union";
  else if (!std::strcmp(f, "+r")) *what = "run-end encoded";
  return kUnsupported;
}

// Strict UTF-8, as R's validUTF8(): no overlong form, no surrogate, nothing
// past U+10FFFF.
bool ValidUtf8(const unsigned char *s, uint64_t n) {
  uint64_t i = 0;
  while (i < n) {
    const unsigned char c = s[i];
    if (c < 0x80) {
      i++;
      continue;
    }
    int len;
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
    if (n - i < static_cast<uint64_t>(len)) return false;
    for (int k = 1; k < len; k++) {
      if ((s[i + k] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (s[i + k] & 0x3F);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;
    }
    i += len;
  }
  return true;
}

// ----------------------------------------------------------------- plan

// What one table column takes from the stream: plain values only, made
// in the .Call frame from R_alloc memory.
struct PlanColumn {
  int child = -1;          // the stream's column, or -1: every value NULL
  int kind = kUnsupported; // of that column
  int64_t width = 0;       // kFixedBin
  int type = 0;            // the table column's zugodb::LogicalTypeId
  const char *name = nullptr;
};

struct Plan {
  const char *table = nullptr;
  PlanColumn *cols = nullptr;
  int ncol = 0;
  int64_t nchild = 0; // the schema's
};

// Whether kind `k` can go into a column of type `id` (as zug_write.cpp's
// Compatible() for the R classes).
bool Compatible(int k, zugodb::LogicalTypeId id) {
  using T = zugodb::LogicalTypeId;
  const bool integer = k >= kI8 && k <= kU64;
  const bool real = k == kF32 || k == kF64;
  switch (id) {
  case T::BOOLEAN:
    return k == kBool;
  case T::TINYINT:
  case T::SMALLINT:
  case T::INTEGER:
  case T::BIGINT:
  case T::FLOAT:
  case T::DOUBLE:
    return integer || real;
  case T::VARCHAR:
    return k == kStr || k == kLargeStr || k == kStrView;
  case T::BLOB:
    return k == kBin || k == kLargeBin || k == kBinView || k == kFixedBin;
  case T::DATE:
    return k == kDate32 || k == kDate64;
  case T::TIMESTAMP:
    return k >= kTsS && k <= kTsNs;
  default:
    return false;
  }
}

bool NullCompatible(zugodb::LogicalTypeId id) {
  using T = zugodb::LogicalTypeId;
  return id == T::BOOLEAN || id == T::TINYINT || id == T::SMALLINT ||
         id == T::INTEGER || id == T::BIGINT || id == T::FLOAT ||
         id == T::DOUBLE || id == T::VARCHAR || id == T::BLOB ||
         id == T::DATE || id == T::TIMESTAMP;
}

// Reads the table's column types into plan->cols[].type, and checks each
// column's kind against its type: every check that needs no row, made
// before the first batch is pulled.
void EnginePlan(zug::ZugConnection *c, const char *quoted, Plan *plan,
                zug::EngineError *err) {
  char msg[1024];
  try {
    std::string probe = std::string("SELECT * FROM ") + quoted + " LIMIT 0";
    zugodb::QueryResult shape = c->con->Query(probe);
    const auto &types = shape.column_types;
    if (static_cast<int>(types.size()) != plan->ncol) {
      std::snprintf(msg, sizeof msg, "The table has %d columns and the stream %d.",
                    static_cast<int>(types.size()), plan->ncol);
      err->set(msg, "argument");
      return;
    }
    for (int k = 0; k < plan->ncol; k++) {
      PlanColumn &col = plan->cols[k];
      const auto id = types[k].id();
      col.type = static_cast<int>(id);
      const bool ok = col.child < 0 ? NullCompatible(id) : Compatible(col.kind, id);
      if (!ok) {
        std::snprintf(msg, sizeof msg,
                      "Column `%s` is an Arrow %s, which cannot be written to a "
                      "%s column.",
                      col.name, col.child < 0 ? "null" : KindName(col.kind),
                      types[k].ToString().c_str());
        err->set(msg, "type");
        return;
      }
    }
  } catch (const std::exception &e) {
    err->set(e.what());
  } catch (...) {
    err->set("unknown C++ exception");
  }
}

// ------------------------------------------------------------- one batch

// A column of the batch, located: element i is at physical index base + i.
struct In {
  const ArrowArray *a = nullptr;
  const uint8_t *valid = nullptr;
  int64_t base = 0;
  int kind = kUnsupported;
  int64_t width = 0;
};

inline bool Bit(const uint8_t *bits, int64_t i) {
  return (bits[i >> 3] >> (i & 7)) & 1;
}

inline bool IsNull(const In &in, int64_t i) {
  return in.valid && !Bit(in.valid, in.base + i);
}

template <typename T> inline T At(const In &in, int64_t i) {
  T x;
  std::memcpy(&x, static_cast<const char *>(in.a->buffers[1]) +
                      (in.base + i) * static_cast<int64_t>(sizeof(T)),
              sizeof(T));
  return x;
}

// An integer kind's value, with `big` set for a uint64 past 2^63 - 1.
int64_t IntOf(const In &in, int64_t i, bool *big) {
  *big = false;
  switch (in.kind) {
  case kI8:
    return At<int8_t>(in, i);
  case kI16:
    return At<int16_t>(in, i);
  case kI32:
    return At<int32_t>(in, i);
  case kI64:
    return At<int64_t>(in, i);
  case kU8:
    return At<uint8_t>(in, i);
  case kU16:
    return At<uint16_t>(in, i);
  case kU32:
    return At<uint32_t>(in, i);
  default: { // kU64
    const uint64_t u = At<uint64_t>(in, i);
    if (u > static_cast<uint64_t>(INT64_MAX)) {
      *big = true;
      return 0;
    }
    return static_cast<int64_t>(u);
  }
  }
}

double RealOf(const In &in, int64_t i) {
  return in.kind == kF32 ? static_cast<double>(At<float>(in, i))
                         : At<double>(in, i);
}

// The bytes of element i of a string or binary kind; false if the batch's
// offsets or views are malformed.
bool BytesOf(const In &in, int64_t i, const char **p, uint64_t *len) {
  const int64_t j = in.base + i;
  const ArrowArray *a = in.a;
  switch (in.kind) {
  case kStr:
  case kBin: {
    const int32_t *off = static_cast<const int32_t *>(a->buffers[1]);
    const int32_t lo = off[j], hi = off[j + 1];
    if (lo < 0 || hi < lo) return false;
    *p = static_cast<const char *>(a->buffers[2]) + lo;
    *len = static_cast<uint64_t>(hi - lo);
    return true;
  }
  case kLargeStr:
  case kLargeBin: {
    const int64_t *off = static_cast<const int64_t *>(a->buffers[1]);
    const int64_t lo = off[j], hi = off[j + 1];
    if (lo < 0 || hi < lo) return false;
    *p = static_cast<const char *>(a->buffers[2]) + lo;
    *len = static_cast<uint64_t>(hi - lo);
    return true;
  }
  case kFixedBin:
    *p = static_cast<const char *>(a->buffers[1]) + j * in.width;
    *len = static_cast<uint64_t>(in.width);
    return true;
  default: { // kStrView, kBinView: 16 bytes each
    const char *v = static_cast<const char *>(a->buffers[1]) + j * 16;
    int32_t n;
    std::memcpy(&n, v, 4);
    if (n < 0) return false;
    if (n <= 12) {
      *p = v + 4;
    } else {
      int32_t buf, off;
      std::memcpy(&buf, v + 8, 4);
      std::memcpy(&off, v + 12, 4);
      // Buffers: validity, views, the variadic data buffers, their sizes.
      if (buf < 0 || off < 0 || buf >= a->n_buffers - 3) return false;
      const char *data = static_cast<const char *>(a->buffers[2 + buf]);
      if (!data) return false;
      *p = data + off;
    }
    *len = static_cast<uint64_t>(n);
    return true;
  }
  }
}

// The number of buffers kind `k` has, or the least for a view.
int64_t BuffersOf(int k) {
  switch (k) {
  case kStr:
  case kLargeStr:
  case kBin:
  case kLargeBin:
  case kStrView:
  case kBinView:
    return 3;
  default:
    return 2;
  }
}

bool IsView(int k) { return k == kStrView || k == kBinView; }

// Why the batch's column cannot be read as the schema says, or nullptr.
const char *Malformed(const ArrowArray *a, int kind, int64_t need) {
  if (!a) return "is missing";
  if (a->length < 0 || a->offset < 0 || a->length < need) {
    return "is shorter than the batch";
  }
  if (IsView(kind) ? a->n_buffers < 3 : a->n_buffers != BuffersOf(kind)) {
    return "does not have its type's buffers";
  }
  if (a->n_children != 0 || a->dictionary) return "does not have its type's layout";
  if (!a->buffers) return "has no buffers";
  if (need > 0 && !a->buffers[1]) return "has no data buffer";
  if (need > 0 && (kind == kStr || kind == kLargeStr || kind == kBin ||
                   kind == kLargeBin) && !a->buffers[2]) {
    return "has no data buffer";
  }
  if (a->null_count != 0 && a->null_count != -1 && !a->buffers[0]) {
    return "counts nulls but has no validity buffer";
  }
  return nullptr;
}

void Fail(zug::EngineError *err, const char *kind, const char *fmt,
          const char *name, uint64_t row, const char *why) {
  char msg[1024];
  std::snprintf(msg, sizeof msg, fmt, name, row, why);
  err->set(msg, kind);
}

const char *kValueFmt = "Column `%s`, row %" PRIu64 ": the value %s.";

// Writes elements [from, from + n) of `in` into `vec` of type `id`, row
// numbers counted from `row0` (1-based, across the stream). False, with
// the error set, at the first value its column cannot hold.
bool FillColumn(zugodb::Vector &vec, zugodb::LogicalTypeId id, const In &in,
                const char *name, int64_t from, int64_t n, uint64_t row0,
                zug::EngineError *err) {
  using T = zugodb::LogicalTypeId;
  auto &valid = vec.GetValidity();
  if (!in.a) { // a table column the stream does not give
    for (int64_t k = 0; k < n; k++) valid.SetInvalid(k);
    return true;
  }
  const bool integral =
      id == T::TINYINT || id == T::SMALLINT || id == T::INTEGER || id == T::BIGINT;
  const bool from_int = in.kind >= kI8 && in.kind <= kU64;
  for (int64_t k = 0; k < n; k++) {
    const int64_t i = from + k;
    const uint64_t row = row0 + static_cast<uint64_t>(i);
    if (IsNull(in, i)) {
      valid.SetInvalid(k);
      continue;
    }
    if (integral) {
      int64_t lo = INT64_MIN, hi = INT64_MAX;
      if (id == T::TINYINT) lo = -128, hi = 127;
      if (id == T::SMALLINT) lo = -32768, hi = 32767;
      if (id == T::INTEGER) lo = INT32_MIN, hi = INT32_MAX;
      int64_t v;
      if (from_int) {
        bool big;
        v = IntOf(in, i, &big);
        if (big || v < lo || v > hi) {
          Fail(err, "type", kValueFmt, name, row, "is out of the column's range");
          return false;
        }
      } else {
        const double x = RealOf(in, i);
        if (!std::isfinite(x) || x != std::trunc(x)) {
          Fail(err, "type", kValueFmt, name, row, "is not a whole number");
          return false;
        }
        if (x < static_cast<double>(lo) ||
            (id == T::BIGINT ? x >= 9223372036854775808.0
                             : x > static_cast<double>(hi))) {
          Fail(err, "type", kValueFmt, name, row, "is out of the column's range");
          return false;
        }
        v = static_cast<int64_t>(x);
      }
      switch (id) {
      case T::TINYINT:
        vec.GetData<int8_t>()[k] = static_cast<int8_t>(v);
        break;
      case T::SMALLINT:
        vec.GetData<int16_t>()[k] = static_cast<int16_t>(v);
        break;
      case T::INTEGER:
        vec.GetData<int32_t>()[k] = static_cast<int32_t>(v);
        break;
      default:
        vec.GetData<int64_t>()[k] = v;
        break;
      }
      continue;
    }
    switch (id) {
    case T::BOOLEAN:
      vec.GetData<bool>()[k] =
          Bit(static_cast<const uint8_t *>(in.a->buffers[1]), in.base + i);
      break;
    case T::FLOAT:
    case T::DOUBLE: {
      double x;
      if (from_int) {
        bool big;
        const int64_t v = IntOf(in, i, &big);
        if (big || v > 9007199254740992LL || v < -9007199254740992LL) {
          Fail(err, "type", kValueFmt, name, row,
               "is beyond 2^53, where a double is no longer exact");
          return false;
        }
        x = static_cast<double>(v);
      } else {
        x = RealOf(in, i);
      }
      if (id == T::FLOAT) {
        if (in.kind == kF32) {
          vec.GetData<float>()[k] = At<float>(in, i);
          break;
        }
        if (std::isfinite(x) && std::fabs(x) > 3.4028234663852886e38) {
          Fail(err, "type", kValueFmt, name, row, "is out of FLOAT's range");
          return false;
        }
        vec.GetData<float>()[k] = static_cast<float>(x);
      } else {
        vec.GetData<double>()[k] = x;
      }
      break;
    }
    case T::DATE: {
      int64_t d;
      if (in.kind == kDate32) {
        d = At<int32_t>(in, i);
      } else {
        const int64_t ms = At<int64_t>(in, i);
        if (ms % 86400000 != 0) {
          Fail(err, "type", kValueFmt, name, row, "is not a whole day");
          return false;
        }
        d = ms / 86400000;
        if (d < INT32_MIN || d > INT32_MAX) {
          Fail(err, "type", kValueFmt, name, row, "is out of DATE's range");
          return false;
        }
      }
      vec.GetData<int32_t>()[k] = static_cast<int32_t>(d);
      break;
    }
    case T::TIMESTAMP: {
      const int64_t v = At<int64_t>(in, i);
      int64_t us = v;
      const int64_t scale =
          in.kind == kTsS ? 1000000 : in.kind == kTsMs ? 1000 : 1;
      if (in.kind == kTsNs) {
        if (v % 1000 != 0) {
          Fail(err, "type", kValueFmt, name, row,
               "has a fraction of a microsecond, which TIMESTAMP cannot hold");
          return false;
        }
        us = v / 1000;
      } else if (scale > 1) {
        if (v > INT64_MAX / scale || v < INT64_MIN / scale) {
          Fail(err, "type", kValueFmt, name, row, "is out of TIMESTAMP's range");
          return false;
        }
        us = v * scale;
      }
      vec.GetData<int64_t>()[k] = us;
      break;
    }
    case T::VARCHAR:
    case T::BLOB: {
      const char *p = nullptr;
      uint64_t len = 0;
      if (!BytesOf(in, i, &p, &len)) {
        Fail(err, "stream", "Column `%s`, row %" PRIu64 ": the batch %s.", name,
             row, "has malformed offsets");
        return false;
      }
      if (len > 0xFFFFFFFFull) {
        Fail(err, "type", kValueFmt, name, row, "is longer than 4 GiB");
        return false;
      }
      if (id == T::VARCHAR) {
        if (std::memchr(p, 0, len)) {
          Fail(err, "type", kValueFmt, name, row, "holds a NUL");
          return false;
        }
        if (!ValidUtf8(reinterpret_cast<const unsigned char *>(p), len)) {
          Fail(err, "type", kValueFmt, name, row, "is not valid UTF-8");
          return false;
        }
      }
      const uint32_t n32 = static_cast<uint32_t>(len);
      if (n32 <= zugodb::string_t::INLINE_LENGTH) {
        vec.GetData<zugodb::string_t>()[k] = zugodb::string_t(p, n32);
      } else {
        const char *own = vec.GetStringBuffer().AddString(p, n32);
        vec.GetData<zugodb::string_t>()[k] = zugodb::string_t(own, n32);
      }
      break;
    }
    default:
      break; // excluded by Compatible()
    }
  }
  return true;
}

// Checks the batch against the plan, then appends it in chunks of
// VECTOR_SIZE rows. *rows counts the rows appended so far.
void EngineAppendBatch(zug::ZugConnection *c, const Plan &plan,
                       const ArrowArray *b, double *rows,
                       zug::EngineError *err) {
  char msg[1024];
  try {
    if (b->length < 0 || b->offset < 0 || b->n_children != plan.nchild ||
        (plan.nchild > 0 && !b->children)) {
      err->set("A batch does not match the stream's schema: it has another "
               "number of columns.",
               "stream");
      return;
    }
    const int64_t n = b->length;
    if (b->n_buffers >= 1 && b->buffers && b->buffers[0] && b->null_count != 0) {
      const uint8_t *top = static_cast<const uint8_t *>(b->buffers[0]);
      for (int64_t i = 0; i < n; i++) {
        if (!Bit(top, b->offset + i)) {
          err->set("A batch of the stream has a null row.", "stream");
          return;
        }
      }
    }
    std::vector<In> ins(plan.ncol);
    std::vector<zugodb::LogicalType> types;
    types.reserve(plan.ncol);
    for (int k = 0; k < plan.ncol; k++) {
      const PlanColumn &pc = plan.cols[k];
      types.emplace_back(static_cast<zugodb::LogicalTypeId>(pc.type));
      if (pc.child < 0) continue;
      const ArrowArray *a = b->children[pc.child];
      if (const char *why = Malformed(a, pc.kind, b->offset + n)) {
        std::snprintf(msg, sizeof msg,
                      "Column `%s`: the batch %s, which does not match the "
                      "stream's schema.",
                      pc.name, why);
        err->set(msg, "stream");
        return;
      }
      In &in = ins[k];
      in.a = a;
      in.valid = static_cast<const uint8_t *>(a->buffers[0]);
      in.base = a->offset + b->offset;
      in.kind = pc.kind;
      in.width = pc.width;
    }
    const uint64_t row0 = static_cast<uint64_t>(*rows) + 1;
    for (int64_t from = 0; from < n; from += zugodb::VECTOR_SIZE) {
      const int64_t m = std::min<int64_t>(zugodb::VECTOR_SIZE, n - from);
      zugodb::DataChunk chunk;
      chunk.Initialize(types);
      for (int k = 0; k < plan.ncol; k++) {
        if (!FillColumn(chunk.GetVector(k), types[k].id(), ins[k],
                        plan.cols[k].name, from, m, row0, err)) {
          return;
        }
      }
      chunk.SetCardinality(m);
      c->con->Append(plan.table, chunk);
      *rows += static_cast<double>(m);
    }
  } catch (const std::exception &e) {
    err->set(e.what());
  } catch (...) {
    err->set("unknown C++ exception");
  }
}

// ------------------------------------------------- the schema, as R sees it

// list(names, sql, what): each column's name, the engine type a new table
// gives it (NA if none), and what it is when it has none (else NA).
SEXP ColumnsOf(const ArrowSchema *s, zug::EngineError *err) {
  if (!s->format || std::strcmp(s->format, "+s") != 0 || s->n_children < 0 ||
      (s->n_children > 0 && !s->children)) {
    err->set("The stream's schema is not a struct of columns, as a table's "
             "is.",
             "stream");
    return R_NilValue;
  }
  if (s->n_children > 100000) {
    err->set("The stream has more than 100000 columns.", "argument");
    return R_NilValue;
  }
  const R_xlen_t n = static_cast<R_xlen_t>(s->n_children);
  for (R_xlen_t k = 0; k < n; k++) {
    const ArrowSchema *ch = s->children[k];
    if (!ch || !ch->name) {
      err->set("A column of the stream's schema has no name.", "argument");
      return R_NilValue;
    }
    if (!ValidUtf8(reinterpret_cast<const unsigned char *>(ch->name),
                   std::strlen(ch->name))) {
      err->set("A column name of the stream's schema is not valid UTF-8.",
               "argument");
      return R_NilValue;
    }
  }
  const char *names[] = {"names", "sql", "what", ""};
  SEXP out = PROTECT(Rf_mkNamed(VECSXP, names));
  SEXP nm = PROTECT(Rf_allocVector(STRSXP, n));
  SEXP sql = PROTECT(Rf_allocVector(STRSXP, n));
  SEXP what = PROTECT(Rf_allocVector(STRSXP, n));
  for (R_xlen_t k = 0; k < n; k++) {
    const ArrowSchema *ch = s->children[k];
    SET_STRING_ELT(nm, k, Rf_mkCharCE(ch->name, CE_UTF8));
    int64_t width;
    const char *w;
    const char *type = SqlTypeOf(KindOf(ch, &width, &w));
    SET_STRING_ELT(sql, k, type ? Rf_mkChar(type) : NA_STRING);
    SET_STRING_ELT(what, k, type ? NA_STRING : Rf_mkChar(w));
  }
  SET_VECTOR_ELT(out, 0, nm);
  SET_VECTOR_ELT(out, 1, sql);
  SET_VECTOR_ELT(out, 2, what);
  UNPROTECT(4);
  return out;
}

// ------------------------------------------------ the tests' producer
//
// A stream made here, for tests/testthat only (it is not exported): two
// columns, x (int32, a NULL at every fifth row) and s (string), with
// `per` rows a batch, for `nbatch` batches. At batch `fail_at` (1-based;
// 0 never) it fails with `message`, or, with `mismatch`, gives a batch of
// one column. Each release of a stream it made is counted, so a test can
// see that the consumer released it once.

int g_test_releases = 0;

struct TestStream {
  int nbatch, per, fail_at, done;
  bool mismatch;
  char message[256];
};

struct TestBatch {
  std::vector<int32_t> x, offs;
  std::vector<uint8_t> xvalid;
  std::string chars;
  const void *xbuf[2], *sbuf[3];
  const void *topbuf[1];
  ArrowArray cx, cs;
  ArrowArray *kids[2];
};

void TestArrayRelease(ArrowArray *a) {
  if (a->private_data) delete static_cast<TestBatch *>(a->private_data);
  a->release = nullptr;
}

void TestChildRelease(ArrowArray *a) { a->release = nullptr; }

void TestSchemaRelease(ArrowSchema *s) {
  if (s->children) {
    for (int64_t k = 0; k < s->n_children; k++) {
      if (s->children[k]->release) s->children[k]->release(s->children[k]);
      delete s->children[k];
    }
    delete[] s->children;
  }
  s->release = nullptr;
}

void TestLeafRelease(ArrowSchema *s) { s->release = nullptr; }

ArrowSchema *TestLeaf(const char *format, const char *name) {
  ArrowSchema *s = new (std::nothrow) ArrowSchema();
  if (!s) return nullptr;
  s->format = format;
  s->name = name;
  s->flags = ARROW_FLAG_NULLABLE;
  s->release = TestLeafRelease;
  return s;
}

int TestGetSchema(ArrowArrayStream *st, ArrowSchema *out) {
  (void)st;
  std::memset(out, 0, sizeof *out);
  out->format = "+s";
  out->name = "";
  ArrowSchema **kids = new (std::nothrow) ArrowSchema *[2];
  ArrowSchema *x = TestLeaf("i", "x");
  ArrowSchema *s = TestLeaf("u", "s");
  if (!kids || !x || !s) {
    delete[] kids;
    delete x;
    delete s;
    return ENOMEM;
  }
  kids[0] = x;
  kids[1] = s;
  out->n_children = 2;
  out->children = kids;
  out->release = TestSchemaRelease;
  return 0;
}

int TestGetNextOrThrow(ArrowArrayStream *st, ArrowArray *out) {
  TestStream *t = static_cast<TestStream *>(st->private_data);
  std::memset(out, 0, sizeof *out);
  if (t->done >= t->nbatch) {
    out->release = nullptr; // the end
    return 0;
  }
  if (t->fail_at && t->done + 1 == t->fail_at && !t->mismatch) {
    return EIO;
  }
  TestBatch *b = new TestBatch(); // a throw is caught by TestGetNext()
  const int n = t->per;
  const int start = t->done * t->per;
  b->x.resize(n);
  b->xvalid.assign((n + 7) / 8 + 1, 0);
  b->offs.resize(n + 1);
  int nulls = 0;
  b->offs[0] = 0;
  for (int i = 0; i < n; i++) {
    const int r = start + i + 1;
    b->x[i] = r;
    if (r % 5 == 0) nulls++;
    else b->xvalid[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
    b->chars += "r" + std::to_string(r);
    b->offs[i + 1] = static_cast<int32_t>(b->chars.size());
  }
  b->xbuf[0] = b->xvalid.data();
  b->xbuf[1] = b->x.data();
  b->sbuf[0] = nullptr;
  b->sbuf[1] = b->offs.data();
  b->sbuf[2] = b->chars.data();
  b->topbuf[0] = nullptr;
  b->cx = ArrowArray{n, nulls, 0, 2, 0, b->xbuf, nullptr, nullptr,
                     TestChildRelease, nullptr};
  b->cs = ArrowArray{n, 0, 0, 3, 0, b->sbuf, nullptr, nullptr,
                     TestChildRelease, nullptr};
  b->kids[0] = &b->cx;
  b->kids[1] = &b->cs;
  const bool bad = t->mismatch && t->fail_at && t->done + 1 == t->fail_at;
  *out = ArrowArray{n, 0, 0, 1, bad ? 1 : 2, b->topbuf, b->kids, nullptr,
                    TestArrayRelease, b};
  t->done++;
  return 0;
}

// No exception may leave a callback the consumer calls from C.
int TestGetNext(ArrowArrayStream *st, ArrowArray *out) {
  try {
    return TestGetNextOrThrow(st, out);
  } catch (...) {
    std::memset(out, 0, sizeof *out);
    return ENOMEM;
  }
}

const char *TestLastError(ArrowArrayStream *st) {
  return static_cast<TestStream *>(st->private_data)->message;
}

void TestStreamRelease(ArrowArrayStream *st) {
  delete static_cast<TestStream *>(st->private_data);
  st->private_data = nullptr;
  st->release = nullptr;
  g_test_releases++;
}

// A second test producer: one batch given whole, as each column's format,
// name, length, null count and buffers (raw bytes, or none), with the
// batch's own validity. It builds what no R producer will: string and
// binary views, half floats, intervals, invalid UTF-8, a NUL, a null row,
// a batch whose layout is not its format's. The schema and the batch own
// copies of what they point to, and each release of the stream is counted
// with the other producer's.

struct RawColumn {
  std::string name, format;
  int64_t length = 0, null_count = 0;
  std::vector<std::vector<uint8_t>> bufs;
  std::vector<bool> present; // false: that buffer is NULL
};

struct RawStream {
  std::vector<RawColumn> cols;
  int64_t length = 0, null_count = 0;
  std::vector<uint8_t> validity;
  bool has_validity = false, done = false;
};

struct RawSchemaPriv {
  std::vector<std::string> names, formats;
  std::vector<ArrowSchema> kids;
  std::vector<ArrowSchema *> kidp;
};

void RawLeafRelease(ArrowSchema *s) { s->release = nullptr; }

void RawSchemaRelease(ArrowSchema *s) {
  delete static_cast<RawSchemaPriv *>(s->private_data);
  s->private_data = nullptr;
  s->release = nullptr;
}

int RawGetSchemaOrThrow(ArrowArrayStream *st, ArrowSchema *out) {
  const RawStream *r = static_cast<const RawStream *>(st->private_data);
  RawSchemaPriv *p = new RawSchemaPriv();
  const size_t n = r->cols.size();
  p->kids.resize(n);
  p->kidp.resize(n);
  for (size_t k = 0; k < n; k++) {
    p->names.push_back(r->cols[k].name);
    p->formats.push_back(r->cols[k].format);
  }
  for (size_t k = 0; k < n; k++) {
    ArrowSchema &c = p->kids[k];
    std::memset(&c, 0, sizeof c);
    c.format = p->formats[k].c_str();
    c.name = p->names[k].c_str();
    c.flags = ARROW_FLAG_NULLABLE;
    c.release = RawLeafRelease;
    p->kidp[k] = &c;
  }
  std::memset(out, 0, sizeof *out);
  out->format = "+s";
  out->name = "";
  out->n_children = static_cast<int64_t>(n);
  out->children = n ? p->kidp.data() : nullptr;
  out->release = RawSchemaRelease;
  out->private_data = p;
  return 0;
}

int RawGetSchema(ArrowArrayStream *st, ArrowSchema *out) {
  try {
    return RawGetSchemaOrThrow(st, out);
  } catch (...) {
    return ENOMEM;
  }
}

struct RawBatchPriv {
  std::vector<std::vector<uint8_t>> data; // every buffer, copied
  std::vector<std::vector<const void *>> bufp;
  std::vector<ArrowArray> kids;
  std::vector<ArrowArray *> kidp;
  std::vector<uint8_t> validity;
  const void *topbuf[1];
};

void RawChildRelease(ArrowArray *a) { a->release = nullptr; }

void RawBatchRelease(ArrowArray *a) {
  delete static_cast<RawBatchPriv *>(a->private_data);
  a->private_data = nullptr;
  a->release = nullptr;
}

int RawGetNextOrThrow(ArrowArrayStream *st, ArrowArray *out) {
  RawStream *r = static_cast<RawStream *>(st->private_data);
  std::memset(out, 0, sizeof *out);
  if (r->done) return 0; // the end
  RawBatchPriv *p = new RawBatchPriv();
  const size_t n = r->cols.size();
  p->bufp.resize(n);
  p->kids.resize(n);
  p->kidp.resize(n);
  for (size_t k = 0; k < n; k++) {
    for (const auto &b : r->cols[k].bufs) p->data.push_back(b);
  }
  size_t at = 0;
  for (size_t k = 0; k < n; k++) {
    const RawColumn &c = r->cols[k];
    for (size_t j = 0; j < c.bufs.size(); j++, at++) {
      // An empty buffer that is present still needs an address.
      if (p->data[at].empty()) p->data[at].push_back(0);
      p->bufp[k].push_back(c.present[j] ? p->data[at].data() : nullptr);
    }
    ArrowArray &a = p->kids[k];
    std::memset(&a, 0, sizeof a);
    a.length = c.length;
    a.null_count = c.null_count;
    a.n_buffers = static_cast<int64_t>(c.bufs.size());
    a.buffers = p->bufp[k].empty() ? nullptr : p->bufp[k].data();
    a.release = RawChildRelease;
    p->kidp[k] = &a;
  }
  p->validity = r->validity;
  p->topbuf[0] = r->has_validity ? p->validity.data() : nullptr;
  out->length = r->length;
  out->null_count = r->null_count;
  out->n_buffers = 1;
  out->buffers = p->topbuf;
  out->n_children = static_cast<int64_t>(n);
  out->children = n ? p->kidp.data() : nullptr;
  out->release = RawBatchRelease;
  out->private_data = p;
  r->done = true;
  return 0;
}

int RawGetNext(ArrowArrayStream *st, ArrowArray *out) {
  try {
    return RawGetNextOrThrow(st, out);
  } catch (...) {
    std::memset(out, 0, sizeof *out);
    return ENOMEM;
  }
}

const char *RawLastError(ArrowArrayStream *) { return ""; }

void RawStreamRelease(ArrowArrayStream *st) {
  delete static_cast<RawStream *>(st->private_data);
  st->private_data = nullptr;
  st->release = nullptr;
  g_test_releases++;
}

void TestXptrFinalizer(SEXP ptr) {
  ArrowArrayStream *st = static_cast<ArrowArrayStream *>(R_ExternalPtrAddr(ptr));
  if (!st) return;
  if (st->release) st->release(st);
  delete st;
  R_ClearExternalPtr(ptr);
}

} // namespace

extern "C" {

// zug_stream_take(stream): moves a nanoarrow_array_stream's stream into a
// holder of this package's, and returns the holder. The source is left
// released.
SEXP zug_stream_take(SEXP src) {
  zug::EngineError err;
  if (TYPEOF(src) != EXTPTRSXP || !R_ExternalPtrAddr(src)) {
    err.set("`value` is not an Arrow C stream.", "argument");
    return zug::ResultList(R_NilValue, err);
  }
  ArrowArrayStream *s = static_cast<ArrowArrayStream *>(R_ExternalPtrAddr(src));
  if (!s->release) {
    err.set(kReleased, "closed");
    return zug::ResultList(R_NilValue, err);
  }
  // Every R allocation before the move, so that an error leaves the
  // stream with its producer.
  SEXP ptr = PROTECT(R_MakeExternalPtr(nullptr, kHolderTag(), R_NilValue));
  R_RegisterCFinalizerEx(ptr, HolderFinalizer, TRUE);
  StreamHolder *h = static_cast<StreamHolder *>(std::calloc(1, sizeof(StreamHolder)));
  if (!h) {
    UNPROTECT(1);
    err.set("Out of memory for a stream holder.", "sql");
    return zug::ResultList(R_NilValue, err);
  }
  h->pid = ZUG_GETPID();
  R_SetExternalPtrAddr(ptr, h);
  std::memcpy(&h->stream, s, sizeof *s);
  s->release = nullptr;
  SEXP out = zug::ResultList(ptr, err);
  UNPROTECT(1);
  return out;
}

// zug_stream_release(holder): releases what the holder still holds. Safe
// to call again.
SEXP zug_stream_release(SEXP ptr) {
  StreamHolder *h = GetHolder(ptr);
  if (h && zug::SameProcess(h->pid)) ReleaseHolder(h);
  return R_NilValue;
}

// zug_stream_columns(holder): the stream's columns, as ColumnsOf().
SEXP zug_stream_columns(SEXP ptr) {
  zug::EngineError err;
  StreamHolder *h = GetHolder(ptr);
  if (!HolderUsable(h, &err) || !EnsureSchema(h, &err)) {
    return zug::ResultList(R_NilValue, err);
  }
  SEXP cols = PROTECT(ColumnsOf(&h->schema, &err));
  SEXP out = zug::ResultList(err.failed ? R_NilValue : cols, err);
  UNPROTECT(1);
  return out;
}

// zug_schema_columns(schema): a nanoarrow_schema's columns, as ColumnsOf().
SEXP zug_schema_columns(SEXP src) {
  zug::EngineError err;
  const ArrowSchema *s =
      TYPEOF(src) == EXTPTRSXP ? static_cast<const ArrowSchema *>(R_ExternalPtrAddr(src))
                               : nullptr;
  if (!s || !s->release) {
    err.set("`value` is not a valid Arrow schema.", "argument");
    return zug::ResultList(R_NilValue, err);
  }
  SEXP cols = PROTECT(ColumnsOf(s, &err));
  SEXP out = zug::ResultList(err.failed ? R_NilValue : cols, err);
  UNPROTECT(1);
  return out;
}

// zug_stream_append(con, holder, table, quoted, map): reads the whole
// stream into `table`, whose column k takes the stream's column map[k]
// (0-based; -1 for NULL), and releases the stream. Returns the rows
// appended. A failure part-way leaves the rows before it in `table`:
// R/arrow.R appends only to a table it made for the load.
SEXP zug_stream_append(SEXP conptr, SEXP holder, SEXP table, SEXP quoted,
                       SEXP map) {
  zug::EngineError err;
  zug::ZugConnection *c = zug::GetConnection(conptr);
  StreamHolder *h = GetHolder(holder);
  if (!zug::ConnectionOpen(c)) {
    err.set(zug::kClosedConnection, "closed");
    return zug::ResultList(R_NilValue, err);
  }
  if (TYPEOF(table) != STRSXP || XLENGTH(table) != 1 ||
      TYPEOF(quoted) != STRSXP || XLENGTH(quoted) != 1 ||
      TYPEOF(map) != INTSXP || XLENGTH(map) > 100000) {
    err.set("zug_stream_append: malformed arguments", "argument");
    return zug::ResultList(R_NilValue, err);
  }
  if (!HolderUsable(h, &err) || !EnsureSchema(h, &err)) {
    return zug::ResultList(R_NilValue, err);
  }
  Plan plan;
  plan.table = CHAR(STRING_ELT(table, 0));
  plan.ncol = static_cast<int>(XLENGTH(map));
  plan.nchild = h->schema.n_children;
  plan.cols = reinterpret_cast<PlanColumn *>(
      R_alloc(plan.ncol ? plan.ncol : 1, sizeof(PlanColumn)));
  if (std::strcmp(h->schema.format ? h->schema.format : "", "+s") != 0 ||
      (plan.nchild > 0 && !h->schema.children)) {
    err.set("The stream's schema is not a struct of columns, as a table's is.",
            "stream");
    return zug::ResultList(R_NilValue, err);
  }
  for (int k = 0; k < plan.ncol; k++) {
    PlanColumn &pc = plan.cols[k];
    pc = PlanColumn();
    pc.child = INTEGER(map)[k];
    if (pc.child == NA_INTEGER || pc.child < -1 || pc.child >= plan.nchild) {
      err.set("zug_stream_append: a column index is out of range", "argument");
      return zug::ResultList(R_NilValue, err);
    }
    if (pc.child < 0) {
      pc.name = "(not in the stream)";
      continue;
    }
    const ArrowSchema *ch = h->schema.children[pc.child];
    pc.name = ch && ch->name ? ch->name : "";
    const char *what;
    pc.kind = ch ? KindOf(ch, &pc.width, &what) : kUnsupported;
    if (pc.kind == kUnsupported) {
      char msg[1024];
      std::snprintf(msg, sizeof msg,
                    "Column `%s` is %s, which zugodb cannot hold.", pc.name,
                    ch ? what : "missing");
      err.set(msg, "type");
      return zug::ResultList(R_NilValue, err);
    }
  }
  EnginePlan(c, CHAR(STRING_ELT(quoted, 0)), &plan, &err);
  if (err.failed) return zug::ResultList(R_NilValue, err);

  double rows = 0;
  for (;;) {
    R_CheckUserInterrupt(); // no C++ object live; the holder owns the rest
    std::memset(&h->batch, 0, sizeof h->batch);
    const int rc = h->stream.get_next(&h->stream, &h->batch);
    if (rc != 0) {
      h->batch.release = nullptr;
      ProducerError(&h->stream, rc, "to give a batch", &err);
      break;
    }
    if (!h->batch.release) break; // the end of the stream
    EngineAppendBatch(c, plan, &h->batch, &rows, &err);
    ReleaseBatch(h);
    if (err.failed) break;
  }
  ReleaseHolder(h);
  return zug::ResultList(err.failed ? R_NilValue : Rf_ScalarReal(rows), err);
}

// zug_test_stream(nbatch, per, fail_at, message, mismatch): the tests'
// producer, above, as a nanoarrow_array_stream.
SEXP zug_test_stream(SEXP nbatch, SEXP per, SEXP fail_at, SEXP message,
                     SEXP mismatch) {
  // Every R call first; then the allocations, which cannot longjmp.
  const int nb = Rf_asInteger(nbatch), n = Rf_asInteger(per),
            fail = Rf_asInteger(fail_at);
  const bool bad = Rf_asLogical(mismatch) == TRUE;
  const char *msg = Rf_translateCharUTF8(STRING_ELT(message, 0));
  SEXP ptr = PROTECT(R_MakeExternalPtr(nullptr, R_NilValue, R_NilValue));
  R_RegisterCFinalizerEx(ptr, TestXptrFinalizer, TRUE);
  SEXP cls = PROTECT(Rf_mkString("nanoarrow_array_stream"));
  Rf_setAttrib(ptr, R_ClassSymbol, cls);
  ArrowArrayStream *st = new (std::nothrow) ArrowArrayStream();
  TestStream *t = new (std::nothrow) TestStream();
  if (!st || !t) {
    delete st;
    delete t;
    UNPROTECT(2);
    return R_NilValue;
  }
  t->nbatch = nb;
  t->per = n;
  t->fail_at = fail;
  t->mismatch = bad;
  t->done = 0;
  std::snprintf(t->message, sizeof t->message, "%s", msg);
  st->get_schema = TestGetSchema;
  st->get_next = TestGetNext;
  st->get_last_error = TestLastError;
  st->release = TestStreamRelease;
  st->private_data = t;
  R_SetExternalPtrAddr(ptr, st);
  UNPROTECT(2);
  return ptr;
}

// zug_test_raw_stream(cols, length, null_count, validity): the second test
// producer, above, as a nanoarrow_array_stream. `cols` is a list of
// list(name, format, length, null_count, buffers), each buffer a raw
// vector or NULL; `validity` is the batch's own, a raw vector or NULL.
// Returns NULL if the arguments are not of that shape.
SEXP zug_test_raw_stream(SEXP cols, SEXP length, SEXP null_count,
                         SEXP validity) {
  // Every R read first, into plain values; then the C++ copies, inside
  // try/catch, with no R call.
  struct Buf {
    const uint8_t *p;
    R_xlen_t n;
    bool present;
  };
  struct Col {
    const char *name, *format;
    double length, null_count;
    Buf *bufs;
    R_xlen_t nbuf;
  };
  if (TYPEOF(cols) != VECSXP || TYPEOF(validity) == VECSXP) return R_NilValue;
  const R_xlen_t n = XLENGTH(cols);
  Col *cs = reinterpret_cast<Col *>(R_alloc(n ? n : 1, sizeof(Col)));
  for (R_xlen_t k = 0; k < n; k++) {
    SEXP c = VECTOR_ELT(cols, k);
    if (TYPEOF(c) != VECSXP || XLENGTH(c) != 5) return R_NilValue;
    SEXP nm = VECTOR_ELT(c, 0), fm = VECTOR_ELT(c, 1), bs = VECTOR_ELT(c, 4);
    if (TYPEOF(nm) != STRSXP || TYPEOF(fm) != STRSXP || TYPEOF(bs) != VECSXP) {
      return R_NilValue;
    }
    cs[k].name = Rf_translateCharUTF8(STRING_ELT(nm, 0));
    cs[k].format = CHAR(STRING_ELT(fm, 0));
    cs[k].length = Rf_asReal(VECTOR_ELT(c, 2));
    cs[k].null_count = Rf_asReal(VECTOR_ELT(c, 3));
    cs[k].nbuf = XLENGTH(bs);
    cs[k].bufs = reinterpret_cast<Buf *>(R_alloc(cs[k].nbuf ? cs[k].nbuf : 1, sizeof(Buf)));
    for (R_xlen_t j = 0; j < cs[k].nbuf; j++) {
      SEXP b = VECTOR_ELT(bs, j);
      if (b != R_NilValue && TYPEOF(b) != RAWSXP) return R_NilValue;
      cs[k].bufs[j].present = b != R_NilValue;
      cs[k].bufs[j].n = b == R_NilValue ? 0 : XLENGTH(b);
      cs[k].bufs[j].p = cs[k].bufs[j].n ? RAW(b) : nullptr;
    }
  }
  const double len = Rf_asReal(length), nulls = Rf_asReal(null_count);
  const bool has_valid = validity != R_NilValue && TYPEOF(validity) == RAWSXP;
  const uint8_t *vp = has_valid && XLENGTH(validity) ? RAW(validity) : nullptr;
  const R_xlen_t vn = has_valid ? XLENGTH(validity) : 0;
  SEXP ptr = PROTECT(R_MakeExternalPtr(nullptr, R_NilValue, R_NilValue));
  R_RegisterCFinalizerEx(ptr, TestXptrFinalizer, TRUE);
  SEXP cls = PROTECT(Rf_mkString("nanoarrow_array_stream"));
  Rf_setAttrib(ptr, R_ClassSymbol, cls);
  ArrowArrayStream *st = nullptr;
  try {
    RawStream *r = new RawStream();
    r->length = static_cast<int64_t>(len);
    r->null_count = static_cast<int64_t>(nulls);
    r->has_validity = has_valid;
    if (vp) r->validity.assign(vp, vp + vn);
    if (has_valid && r->validity.empty()) r->validity.push_back(0);
    for (R_xlen_t k = 0; k < n; k++) {
      RawColumn c;
      c.name = cs[k].name;
      c.format = cs[k].format;
      c.length = static_cast<int64_t>(cs[k].length);
      c.null_count = static_cast<int64_t>(cs[k].null_count);
      for (R_xlen_t j = 0; j < cs[k].nbuf; j++) {
        const Buf &b = cs[k].bufs[j];
        c.bufs.emplace_back(b.p, b.p + b.n);
        c.present.push_back(b.present);
      }
      r->cols.push_back(std::move(c));
    }
    st = new ArrowArrayStream();
    st->get_schema = RawGetSchema;
    st->get_next = RawGetNext;
    st->get_last_error = RawLastError;
    st->release = RawStreamRelease;
    st->private_data = r;
  } catch (...) {
    st = nullptr; // the test sees NULL; a leak only on out of memory
  }
  if (!st) {
    UNPROTECT(2);
    return R_NilValue;
  }
  R_SetExternalPtrAddr(ptr, st);
  UNPROTECT(2);
  return ptr;
}

// zug_test_stream_releases(): how many of the tests' streams were released.
SEXP zug_test_stream_releases(void) { return Rf_ScalarInteger(g_test_releases); }

} // extern "C"
