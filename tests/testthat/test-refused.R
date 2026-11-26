# SQL reads and writes no file (design §7.5, Q17; roadmap Stage 6): since
# libzugodb 0.2.0 the parser refuses every file route before anything is
# opened, and 0.4.0 removed the readers behind the refusal. Each route is
# tried on a real file of its format (fixtures/, written by
# tools/fixtures.R and kept for Stage 10's readers, or written here), so a
# route that opened its file would answer, not fail. Every one is a
# zug_sql_error with the engine's one message, the connection then answers
# a query, and a COPY TO writes nothing.

test_that("every file table function is refused, on a real file of its format", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  f <- refused_files(withr::local_tempdir())
  calls <- c(
    sprintf("read_csv('%s')", f$csv),
    sprintf("read_csv_auto('%s')", f$csv),
    sprintf("read_csv('%s', delim = '\t')", f$tsv),
    sprintf("read_parquet('%s')", f$parquet),
    sprintf("parquet_scan('%s')", f$parquet),
    sprintf("read_json('%s')", f$json),
    sprintf("read_json_auto('%s')", f$json),
    sprintf("read_ndjson('%s')", f$ndjson),
    sprintf("read_arrow('%s')", f$arrow),
    sprintf("sqlite_scan('%s', 't')", f$sqlite),
    sprintf("read_xlsx('%s')", f$xlsx),
    sprintf("read_avro('%s')", f$avro)
  )
  for (call in calls) {
    expect_refused(con, paste("SELECT * FROM", call))
    expect_refused(con, paste("SELECT count(*) AS n FROM", call))
  }
})

test_that("a file path used as a table is refused, for every extension and a glob", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  dir <- withr::local_tempdir()
  f <- refused_files(dir)
  paths <- c(unlist(f), file.path(normalizePath(dir, "/"), "*.csv"))
  for (path in paths) {
    expect_refused(con, sprintf("SELECT * FROM '%s'", path))
    expect_refused(con, sprintf("DESCRIBE '%s'", path))
  }
})

test_that("COPY to and from a file is refused, and writes no file", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  dir <- withr::local_tempdir()
  f <- refused_files(dir)
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  before <- sort(list.files(dir, all.files = TRUE, no.. = TRUE))
  out <- normalizePath(dir, "/")
  for (fmt in c("csv", "parquet", "json")) {
    target <- file.path(out, paste0("out.", fmt))
    expect_refused(con, sprintf("COPY t TO '%s'", target))
    expect_refused(con, sprintf("COPY t TO '%s' (FORMAT %s)", target, fmt))
    expect_refused(con, sprintf("COPY (SELECT 1 AS x) TO '%s'", target))
  }
  expect_refused(con, sprintf("COPY t FROM '%s'", f$csv))
  expect_refused(con, sprintf("COPY t FROM '%s' (FORMAT parquet)", f$parquet))
  expect_identical(sort(list.files(dir, all.files = TRUE, no.. = TRUE)), before)
  expect_identical(DBI::dbGetQuery(con, "SELECT count(*) AS n FROM t")$n, 0)
})

test_that("a batch holding a refused statement runs none of its statements", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  f <- refused_files(withr::local_tempdir())
  sql <- sprintf(
    "CREATE TABLE b (x INTEGER); SELECT * FROM read_csv('%s')",
    f$csv
  )
  err <- expect_error(DBI::dbExecute(con, sql), class = "zug_sql_error")
  expect_match(conditionMessage(err), refused_message, fixed = TRUE)
  expect_false(DBI::dbExistsTable(con, "b"))
})

test_that("CREATE LIVE VIEW is refused", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  f <- refused_files(withr::local_tempdir())
  expect_refused(
    con,
    sprintf("CREATE LIVE VIEW v AS SELECT * FROM '%s'", f$csv)
  )
  expect_false("v" %in% DBI::dbListTables(con))
})
