# Argument checks. Each failure is a zuslothdb_argument_error naming the
# argument, so a caller can tell a bad argument from an engine error.

check_flag <- function(x, arg, call = NULL) {
  if (!is.logical(x) || length(x) != 1L || is.na(x)) {
    sdb_abort(sprintf("`%s` must be TRUE or FALSE.", arg), "argument", call)
  }
  x
}

check_string <- function(x, arg, call = NULL) {
  if (!is.character(x) || length(x) != 1L || is.na(x)) {
    sdb_abort(sprintf("`%s` must be a single string.", arg), "argument", call)
  }
  x
}

# A whole number in 1 .. .Machine$integer.max, as an integer.
check_count <- function(x, arg, call = NULL) {
  ok <- is.numeric(x) && length(x) == 1L && !is.na(x) && is.finite(x) &&
    x == trunc(x) && x >= 1 && x <= .Machine$integer.max
  if (!ok) {
    sdb_abort(
      sprintf(
        "`%s` must be a whole number from 1 to %d.",
        arg, .Machine$integer.max
      ),
      "argument", call
    )
  }
  as.integer(x)
}

# ":memory:" (or "") for an in-memory database, else an absolute path.
check_dbdir <- function(dbdir, call = NULL) {
  check_string(dbdir, "dbdir", call)
  if (dbdir %in% c(":memory:", "")) {
    return(":memory:")
  }
  dbdir <- path.expand(dbdir)
  dir <- normalizePath(dirname(dbdir), winslash = "/", mustWork = FALSE)
  file.path(dir, basename(dbdir))
}

# The database can be opened as asked: the engine itself reports neither a
# missing directory (it would lose the data at close) nor a missing file
# opened read-only (it would start empty).
check_open <- function(dbdir, read_only, call = NULL) {
  if (dbdir == ":memory:") {
    if (read_only) {
      sdb_abort(
        "An in-memory database cannot be opened read-only.",
        "argument", call
      )
    }
    return(invisible())
  }
  if (read_only && !file.exists(dbdir)) {
    sdb_abort(
      sprintf("`dbdir` does not exist, so it cannot be opened read-only: %s", dbdir),
      "argument", call
    )
  }
  if (!dir.exists(dirname(dbdir))) {
    sdb_abort(
      sprintf("The directory of `dbdir` does not exist: %s", dirname(dbdir)),
      "argument", call
    )
  }
  if (dir.exists(dbdir)) {
    sdb_abort(sprintf("`dbdir` is a directory: %s", dbdir), "argument", call)
  }
  invisible()
}

check_choice <- function(x, choices, arg, call = NULL) {
  if (!is.character(x) || length(x) != 1L || is.na(x) || !x %in% choices) {
    sdb_abort(
      sprintf(
        "`%s` must be one of %s.",
        arg, paste0("\"", choices, "\"", collapse = ", ")
      ),
      "argument", call
    )
  }
  x
}

# A time zone R knows, or "" for the session's (design §6). duckdb warns and
# falls back to UTC; an unknown zone here is an error, so that no POSIXct
# is shown in a zone the caller did not ask for.
check_tz <- function(x, arg, call = NULL) {
  ok <- is.character(x) &&
    length(x) == 1L &&
    !is.na(x) &&
    (identical(x, "") || x %in% OlsonNames())
  if (!ok) {
    sdb_abort(
      sprintf(
        "`%s` must be a time zone from OlsonNames(), or \"\" for the session's.",
        arg
      ),
      "argument",
      call
    )
  }
  x
}
