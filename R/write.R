#' Write, list and remove tables
#'
#' [DBI::dbWriteTable()] creates a table from a data frame, or replaces one
#' (`overwrite = TRUE`) or adds to one (`append = TRUE`);
#' [DBI::dbAppendTable()] adds the rows of a data frame to a table that
#' exists. Columns are written as these SQL types, which
#' [DBI::dbDataType()] reports:
#'
#' | R | Engine |
#' | --- | --- |
#' | logical | `BOOLEAN` |
#' | integer | `INTEGER` |
#' | double | `DOUBLE` |
#' | character, factor | `VARCHAR` |
#' | `Date` | `DATE` |
#' | `POSIXct` | `TIMESTAMP`, the instant in UTC, to the microsecond |
#' | `bit64::integer64` | `BIGINT` |
#' | `blob::blob()`, or a list of raw vectors | `BLOB` |
#'
#' A column of another class, a value that does not fit its column (a
#' fraction for an `INTEGER`, a number out of range, a string that is not
#' valid UTF-8) or a column the table does not have is an error, raised
#' before any row is written: the table is as it was. `NA` is written as
#' `NULL`; `NaN` stays `NaN`.
#'
#' The engine has no transactions: [DBI::dbBegin()], [DBI::dbCommit()] and
#' [DBI::dbRollback()] are `zug_unsupported` errors, and SQL `BEGIN`,
#' `COMMIT` and `ROLLBACK` are refused.
#'
#' @param conn A `zugodb_connection`.
#' @param name The table's name: a string, [DBI::Id()] with only a `table`,
#'   or [DBI::SQL()] of a quoted name. Names keep their spelling and match
#'   ignoring ASCII case, as DuckDB's; a `"` in a name is doubled when it is
#'   quoted.
#' @param value A data frame.
#' @param overwrite,append `TRUE` to replace or to add to a table that
#'   exists; at most one of them.
#' @param field.types A named character vector of SQL types for some or all
#'   columns, in place of [DBI::dbDataType()]'s.
#' @param temporary Not supported: the engine has no temporary tables.
#' @param row.names As in [DBI::sqlRownamesToColumn()]: `FALSE` (the
#'   default) drops them, `TRUE` or a string writes them as a column.
#' @param fail_if_missing For `dbRemoveTable()`: `FALSE` to do nothing when
#'   the table does not exist.
#' @param dbObj A `zugodb_driver` or `zugodb_connection`.
#' @param obj A vector, or a data frame for the type of each column.
#' @param x A character vector of identifiers.
#' @param ... Not used.
#' @return `dbWriteTable()`, `dbRemoveTable()`: `TRUE`, invisibly.
#'   `dbAppendTable()`: the number of rows written. `dbListTables()`,
#'   `dbListFields()`: a character vector. `dbExistsTable()`: `TRUE` or
#'   `FALSE`. `dbDataType()`: a character vector. `dbQuoteIdentifier()`,
#'   `dbQuoteString()`: [DBI::SQL()]. `dbReadTable()`: a data frame.
#'   `dbListObjects()`: a data frame of `table` ([DBI::Id()]s) and
#'   `is_prefix`, always `FALSE`.
#' @name zugodb_tables
#' @examples
#' con <- DBI::dbConnect(zugodb())
#' DBI::dbWriteTable(con, "cars", head(mtcars))
#' DBI::dbListTables(con)
#' DBI::dbReadTable(con, "cars")
#' DBI::dbAppendTable(con, "cars", tail(mtcars))
#' DBI::dbRemoveTable(con, "cars")
#' DBI::dbDisconnect(con)
NULL

# The payload codes of src/zug_write.cpp.
payload_codes <- c(
  logical = 0L,
  integer = 1L,
  double = 2L,
  integer64 = 3L,
  character = 4L,
  raw = 5L,
  Date = 6L,
  POSIXct = 7L
)

sql_types <- c(
  logical = "BOOLEAN",
  integer = "INTEGER",
  double = "DOUBLE",
  integer64 = "BIGINT",
  character = "VARCHAR",
  raw = "BLOB",
  Date = "DATE",
  POSIXct = "TIMESTAMP"
)

is_raw_list <- function(x) {
  is.list(x) && all(vapply(x, function(e) is.null(e) || is.raw(e), TRUE))
}

# `x` without the class I() gives it, which says nothing of its type.
drop_asis <- function(x) {
  if (inherits(x, "AsIs")) {
    class(x) <- setdiff(class(x), "AsIs")
    if (!length(class(x))) {
      x <- unclass(x)
    }
  }
  x
}

# The payload a column is written as, or NA for a class that cannot be.
payload_of <- function(x) {
  x <- drop_asis(x)
  if (is.factor(x)) {
    return("character")
  }
  if (inherits(x, "integer64")) {
    return("integer64")
  }
  if (inherits(x, "Date")) {
    return("Date")
  }
  if (inherits(x, "POSIXct")) {
    return("POSIXct")
  }
  if (inherits(x, "blob") || (identical(class(x), "list") && is_raw_list(x))) {
    return("raw")
  }
  if (is.object(x)) {
    return(NA_character_)
  }
  switch(
    typeof(x),
    logical = "logical",
    integer = "integer",
    double = "double",
    character = "character",
    NA_character_
  )
}

cannot_write <- function(x, what, call) {
  zug_abort(
    sprintf(
      "%s is of class %s, which zugodb cannot write.",
      what,
      paste(class(x), collapse = "/")
    ),
    "type",
    call
  )
}

# Column `x` as the plain vector src/zug_write.cpp reads.
payload_value <- function(x, payload, name, call) {
  switch(
    payload,
    logical = as.logical(unclass(x)),
    integer = as.integer(unclass(x)),
    double = as.double(unclass(x)),
    integer64 = as.double(unclass(x)),
    Date = as.double(unclass(x)),
    POSIXct = as.double(unclass(x)),
    raw = {
      x <- unclass(x)
      attributes(x) <- NULL
      x
    },
    character = {
      # A factor by its levels; every string in UTF-8.
      s <- if (is.factor(x)) as.character(x) else as.character(unclass(x))
      s <- enc2utf8(s)
      bad <- which(!is.na(s) & !validUTF8(s))
      if (length(bad)) {
        zug_abort(
          sprintf(
            "Column `%s`, row %d: the string is not valid UTF-8.",
            name,
            bad[[1L]]
          ),
          "type",
          call
        )
      }
      s
    }
  )
}

# The data frame as zug_append()'s arguments: every column checked here
# before anything is written.
prepare_frame <- function(value, call) {
  if (!is.data.frame(value)) {
    zug_abort("`value` must be a data frame.", "argument", call)
  }
  nms <- names(value)
  if (is.null(nms) || anyNA(nms) || any(!nzchar(nms))) {
    zug_abort("Every column of `value` must have a name.", "argument", call)
  }
  if (anyDuplicated(nms)) {
    zug_abort(
      sprintf("`value` has the column `%s` twice.", nms[anyDuplicated(nms)]),
      "argument",
      call
    )
  }
  # Checked here, before any SQL is built from them.
  quote_name(nms, call)
  payloads <- character(length(value))
  cols <- vector("list", length(value))
  for (k in seq_along(value)) {
    x <- value[[k]]
    p <- payload_of(x)
    if (is.na(p)) {
      cannot_write(x, sprintf("Column `%s`", nms[[k]]), call)
    }
    payloads[[k]] <- p
    cols[[k]] <- payload_value(x, p, nms[[k]], call)
  }
  list(
    cols = cols,
    payloads = unname(payload_codes[payloads]),
    kinds = payloads,
    names = enc2utf8(nms)
  )
}

# The table name, unquoted: a string, Id(table = ) or SQL of a quoted name.
table_name <- function(conn, name, call) {
  if (methods::is(name, "SQL")) {
    name <- DBI::dbUnquoteIdentifier(conn, name)[[1L]]
  }
  if (methods::is(name, "Id")) {
    # Id(table = "t") names its part; dbUnquoteIdentifier() gives Id("t").
    parts <- name@name
    named <- !is.null(names(parts)) && all(nzchar(names(parts)))
    table <- if (named) parts[names(parts) == "table"] else parts[length(parts)]
    rest <- if (named) parts[names(parts) != "table"] else parts[-length(parts)]
    if (length(rest) && any(nzchar(rest))) {
      zug_abort(
        "The engine has no schemas or catalogs: give the table's name alone.",
        "unsupported",
        call
      )
    }
    name <- unname(table)
  }
  check_string(name, "name", call)
  # Checked here, before any SQL is built from it.
  quote_name(name, call)
  enc2utf8(name)
}

# Each name in double quotes, a `"` inside doubled ("a""b" is a"b), as
# the engine's tokenizer reads it. An empty name is the engine's error.
quote_name <- function(x, call) {
  if (anyNA(x)) {
    zug_abort("An identifier cannot be NA.", "argument", call)
  }
  if (!length(x)) {
    return(character())
  }
  x <- gsub("\"", "\"\"", enc2utf8(x), fixed = TRUE)
  paste0("\"", x, "\"")
}

append_frame <- function(conn, table, prepared, check_only, call) {
  zug_value(
    .Call(
      zug_append,
      conn@state$ptr,
      table,
      quote_name(table, call),
      prepared$cols,
      prepared$payloads,
      prepared$names,
      check_only
    ),
    call
  )
}

create_sql <- function(table, names, types, call) {
  sprintf(
    "CREATE TABLE %s (%s)",
    quote_name(table, call),
    paste(quote_name(names, call), types, collapse = ", ")
  )
}

check_open_connection <- function(conn, call) {
  if (!dbIsValid(conn)) {
    zug_abort("The connection is closed.", "closed", call)
  }
}

write_table_method <- function(
  conn,
  name,
  value,
  ...,
  row.names = FALSE,
  overwrite = FALSE,
  append = FALSE,
  field.types = NULL,
  temporary = FALSE
) {
  call <- sys.call()
  check_open_connection(conn, call)
  table <- table_name(conn, name, call)
  overwrite <- check_flag(overwrite, "overwrite", call)
  append <- check_flag(append, "append", call)
  temporary <- check_flag(temporary, "temporary", call)
  if (temporary) {
    zug_abort("The engine has no temporary tables.", "unsupported", call)
  }
  if (overwrite && append) {
    zug_abort(
      "`overwrite` and `append` cannot both be TRUE.",
      "argument",
      call
    )
  }
  check_row_names(row.names, call)
  if (append && !is.null(field.types)) {
    zug_abort(
      "`field.types` cannot be given with `append = TRUE`.",
      "argument",
      call
    )
  }
  if (!is.data.frame(value)) {
    zug_abort("`value` must be a data frame.", "argument", call)
  }
  value <- DBI::sqlRownamesToColumn(value, row.names)
  prepared <- prepare_frame(value, call)
  exists <- table_exists(conn, table)
  if (exists && !overwrite && !append) {
    zug_abort(
      sprintf(
        "The table `%s` exists; use `overwrite = TRUE` or `append = TRUE`.",
        table
      ),
      "argument",
      call
    )
  }
  if (exists && append) {
    dbAppendTable(conn, table, value)
    return(invisible(TRUE))
  }
  types <- unname(sql_types[prepared$kinds])
  if (!is.null(field.types)) {
    if (
      !is.character(field.types) ||
        is.null(names(field.types)) ||
        anyNA(field.types) ||
        anyDuplicated(names(field.types)) ||
        !all(names(field.types) %in% prepared$names)
    ) {
      zug_abort(
        "`field.types` must be SQL types named by columns of `value`.",
        "argument",
        call
      )
    }
    types[match(names(field.types), prepared$names)] <- unname(field.types)
  }
  # The data is checked against the new table's types in a probe table
  # before an existing table is dropped: the engine has no rollback (its
  # transaction statements are refused, design §5) and no rename.
  probe <- paste0("__zugodb_write_", table, "__")
  while (table_exists(conn, probe)) {
    probe <- paste0(probe, "_")
  }
  dbExecute(conn, create_sql(probe, prepared$names, types, call))
  checked <- tryCatch(
    {
      append_frame(conn, probe, prepared, TRUE, call)
      NULL
    },
    error = function(e) e
  )
  dbExecute(conn, paste("DROP TABLE", quote_name(probe, call)))
  if (!is.null(checked)) {
    stop(checked)
  }
  if (exists) {
    dbExecute(conn, paste("DROP TABLE", quote_name(table, call)))
  }
  dbExecute(conn, create_sql(table, prepared$names, types, call))
  append_frame(conn, table, prepared, FALSE, call)
  invisible(TRUE)
}

#' @rdname zugodb_tables
#' @export
setMethod(
  "dbWriteTable",
  c("zugodb_connection", "character", "ANY"),
  write_table_method
)

#' @rdname zugodb_tables
#' @export
setMethod(
  "dbWriteTable",
  c("zugodb_connection", "Id", "ANY"),
  write_table_method
)

#' @rdname zugodb_tables
#' @export
setMethod(
  "dbWriteTable",
  c("zugodb_connection", "SQL", "ANY"),
  write_table_method
)


append_table_method <- function(conn, name, value, ..., row.names = NULL) {
  call <- sys.call()
  check_open_connection(conn, call)
  table <- table_name(conn, name, call)
  if (!is.null(row.names)) {
    zug_abort(
      "`row.names` must be NULL for dbAppendTable().",
      "argument",
      call
    )
  }
  if (!is.data.frame(value)) {
    zug_abort("`value` must be a data frame.", "argument", call)
  }
  fields <- dbListFields(conn, table)
  extra <- setdiff(names(value), fields)
  if (length(extra)) {
    zug_abort(
      sprintf(
        "The table `%s` has no column `%s`.",
        table,
        extra[[1L]]
      ),
      "argument",
      call
    )
  }
  prepared <- prepare_frame(value, call)
  # By name, in the table's order; a column not given is NULL.
  idx <- match(fields, prepared$names)
  n <- nrow(value)
  missing <- is.na(idx)
  prepared$cols <- lapply(seq_along(fields), function(k) {
    if (missing[[k]]) rep(NA, n) else prepared$cols[[idx[[k]]]]
  })
  prepared$payloads <- ifelse(missing, -1L, prepared$payloads[idx])
  prepared$names <- enc2utf8(fields)
  append_frame(conn, table, prepared, FALSE, call)
}

#' @rdname zugodb_tables
#' @export
setMethod(
  "dbAppendTable",
  c("zugodb_connection", "character", "ANY"),
  append_table_method
)

#' @rdname zugodb_tables
#' @export
setMethod(
  "dbAppendTable",
  c("zugodb_connection", "Id", "ANY"),
  append_table_method
)

#' @rdname zugodb_tables
#' @export
setMethod(
  "dbAppendTable",
  c("zugodb_connection", "SQL", "ANY"),
  append_table_method
)


#' @rdname zugodb_tables
#' @export
setMethod("dbListTables", "zugodb_connection", function(conn, ...) {
  call <- sys.call()
  check_open_connection(conn, call)
  out <- DBI::dbGetQuery(conn, "SHOW TABLES")[[1L]]
  sort(as.character(out), method = "radix")
})

exists_table_method <- function(conn, name, ...) {
  call <- sys.call()
  check_open_connection(conn, call)
  table_exists(conn, table_name(conn, name, call))
}

#' @rdname zugodb_tables
#' @export
setMethod(
  "dbExistsTable",
  c("zugodb_connection", "character"),
  exists_table_method
)

#' @rdname zugodb_tables
#' @export
setMethod("dbExistsTable", c("zugodb_connection", "Id"), exists_table_method)

#' @rdname zugodb_tables
#' @export
setMethod(
  "dbExistsTable",
  c("zugodb_connection", "SQL"),
  exists_table_method
)


list_fields_method <- function(conn, name, ...) {
  call <- sys.call()
  check_open_connection(conn, call)
  table <- table_name(conn, name, call)
  if (!table_exists(conn, table)) {
    zug_abort(
      sprintf("The table `%s` does not exist.", table),
      "argument",
      call
    )
  }
  res <- dbSendQuery(
    conn,
    paste("SELECT * FROM", quote_name(table, call), "LIMIT 0")
  )
  on.exit(dbClearResult(res))
  dbColumnInfo(res)$name
}

#' @rdname zugodb_tables
#' @export
setMethod(
  "dbListFields",
  c("zugodb_connection", "character"),
  list_fields_method
)

#' @rdname zugodb_tables
#' @export
setMethod("dbListFields", c("zugodb_connection", "Id"), list_fields_method)

#' @rdname zugodb_tables
#' @export
setMethod("dbListFields", c("zugodb_connection", "SQL"), list_fields_method)


remove_table_method <- function(
  conn,
  name,
  ...,
  temporary = FALSE,
  fail_if_missing = TRUE
) {
  call <- sys.call()
  check_open_connection(conn, call)
  table <- table_name(conn, name, call)
  temporary <- check_flag(temporary, "temporary", call)
  fail_if_missing <- check_flag(fail_if_missing, "fail_if_missing", call)
  if (!table_exists(conn, table)) {
    if (fail_if_missing) {
      zug_abort(
        sprintf("The table `%s` does not exist.", table),
        "argument",
        call
      )
    }
    return(invisible(TRUE))
  }
  dbExecute(conn, paste("DROP TABLE", quote_name(table, call)))
  invisible(TRUE)
}

#' @rdname zugodb_tables
#' @export
setMethod(
  "dbRemoveTable",
  c("zugodb_connection", "character"),
  remove_table_method
)

#' @rdname zugodb_tables
#' @export
setMethod("dbRemoveTable", c("zugodb_connection", "Id"), remove_table_method)

#' @rdname zugodb_tables
#' @export
setMethod(
  "dbRemoveTable",
  c("zugodb_connection", "SQL"),
  remove_table_method
)


#' @rdname zugodb_tables
#' @export
setMethod(
  "dbQuoteIdentifier",
  c("zugodb_connection", "character"),
  function(conn, x, ...) {
    DBI::SQL(quote_name(x, sys.call()), names = names(x))
  }
)

#' @rdname zugodb_tables
#' @export
setMethod(
  "dbQuoteString",
  c("zugodb_connection", "character"),
  function(conn, x, ...) {
    # A ' inside doubled, as the engine's tokenizer reads it; NA is NULL.
    out <- if (length(x)) {
      paste0("'", gsub("'", "''", enc2utf8(x), fixed = TRUE), "'")
    } else {
      character()
    }
    out[is.na(x)] <- "NULL"
    DBI::SQL(out, names = names(x))
  }
)

# SQL is quoted already, so it is returned as given.
#' @rdname zugodb_tables
#' @export
setMethod(
  "dbQuoteString",
  c("zugodb_connection", "SQL"),
  function(conn, x, ...) x
)

#' @rdname zugodb_tables
#' @export
setMethod(
  "dbQuoteIdentifier",
  c("zugodb_connection", "SQL"),
  function(conn, x, ...) x
)

#' @rdname zugodb_tables
#' @export
setMethod("dbQuoteLiteral", "zugodb_connection", function(conn, x, ...) {
  if (
    !inherits(x, "blob") && !(identical(class(x), "list") && is_raw_list(x))
  ) {
    return(methods::callNextMethod())
  }
  # The engine reads no X'..' literal; a string of \xHH escapes cast to
  # BLOB is its spelling of bytes, as DuckDB's.
  out <- vapply(
    x,
    function(e) {
      if (is.null(e)) {
        return("NULL")
      }
      hex <- if (length(e)) {
        paste0("\\x", sprintf("%02x", as.integer(e)), collapse = "")
      } else {
        ""
      }
      paste0("CAST('", hex, "' AS BLOB)")
    },
    ""
  )
  DBI::SQL(unname(out), names = names(x))
})

read_table_method <- function(
  conn,
  name,
  ...,
  row.names = FALSE,
  check.names = TRUE
) {
  call <- sys.call()
  check_open_connection(conn, call)
  table <- table_name(conn, name, call)
  check_row_names(row.names, call)
  check.names <- check_flag(check.names, "check.names", call)
  if (!table_exists(conn, table)) {
    zug_abort(
      sprintf("The table `%s` does not exist.", table),
      "argument",
      call
    )
  }
  out <- DBI::dbGetQuery(conn, paste("SELECT * FROM", quote_name(table, call)))
  if (check.names) {
    names(out) <- make.names(names(out), unique = TRUE)
  }
  DBI::sqlColumnToRownames(out, row.names)
}

#' @rdname zugodb_tables
#' @param check.names For `dbReadTable()`: `TRUE` (the default) to make the
#'   column names syntactic with [make.names()].
#' @export
setMethod(
  "dbReadTable",
  c("zugodb_connection", "character"),
  read_table_method
)

#' @rdname zugodb_tables
#' @export
setMethod("dbReadTable", c("zugodb_connection", "Id"), read_table_method)

#' @rdname zugodb_tables
#' @export
setMethod("dbReadTable", c("zugodb_connection", "SQL"), read_table_method)

#' @rdname zugodb_tables
#' @param prefix For `dbListObjects()`: `NULL`, since the engine has no
#'   schemas.
#' @export
setMethod(
  "dbListObjects",
  "zugodb_connection",
  function(conn, prefix = NULL, ...) {
    call <- sys.call()
    check_open_connection(conn, call)
    if (!is.null(prefix)) {
      zug_abort(
        "The engine has no schemas or catalogs: `prefix` must be NULL.",
        "unsupported",
        call
      )
    }
    tables <- dbListTables(conn)
    out <- data.frame(
      table = I(lapply(tables, function(t) DBI::Id(table = t))),
      is_prefix = rep(FALSE, length(tables))
    )
    out
  }
)

data_type <- function(obj, call) {
  if (is.data.frame(obj)) {
    out <- vapply(obj, data_type, "", call = call)
    return(out)
  }
  p <- payload_of(obj)
  if (is.na(p)) {
    cannot_write(obj, "The value", call)
  }
  unname(sql_types[[p]])
}

#' @rdname zugodb_tables
#' @export
setMethod("dbDataType", "zugodb_connection", function(dbObj, obj, ...) {
  data_type(obj, sys.call())
})

#' @rdname zugodb_tables
#' @export
setMethod("dbDataType", "zugodb_driver", function(dbObj, obj, ...) {
  data_type(obj, sys.call())
})

transactions_unsupported <- function(call) {
  zug_abort(
    "The engine has no transactions: every statement commits as it runs.",
    "unsupported",
    call
  )
}

#' @rdname zugodb_tables
#' @export
setMethod("dbBegin", "zugodb_connection", function(conn, ...) {
  transactions_unsupported(sys.call())
})

#' @rdname zugodb_tables
#' @export
setMethod("dbCommit", "zugodb_connection", function(conn, ...) {
  transactions_unsupported(sys.call())
})

#' @rdname zugodb_tables
#' @export
setMethod("dbRollback", "zugodb_connection", function(conn, ...) {
  transactions_unsupported(sys.call())
})
