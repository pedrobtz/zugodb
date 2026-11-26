#' Write Arrow C streams into tables
#'
#' [DBI::dbWriteTableArrow()] creates a table from an Arrow C stream, or
#' replaces one (`overwrite = TRUE`) or adds to one (`append = TRUE`);
#' [DBI::dbAppendTableArrow()] adds a stream's rows to a table that exists;
#' [DBI::dbCreateTableArrow()] creates an empty table from an Arrow schema.
#' Any producer of the Arrow C stream interface works: a
#' `nanoarrow_array_stream`, or anything [nanoarrow::as_nanoarrow_array_stream()]
#' converts, such as a data frame (`nanoarrow` is then needed). The stream
#' is read once, one batch at a time, and released: it cannot be read
#' again.
#'
#' A new table's columns take these engine types:
#'
#' | Arrow | Engine |
#' | --- | --- |
#' | boolean | `BOOLEAN` |
#' | int8 | `TINYINT` |
#' | int16, uint8 | `SMALLINT` |
#' | int32, uint16 | `INTEGER` |
#' | int64, uint32, uint64 | `BIGINT` |
#' | float, double | `FLOAT`, `DOUBLE` |
#' | string, large string, string view | `VARCHAR` |
#' | binary, large binary, binary view, fixed-size binary | `BLOB` |
#' | date32, date64 | `DATE` |
#' | timestamp, any unit and time zone | `TIMESTAMP`, the instant in UTC |
#'
#' Another Arrow type (a decimal, a time of day, a duration, an interval,
#' a list, a struct, a map, a union, a dictionary, an extension type, the
#' null type, a half float) is a `zug_type_error` naming the column, raised
#' from the stream's schema before any row is read. So is a value its
#' column cannot hold: a uint64 past 2^63 - 1, a timestamp with a fraction
#' of a microsecond, a string that is not valid UTF-8 or holds a NUL. An
#' append matches columns by name, as [DBI::dbAppendTable()] does, and
#' converts as it does (a whole double into an `INTEGER`, say).
#'
#' A load that fails part-way, by such a value, by the producer's own error
#' (a `zug_stream_error` carrying its message) or by an interrupt, leaves
#' nothing: a new table is removed, and an append or an overwrite reads the
#' stream into a staging table first, so that the table it writes is only
#' changed once the whole stream has been read. The engine has no
#' transactions, so this costs a copy of the rows.
#'
#' @param conn A `zugodb_connection`.
#' @param name The table's name, as for [DBI::dbWriteTable()].
#' @param value For `dbWriteTableArrow()` and `dbAppendTableArrow()`, an
#'   Arrow C stream (a `nanoarrow_array_stream`) or what
#'   [nanoarrow::as_nanoarrow_array_stream()] converts. For
#'   `dbCreateTableArrow()`, a `nanoarrow_schema`, or what
#'   [nanoarrow::infer_nanoarrow_schema()] reads one from.
#' @param overwrite,append `TRUE` to replace or to add to a table that
#'   exists; at most one of them.
#' @param temporary Not supported: the engine has no temporary tables.
#' @param ... Not used.
#' @return `dbWriteTableArrow()`, `dbCreateTableArrow()`: `TRUE`,
#'   invisibly. `dbAppendTableArrow()`: the number of rows written.
#' @name zugodb_arrow
#' @examplesIf requireNamespace("nanoarrow", quietly = TRUE)
#' con <- DBI::dbConnect(zugodb())
#' stream <- nanoarrow::as_nanoarrow_array_stream(head(mtcars))
#' DBI::dbWriteTableArrow(con, "cars", stream)
#' DBI::dbAppendTableArrow(con, "cars", tail(mtcars))
#' DBI::dbGetQuery(con, "SELECT count(*) AS n FROM cars")
#' DBI::dbDisconnect(con)
NULL

# A holder of this package's that owns the stream `value` (src/zug_stream.cpp);
# the producer's object is left released.
take_stream <- function(value, call) {
  if (!inherits(value, "nanoarrow_array_stream")) {
    if (!requireNamespace("nanoarrow", quietly = TRUE)) {
      zug_abort(
        "`value` must be a nanoarrow_array_stream; to convert another object, install nanoarrow.",
        "argument",
        call
      )
    }
    value <- tryCatch(
      nanoarrow::as_nanoarrow_array_stream(value),
      error = function(e) {
        zug_abort(
          paste(
            "`value` cannot be read as an Arrow C stream:",
            conditionMessage(e)
          ),
          "argument",
          call
        )
      }
    )
  }
  zug_value(.Call(zug_stream_take, value), call)
}

release_stream <- function(holder) {
  invisible(.Call(zug_stream_release, holder))
}

# The columns of a stream or schema, list(names, sql, what), checked: each
# named once, and each of a type a table can hold.
check_arrow_columns <- function(cols, call) {
  nms <- cols$names
  if (any(!nzchar(nms))) {
    zug_abort("Every column of the stream must have a name.", "argument", call)
  }
  # Names match ignoring ASCII case, as the engine's do.
  dup <- anyDuplicated(ascii_lower(nms))
  if (dup) {
    zug_abort(
      sprintf("The stream has the column `%s` twice.", nms[[dup]]),
      "argument",
      call
    )
  }
  quote_name(nms, call)
  bad <- which(is.na(cols$sql))
  if (length(bad)) {
    k <- bad[[1L]]
    zug_abort(
      sprintf(
        "Column `%s` is %s, which zugodb cannot hold.",
        nms[[k]],
        cols$what[[k]]
      ),
      "type",
      call
    )
  }
  cols
}

schema_columns <- function(value, call) {
  if (!inherits(value, "nanoarrow_schema")) {
    if (!requireNamespace("nanoarrow", quietly = TRUE)) {
      zug_abort(
        "`value` must be a nanoarrow_schema; to read one from another object, install nanoarrow.",
        "argument",
        call
      )
    }
    value <- tryCatch(
      nanoarrow::infer_nanoarrow_schema(value),
      error = function(e) {
        zug_abort(
          paste("`value` has no Arrow schema:", conditionMessage(e)),
          "argument",
          call
        )
      }
    )
  }
  check_arrow_columns(zug_value(.Call(zug_schema_columns, value), call), call)
}

# A table name no other table has, for a load's staging table.
staging_name <- function(conn, table) {
  staging <- paste0("__zugodb_load_", table, "__")
  tables <- dbListTables(conn)
  while (ascii_lower(staging) %in% ascii_lower(tables)) {
    staging <- paste0(staging, "_")
  }
  staging
}

drop_quietly <- function(conn, table, call) {
  try(
    dbExecute(conn, paste("DROP TABLE IF EXISTS", quote_name(table, call))),
    silent = TRUE
  )
}

stream_into <- function(conn, holder, table, map, call) {
  zug_value(
    .Call(
      zug_stream_append,
      conn@state$ptr,
      holder,
      table,
      quote_name(table, call),
      as.integer(map)
    ),
    call
  )
}

copy_rows <- function(conn, from, to, fields, call) {
  cols <- paste(quote_name(fields, call), collapse = ", ")
  dbExecute(
    conn,
    sprintf(
      "INSERT INTO %s (%s) SELECT %s FROM %s",
      quote_name(to, call),
      cols,
      cols,
      quote_name(from, call)
    )
  )
}

# The stream's rows appended to the existing `table`, whole or not at all:
# read into a staging table of the same types, then copied.
append_stream <- function(conn, table, holder, call) {
  cols <- zug_value(.Call(zug_stream_columns, holder), call)
  fields <- dbListFields(conn, table)
  extra <- setdiff(cols$names, fields)
  if (length(extra)) {
    zug_abort(
      sprintf("The table `%s` has no column `%s`.", table, extra[[1L]]),
      "argument",
      call
    )
  }
  dup <- anyDuplicated(ascii_lower(cols$names))
  if (dup) {
    zug_abort(
      sprintf("The stream has the column `%s` twice.", cols$names[[dup]]),
      "argument",
      call
    )
  }
  map <- match(fields, cols$names) - 1L
  map[is.na(map)] <- -1L
  staging <- staging_name(conn, table)
  on.exit(drop_quietly(conn, staging, call), add = TRUE)
  dbExecute(
    conn,
    sprintf(
      "CREATE TABLE %s AS SELECT * FROM %s LIMIT 0",
      quote_name(staging, call),
      quote_name(table, call)
    )
  )
  rows <- stream_into(conn, holder, staging, map, call)
  copy_rows(conn, staging, table, fields, call)
  rows
}

create_from_columns <- function(conn, table, cols, call) {
  dbExecute(conn, create_sql(table, cols$names, cols$sql, call))
}

check_not_temporary <- function(temporary, call) {
  if (check_flag(temporary, "temporary", call)) {
    zug_abort("The engine has no temporary tables.", "unsupported", call)
  }
}

ascii_lower <- function(x) {
  chartr("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz", x)
}

table_exists <- function(conn, table) {
  ascii_lower(table) %in% ascii_lower(dbListTables(conn))
}

write_table_arrow_method <- function(
  conn,
  name,
  value,
  ...,
  overwrite = FALSE,
  append = FALSE,
  temporary = FALSE
) {
  call <- sys.call()
  check_open_connection(conn, call)
  table <- table_name(conn, name, call)
  overwrite <- check_flag(overwrite, "overwrite", call)
  append <- check_flag(append, "append", call)
  check_not_temporary(temporary, call)
  if (overwrite && append) {
    zug_abort(
      "`overwrite` and `append` cannot both be TRUE.",
      "argument",
      call
    )
  }
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
  holder <- take_stream(value, call)
  on.exit(release_stream(holder), add = TRUE)
  if (exists && append) {
    append_stream(conn, table, holder, call)
    return(invisible(TRUE))
  }
  cols <- check_arrow_columns(
    zug_value(.Call(zug_stream_columns, holder), call),
    call
  )
  map <- seq_along(cols$names) - 1L
  if (!exists) {
    # A new table, removed if the stream fails part-way.
    create_from_columns(conn, table, cols, call)
    done <- FALSE
    on.exit(if (!done) drop_quietly(conn, table, call), add = TRUE)
    stream_into(conn, holder, table, map, call)
    done <- TRUE
    return(invisible(TRUE))
  }
  # An overwrite: the old table is dropped only once the whole stream is
  # read, into a staging table of the new types (the engine has no rename).
  staging <- staging_name(conn, table)
  on.exit(drop_quietly(conn, staging, call), add = TRUE)
  create_from_columns(conn, staging, cols, call)
  stream_into(conn, holder, staging, map, call)
  dbExecute(conn, paste("DROP TABLE", quote_name(table, call)))
  create_from_columns(conn, table, cols, call)
  copy_rows(conn, staging, table, cols$names, call)
  invisible(TRUE)
}

#' @rdname zugodb_arrow
#' @export
setMethod("dbWriteTableArrow", "zugodb_connection", write_table_arrow_method)

#' @rdname zugodb_arrow
#' @export
setMethod(
  "dbAppendTableArrow",
  "zugodb_connection",
  function(conn, name, value, ...) {
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
    holder <- take_stream(value, call)
    on.exit(release_stream(holder), add = TRUE)
    append_stream(conn, table, holder, call)
  }
)

#' @rdname zugodb_arrow
#' @export
setMethod(
  "dbCreateTableArrow",
  "zugodb_connection",
  function(conn, name, value, ..., temporary = FALSE) {
    call <- sys.call()
    check_open_connection(conn, call)
    table <- table_name(conn, name, call)
    check_not_temporary(temporary, call)
    cols <- schema_columns(value, call)
    if (table_exists(conn, table)) {
      zug_abort(sprintf("The table `%s` exists.", table), "argument", call)
    }
    create_from_columns(conn, table, cols, call)
    invisible(TRUE)
  }
)

# The tests' producer (src/zug_stream.cpp): x (int32, NULL at every fifth
# row) and s ("r<row>"), `per` rows a batch for `nbatch` batches, failing at
# batch `fail_at` with `message`, or there giving a batch of one column with
# `mismatch`. Internal.
zug_scripted_stream <- function(
  nbatch,
  per,
  fail_at = 0L,
  message = "",
  mismatch = FALSE
) {
  .Call(
    zug_test_stream,
    as.integer(nbatch),
    as.integer(per),
    as.integer(fail_at),
    as.character(message),
    as.logical(mismatch)
  )
}

zug_scripted_releases <- function() {
  .Call(zug_test_stream_releases)
}
