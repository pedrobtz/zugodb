# tools/fixtures.R: writes the binary test fixtures under
# tests/testthat/fixtures/ with duckdb, from the package root, as
# `Rscript tools/fixtures.R`. Needs duckdb, which design §10 keeps out of
# Suggests; the files are committed, so the tests do not.
#
# parquet-row-groups.parquet: one INTEGER column x = 0, 1, ..., n - 1 in
# row groups of 2048 rows. The engine writes a Parquet file as a single
# row group, so it cannot make this itself. Several row groups, more than
# its decode workers read ahead (2 * threads + 2), are what start and
# throttle those workers; test-parquet-stop.R (#32) needs them.
main <- function() {
  dir <- file.path("tests", "testthat", "fixtures")
  dir.create(dir, showWarnings = FALSE, recursive = TRUE)
  con <- DBI::dbConnect(duckdb::duckdb(shared_home = FALSE))
  on.exit(DBI::dbDisconnect(con, shutdown = TRUE))
  path <- file.path(dir, "parquet-row-groups.parquet")
  n <- 40960L # helper-parquet.R checks answers against x < 40960
  DBI::dbExecute(
    con,
    sprintf(
      "COPY (SELECT range::INTEGER AS x FROM range(%d)) TO %s
       (FORMAT parquet, ROW_GROUP_SIZE 2048)",
      n,
      DBI::dbQuoteString(con, path)
    )
  )
  groups <- DBI::dbGetQuery(
    con,
    sprintf(
      "SELECT count(DISTINCT row_group_id) AS n FROM parquet_metadata(%s)",
      DBI::dbQuoteString(con, path)
    )
  )$n
  message(
    path,
    ": ",
    n,
    " rows in ",
    groups,
    " row groups, ",
    file.size(path),
    " bytes"
  )
}

main()
