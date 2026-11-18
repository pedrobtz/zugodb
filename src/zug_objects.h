// The glue's long-lived objects, shared by zug_database.cpp and
// zug_result.cpp. Ownership is described in zug_database.cpp.
#pragma once

#include "zug_common.h"

#include <memory>
#include <vector>

namespace zug {

struct ZugConnection;

struct ZugDatabase {
  std::unique_ptr<zugodb::Database> db;
  std::vector<ZugConnection *> conns;
  long pid = ZUG_GETPID();
};

struct ZugConnection {
  std::unique_ptr<zugodb::Connection> con;
  ZugDatabase *owner = nullptr;
  long pid = ZUG_GETPID();
};

inline bool SameProcess(long pid) { return pid == ZUG_GETPID(); }

inline ZugConnection *GetConnection(SEXP ptr) {
  if (TYPEOF(ptr) != EXTPTRSXP) return nullptr;
  return static_cast<ZugConnection *>(R_ExternalPtrAddr(ptr));
}

inline bool ConnectionOpen(ZugConnection *c) {
  return c && SameProcess(c->pid) && c->con && c->owner && c->owner->db;
}

// Whether the last statement of `sql` is INSERT, UPDATE or DELETE: only then
// is a one-row "Count" result the rows affected, not a query's answer
// (`SELECT 5 AS Count`). Throws what the parser throws.
bool LastIsDml(const std::string &sql);

extern const char *kClosedConnection;

} // namespace zug
