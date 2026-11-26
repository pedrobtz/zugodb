// Shared by the glue's translation units: the engine headers, then R's (R
// after C++, so R's macros cannot rewrite the C++ library), and the
// error-return convention of design §4.3.
#pragma once

#include "zugodb/common/parallel.hpp"
#include "zugodb/common/thread_pool.hpp"
#include "zugodb/main/connection.hpp"
#include "zugodb/main/database.hpp"

#include <cstring>
#include <exception>
#include <string>

#ifdef _WIN32
#include <process.h>
#define ZUG_GETPID() static_cast<long>(_getpid())
#else
#include <unistd.h>
#define ZUG_GETPID() static_cast<long>(getpid())
#endif

#define R_NO_REMAP
#include <R.h>
#include <R_ext/Rdynload.h>
#include <R_ext/Visibility.h>
#include <Rinternals.h>

namespace zug {

// An error to hand back to R. A fixed-size buffer, so the message outlives
// the C++ frame that produced it without a destructor of its own: entry
// points keep one on their stack across R API calls, which may longjmp.
// `kind` names the condition zug_abort() raises in R: "sql" (the engine's
// own error), "type", "argument" or "closed".
struct EngineError {
  bool failed = false;
  const char *kind = "sql";
  char message[2048] = {0};

  void set(const char *msg, const char *k = "sql") {
    failed = true;
    kind = k;
    std::strncpy(message, msg ? msg : "unknown error", sizeof(message) - 1);
    message[sizeof(message) - 1] = '\0';
  }
};

// list(value = <value>, error = <string or NULL>, kind = <string>): every
// entry point that can fail returns this; R raises the condition.
inline SEXP ResultList(SEXP value, const EngineError &err) {
  const char *names[] = {"value", "error", "kind", ""};
  SEXP val = PROTECT(value);
  SEXP out = PROTECT(Rf_mkNamed(VECSXP, names));
  SET_VECTOR_ELT(out, 0, val);
  if (err.failed) {
    SET_VECTOR_ELT(out, 1, Rf_mkString(err.message));
  }
  SET_VECTOR_ELT(out, 2, Rf_mkString(err.failed ? err.kind : "ok"));
  UNPROTECT(2);
  return out;
}

} // namespace zug
