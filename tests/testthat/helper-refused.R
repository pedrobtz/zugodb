# Helpers for test-refused.R.

refused_message <- "Reading or writing files from SQL is not supported"

# Real files of each format, under a fresh directory: one per extension.
refused_files <- function(dir) {
  fixture <- function(...) test_path("fixtures", ...)
  copy <- function(from, name) {
    to <- file.path(dir, name)
    stopifnot(file.copy(from, to))
    normalizePath(to, "/")
  }
  csv <- file.path(dir, "t.csv")
  writeLines(c("x,y", "1,a", "2,b"), csv)
  tsv <- file.path(dir, "t.tsv")
  writeLines(c("x\ty", "1\ta"), tsv)
  json <- file.path(dir, "t.json")
  writeLines('[{"x": 1}, {"x": 2}]', json)
  ndjson <- file.path(dir, "t.ndjson")
  writeLines(c('{"x": 1}', '{"x": 2}'), ndjson)
  list(
    csv = normalizePath(csv, "/"),
    tsv = normalizePath(tsv, "/"),
    json = normalizePath(json, "/"),
    ndjson = normalizePath(ndjson, "/"),
    jsonl = copy(ndjson, "t.jsonl"),
    parquet = copy(fixture("parquet-row-groups.parquet"), "t.parquet"),
    arrow = copy(fixture("arrow", "flat.arrow"), "t.arrow"),
    feather = copy(fixture("arrow", "flat.arrow"), "t.feather"),
    sqlite = copy(fixture("refused", "t.sqlite"), "t.sqlite"),
    db = copy(fixture("refused", "t.sqlite"), "t.db"),
    xlsx = copy(fixture("refused", "t.xlsx"), "t.xlsx"),
    avro = copy(fixture("refused", "t.avro"), "t.avro")
  )
}

expect_refused <- function(con, sql) {
  err <- expect_error(DBI::dbGetQuery(con, sql), class = "zug_sql_error", label = sql)
  expect_match(conditionMessage(err), refused_message, fixed = TRUE, label = sql)
  # The connection is still good.
  expect_identical(DBI::dbGetQuery(con, "SELECT 1 AS x")$x, 1L, label = sql)
}
