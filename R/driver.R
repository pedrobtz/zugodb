#' The zuslothdb driver
#'
#' A DBI driver for the bundled SlothDB engine. The driver holds one
#' database, in memory or in a file, which every connection made from it
#' shares, as `duckdb::duckdb()` does. The database opens at the first
#' [DBI::dbConnect()] and closes with its last connection.
#'
#' A file-backed database is read into memory when it opens and written
#' back, whole, when it closes: through [DBI::dbDisconnect()] of its last
#' connection, or at the end of the R session. The file is tied to the
#' engine commit that [zuslothdb_info()] reports.
#'
#' @param dbdir `":memory:"` (the default) for an in-memory database, or the
#'   path of a database file. The file need not exist unless `read_only` is
#'   `TRUE`; its directory must.
#' @param read_only `TRUE` to refuse every statement that would change the
#'   database, and never write its file.
#' @return An object of class `zuslothdb_driver`, for [DBI::dbConnect()].
#' @seealso [DBI::dbConnect()], whose zuslothdb method takes `threads`.
#' @export
#' @examples
#' drv <- zuslothdb()
#' con <- DBI::dbConnect(drv)
#' DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
#' DBI::dbExecute(con, "INSERT INTO t VALUES (1), (2)")
#' DBI::dbDisconnect(con)
zuslothdb <- function(dbdir = ":memory:", read_only = FALSE) {
  call <- sys.call()
  dbdir <- check_dbdir(dbdir, call)
  read_only <- check_flag(read_only, "read_only", call)
  check_open(dbdir, read_only, call)
  state <- new.env(parent = emptyenv())
  state$db <- NULL
  reg.finalizer(state, driver_finalizer)
  methods::new(
    "zuslothdb_driver",
    dbdir = dbdir, read_only = read_only, state = state
  )
}

#' @rdname zuslothdb
#' @export
setClass(
  "zuslothdb_driver",
  contains = "DBIDriver",
  slots = c(dbdir = "character", read_only = "logical", state = "environment")
)

# A database the driver kept open (through `shutdown = FALSE`) is closed,
# with a warning, when the driver is collected. While a connection lives,
# it holds the driver, so this never closes a database in use.
driver_finalizer <- function(state) {
  if (!is.null(state$db) && .Call(sdb_db_is_open, state$db)) {
    warning(
      "A zuslothdb database was garbage-collected while open; ",
      "use dbDisconnect(con) to close it.",
      call. = FALSE
    )
    .Call(sdb_db_close, state$db)
  }
}

# The driver's database, opened if it is not open (at the first connection,
# or again after its last connection closed it).
driver_database <- function(drv, call = NULL) {
  state <- drv@state
  if (is.null(state$db) || !.Call(sdb_db_is_open, state$db)) {
    path <- if (drv@dbdir == ":memory:") "" else enc2utf8(drv@dbdir)
    state$db <- sdb_value(.Call(sdb_db_open, path, drv@read_only), call)
  }
  state$db
}

#' @rdname zuslothdb
#' @param object A `zuslothdb_driver`.
#' @export
setMethod("show", "zuslothdb_driver", function(object) {
  cat(
    "<zuslothdb_driver> ", object@dbdir,
    if (object@read_only) " (read-only)",
    "\n",
    sep = ""
  )
  invisible(object)
})

#' @rdname zuslothdb
#' @param dbObj A `zuslothdb_driver`.
#' @param ... Not used.
#' @export
setMethod("dbIsValid", "zuslothdb_driver", function(dbObj, ...) TRUE)

#' @rdname zuslothdb
#' @export
setMethod("dbGetInfo", "zuslothdb_driver", function(dbObj, ...) {
  info <- zuslothdb_info()
  list(
    driver.version = utils::packageVersion("zuslothdb"),
    client.version = paste("SlothDB", info$describe, substr(info$commit, 1, 7)),
    max.connections = NA_integer_
  )
})
