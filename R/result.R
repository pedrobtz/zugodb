#' Query results
#'
#' [DBI::dbSendQuery()] runs a query and returns a `zuslothdb_result`, from
#' which [DBI::dbFetch()] returns rows as a data frame; [DBI::dbGetQuery()]
#' does both at once. The whole result is computed when the query runs
#' (design D8); `dbFetch(res, n)` hands it over `n` rows at a time.
#'
#' Columns arrive as these R types:
#'
#' | SlothDB | R |
#' | --- | --- |
#' | `BOOLEAN` | logical |
#' | `TINYINT`, `SMALLINT`, `INTEGER`, `UTINYINT`, `USMALLINT` | integer |
#' | `BIGINT`, `UINTEGER`, `UBIGINT` | double, `integer64` or character, by `bigint` |
#' | `FLOAT`, `DOUBLE` | double |
#' | `VARCHAR` | character, marked UTF-8 |
#' | `DATE` | `Date` |
#' | `TIMESTAMP` | `POSIXct`, shown in the connection's `timezone_out` |
#' | `BLOB` | a list of raw vectors with the classes of `blob::blob()`, `NULL` for a missing value |
#'
#' A value R cannot hold as asked is a `zuslothdb_type_error` naming the
#' column and the row, never a silent change: a 64-bit integer beyond 2^53
#' with `bigint = "numeric"`, an `INTEGER` of -2^31 (R's `NA`), or a string
#' that is not valid UTF-8 or holds a NUL. A column of another type is a
#' `zuslothdb_type_error` naming it: `HUGEINT` and `TIME`, whose values
#' SlothDB computes wrongly, and `INTERVAL` and `UUID`, not read yet.
#' SlothDB produces no `DECIMAL`, `TIMESTAMP WITH TIME ZONE`, list,
#' struct, map or union column.
#'
#' A connection has one open result: sending another query clears the
#' first, with a warning.
#'
#' @param conn A `zuslothdb_connection`.
#' @param statement A single SQL string.
#' @param params Not supported: SlothDB has no prepared statements, so a
#'   non-`NULL` value is a `zuslothdb_unsupported` error.
#' @param immediate Ignored: every statement runs at once.
#' @param res,dbObj,object A `zuslothdb_result`.
#' @param n The number of rows to fetch: a whole number from 0, or `-1` or
#'   `Inf` for all that are left.
#' @param ... Not used.
#' @return `dbSendQuery()` and `dbSendStatement()`: a `zuslothdb_result`.
#'   `dbFetch()`: a data frame. The rest as in DBI.
#' @name zuslothdb_result
#' @examples
#' con <- DBI::dbConnect(zuslothdb())
#' res <- DBI::dbSendQuery(con, "SELECT 1 AS a, 'x' AS b")
#' DBI::dbFetch(res)
#' DBI::dbClearResult(res)
#' DBI::dbGetQuery(con, "SELECT 42 AS answer")
#' DBI::dbDisconnect(con)
NULL

#' @rdname zuslothdb_result
#' @export
setClass(
  "zuslothdb_result",
  contains = "DBIResult",
  slots = c(
    connection = "zuslothdb_connection",
    statement = "character",
    state = "environment"
  )
)

send_result <- function(conn, statement, params, call) {
  if (!is.null(params)) {
    sdb_abort(
      "Parameters are not supported: SlothDB has no prepared statements.",
      "unsupported", call
    )
  }
  check_string(statement, "statement", call)
  cstate <- conn@state
  old <- cstate$result
  if (!is.null(old) && !is.null(old$ptr)) {
    warning(
      "Clearing the connection's open result before sending a new query.",
      call. = FALSE
    )
    clear_result_state(old)
  }
  ptr <- sdb_value(.Call(sdb_send, cstate$ptr, enc2utf8(statement)), call)
  state <- new.env(parent = emptyenv())
  state$ptr <- ptr
  cstate$result <- state
  methods::new(
    "zuslothdb_result",
    connection = conn, statement = statement, state = state
  )
}

clear_result_state <- function(state) {
  if (!is.null(state$ptr)) {
    .Call(sdb_result_clear, state$ptr)
    state$ptr <- NULL
  }
  invisible()
}

result_valid <- function(res) {
  !is.null(res@state$ptr) &&
    !is.null(.Call(sdb_result_info, res@state$ptr)) &&
    dbIsValid(res@connection)
}

check_result <- function(res, call) {
  if (!result_valid(res)) {
    sdb_abort(
      "The result is cleared, or its connection closed.",
      "closed", call
    )
  }
  .Call(sdb_result_info, res@state$ptr)
}

check_fetch_n <- function(n, call) {
  ok <- is.numeric(n) && length(n) == 1L && !is.na(n) &&
    (n == -1 || (n >= 0 && (is.infinite(n) || n == trunc(n))))
  if (!ok) {
    sdb_abort(
      "`n` must be a whole number from 0, or -1 or Inf for all rows.",
      "argument", call
    )
  }
  if (is.infinite(n)) -1 else as.double(n)
}

bigint_code <- function(conn) {
  match(conn@state$bigint, c("numeric", "integer64", "character")) - 1L
}

new_data_frame <- function(cols, names) {
  n <- if (length(cols)) length(cols[[1L]]) else 0L
  names(cols) <- names
  structure(cols, class = "data.frame", row.names = .set_row_names(n))
}

#' @rdname zuslothdb_result
#' @export
setMethod(
  "dbSendQuery", c("zuslothdb_connection", "character"),
  function(conn, statement, ..., params = NULL, immediate = NULL) {
    send_result(conn, statement, params, sys.call())
  }
)

#' @rdname zuslothdb_result
#' @export
setMethod(
  "dbSendStatement", c("zuslothdb_connection", "character"),
  function(conn, statement, ..., params = NULL, immediate = NULL) {
    send_result(conn, statement, params, sys.call())
  }
)

#' @rdname zuslothdb_result
#' @export
setMethod("dbFetch", "zuslothdb_result", function(res, n = -1, ...) {
  call <- sys.call()
  info <- check_result(res, call)
  n <- check_fetch_n(n, call)
  if (!info$is_query) {
    warning(
      "Fetching from a statement's result: it has no rows.",
      call. = FALSE
    )
    return(new_data_frame(list(), character()))
  }
  cols <- sdb_value(
    .Call(
      sdb_fetch, res@state$ptr, n, bigint_code(res@connection),
      res@connection@state$timezone_out
    ),
    call
  )
  new_data_frame(cols, info$names)
})

#' @rdname zuslothdb_result
#' @export
setMethod("dbClearResult", "zuslothdb_result", function(res, ...) {
  if (is.null(res@state$ptr)) {
    warning("The result is already cleared.", call. = FALSE)
    return(invisible(FALSE))
  }
  clear_result_state(res@state)
  invisible(TRUE)
})

#' @rdname zuslothdb_result
#' @export
setMethod("dbIsValid", "zuslothdb_result", function(dbObj, ...) {
  result_valid(dbObj)
})

#' @rdname zuslothdb_result
#' @export
setMethod("dbHasCompleted", "zuslothdb_result", function(res, ...) {
  info <- check_result(res, sys.call())
  info$pos >= info$nrow
})

#' @rdname zuslothdb_result
#' @export
setMethod("dbGetRowCount", "zuslothdb_result", function(res, ...) {
  check_result(res, sys.call())$pos
})

#' @rdname zuslothdb_result
#' @export
setMethod("dbGetRowsAffected", "zuslothdb_result", function(res, ...) {
  check_result(res, sys.call())$rows_affected
})

#' @rdname zuslothdb_result
#' @export
setMethod("dbGetStatement", "zuslothdb_result", function(res, ...) {
  check_result(res, sys.call())
  res@statement
})

#' @rdname zuslothdb_result
#' @export
setMethod("dbColumnInfo", "zuslothdb_result", function(res, ...) {
  info <- check_result(res, sys.call())
  data.frame(
    name = info$names,
    type = r_type(info$sql_types, res@connection@state$bigint),
    sql_type = info$sql_types,
    stringsAsFactors = FALSE
  )
})

#' @rdname zuslothdb_result
#' @export
setMethod("dbBind", "zuslothdb_result", function(res, params, ...) {
  sdb_abort(
    "Parameters are not supported: SlothDB has no prepared statements.",
    "unsupported", sys.call()
  )
})

#' @rdname zuslothdb_result
#' @export
setMethod("show", "zuslothdb_result", function(object) {
  cat(
    "<zuslothdb_result> ", object@statement,
    if (!dbIsValid(object)) " (cleared)",
    "\n",
    sep = ""
  )
  invisible(object)
})

# The R type each SQL type arrives as (the table in ?zuslothdb_result).
r_type <- function(sql_types, bigint) {
  big <- switch(
    bigint,
    numeric = "numeric",
    integer64 = "integer64",
    character = "character"
  )
  map <- c(
    BOOLEAN = "logical", TINYINT = "integer", SMALLINT = "integer",
    INTEGER = "integer", UTINYINT = "integer", USMALLINT = "integer",
    BIGINT = big, UINTEGER = big, UBIGINT = big,
    FLOAT = "numeric", DOUBLE = "numeric", VARCHAR = "character",
    "NULL" = "integer", DATE = "Date", TIMESTAMP = "POSIXct", BLOB = "blob"
  )
  out <- unname(map[sql_types])
  out[is.na(out)] <- NA_character_
  out
}
