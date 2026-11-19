#' The zugodb driver
#'
#' A DBI driver for the bundled engine, libzugodb, derived from SlothDB.
#' The driver holds one
#' database, in memory or in a file, which every connection made from it
#' shares, as `duckdb::duckdb()` does. The database opens at the first
#' [DBI::dbConnect()] and closes with its last connection.
#'
#' A file-backed database is read into memory when it opens and written
#' back, whole, when it closes: through [DBI::dbDisconnect()] of its last
#' connection, or at the end of the R session. The file is tied to the
#' engine commit that [zug_info()] reports.
#'
#' @param dbdir `":memory:"` (the default) for an in-memory database, or the
#'   path of a database file. The file need not exist unless `read_only` is
#'   `TRUE`; its directory must.
#' @param read_only `TRUE` to refuse every statement that would change the
#'   database, and never write its file.
#' @return An object of class `zugodb_driver`, for [DBI::dbConnect()].
#' @seealso [DBI::dbConnect()], whose zugodb method takes `threads`.
#' @export
#' @examples
#' drv <- zugodb()
#' con <- DBI::dbConnect(drv)
#' DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
#' DBI::dbExecute(con, "INSERT INTO t VALUES (1), (2)")
#' DBI::dbDisconnect(con)
zugodb <- function(dbdir = ":memory:", read_only = FALSE) {
  call <- sys.call()
  dbdir <- check_dbdir(dbdir, call)
  read_only <- check_flag(read_only, "read_only", call)
  check_open(dbdir, read_only, call)
  state <- new.env(parent = emptyenv())
  state$db <- NULL
  reg.finalizer(state, driver_finalizer)
  methods::new(
    "zugodb_driver",
    dbdir = dbdir, read_only = read_only, state = state
  )
}

#' @rdname zugodb
#' @export
setClass(
  "zugodb_driver",
  contains = "DBIDriver",
  slots = c(dbdir = "character", read_only = "logical", state = "environment")
)

# A database the driver kept open (through `shutdown = FALSE`) is closed,
# with a warning, when the driver is collected. While a connection lives,
# it holds the driver, so this never closes a database in use.
driver_finalizer <- function(state) {
  if (!is.null(state$db) && .Call(zug_db_is_open, state$db)) {
    warning(
      "A zugodb database was garbage-collected while open; ",
      "use dbDisconnect(con) to close it.",
      call. = FALSE
    )
    .Call(zug_db_close, state$db)
  }
}

# The driver's database, opened if it is not open (at the first connection,
# or again after its last connection closed it).
driver_database <- function(drv, call = NULL) {
  state <- drv@state
  if (is.null(state$db) || !.Call(zug_db_is_open, state$db)) {
    path <- if (drv@dbdir == ":memory:") "" else enc2utf8(drv@dbdir)
    state$db <- zug_value(.Call(zug_db_open, path, drv@read_only), call)
  }
  state$db
}

#' @rdname zugodb
#' @param object A `zugodb_driver`.
#' @export
setMethod("show", "zugodb_driver", function(object) {
  cat(
    "<zugodb_driver> ", object@dbdir,
    if (object@read_only) " (read-only)",
    "\n",
    sep = ""
  )
  invisible(object)
})

#' @rdname zugodb
#' @param dbObj A `zugodb_driver`.
#' @param ... Not used.
#' @export
setMethod("dbIsValid", "zugodb_driver", function(dbObj, ...) TRUE)

#' @rdname zugodb
#' @export
setMethod("dbGetInfo", "zugodb_driver", function(dbObj, ...) {
  info <- zug_info()
  list(
    driver.version = utils::packageVersion("zugodb"),
    client.version = paste("libzugodb", info$tag, substr(info$commit, 1, 7)),
    max.connections = NA_integer_
  )
})
