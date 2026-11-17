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
