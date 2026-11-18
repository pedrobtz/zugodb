# Helpers for the result tests (Stage 3).

# Every row of `sql`, fetched `n` at a time: the bound data frame, the row
# count the result reports, and each slice's length.
fetch_all_by <- function(con, sql, n) {
  res <- DBI::dbSendQuery(con, sql)
  on.exit(DBI::dbClearResult(res))
  parts <- list()
  repeat {
    part <- DBI::dbFetch(res, n)
    parts[[length(parts) + 1L]] <- part
    if (DBI::dbHasCompleted(res)) break
  }
  out <- do.call(rbind, parts)
  attr(out, "row.names") <- .set_row_names(nrow(out))
  list(df = out, count = DBI::dbGetRowCount(res), parts = lengths(lapply(parts, `[[`, 1L)))
}

# The BIGINT values of `literals`, in order, through one query.
bigint_query <- function(con, literals) {
  sql <- sprintf(
    "SELECT v FROM (VALUES %s) AS t(i, v) ORDER BY i",
    paste(sprintf("(%d, CAST(%s AS BIGINT))", seq_along(literals), literals),
          collapse = ", ")
  )
  DBI::dbGetQuery(con, sql)$v
}

# The classes zuslothdb gives a BLOB column, those of blob::blob() (Stage 4).
blob_classes <- c("blob", "vctrs_list_of", "vctrs_vctr", "list")

# `df` with each BLOB column a plain list, as duckdb returns one.
strip_blob <- function(df) {
  for (k in seq_along(df)) {
    if (inherits(df[[k]], "blob")) {
      df[[k]] <- unclass(`attr<-`(df[[k]], "ptype", NULL))
    }
  }
  df
}

# A data frame of every type dbWriteTable() writes, n rows, with NA in each
# column (Stage 5). Timestamps are whole microseconds, as a TIMESTAMP holds.
roundtrip_frame <- function(n) {
  i <- seq_len(n)
  na <- i %% 7L == 3L
  pick <- function(x) {
    x[na] <- NA
    x
  }
  ts <- round(((i - 2500) * 86400.123457) * 1e6) / 1e6
  blob <- lapply(i, function(k) if (k %% 7L == 3L) NULL else as.raw(c(k %% 256L, 0L, 255L)))
  df <- data.frame(
    lgl = pick(i %% 2L == 0L),
    int = pick(i - 1000L),
    dbl = pick(i / 3 - 10),
    chr = pick(sprintf("s%d\u00e9", i)),
    date = structure(pick(as.double(i - 30000L)), class = "Date"),
    ts = structure(pick(ts), class = c("POSIXct", "POSIXt"), tzone = "UTC"),
    stringsAsFactors = FALSE
  )
  # A list column, without the AsIs class I() would add.
  df$blob <- structure(blob, ptype = raw(0), class = blob_classes)
  df
}

# Every row of the table `name`, quoted (Stage 5).
table_rows <- function(con, name) {
  DBI::dbGetQuery(con, sprintf("SELECT * FROM \"%s\"", name))
}

# The rows of `SELECT * FROM '<file>'` for a JSON file holding `lines`
# (test-json.R).
json_rows <- function(con, lines, ext = ".json") {
  path <- withr::local_tempfile(fileext = ext, .local_envir = parent.frame())
  writeLines(lines, path, useBytes = TRUE)
  DBI::dbGetQuery(con, sprintf("SELECT * FROM '%s'", normalizePath(path, "/")))
}
