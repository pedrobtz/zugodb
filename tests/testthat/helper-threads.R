# Helpers for test-threads.R: a partitioned window query, and its answer
# computed in R.

# A table of n rows in four partitions (g), with v NULL on every 97th row,
# and the rank and the previous row of each in its partition, ordered by v
# descending (NULLs last, duckdb's default) and then i.
window_rows <- function(n) {
  i <- seq_len(n)
  data.frame(
    i = i,
    g = i %% 4L,
    v = ifelse(i %% 97L == 0L, NA_integer_, (i * 7919L) %% 1000L)
  )
}

window_answer <- function(df) {
  o <- order(df$g, -df$v, df$i, na.last = TRUE)
  rn <- numeric(nrow(df))
  prev <- rep(NA_integer_, nrow(df))
  for (k in split(o, df$g[o])) {
    rn[k] <- seq_along(k)
    prev[k[-1L]] <- df$i[k[-length(k)]]
  }
  data.frame(i = df$i, rn = rn, prev = prev)
}

window_query <- function(df, threads) {
  con <- DBI::dbConnect(zugodb(), threads = threads)
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbWriteTable(con, "t", df)
  DBI::dbGetQuery(
    con,
    "
    SELECT i,
           row_number() OVER (PARTITION BY g ORDER BY v DESC, i) AS rn,
           lag(i) OVER (PARTITION BY g ORDER BY v DESC, i) AS prev
    FROM t ORDER BY i"
  )
}
