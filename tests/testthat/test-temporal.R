# DATE and TIMESTAMP (design §6; roadmap Stage 4): duckdb's recorded
# answers before 1970, on leap days, to the microsecond and at each type's
# extremes, and the time zone that timezone_out gives a POSIXct. TIME is
# refused (test-conditions.R, diff-time-cast).

test_that("every temporal query agrees with duckdb's recorded answer", {
  names <- oracle_names("temporal-")
  expect_length(names, 2L)
  for (name in names) {
    expect_identical(zugodb_answer(name), oracle_answer(name), label = name)
  }
})

test_that("timezone_out sets the zone and keeps the instant", {
  want <- oracle_answer("temporal-timestamp")$ts
  for (tz in c("Europe/Lisbon", "America/Sao_Paulo", "")) {
    got <- zugodb_answer("temporal-timestamp", timezone_out = tz)$ts
    expect_identical(attr(got, "tzone"), tz, label = tz)
    expect_identical(unclass(got), `attr<-`(unclass(want), "tzone", tz))
  }
  # As in duckdb, whose answer with this zone was recorded by hand:
  # TIMESTAMP '2024-07-01 00:00:00' is 1719792000 in any zone.
  con <- DBI::dbConnect(zugodb(), timezone_out = "Europe/Lisbon")
  withr::defer(DBI::dbDisconnect(con))
  got <- DBI::dbGetQuery(con, "SELECT TIMESTAMP '2024-07-01 00:00:00' AS ts")$ts
  expect_identical(
    got,
    structure(
      1719792000,
      class = c("POSIXct", "POSIXt"),
      tzone = "Europe/Lisbon"
    )
  )
  expect_identical(format(got), "2024-07-01 01:00:00")
})

test_that("timezone_out must be a time zone R knows, or \"\"", {
  for (bad in list(
    "Mars/Olympus",
    "utc",
    NA_character_,
    c("UTC", "UTC"),
    1,
    NULL
  )) {
    expect_error(
      DBI::dbConnect(zugodb(), timezone_out = bad),
      class = "zug_argument_error",
      label = deparse(bad)
    )
  }
})

test_that("temporal columns keep their class in every slice", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  for (s in oracle_statements(test_path("oracle", "temporal-timestamp.sql"))[
    1:2
  ]) {
    DBI::dbExecute(con, s)
  }
  res <- DBI::dbSendQuery(
    con,
    "SELECT ts, CAST(ts AS DATE) AS d FROM t ORDER BY k"
  )
  withr::defer(DBI::dbClearResult(res))
  parts <- list(DBI::dbFetch(res, 3), DBI::dbFetch(res, 0), DBI::dbFetch(res))
  want <- oracle_answer("temporal-timestamp")$ts
  expect_identical(
    c(nrow(parts[[1]]), nrow(parts[[2]]), nrow(parts[[3]])),
    c(3L, 0L, 5L)
  )
  for (p in parts) {
    expect_s3_class(p$ts, "POSIXct")
    expect_identical(attr(p$ts, "tzone"), "UTC")
    expect_s3_class(p$d, "Date")
  }
  expect_identical(
    c(unclass(parts[[1]]$ts), unclass(parts[[3]]$ts)),
    as.vector(unclass(want))
  )
})

test_that("dbColumnInfo() names the R class each temporal type arrives as", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  res <- DBI::dbSendQuery(
    con,
    "SELECT DATE '2024-01-01' AS d, TIMESTAMP '2024-01-01 00:00:00' AS ts,
            'a'::BLOB AS b"
  )
  withr::defer(DBI::dbClearResult(res))
  info <- DBI::dbColumnInfo(res)
  expect_identical(info$.sql_type, c("DATE", "TIMESTAMP", "BLOB"))
  expect_identical(info$type, c("Date", "POSIXct", "blob"))
})
