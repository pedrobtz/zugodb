# Data frames written and read back are identical to what went in (roadmap
# Stage 5): every type dbWriteTable() writes, with NA, zero rows and more
# rows than an engine chunk (2048).

test_that("every writable type round-trips, with NA, through one chunk and many", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  for (n in c(0L, 1L, 5L, 2048L, 2049L, 5000L)) {
    df <- roundtrip_frame(n)
    DBI::dbWriteTable(con, "t", df, overwrite = TRUE)
    expect_identical(DBI::dbReadTable(con, "t"), df, label = paste(n, "rows"))
    expect_identical(
      DBI::dbGetQuery(con, "SELECT count(*) AS n FROM t")$n,
      as.double(n)
    )
  }
})

test_that("POSIXct keeps the microsecond, before 1970 and at the extremes", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  # Values read from duckdb's recorded answer: what a TIMESTAMP holds.
  ts <- oracle_answer("temporal-timestamp")$ts
  df <- data.frame(ts = ts)
  DBI::dbWriteTable(con, "t", df)
  expect_identical(DBI::dbReadTable(con, "t"), df)
  d <- oracle_answer("temporal-date")$d
  DBI::dbWriteTable(con, "d", data.frame(d = d))
  expect_identical(DBI::dbReadTable(con, "d")$d, d)
})

test_that("doubles keep NaN, Inf and their last bit; NA stays NA", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  x <- c(NaN, Inf, -Inf, NA, .Machine$double.xmin, 5e-324, 0.1 + 0.2, -0)
  DBI::dbWriteTable(con, "t", data.frame(x = x))
  got <- DBI::dbReadTable(con, "t")$x
  expect_identical(got, x)
  # identical() tells NA from NaN; so does this.
  expect_true(is.nan(got[[1]]))
  expect_false(is.nan(got[[4]]))
})

test_that("integer64 round-trips as BIGINT, at its extremes", {
  skip_if_not_installed("bit64")
  con <- DBI::dbConnect(zugodb(), bigint = "integer64")
  withr::defer(DBI::dbDisconnect(con))
  x <- bit64::as.integer64(c(
    "9223372036854775807",
    "-9223372036854775807",
    "0",
    NA,
    "9007199254740993"
  ))
  DBI::dbWriteTable(con, "t", data.frame(x = x))
  expect_identical(DBI::dbReadTable(con, "t")$x, x)
  expect_identical(DBI::dbListFields(con, "t"), "x")
  expect_identical(
    DBI::dbGetQuery(con, "SELECT CAST(x AS VARCHAR) AS s FROM t")$s,
    c(
      "9223372036854775807",
      "-9223372036854775807",
      "0",
      NA,
      "9007199254740993"
    )
  )
})

test_that("strings round-trip in UTF-8, whatever their declared encoding", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  s <- c("é", "日本", "a\"b'c", "", NA, strrep("x", 100))
  latin <- iconv("é", "UTF-8", "latin1")
  DBI::dbWriteTable(con, "t", data.frame(s = s, l = c(latin, rep("a", 5))))
  got <- DBI::dbReadTable(con, "t")
  expect_identical(got$s, s)
  expect_identical(got$l[[1]], "é")
  expect_identical(Encoding(got$l[[1]]), "UTF-8")
})

test_that("a round trip is the same under a hostile locale and options", {
  withr::local_options(OutDec = ",", scipen = -5, digits = 3)
  withr::local_locale(c(LC_COLLATE = "C"))
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  df <- roundtrip_frame(50L)
  DBI::dbWriteTable(con, "t", df)
  expect_identical(DBI::dbReadTable(con, "t"), df)
})
