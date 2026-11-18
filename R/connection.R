#' Connect to a zuslothdb database
#'
#' Opens a connection to the driver's database. Two connections from the
#' same driver share one database; `dbConnect(zuslothdb(), dbdir = path)` is
#' the short form for a database of its own.
#'
#' @param drv A driver from [zuslothdb()].
#' @param dbdir,read_only As for [zuslothdb()]. When either differs from the
#'   driver's, the connection gets a new driver, and so a database of its
#'   own.
#' @param bigint How 64-bit integers (`BIGINT`, `UINTEGER`, `UBIGINT`)
#'   arrive: `"numeric"` (the default, as in `duckdb`), a double, with a
#'   value beyond 2^53 a `zuslothdb_type_error`; `"integer64"`, the
#'   `bit64` class; or `"character"`, the decimal digits.
#' @param timezone_out The time zone a `TIMESTAMP` column's `POSIXct` is
#'   shown in: `"UTC"` (the default, as in `duckdb`), another of
#'   [OlsonNames()], or `""` for the session's. The instant is the same in
#'   any zone; an unknown zone is an error.
#' @param threads The number of threads the engine may use, a whole number
#'   from 1. The engine has one thread pool per R process, so this is a cap
#'   for the whole process, not for this connection: the last
#'   `dbConnect()` sets it, and [zuslothdb_info()] reports it.
#' @param ... Not used; an unknown argument is an error.
#' @return An object of class `zuslothdb_connection`.
#' @export
#' @examples
#' con <- DBI::dbConnect(zuslothdb())
#' DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
#' DBI::dbDisconnect(con)
setMethod(
  "dbConnect", "zuslothdb_driver",
  function(drv, dbdir, read_only, bigint = "numeric", threads = 2L,
           timezone_out = "UTC", ...) {
    call <- sys.call()
    if (...length()) {
      sdb_abort(
        sprintf(
          "Unknown argument%s to dbConnect(): %s.",
          if (...length() > 1L) "s" else "",
          paste(names(list(...)), collapse = ", ")
        ),
        "argument", call
      )
    }
    new_dbdir <- if (missing(dbdir)) drv@dbdir else check_dbdir(dbdir, call)
    new_ro <- if (missing(read_only)) {
      drv@read_only
    } else {
      check_flag(read_only, "read_only", call)
    }
    if (new_dbdir != drv@dbdir || new_ro != drv@read_only) {
      drv <- zuslothdb(new_dbdir, new_ro)
    }
    bigint <- check_choice(
      bigint, c("numeric", "integer64", "character"), "bigint", call
    )
    threads <- check_count(threads, "threads", call)
    timezone_out <- check_tz(timezone_out, "timezone_out", call)
    sdb_value(.Call(sdb_set_threads, threads), call)

    db <- driver_database(drv, call)
    ptr <- sdb_value(.Call(sdb_connect, db), call)
    state <- new.env(parent = emptyenv())
    state$ptr <- ptr
    state$db <- db
    state$bigint <- bigint
    state$timezone_out <- timezone_out
    state$result <- NULL
    reg.finalizer(state, connection_finalizer)
    methods::new("zuslothdb_connection", driver = drv, state = state)
  }
)

#' @rdname dbConnect-zuslothdb_driver-method
#' @export
setClass(
  "zuslothdb_connection",
  contains = "DBIConnection",
  slots = c(driver = "zuslothdb_driver", state = "environment")
)

# A connection collected while open is disconnected with a warning, and its
# database closed if it was the last one, as dbDisconnect(con) would.
connection_finalizer <- function(state) {
  if (.Call(sdb_conn_is_open, state$ptr)) {
    warning(
      "A zuslothdb connection was garbage-collected while open; ",
      "use dbDisconnect(con) to close it.",
      call. = FALSE
    )
    disconnect_state(state, shutdown = TRUE)
  }
}

disconnect_state <- function(state, shutdown) {
  left <- .Call(sdb_disconnect, state$ptr)
  if (shutdown && identical(left, 0L)) {
    .Call(sdb_db_close, state$db)
  }
  invisible()
}

#' Disconnect from a zuslothdb database
#'
#' Closes the connection. With `shutdown = TRUE`, the default, the database
#' closes too once this was its last open connection, and a file-backed
#' database is written to its file. The driver's next [DBI::dbConnect()]
#' opens it again.
#'
#' @param conn A `zuslothdb_connection`.
#' @param shutdown `FALSE` to keep the database open, for the driver's next
#'   connection, after its last connection closes. Other open connections
#'   to the database are never closed by this.
#' @param ... Not used.
#' @return `TRUE`, invisibly; with a warning and `FALSE` if the connection
#'   was already closed.
#' @export
setMethod(
  "dbDisconnect", "zuslothdb_connection",
  function(conn, shutdown = TRUE, ...) {
    shutdown <- check_flag(shutdown, "shutdown", sys.call())
    if (!dbIsValid(conn)) {
      warning("The connection is already closed.", call. = FALSE)
      return(invisible(FALSE))
    }
    disconnect_state(conn@state, shutdown)
    invisible(TRUE)
  }
)

#' @rdname dbConnect-zuslothdb_driver-method
#' @param dbObj,object A `zuslothdb_connection`.
#' @export
setMethod("dbIsValid", "zuslothdb_connection", function(dbObj, ...) {
  .Call(sdb_conn_is_open, dbObj@state$ptr)
})

#' @rdname dbConnect-zuslothdb_driver-method
#' @export
setMethod("show", "zuslothdb_connection", function(object) {
  cat(
    "<zuslothdb_connection> ", object@driver@dbdir,
    if (object@driver@read_only) " (read-only)",
    if (!dbIsValid(object)) " DISCONNECTED",
    "\n",
    sep = ""
  )
  invisible(object)
})

#' @rdname dbConnect-zuslothdb_driver-method
#' @export
setMethod("dbGetInfo", "zuslothdb_connection", function(dbObj, ...) {
  list(
    dbname = dbObj@driver@dbdir,
    db.version = zuslothdb_info()$commit,
    username = "",
    host = "",
    port = "",
    read_only = dbObj@driver@read_only
  )
})

#' Run a statement
#'
#' Runs SQL that returns no result set, such as `CREATE TABLE` or
#' `INSERT`, and returns the number of rows it changed.
#'
#' @param conn A `zuslothdb_connection`.
#' @param statement A single SQL string; several statements separated by
#'   `;` run in order.
#' @param params Not supported: SlothDB has no prepared statements, so a
#'   non-`NULL` value is a `zuslothdb_unsupported` error.
#' @param ... Not used.
#' @return The rows inserted, updated or deleted, as a number; 0 for any
#'   other statement.
#' @export
setMethod(
  "dbExecute", c("zuslothdb_connection", "character"),
  function(conn, statement, ..., params = NULL) {
    call <- sys.call()
    if (!is.null(params)) {
      sdb_abort(
        "Parameters are not supported: SlothDB has no prepared statements.",
        "unsupported", call
      )
    }
    check_string(statement, "statement", call)
    sdb_value(.Call(sdb_execute, conn@state$ptr, enc2utf8(statement)), call)
  }
)
