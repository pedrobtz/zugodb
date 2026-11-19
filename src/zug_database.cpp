// Databases and connections behind external pointers (design §4.3, Q9, Q13).
//
// Ownership:
// - An ZugDatabase owns a zugodb::Database and knows its open connections.
//   Its external pointer is held by the driver (R/driver.R).
// - An ZugConnection owns a zugodb::Connection, which refers to the
//   Database by reference. Its external pointer's `prot` slot is the
//   database's external pointer, so the ZugDatabase struct outlives every
//   connection to it: `owner` stays a valid pointer for as long as the
//   connection's pointer exists.
// - Closing a database (CloseDatabase) first destroys each of its
//   connections' zugodb::Connection, then the zugodb::Database, which
//   writes a file-backed database to disk. The structs stay, marked
//   closed, so a later call reports zug_closed_error.
//
// Fork guard (Q13): each struct records the process that made it. In any
// other process (a parallel::mcparallel() child) every entry point reports
// "closed", and the finalizers do nothing: a child neither uses the
// parent's engine, whose threads and file did not survive fork(), nor
// writes the parent's database file when it exits.
//
// Every engine call is inside try/catch, with no R API call while a C++
// object with a destructor is live in the same frame (design §4.3).

#include "zug_objects.h"

#include "slothdb/parser/parser.hpp"

#include <algorithm>
#include <new>

namespace zug {

bool LastIsDml(const std::string &sql) {
  auto statements = zugodb::Parser::Parse(sql);
  if (statements.empty()) return false;
  switch (statements.back()->GetType()) {
  case zugodb::StatementType::INSERT:
  case zugodb::StatementType::UPDATE:
  case zugodb::StatementType::DELETE_STMT:
    return true;
  default:
    return false;
  }
}

const char *kClosedConnection =
    "The connection is closed (disconnected, its database shut down, or "
    "used from another process).";

} // namespace zug

namespace {

using zug::SameProcess;
using zug::ZugConnection;
using zug::ZugDatabase;

ZugDatabase *GetDatabase(SEXP ptr) {
  if (TYPEOF(ptr) != EXTPTRSXP) return nullptr;
  return static_cast<ZugDatabase *>(R_ExternalPtrAddr(ptr));
}

using zug::ConnectionOpen;
using zug::GetConnection;

// Destroys the database's connections, then the database (which saves a
// file-backed one). No R API call in here.
void CloseDatabase(ZugDatabase *d) {
  try {
    for (ZugConnection *c : d->conns) {
      c->con.reset();
      c->owner = nullptr;
    }
    d->conns.clear();
    d->db.reset();
  } catch (...) {
    // ~Database() catches its own errors; nothing else here throws.
    d->conns.clear();
  }
}

void CloseConnection(ZugConnection *c) {
  try {
    c->con.reset();
  } catch (...) {
  }
  if (c->owner) {
    auto &v = c->owner->conns;
    v.erase(std::remove(v.begin(), v.end(), c), v.end());
  }
}

void DatabaseFinalizer(SEXP ptr) {
  ZugDatabase *d = GetDatabase(ptr);
  if (!d) return;
  R_ClearExternalPtr(ptr);
  if (!SameProcess(d->pid)) return; // a forked child: leave it alone
  CloseDatabase(d);
  delete d;
}

void ConnectionFinalizer(SEXP ptr) {
  ZugConnection *c = GetConnection(ptr);
  if (!c) return;
  R_ClearExternalPtr(ptr);
  if (!SameProcess(c->pid)) return;
  CloseConnection(c);
  delete c;
}

void EngineOpen(const char *path, bool read_only, ZugDatabase *d,
                zug::EngineError *err) {
  try {
    zugodb::DatabaseConfig config;
    config.path = path;
    config.read_only = read_only;
    d->db = std::make_unique<zugodb::Database>(std::move(config));
  } catch (const std::exception &e) {
    err->set(e.what());
  } catch (...) {
    err->set("unknown C++ exception while opening the database");
  }
}

void EngineConnect(ZugDatabase *d, ZugConnection *c, zug::EngineError *err) {
  try {
    c->con = std::make_unique<zugodb::Connection>(*d->db);
    c->owner = d;
    d->conns.push_back(c);
  } catch (const std::exception &e) {
    err->set(e.what());
  } catch (...) {
    err->set("unknown C++ exception while connecting");
  }
}

// Runs `sql`; for INSERT, UPDATE and DELETE the engine returns one row,
// "Count", with the rows affected. Anything else affected none.
void EngineExecute(ZugConnection *c, const char *sql, double *affected,
                   zug::EngineError *err) {
  try {
    // Parsed first: a "Count" row means rows affected only after DML.
    const bool dml = zug::LastIsDml(sql);
    zugodb::QueryResult res = c->con->Query(sql);
    *affected = 0;
    if (dml && res.ColumnCount() == 1 && res.RowCount() == 1 &&
        res.column_names[0] == "Count") {
      const zugodb::Value &v = res.GetValue(0, 0);
      if (!v.IsNull() && v.type().id() == zugodb::LogicalTypeId::BIGINT) {
        *affected = static_cast<double>(v.GetValue<int64_t>());
      }
    }
  } catch (const std::exception &e) {
    err->set(e.what());
  } catch (...) {
    err->set("unknown C++ exception");
  }
}

} // namespace

extern "C" {

// zug_db_open(path, read_only): a new database. "" is in memory.
SEXP zug_db_open(SEXP path, SEXP read_only) {
  zug::EngineError err;
  if (TYPEOF(path) != STRSXP || XLENGTH(path) != 1 ||
      STRING_ELT(path, 0) == NA_STRING || TYPEOF(read_only) != LGLSXP ||
      XLENGTH(read_only) != 1 || LOGICAL(read_only)[0] == NA_LOGICAL) {
    err.set("path must be a string and read_only TRUE or FALSE", "argument");
    return zug::ResultList(R_NilValue, err);
  }
  // Allocates (and can longjmp) before any C++ object exists.
  const char *p = Rf_translateCharUTF8(STRING_ELT(path, 0));
  const bool ro = LOGICAL(read_only)[0];
  // The pointer exists before the struct, so no allocation of R's can
  // longjmp past an unowned struct; nothrow, so no exception leaves here.
  SEXP ptr = PROTECT(R_MakeExternalPtr(nullptr, R_NilValue, R_NilValue));
  R_RegisterCFinalizerEx(ptr, DatabaseFinalizer, TRUE);
  ZugDatabase *d = new (std::nothrow) ZugDatabase();
  if (!d) {
    err.set("out of memory");
    UNPROTECT(1);
    return zug::ResultList(R_NilValue, err);
  }
  R_SetExternalPtrAddr(ptr, d);
  EngineOpen(p, ro, d, &err);
  SEXP out = zug::ResultList(err.failed ? R_NilValue : ptr, err);
  UNPROTECT(1);
  return out;
}

// zug_db_close(db): closes the database and every connection to it.
SEXP zug_db_close(SEXP ptr) {
  ZugDatabase *d = GetDatabase(ptr);
  if (d && SameProcess(d->pid)) CloseDatabase(d);
  return R_NilValue;
}

SEXP zug_db_is_open(SEXP ptr) {
  ZugDatabase *d = GetDatabase(ptr);
  return Rf_ScalarLogical(d && SameProcess(d->pid) && d->db != nullptr);
}

SEXP zug_db_connections(SEXP ptr) {
  ZugDatabase *d = GetDatabase(ptr);
  int n = (d && SameProcess(d->pid)) ? static_cast<int>(d->conns.size()) : 0;
  return Rf_ScalarInteger(n);
}

// zug_connect(db): a connection to an open database.
SEXP zug_connect(SEXP dbptr) {
  zug::EngineError err;
  ZugDatabase *d = GetDatabase(dbptr);
  if (!d || !SameProcess(d->pid) || !d->db) {
    err.set("The database is closed.", "closed");
    return zug::ResultList(R_NilValue, err);
  }
  // prot = the database's pointer: the ZugDatabase outlives the connection.
  SEXP ptr = PROTECT(R_MakeExternalPtr(nullptr, R_NilValue, dbptr));
  R_RegisterCFinalizerEx(ptr, ConnectionFinalizer, TRUE);
  ZugConnection *c = new (std::nothrow) ZugConnection();
  if (!c) {
    err.set("out of memory");
    UNPROTECT(1);
    return zug::ResultList(R_NilValue, err);
  }
  R_SetExternalPtrAddr(ptr, c);
  EngineConnect(d, c, &err);
  SEXP out = zug::ResultList(err.failed ? R_NilValue : ptr, err);
  UNPROTECT(1);
  return out;
}

SEXP zug_conn_is_open(SEXP ptr) {
  return Rf_ScalarLogical(ConnectionOpen(GetConnection(ptr)));
}

// zug_disconnect(con): closes the connection; returns the number of
// connections its database still has open, or NA when it had no database.
SEXP zug_disconnect(SEXP ptr) {
  ZugConnection *c = GetConnection(ptr);
  if (!c || !SameProcess(c->pid)) return Rf_ScalarInteger(NA_INTEGER);
  ZugDatabase *owner = c->owner;
  CloseConnection(c);
  c->owner = nullptr;
  if (!owner) return Rf_ScalarInteger(NA_INTEGER);
  return Rf_ScalarInteger(static_cast<int>(owner->conns.size()));
}

// zug_execute(con, sql): rows affected, as a double.
SEXP zug_execute(SEXP ptr, SEXP sql) {
  zug::EngineError err;
  ZugConnection *c = GetConnection(ptr);
  if (!ConnectionOpen(c)) {
    err.set(zug::kClosedConnection, "closed");
    return zug::ResultList(R_NilValue, err);
  }
  if (TYPEOF(sql) != STRSXP || XLENGTH(sql) != 1 ||
      STRING_ELT(sql, 0) == NA_STRING) {
    err.set("statement must be a single string", "argument");
    return zug::ResultList(R_NilValue, err);
  }
  const char *text = Rf_translateCharUTF8(STRING_ELT(sql, 0));
  double affected = 0;
  EngineExecute(c, text, &affected, &err);
  return zug::ResultList(err.failed ? R_NilValue : Rf_ScalarReal(affected),
                         err);
}

// zug_set_threads(n): the engine's process-wide thread cap (patch 0004).
// The global pool is rebuilt at its next use, sized by HWThreads(): the
// smaller of the cap and the hardware's threads. (Its count is passed as 0
// for that reason: a count of n would start n threads, however large.)
SEXP zug_set_threads(SEXP n) {
  zug::EngineError err;
  if (TYPEOF(n) != INTSXP || XLENGTH(n) != 1 || INTEGER(n)[0] == NA_INTEGER ||
      INTEGER(n)[0] < 1) {
    err.set("threads must be a positive integer", "argument");
    return zug::ResultList(R_NilValue, err);
  }
  const unsigned int v = static_cast<unsigned int>(INTEGER(n)[0]);
  try {
    zugodb::SetThreadCap(v);
    zugodb::ThreadPool::SetGlobalThreadCount(0);
  } catch (const std::exception &e) {
    err.set(e.what());
  }
  return zug::ResultList(err.failed ? R_NilValue : Rf_ScalarInteger(INTEGER(n)[0]),
                         err);
}

} // extern "C"
