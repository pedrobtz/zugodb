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
