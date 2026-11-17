# The Stage 1 spike's one query path: an in-memory database, one query, the
# first value as an R integer. Internal; Stage 2's DBI driver replaces it.
sdb_query_int <- function(sql) {
  res <- .Call(sdb_scalar_int, sql)
  if (!is.null(res$error)) {
    sdb_abort(res$error, res$kind)
  }
  res$value
}

# The glue returns an engine error as a string, after every C++ frame has
# unwound (design §4.3); it becomes a condition only here, in R.
sdb_abort <- function(message, kind) {
  class <- switch(
    kind,
    sql = "zuslothdb_sql_error",
    type = "zuslothdb_type_error",
    argument = "zuslothdb_argument_error",
    "zuslothdb_internal_error"
  )
  stop(structure(
    class = c(class, "zuslothdb_error", "error", "condition"),
    list(message = message, call = NULL)
  ))
}
