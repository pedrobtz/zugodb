# Parquet columns the reader decodes faithfully arrive with the values duckdb
# wrote; the ones it cannot (#36 to #38) are refused, naming the column,
# rather than read as wrong values. The fixtures, and so the values below,
# are written by tools/fixtures.R with duckdb (parquet_type() in
# helper-parquet.R).

test_that("the types the reader decodes faithfully arrive as written", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  utc <- function(x) structure(x, class = c("POSIXct", "POSIXt"), tzone = "UTC")
  want <- list(
    date = structure(c(-165, NA), class = "Date"),
    ts_us = utc(c(-0.5, NA)),
    tstz = utc(c(1704067200, NA)),
    u8 = c(200L, NA),
    u16 = c(60000L, NA),
    i8 = c(-5L, NA),
    i16 = c(-300L, NA)
  )
  for (name in names(want)) {
    expect_identical(parquet_type(con, name)$v, want[[name]], label = name)
  }
})

test_that("the types it would read wrongly are refused, naming the column", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  refused <- c(
    dec4 = "DECIMAL", dec18 = "DECIMAL", dec38 = "DECIMAL", time = "TIME",
    ts_ms = "milliseconds or nanoseconds", ts_ns = "milliseconds or nanoseconds",
    uuid = "UUID", interval = "INTERVAL", u32 = "unsigned", u64 = "unsigned"
  )
  for (name in names(refused)) {
    err <- expect_error(
      parquet_type(con, name), class = "zug_sql_error", label = name
    )
    expect_match(conditionMessage(err), "Parquet column 'v'", fixed = TRUE, label = name)
    expect_match(conditionMessage(err), refused[[name]], fixed = TRUE, label = name)
  }
  # The connection is still good.
  expect_identical(parquet_type(con, "i8")$v, c(-5L, NA))
})

test_that("a refused column does not stop queries on the other columns", {
  con <- DBI::dbConnect(zugodb(), threads = 4L)
  withr::defer(DBI::dbDisconnect(con))
  mixed <- function(sql) parquet_type(con, "mixed", sql)
  # A plan may decode every column (a bare LIMIT, ORDER BY); the refusal
  # is at bind time, so it fires only for a query that names `dec`.
  expect_identical(mixed("SELECT ok FROM '%s' WHERE ok < 3 ORDER BY ok")$ok, 0:2)
  expect_identical(mixed("SELECT ok FROM '%s' LIMIT 3")$ok, 0:2)
  expect_identical(mixed("SELECT ok FROM '%s' ORDER BY ok DESC LIMIT 2")$ok, 4999:4998)
  expect_identical(mixed("SELECT count(*) AS n FROM '%s'")$n, 5000)
  expect_identical(mixed("SELECT sum(ok) AS s FROM '%s'")$s, sum(as.double(0:4999)))
  expect_identical(
    mixed("SELECT ok %% 7 AS g, count(*) AS n FROM '%s' GROUP BY ok %% 7 ORDER BY g")$n,
    as.double(tabulate(0:4999 %% 7 + 1))
  )
  # Reading `dec` is refused on every route, across row groups and four
  # threads, and the session survives it (a decode worker that threw would
  # end it).
  for (sql in c(
    "SELECT dec FROM '%s'",
    "SELECT * FROM '%s'",
    "SELECT ok FROM '%s' WHERE dec > 1",
    "SELECT sum(dec) AS s FROM '%s'",
    "SELECT ok %% 7 AS g, sum(dec) AS s FROM '%s' GROUP BY ok %% 7",
    "SELECT t.* FROM '%s' AS t",
    "SELECT ok FROM '%s' ORDER BY dec LIMIT 1"
  )) {
    expect_error(mixed(sql), class = "zug_sql_error", label = sql)
  }
  expect_identical(mixed("SELECT count(*) AS n FROM '%s'")$n, 5000)
})
