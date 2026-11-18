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

  parquet_types(con, dir)
  arrow_file(dir)
}

# arrow/flat.arrow: an Arrow IPC file written by R's arrow, for
# test-arrow.R (#40): SlothDB refuses Arrow IPC, and the test checks that
# a real file is refused by every route. Needs arrow; skipped without it.
arrow_file <- function(dir) {
  if (!requireNamespace("arrow", quietly = TRUE)) {
    message("arrow not installed: fixtures/arrow left as it is")
    return(invisible())
  }
  out <- file.path(dir, "arrow")
  dir.create(out, showWarnings = FALSE)
  path <- file.path(out, "flat.arrow")
  arrow::write_ipc_file(
    arrow::arrow_table(i = 1:3, s = c("a", NA, "c")),
    path
  )
  message(path, ": ", file.size(path), " bytes")
}

# parquet-types/<name>.parquet: one column `v` of each type, a value and a
# NULL, for test-parquet-types.R (#36 to #38). The reader decodes some
# faithfully and must refuse the rest; the values here are that test's
# oracle, so keep the two lists in step. mixed.parquet holds an INTEGER
# `ok` beside a DECIMAL `dec`, in row groups of 2048 rows.
parquet_types <- function(con, dir) {
  out <- file.path(dir, "parquet-types")
  dir.create(out, showWarnings = FALSE)
  types <- c(
    date = "DATE '1969-07-20'",
    ts_us = "TIMESTAMP '1969-12-31 23:59:59.5'",
    tstz = "TIMESTAMPTZ '2024-01-01 00:00:00+00'",
    u8 = "200::UTINYINT",
    u16 = "60000::USMALLINT",
    i8 = "(-5)::TINYINT",
    i16 = "(-300)::SMALLINT",
    dec4 = "1.25::DECIMAL(4,2)",
    dec18 = "123456789012.345678::DECIMAL(18,6)",
    dec38 = "12345678901234567890123456.789012345678::DECIMAL(38,12)",
    time = "TIME '12:34:56.789'",
    ts_ms = "TIMESTAMP_MS '2024-02-29 01:02:03.004'",
    ts_ns = "TIMESTAMP_NS '2024-02-29 01:02:03.004005006'",
    uuid = "'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'::UUID",
    interval = "INTERVAL 1 DAY",
    u32 = "3000000000::UINTEGER",
    u64 = "18000000000000000000::UBIGINT"
  )
  for (name in names(types)) {
    path <- file.path(out, paste0(name, ".parquet"))
    DBI::dbExecute(
      con,
      sprintf(
        "COPY (SELECT %s AS v UNION ALL SELECT NULL) TO %s (FORMAT parquet)",
        types[[name]], DBI::dbQuoteString(con, path)
      )
    )
  }
  path <- file.path(out, "mixed.parquet")
  DBI::dbExecute(
    con,
    sprintf(
      "COPY (SELECT range::INTEGER AS ok, (range / 4)::DECIMAL(9,2) AS dec
       FROM range(5000)) TO %s (FORMAT parquet, ROW_GROUP_SIZE 2048)",
      DBI::dbQuoteString(con, path)
    )
  )
  message(out, ": ", length(types) + 1L, " files, ",
          sum(file.size(list.files(out, full.names = TRUE))), " bytes")
}

main()
