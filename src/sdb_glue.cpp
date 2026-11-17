// zuslothdb's glue over SlothDB's C++ API (design §4, D2).
//
// The rule of §4.3: no C++ exception crosses into R, and no R API call that
// can longjmp runs while a SlothDB object with a destructor is live on the
// stack. So every entry point does its engine work in an `Engine*` helper,
// inside try/catch, with every C++ object scoped to that helper; the helper
// returns plain values and an error string, and only then, with the C++
// frames gone, does the entry point build R objects. Errors are returned to
// R, never raised here: the R side turns them into zuslothdb_error
// conditions. Nothing here calls Rf_error().

#include "slothdb/common/parallel.hpp"
#include "slothdb/main/connection.hpp"
#include "slothdb/main/database.hpp"

#include <cstdint>
#include <cstring>
#include <exception>
#include <string>

#include "sdb_provenance.h"

#define R_NO_REMAP
#include <R.h>
#include <R_ext/Rdynload.h>
#include <R_ext/Visibility.h>
#include <Rinternals.h>

namespace {

// A fixed-size message buffer, so an error string outlives the C++ frame
// that produced it without a destructor of its own on the R side.
struct EngineError {
  bool failed = false;
  // "sql" for the engine's own errors, "type" for a value R cannot take;
  // sdb_abort() in R maps each to its condition class.
  const char *kind = "sql";
  char message[2048] = {0};

  void set(const char *msg, const char *k = "sql") {
    failed = true;
    kind = k;
    std::strncpy(message, msg ? msg : "unknown error", sizeof(message) - 1);
    message[sizeof(message) - 1] = '\0';
  }
};

enum class ScalarKind { kNull, kInt, kTooWide };

struct ScalarInt {
  ScalarKind kind = ScalarKind::kNull;
  int value = 0;
};

// Opens an in-memory database, runs `sql`, and reads the first column of
// the first row as an integer. The spike's one query path (roadmap Stage 1);
// Stage 2 replaces it with database and connection objects.
void EngineScalarInt(const char *sql, ScalarInt *out, EngineError *err) {
  try {
    slothdb::Database db;
    slothdb::Connection con(db);
    slothdb::QueryResult res = con.Query(sql);
    if (res.RowCount() == 0 || res.ColumnCount() == 0) {
      err->set("the query returned no value");
      return;
    }
    const slothdb::Value &v = res.GetValue(0, 0);
    if (v.IsNull()) {
      out->kind = ScalarKind::kNull;
      return;
    }
    int64_t x;
    switch (v.type().id()) {
    case slothdb::LogicalTypeId::INTEGER:
      x = v.GetValue<int32_t>();
      break;
    case slothdb::LogicalTypeId::BIGINT:
      x = v.GetValue<int64_t>();
      break;
    default:
      err->set(("the value is not an integer but " + v.type().ToString())
                   .c_str(),
               "type");
      return;
    }
    // INT_MIN is R's NA_integer_, so it does not fit either.
    if (x <= INT32_MIN || x > INT32_MAX) {
      out->kind = ScalarKind::kTooWide;
      return;
    }
    out->kind = ScalarKind::kInt;
    out->value = static_cast<int>(x);
  } catch (const std::exception &e) {
    err->set(e.what());
  } catch (...) {
    err->set("unknown C++ exception");
  }
}

// list(value = <integer or NULL>, error = <string or NULL>, kind = <string>)
SEXP ResultList(SEXP value, const EngineError &err, const char *kind) {
  const char *names[] = {"value", "error", "kind", ""};
  SEXP out = PROTECT(Rf_mkNamed(VECSXP, names));
  SET_VECTOR_ELT(out, 0, value);
  if (err.failed) {
    SET_VECTOR_ELT(out, 1, Rf_mkString(err.message));
  }
  SET_VECTOR_ELT(out, 2, Rf_mkString(kind));
  UNPROTECT(1);
  return out;
}

} // namespace

extern "C" {

SEXP sdb_scalar_int(SEXP sql) {
  if (TYPEOF(sql) != STRSXP || XLENGTH(sql) != 1 ||
      STRING_ELT(sql, 0) == NA_STRING) {
    EngineError err;
    err.set("sql must be a single string");
    return ResultList(R_NilValue, err, "argument");
  }
  // Translated to UTF-8 before any C++ object exists: Rf_translateCharUTF8
  // allocates and can longjmp.
  const char *text = Rf_translateCharUTF8(STRING_ELT(sql, 0));
  ScalarInt v;
  EngineError err;
  EngineScalarInt(text, &v, &err);
  if (err.failed) return ResultList(R_NilValue, err, err.kind);
  switch (v.kind) {
  case ScalarKind::kNull:
    return ResultList(Rf_ScalarInteger(NA_INTEGER), err, "ok");
  case ScalarKind::kTooWide:
    err.set("the value does not fit an R integer");
    return ResultList(R_NilValue, err, "type");
  case ScalarKind::kInt:
    break;
  }
  return ResultList(Rf_ScalarInteger(v.value), err, "ok");
}

SEXP sdb_info(void) {
  static const char *patches[] = SDB_PATCHES;
  const int n = SDB_PATCH_COUNT;
  const unsigned int threads = slothdb::ThreadCap();
  const char *names[] = {"engine", "repository", "commit", "describe",
                         "patches", "threads", ""};
  SEXP out = PROTECT(Rf_mkNamed(VECSXP, names));
  SET_VECTOR_ELT(out, 0, Rf_mkString("SlothDB"));
  SET_VECTOR_ELT(out, 1, Rf_mkString(SDB_UPSTREAM_REPOSITORY));
  SET_VECTOR_ELT(out, 2, Rf_mkString(SDB_UPSTREAM_COMMIT));
  SET_VECTOR_ELT(out, 3, Rf_mkString(SDB_UPSTREAM_DESCRIBE));
  SEXP p = PROTECT(Rf_allocVector(STRSXP, n));
  for (int i = 0; i < n; i++) SET_STRING_ELT(p, i, Rf_mkChar(patches[i]));
  SET_VECTOR_ELT(out, 4, p);
  SET_VECTOR_ELT(out, 5, Rf_ScalarInteger(static_cast<int>(threads)));
  UNPROTECT(2);
  return out;
}

static const R_CallMethodDef call_methods[] = {
    {"sdb_scalar_int", (DL_FUNC)&sdb_scalar_int, 1},
    {"sdb_info", (DL_FUNC)&sdb_info, 0},
    {NULL, NULL, 0}};

void attribute_visible R_init_zuslothdb(DllInfo *dll) {
  R_registerRoutines(dll, NULL, call_methods, NULL, NULL);
  R_useDynamicSymbols(dll, FALSE);
  R_forceSymbols(dll, TRUE);
}

} // extern "C"
