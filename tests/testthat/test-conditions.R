# Conditions (design §5): every error is a zuslothdb_error; the engine's own
# carry its message.

test_that("an engine error is a zuslothdb_sql_error with the engine's message", {
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  err <- expect_error(
    DBI::dbExecute(con, "INSERT INTO no_such_table VALUES (1)"),
    class = "zuslothdb_sql_error"
  )
  expect_s3_class(err, "zuslothdb_error")
  # The engine's message names the table; the glue adds nothing to it.
  expect_match(conditionMessage(err), "no_such_table", fixed = TRUE)
  expect_error(DBI::dbExecute(con, "SELEC 1"), class = "zuslothdb_sql_error")
})

test_that("params is zuslothdb_unsupported: SlothDB has no prepared statements", {
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  expect_error(
    DBI::dbExecute(con, "INSERT INTO t VALUES (?)", params = list(1L)),
    class = "zuslothdb_unsupported"
  )
})

test_that("the statement must be one string", {
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  for (bad in list(NA_character_, c("SELECT 1", "SELECT 2"), character())) {
    expect_error(DBI::dbExecute(con, bad), class = "zuslothdb_argument_error")
  }
})

test_that("params is unsupported on every route that takes it", {
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  expect_error(
    DBI::dbSendQuery(con, "SELECT ? AS a", params = list(1L)),
    class = "zuslothdb_unsupported"
  )
  expect_error(
    DBI::dbGetQuery(con, "SELECT ? AS a", params = list(1L)),
    class = "zuslothdb_unsupported"
  )
  expect_error(
    DBI::dbSendStatement(con, "INSERT INTO t VALUES (?)", params = list(1L)),
    class = "zuslothdb_unsupported"
  )
  res <- DBI::dbSendQuery(con, "SELECT 1 AS a")
  withr::defer(DBI::dbClearResult(res))
  expect_error(DBI::dbBind(res, list(1L)), class = "zuslothdb_unsupported")
})
