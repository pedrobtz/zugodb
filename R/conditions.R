# Conditions (design §5): every error the package raises inherits from
# zuslothdb_error. The glue never raises: it returns the error's message and
# kind after every C++ frame has unwound (design §4.3), and this turns it
# into a condition.
#
#   sql          zuslothdb_sql_error, carrying the engine's message
#   type         zuslothdb_type_error, a value R cannot hold as asked
#   argument     zuslothdb_argument_error, a bad argument
#   closed       zuslothdb_closed_error, a closed or foreign-process object
#   unsupported  zuslothdb_unsupported, a feature v0.1.0 does not have
sdb_abort <- function(message, kind = "internal", call = NULL) {
  class <- switch(
    kind,
    sql = "zuslothdb_sql_error",
    type = "zuslothdb_type_error",
    argument = "zuslothdb_argument_error",
    closed = "zuslothdb_closed_error",
    unsupported = "zuslothdb_unsupported",
    "zuslothdb_internal_error"
  )
  stop(structure(
    class = c(class, "zuslothdb_error", "error", "condition"),
    list(message = message, call = call)
  ))
}

# The value of a glue result list(value, error, kind), or its condition.
sdb_value <- function(res, call = NULL) {
  if (!is.null(res$error)) {
    sdb_abort(res$error, res$kind, call)
  }
  res$value
}

# The Stage 1 spike's one-query path: an in-memory database, one query, the
# first value as an R integer. Internal; Stage 3's results replace it.
sdb_query_int <- function(sql) {
  sdb_value(.Call(sdb_scalar_int, sql))
}
