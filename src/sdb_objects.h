// The glue's long-lived objects, shared by sdb_database.cpp and
// sdb_result.cpp. Ownership is described in sdb_database.cpp.
#pragma once

#include "sdb_common.h"

#include <memory>
#include <vector>

namespace sdb {

struct SdbConnection;

struct SdbDatabase {
  std::unique_ptr<slothdb::Database> db;
  std::vector<SdbConnection *> conns;
  long pid = SDB_GETPID();
};

struct SdbConnection {
  std::unique_ptr<slothdb::Connection> con;
  SdbDatabase *owner = nullptr;
  long pid = SDB_GETPID();
};

inline bool SameProcess(long pid) { return pid == SDB_GETPID(); }

inline SdbConnection *GetConnection(SEXP ptr) {
  if (TYPEOF(ptr) != EXTPTRSXP) return nullptr;
  return static_cast<SdbConnection *>(R_ExternalPtrAddr(ptr));
}

inline bool ConnectionOpen(SdbConnection *c) {
  return c && SameProcess(c->pid) && c->con && c->owner && c->owner->db;
}

// Whether the last statement of `sql` is INSERT, UPDATE or DELETE: only then
// is a one-row "Count" result the rows affected, not a query's answer
// (`SELECT 5 AS Count`). Throws what the parser throws.
bool LastIsDml(const std::string &sql);

extern const char *kClosedConnection;

} // namespace sdb
