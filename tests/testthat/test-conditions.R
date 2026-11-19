# Conditions (design §5): every error is a zug_error; the engine's own
# carry its message.

test_that("an engine error is a zug_sql_error with the engine's message", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  err <- expect_error(
    DBI::dbExecute(con, "INSERT INTO no_such_table VALUES (1)"),
    class = "zug_sql_error"
  )
  expect_s3_class(err, "zug_error")
  # The engine's message names the table; the glue adds nothing to it.
  expect_match(conditionMessage(err), "no_such_table", fixed = TRUE)
  expect_error(DBI::dbExecute(con, "SELEC 1"), class = "zug_sql_error")
})

test_that("params is zug_unsupported: SlothDB has no prepared statements", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  expect_error(
    DBI::dbExecute(con, "INSERT INTO t VALUES (?)", params = list(1L)),
    class = "zug_unsupported"
  )
})

test_that("the statement must be one string", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  for (bad in list(NA_character_, c("SELECT 1", "SELECT 2"), character())) {
    expect_error(DBI::dbExecute(con, bad), class = "zug_argument_error")
  }
})

test_that("params is unsupported on every route that takes it", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  expect_error(
    DBI::dbSendQuery(con, "SELECT ? AS a", params = list(1L)),
    class = "zug_unsupported"
  )
  expect_error(
    DBI::dbGetQuery(con, "SELECT ? AS a", params = list(1L)),
    class = "zug_unsupported"
  )
  expect_error(
    DBI::dbSendStatement(con, "INSERT INTO t VALUES (?)", params = list(1L)),
    class = "zug_unsupported"
  )
  res <- DBI::dbSendQuery(con, "SELECT 1 AS a")
  withr::defer(DBI::dbClearResult(res))
  expect_error(DBI::dbBind(res, list(1L)), class = "zug_unsupported")
})

test_that("HUGEINT, TIME, INTERVAL and UUID are type errors naming the column", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (tm TIME, iv INTERVAL, id UUID)")
  DBI::dbExecute(
    con,
    "INSERT INTO t VALUES (CAST('01:00:00' AS TIME), NULL, NULL)"
  )
  # HUGEINT reaches a result only through a cast of a column, and its
  # values are wrong there (design §6, Q1): an error whatever bigint says.
  huge <- "SELECT CAST(x AS HUGEINT) AS huge_col FROM (SELECT 42 AS x)"
  for (bigint in c("numeric", "integer64", "character")) {
    con2 <- DBI::dbConnect(zugodb(), bigint = bigint)
    err <- expect_error(
      DBI::dbGetQuery(con2, huge),
      class = "zug_type_error"
    )
    expect_match(conditionMessage(err), "`huge_col`", fixed = TRUE)
    expect_match(conditionMessage(err), "HUGEINT", fixed = TRUE)
    DBI::dbDisconnect(con2)
  }
  # TIME: every value SlothDB computes is midnight (#43).
  for (col in c("tm", "iv", "id")) {
    err <- expect_error(
      DBI::dbGetQuery(con, sprintf("SELECT %s FROM t", col)),
      class = "zug_type_error"
    )
    expect_match(conditionMessage(err), sprintf("`%s`", col), fixed = TRUE)
  }
})
