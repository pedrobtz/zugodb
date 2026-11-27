# Conditions (design §5): every error the package raises inherits from
# zug_error. The glue never raises: it returns the error's message and
# kind after every C++ frame has unwound (design §4.3), and this turns it
# into a condition.
#
#   sql          zug_sql_error, carrying the engine's message
#   type         zug_type_error, a value R cannot hold as asked
#   argument     zug_argument_error, a bad argument
#   closed       zug_closed_error, a closed or foreign-process object
#   unsupported  zug_unsupported, a feature v0.1.0 does not have
#   stream       zug_stream_error, an Arrow C stream's producer failed or
#                gave a batch that does not match its schema
zug_abort <- function(message, kind = "internal", call = NULL) {
  class <- switch(
    kind,
    sql = "zug_sql_error",
    type = "zug_type_error",
    stream = "zug_stream_error",
    argument = "zug_argument_error",
    closed = "zug_closed_error",
    unsupported = "zug_unsupported",
    "zug_internal_error"
  )
  stop(structure(
    class = c(class, "zug_error", "error", "condition"),
    list(message = message, call = call)
  ))
}

# The value of a glue result list(value, error, kind), or its condition.
zug_value <- function(res, call = NULL) {
  if (!is.null(res$error)) {
    zug_abort(res$error, res$kind, call)
  }
  res$value
}

# The Stage 1 spike's one-query path: an in-memory database, one query, the
# first value as an R integer. Internal; Stage 3's results replace it.
zug_query_int <- function(sql) {
  zug_value(.Call(zug_scalar_int, sql))
}
