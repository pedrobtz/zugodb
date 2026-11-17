# zuslothdb and duckdb in one R session (roadmap Stage 1). SlothDB carries
# DuckDB's duckdb_fastpforlib namespace; were its symbols exported, one
# package's calls could bind to the other's copy. Each must answer its own
# query, in both load orders, before and after the other has run.
#
#   Rscript tools/ci/coexist.R zuslothdb-first | duckdb-first
#
# Run by tools/ci/platform.sh with both packages installed. Not in the
# testthat suite: design §10 keeps duckdb out of Suggests.
order <- commandArgs(trailingOnly = TRUE)[1]
stopifnot(order %in% c("zuslothdb-first", "duckdb-first"))

duck <- function() {
  con <- DBI::dbConnect(duckdb::duckdb(shared_home = FALSE))
  on.exit(DBI::dbDisconnect(con, shutdown = TRUE))
  # A Parquet round trip goes through DuckDB's own fastpforlib.
  path <- tempfile(fileext = ".parquet")
  DBI::dbExecute(con, sprintf(
    "COPY (SELECT range::INTEGER AS i FROM range(10000)) TO '%s'", path
  ))
  DBI::dbGetQuery(con, sprintf("SELECT sum(i)::INTEGER AS s FROM '%s'", path))$s
}
zu <- function() zuslothdb:::sdb_query_int("SELECT 42")

if (order == "zuslothdb-first") {
  loadNamespace("zuslothdb")
  a <- zu()
  loadNamespace("duckdb")
  b <- duck()
} else {
  loadNamespace("duckdb")
  b <- duck()
  loadNamespace("zuslothdb")
  a <- zu()
}
again <- c(zu(), duck())
want <- c(42L, 49995000L)
got <- c(a, b)
cat(order, ": zuslothdb", a, "duckdb", b, "then", again, "\n")
if (!identical(got, want) || !identical(again, want)) {
  cat("FAIL: expected", want, "\n")
  quit(status = 1)
}
cat("ok\n")
