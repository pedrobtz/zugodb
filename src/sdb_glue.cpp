// zuslothdb's glue over SlothDB's C++ API (design §4, D2): registration,
// zuslothdb_info(), and the Stage 1 spike's one-query path.
//
// The rule of §4.3: no C++ exception crosses into R, and no R API call that
// can longjmp runs while a SlothDB object with a destructor is live on the
// stack. So every entry point does its engine work in an `Engine*` helper,
// inside try/catch, with every C++ object scoped to that helper; the helper
// returns plain values and an sdb::EngineError, and only then, with the C++
// frames gone, does the entry point build R objects. Errors are returned to
// R, never raised here: sdb_abort() in R turns them into zuslothdb_error
// conditions. Nothing in the glue calls Rf_error().

#include "sdb_common.h"

#include <cstdint>

#include "sdb_provenance.h"

namespace {

enum class ScalarKind { kNull, kInt, kTooWide };

struct ScalarInt {
  ScalarKind kind = ScalarKind::kNull;
  int value = 0;
};

// Opens an in-memory database, runs `sql`, and reads the first column of
// the first row as an integer.
void EngineScalarInt(const char *sql, ScalarInt *out, sdb::EngineError *err) {
  try {
    zugodb::Database db;
    zugodb::Connection con(db);
    zugodb::QueryResult res = con.Query(sql);
    if (res.RowCount() == 0 || res.ColumnCount() == 0) {
      err->set("the query returned no value");
      return;
    }
    const zugodb::Value &v = res.GetValue(0, 0);
    if (v.IsNull()) {
      out->kind = ScalarKind::kNull;
      return;
    }
    int64_t x;
    switch (v.type().id()) {
    case zugodb::LogicalTypeId::INTEGER:
      x = v.GetValue<int32_t>();
      break;
    case zugodb::LogicalTypeId::BIGINT:
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

} // namespace

extern "C" {

SEXP sdb_scalar_int(SEXP sql) {
  sdb::EngineError err;
  if (TYPEOF(sql) != STRSXP || XLENGTH(sql) != 1 ||
      STRING_ELT(sql, 0) == NA_STRING) {
    err.set("sql must be a single string", "argument");
    return sdb::ResultList(R_NilValue, err);
  }
  // Translated to UTF-8 before any C++ object exists: Rf_translateCharUTF8
  // allocates and can longjmp.
  const char *text = Rf_translateCharUTF8(STRING_ELT(sql, 0));
  ScalarInt v;
  EngineScalarInt(text, &v, &err);
  if (err.failed) return sdb::ResultList(R_NilValue, err);
  switch (v.kind) {
  case ScalarKind::kNull:
    return sdb::ResultList(Rf_ScalarInteger(NA_INTEGER), err);
  case ScalarKind::kTooWide:
    err.set("the value does not fit an R integer", "type");
    return sdb::ResultList(R_NilValue, err);
  case ScalarKind::kInt:
    break;
  }
  return sdb::ResultList(Rf_ScalarInteger(v.value), err);
}

// sdb_info(): the source the engine was compiled from (src/sdb_provenance.h,
// from tools/vendor.yaml): libzugodb's repository, commit and tag, and the
// SlothDB commit it descends from.
SEXP sdb_info(void) {
  const unsigned int threads = zugodb::ThreadCap();
  const char *names[] = {"engine",   "repository", "commit",        "tag",
                         "origin",   "origin_commit", "describe",   "threads",
                         ""};
  SEXP out = PROTECT(Rf_mkNamed(VECSXP, names));
  SET_VECTOR_ELT(out, 0, Rf_mkString("libzugodb"));
  SET_VECTOR_ELT(out, 1, Rf_mkString(SDB_SOURCE_REPOSITORY));
  SET_VECTOR_ELT(out, 2, Rf_mkString(SDB_SOURCE_COMMIT));
  SET_VECTOR_ELT(out, 3, Rf_mkString(SDB_SOURCE_TAG));
  SET_VECTOR_ELT(out, 4, Rf_mkString(SDB_ORIGIN_REPOSITORY));
  SET_VECTOR_ELT(out, 5, Rf_mkString(SDB_ORIGIN_COMMIT));
  SET_VECTOR_ELT(out, 6, Rf_mkString(SDB_ORIGIN_DESCRIBE));
  SET_VECTOR_ELT(out, 7, Rf_ScalarInteger(static_cast<int>(threads)));
  UNPROTECT(1);
  return out;
}

// sdb_database.cpp
SEXP sdb_db_open(SEXP path, SEXP read_only);
SEXP sdb_db_close(SEXP ptr);
SEXP sdb_db_is_open(SEXP ptr);
SEXP sdb_db_connections(SEXP ptr);
SEXP sdb_connect(SEXP dbptr);
SEXP sdb_conn_is_open(SEXP ptr);
SEXP sdb_disconnect(SEXP ptr);
SEXP sdb_execute(SEXP ptr, SEXP sql);
SEXP sdb_set_threads(SEXP n);

// sdb_result.cpp
SEXP sdb_send(SEXP conptr, SEXP sql);
SEXP sdb_result_info(SEXP ptr);
SEXP sdb_fetch(SEXP ptr, SEXP n, SEXP bigint, SEXP tz);
SEXP sdb_append(SEXP conptr, SEXP table, SEXP quoted, SEXP cols, SEXP payloads,
                SEXP names, SEXP check_only);
SEXP sdb_result_clear(SEXP ptr);

static const R_CallMethodDef call_methods[] = {
    {"sdb_scalar_int", (DL_FUNC)&sdb_scalar_int, 1},
    {"sdb_info", (DL_FUNC)&sdb_info, 0},
    {"sdb_db_open", (DL_FUNC)&sdb_db_open, 2},
    {"sdb_db_close", (DL_FUNC)&sdb_db_close, 1},
    {"sdb_db_is_open", (DL_FUNC)&sdb_db_is_open, 1},
    {"sdb_db_connections", (DL_FUNC)&sdb_db_connections, 1},
    {"sdb_connect", (DL_FUNC)&sdb_connect, 1},
    {"sdb_conn_is_open", (DL_FUNC)&sdb_conn_is_open, 1},
    {"sdb_disconnect", (DL_FUNC)&sdb_disconnect, 1},
    {"sdb_execute", (DL_FUNC)&sdb_execute, 2},
    {"sdb_set_threads", (DL_FUNC)&sdb_set_threads, 1},
    {"sdb_send", (DL_FUNC)&sdb_send, 2},
    {"sdb_result_info", (DL_FUNC)&sdb_result_info, 1},
    {"sdb_fetch", (DL_FUNC)&sdb_fetch, 4},
    {"sdb_append", (DL_FUNC)&sdb_append, 7},
    {"sdb_result_clear", (DL_FUNC)&sdb_result_clear, 1},
    {NULL, NULL, 0}};

void attribute_visible R_init_zuslothdb(DllInfo *dll) {
  R_registerRoutines(dll, NULL, call_methods, NULL, NULL);
  R_useDynamicSymbols(dll, FALSE);
  R_forceSymbols(dll, TRUE);
}

} // extern "C"
